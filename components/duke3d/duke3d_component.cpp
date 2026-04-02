#include "duke3d_component.h"
#include "duke3d_wifi_sync.h"
#include "esphome/components/hub75_matrix/hub75_matrix.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/log.h"
#include "esp_task_wdt.h"
#include "esphome/components/sd_card/sd_card.h"
#include "esphome/components/hud/hud.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/idf_additions.h"
#include "esp32_hal.h"
#include "tilecache.h"
#include "input.h"
#include "usb_gamepad.h"
#include <cstring>
#include <dirent.h>
#include <strings.h>

// Duke3D engine entry point — defined in engine_main_shim.c, which calls
// the engine's main() in game.c.
extern "C" int duke3d_main(int argc, char** argv);

// Forward-declare the HUD instance pointer from the hud component.
// Defined in hud.cpp (esphome::hud namespace); accessed in setup() below.
namespace esphome { namespace hud { extern Hud* global_hud_instance; } }

// global_hud: wires the HUD into platform_blit_frame() for per-frame overlay.
// Declared extern in esp32_hal.cpp; defined here.
esphome::hud::Hud* global_hud = nullptr;

// Set by loop() (Core 0) to request cooperative suspension of the game task.
// Cleared by loop() after WiFi window ends, before vTaskResume().
volatile bool g_wifi_window_requested = false;

// Engine dependency smoke-test: call a real Duke engine routine.
// Implemented in `engine/components/Engine/fixedPoint_math.c`.
extern "C" void clearbufbyte(void* D, int32_t c, int32_t a);

static esphome::duke3d::Duke3DComponent* g_duke3d_component = nullptr;

namespace esphome {
namespace duke3d {

static const char* TAG = "duke3d";

namespace {

constexpr EventBits_t DUKE3D_BOOTSTRAP_OK = 1 << 0;

bool filename_is_dmo(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot)
        return false;
    return strcasecmp(dot, ".dmo") == 0;
}

// Collect *.dmo from game_dir on SD, pick one at random. Returns false if none found.
bool pick_random_demo_dmo(const char* game_dir, char* out, size_t out_sz) {
    constexpr size_t kMaxDemos = 24;
    char names[kMaxDemos][32];
    size_t n = 0;

    DIR* d = opendir(game_dir);
    if (!d) {
        ESP_LOGE(TAG, "opendir(%s) failed — cannot scan for .dmo", game_dir);
        return false;
    }
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr && n < kMaxDemos) {
        if (ent->d_name[0] == '.')
            continue;
#if defined(DT_REG) && defined(DT_UNKNOWN)
        if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN)
            continue;
#endif
        if (!filename_is_dmo(ent->d_name))
            continue;
        strncpy(names[n], ent->d_name, sizeof(names[0]) - 1);
        names[n][sizeof(names[0]) - 1] = '\0';
        n++;
    }
    closedir(d);

    if (n == 0) {
        ESP_LOGW(TAG, "no .dmo files in %s", game_dir);
        return false;
    }
    const size_t pick = (size_t)(esp_random() % (uint32_t)n);
    snprintf(out, out_sz, "%s", names[pick]);
    return true;
}

}  // namespace

void Duke3DComponent::setup() {
    ESP_LOGI(TAG, "setup(smoke_test=%s)", smoke_test_ ? "true" : "false");
    if (!smoke_test_) {
        auto* sd = sd_card::global_sd_card;
        if (!sd || !sd->grp_present()) {
            ESP_LOGE(TAG, "DUKE3D.GRP not found on SD card — halting game component");
            mark_failed();
            return;
        }
    }

    sync_events_ = xEventGroupCreate();
    if (!sync_events_) {
        ESP_LOGE(TAG, "xEventGroupCreate failed");
        mark_failed();
        return;
    }

    global_hud = esphome::hud::global_hud_instance;
    // Until duke3d_main(), Hud::loop() drives render+swap from back_buf_ (splash rows 0–39 from
    // Hub75::setup, same direct drawPixel path as the old red/blue diagnostic). Smoke test skips HUD.
    if (global_hud) global_hud->set_game_running(smoke_test_);

    g_duke3d_component = this;
    input_init();

    if (usb_gamepad_) {
        bool started = usb_gamepad_start();
        if (started) {
            ESP_LOGI(TAG, "USB gamepad host started (hold BOOT button at power-on to switch to debug mode)");
        } else {
            ESP_LOGW(TAG, "USB debug mode active — connect USB-C for JTAG/CDC logging");
        }
    }

    ESP_LOGI(TAG, "Free heap before task create: %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "Free internal heap: %lu bytes",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    const int stack = smoke_test_ ? SMOKE_TASK_STACK : TASK_STACK;
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        smoke_test_ ? smoke_task : game_task,
        smoke_test_ ? "duke3d_smoke" : "duke3d",
        stack,
        this,
        5,
        &task_handle_,
        1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore FAILED (rc=%d) — not enough RAM?", rc);
        mark_failed();
        return;
    }
    ESP_LOGI(TAG, "Duke3D %s task started on Core 1 (stack=%d)", smoke_test_ ? "smoke" : "game", stack);
}

void Duke3DComponent::loop() {
    if (!pause_wifi_) return;

    const uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    const int64_t now_us = esp_timer_get_time();

    // --- Phase 1: keep WiFi up until Home Assistant / API have had time to push state ---
    // Releasing too early (e.g. fixed grace only) stops WiFi before HA time sync + sensor pushes,
    // leaving the HUD at 00:00 and 0° until the next in-game WiFi window.
    if (!bootstrap_released_) {
        wifi_ap_record_t ap{};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            if (first_wifi_connected_at_us_ == 0) first_wifi_connected_at_us_ = now_us;
        }
        const bool grace_ok =
            first_wifi_connected_at_us_ != 0 &&
            (now_us - first_wifi_connected_at_us_) >=
                (int64_t)wifi_bootstrap_grace_s_ * 1000000LL;
        const bool ha_time_ready = (time_id_ == nullptr) || time_id_->now().is_valid();
        const bool timeout = now_us >= 120000000LL;  // 120 s absolute cap

        if ((grace_ok && ha_time_ready) || timeout) {
            xEventGroupSetBits(sync_events_, DUKE3D_BOOTSTRAP_OK);
            bootstrap_released_ = true;
            ESP_LOGI(TAG,
                     "Bootstrap: released (assoc_grace_ok=%s ha_time_ready=%s timeout=%s)",
                     grace_ok ? "yes" : "no", ha_time_ready ? "yes" : "no", timeout ? "yes" : "no");
        }
        return;
    }

    // --- Phase 2: cooperative HA sync on non-demo level loads (debounced) ---
    switch (wifi_state_) {
        case WifiWindowState::STOPPED:
            if (ha_sync_pending_) {
                ha_sync_pending_ = false;
                ESP_LOGI(TAG, "HA sync: requesting game suspend");
                g_wifi_window_requested = true;
                wifi_state_ = WifiWindowState::REQUESTING_SUSPEND;
            }
            break;

        case WifiWindowState::REQUESTING_SUSPEND:
            if (task_handle_ && eTaskGetState(task_handle_) == eSuspended) {
                ESP_LOGI(TAG, "HA sync: game suspended, starting WiFi");
                esp_wifi_start();
                wifi_window_start_s_ = now_s;
                wifi_state_ = WifiWindowState::WIFI_UP;
            }
            break;

        case WifiWindowState::WIFI_UP:
            if (now_s - wifi_window_start_s_ >= WIFI_WINDOW_DURATION_S) {
                ESP_LOGI(TAG, "HA sync: closing, resuming game");
                esp_wifi_stop();
                g_wifi_window_requested = false;
                if (task_handle_) vTaskResume(task_handle_);
                last_ha_sync_completed_us_ = esp_timer_get_time();
                wifi_state_ = WifiWindowState::STOPPED;
            }
            break;
    }
}

void Duke3DComponent::game_task(void* arg) {
    auto* self = static_cast<Duke3DComponent*>(arg);

    esp_task_wdt_add(nullptr);

    if (self->pause_wifi_) {
        ESP_LOGI(TAG, "Waiting for bootstrap (WiFi + HA data) before stopping WiFi…");
        const EventBits_t bits =
            xEventGroupWaitBits(self->sync_events_, DUKE3D_BOOTSTRAP_OK, pdTRUE, pdFALSE, pdMS_TO_TICKS(125000));
        if ((bits & DUKE3D_BOOTSTRAP_OK) == 0) {
            ESP_LOGW(TAG, "Bootstrap wait timed out — continuing");
        }
        printf("[duke3d] stopping WiFi to free PSRAM for tile cache\n");
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    char game_dir[48];
    snprintf(game_dir, sizeof(game_dir), "%s/duke3d", sd_card::SdCard::MOUNT_POINT);
    if (!pick_random_demo_dmo(game_dir, self->current_demo_, sizeof(self->current_demo_))) {
        strncpy(self->current_demo_, "DEMO1.DMO", sizeof(self->current_demo_) - 1);
        self->current_demo_[sizeof(self->current_demo_) - 1] = '\0';
        ESP_LOGW(TAG, "falling back to %s", self->current_demo_);
    }
    ESP_LOGI(TAG, "Random demo (from disk): %s", self->current_demo_);

    char demo_arg[40];
    snprintf(demo_arg, sizeof(demo_arg), "/d%s", self->current_demo_);

    ESP_LOGI(TAG, "Starting Duke3D engine (game_dir=/sdcard/duke3d)");

    if (self->tile_cache_) {
        printf("[duke3d] calling tilecache_build_if_needed\n");
        bool tc_built = tilecache_build_if_needed("/sdcard/duke3d/DUKE3D.GRP",
                                                  "/sdcard/duke3d/TCACHE.BIN");
        printf("[duke3d] tilecache_build_if_needed returned %d\n", (int)tc_built);
        bool tc_open = tilecache_open("/sdcard/duke3d/TCACHE.BIN");
        printf("[duke3d] tilecache_open returned %d\n", (int)tc_open);
    } else {
        printf("[duke3d] tile_cache disabled — loading tiles directly from GRP\n");
    }

    if (global_hud) global_hud->set_game_running(true);

    char* argv[] = {
        (char*)"duke3d",
        (char*)"-game_dir", (char*)"/sdcard/duke3d",
        (char*)"/nm",       // music disabled (OPL2 not ported)
        demo_arg,
        nullptr
    };
    // Splash hold must start here, not in Hub75::setup — pause_wifi bootstrap can delay the engine
    // by 10+ s, so a timer begun at panel init expires before the first blit (splash erased to black).
    {
        auto* m = esphome::hub75_matrix::global_hub75;
        if (m) {
            esphome::hub75_matrix::hub75_arm_boot_splash_hold(5000);
            if (global_hud) global_hud->render(*m);
            m->swap_buffers();
        }
    }
    duke3d_main(5, argv);
    if (self->pause_wifi_) {
        printf("[duke3d] game exited — restarting WiFi\n");
        esp_wifi_start();
    }
    tilecache_close();
    ESP_LOGI(TAG, "Duke3D engine exited");
    vTaskDelete(nullptr);
}

void Duke3DComponent::smoke_task(void* arg) {
    auto* self = static_cast<Duke3DComponent*>(arg);

    strncpy(self->current_demo_, "SMOKE", sizeof(self->current_demo_) - 1);

    esp_task_wdt_add(nullptr);

    constexpr int SRC_W = 320;
    constexpr int SRC_H = 200;
    constexpr size_t FB_SIZE  = SRC_W * SRC_H;
    constexpr size_t PAL_SIZE = 256 * 3;

    auto* fb = static_cast<uint8_t*>(heap_caps_malloc(FB_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!fb) fb = static_cast<uint8_t*>(heap_caps_malloc(FB_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    auto* pal = static_cast<uint8_t*>(heap_caps_malloc(PAL_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!fb || !pal) {
        ESP_LOGE(TAG, "Smoke test alloc failed (fb=%p pal=%p)", fb, pal);
        if (fb)  heap_caps_free(fb);
        if (pal) heap_caps_free(pal);
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "smoke_task: fb=%p (PSRAM=%d)", (void*)fb,
             esp_ptr_external_ram(fb) ? 1 : 0);

    for (int i = 0; i < 256; i++) {
        const int seg = (i * 3) / 256;
        const int v   = (i * 3) % 256;
        switch (seg) {
            case 0:
                pal[i*3+0] = (uint8_t)(255 - v); pal[i*3+1] = (uint8_t)v;        pal[i*3+2] = 0; break;
            case 1:
                pal[i*3+0] = 0;                  pal[i*3+1] = (uint8_t)(255 - v); pal[i*3+2] = (uint8_t)v; break;
            default:
                pal[i*3+0] = (uint8_t)v;         pal[i*3+1] = 0;                 pal[i*3+2] = (uint8_t)(255 - v); break;
        }
    }

    int t = 85;
    int64_t last_log_us = esp_timer_get_time();
    while (true) {
        const uint8_t bg = (uint8_t)(t & 0xFF);
        const uint32_t raw = (uint32_t)bg | ((uint32_t)bg << 8) | ((uint32_t)bg << 16) | ((uint32_t)bg << 24);
        clearbufbyte(fb, FB_SIZE, (int32_t)raw);

        const uint8_t box_idx = (uint8_t)((t + 128) & 0xFF);
        const int box_w = 100, box_h = 60;
        const int ox = (t * 10) % (SRC_W - box_w);
        const int oy = (t * 5)  % (SRC_H - box_h);
        for (int y = 0; y < box_h; y++) {
            uint8_t* row = fb + (oy + y) * SRC_W + ox;
            for (int x = 0; x < box_w; x++) row[x] = box_idx;
        }
        for (int i = 0; i < SRC_H; i++) {
            const int x = (i + (t * 15)) % SRC_W;
            fb[i * SRC_W + x] = (uint8_t)((t + 64 + i) & 0xFF);
        }

        platform_blit_frame(fb, pal);

        int64_t now = esp_timer_get_time();
        if (now - last_log_us >= 1000000) {
            ESP_LOGI(TAG, "smoke running (t=%d, fb_psram=%d)", t,
                     esp_ptr_external_ram(fb) ? 1 : 0);
            last_log_us = now;
        }

        t++;
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}

void Duke3DComponent::queue_ha_sync_if_eligible(uint8_t g_mode) {
    if (!pause_wifi_) return;
    if ((g_mode & DUKE3D_MODE_DEMO) != 0) return;

    const int64_t now = esp_timer_get_time();
    if (last_ha_sync_completed_us_ != 0 &&
        (now - last_ha_sync_completed_us_) < (int64_t) wifi_sync_min_interval_s_ * 1000000LL) {
        return;
    }
    ha_sync_pending_ = true;
    ESP_LOGI(TAG, "Level loaded (non-demo): queued HA WiFi sync");
}

}  // namespace duke3d
}  // namespace esphome

extern "C" void duke3d_notify_level_enter(uint8_t g_mode) {
    if (g_duke3d_component != nullptr) g_duke3d_component->queue_ha_sync_if_eligible(g_mode);
}

#include "duke3d_component.h"
#include "esphome/components/hub75_matrix/hub75_matrix.h"
#include "esphome/core/log.h"
#include "esp_task_wdt.h"
#include "esphome/components/sd_card/sd_card.h"
#include "esphome/components/hud/hud.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "input.h"
#include <cstring>

// Duke3D engine entry point — defined in engine_main_shim.c, which calls
// the engine's main() in game.c.
extern "C" int duke3d_main(int argc, char** argv);

// Forward-declare the HUD instance pointer from the hud component.
// Defined in hud.cpp (esphome::hud namespace); accessed in setup() below.
namespace esphome { namespace hud { extern Hud* global_hud_instance; } }

// global_hud: wires the HUD into platform_blit_frame() for per-frame overlay.
// Declared extern in esp32_hal.cpp; defined here.
esphome::hud::Hud* global_hud = nullptr;

// Engine dependency smoke-test: call a real Duke engine routine.
// Implemented in `engine/components/Engine/fixedPoint_math.c`.
extern "C" void clearbufbyte(void* D, int32_t c, int32_t a);


namespace esphome {
namespace duke3d {

static const char* TAG = "duke3d";
static Duke3DComponent* instance_ = nullptr;

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

    // Wire HUD global. For both game and smoke modes we drive swap_buffers()
    // from the Core 1 task via platform_blit_frame(), so prevent Hud::loop()
    // from also swapping concurrently.
    // Access via fully qualified name — local extern inside duke3d namespace would
    // resolve to esphome::duke3d::global_hud_instance instead of esphome::hud::.
    global_hud = esphome::hud::global_hud_instance;
    // Only disable HUD's own swap loop if we are actually going to drive frames
    // from a dedicated task (game or smoke). Otherwise the display will stay
    // on the HUB75 driver's diagnostic pattern.
    if (global_hud) global_hud->set_game_running(true);

    instance_ = this;
    input_init();

    xTaskCreatePinnedToCore(
        smoke_test_ ? smoke_task : game_task,
        smoke_test_ ? "duke3d_smoke" : "duke3d",
        TASK_STACK,
        this,
        5,      // priority — below HUB-75 ISR (which must be highest)
        &task_handle_,
        1       // Core 1
    );
    ESP_LOGI(TAG, "Duke3D %s task started on Core 1", smoke_test_ ? "smoke" : "game");
}

void Duke3DComponent::game_task(void* arg) {
    auto* self = static_cast<Duke3DComponent*>(arg);

    // Register with TWDT so watchdog does not reset us
    esp_task_wdt_add(nullptr);

    // Set game data directory to SD card mount point.
    // The engine uses open("DUKE3D.GRP", ...) with relative paths; -game_dir tells
    // it where to look. GRP file must be at /sdcard/DUKE3D.GRP (case-insensitive).
    strncpy(self->current_demo_, "DEMO1.DMO", sizeof(self->current_demo_) - 1);
    ESP_LOGI(TAG, "Starting Duke3D engine (game_dir=/sdcard)");

    // Engine argv: -game_dir /sdcard /nm(no music) /ns(no sound) /dDEMO1.DMO
    // Demo flag format: /d<filename> — engine cycles demos internally.
    char* argv[] = {
        (char*)"duke3d",
        (char*)"-game_dir", (char*)"/sdcard",
        (char*)"/nm",
        (char*)"/ns",
        (char*)"/dDEMO1.DMO",
        nullptr
    };
    duke3d_main(6, argv);
    // main() returns when the engine exits. TWDT fed per-frame in spi_lcd_send_boarder.
    ESP_LOGI(TAG, "Duke3D engine exited");
    vTaskDelete(nullptr);
}

void Duke3DComponent::smoke_task(void* arg) {
    auto* self = static_cast<Duke3DComponent*>(arg);

    strncpy(self->current_demo_, "SMOKE", sizeof(self->current_demo_) - 1);

    // Register with TWDT; we feed it via esp_task_wdt_reset() each frame.
    esp_task_wdt_add(nullptr);

    // Use output-resolution buffer (64x40 = 2560 bytes) — fits in internal RAM
    // with no PSRAM required. clearbufbyte still exercises the real engine routine.
    constexpr int DST_W = 64;
    constexpr int DST_H = 40;
    constexpr size_t FB_SIZE = DST_W * DST_H;   // 2560 bytes
    constexpr size_t PAL_SIZE = 256 * 3;

    auto* fb  = static_cast<uint8_t*>(heap_caps_malloc(FB_SIZE,  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    auto* pal = static_cast<uint8_t*>(heap_caps_malloc(PAL_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!fb || !pal) {
        ESP_LOGE(TAG, "Smoke test alloc failed (fb=%p pal=%p)", fb, pal);
        if (fb)  heap_caps_free(fb);
        if (pal) heap_caps_free(pal);
        vTaskDelete(nullptr);
        return;
    }

    // Vivid 3-segment spectrum: R→G, G→B, B→R — every index produces a bright saturated colour.
    // Index 0 = bright red, 85 = bright green, 170 = bright blue — no black anywhere.
    for (int i = 0; i < 256; i++) {
        const int seg = (i * 3) / 256;          // 0, 1, or 2
        const int v   = (i * 3) % 256;          // 0..255 ramp within segment
        switch (seg) {
            case 0:
                pal[i*3+0] = (uint8_t)(255 - v); pal[i*3+1] = (uint8_t)v;       pal[i*3+2] = 0; break;
            case 1:
                pal[i*3+0] = 0;                  pal[i*3+1] = (uint8_t)(255 - v); pal[i*3+2] = (uint8_t)v; break;
            default:
                pal[i*3+0] = (uint8_t)v;         pal[i*3+1] = 0;                 pal[i*3+2] = (uint8_t)(255 - v); break;
        }
    }

    // Direct handle to the matrix — smoke test bypasses platform_blit_frame
    // to avoid blocking on the HUD mutex (HA time callbacks hold it every second).
    auto* m = esphome::hub75_matrix::global_hub75;
    if (!m) {
        ESP_LOGE(TAG, "smoke_task: global_hub75 is null — aborting");
        heap_caps_free(fb);
        heap_caps_free(pal);
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "smoke_task: entering render loop (global_hub75=%p)", (void*)m);

    // Start at t=85 → palette[85]=(0,255,0)=GREEN so the first frame is
    // immediately distinguishable from the red/blue hub75 diagnostic.
    int t = 85;
    int64_t last_log_us = esp_timer_get_time();
    while (true) {
        // Exercise a real engine routine (linked from fixedPoint_math.c).
        // Pack the same palette index into all 4 bytes so clearbufbyte gives a uniform fill.
        const uint8_t bg = (uint8_t)(t & 0xFF);
        const uint32_t raw = (uint32_t)bg | ((uint32_t)bg << 8) | ((uint32_t)bg << 16) | ((uint32_t)bg << 24);
        clearbufbyte(fb, FB_SIZE, (int32_t)raw);

        // Moving box in complementary colour (~180° opposite on palette wheel).
        const uint8_t box_idx = (uint8_t)((t + 128) & 0xFF);
        const int box_w = 20, box_h = 12;
        const int ox = (t * 2) % (DST_W - box_w);
        const int oy = t % (DST_H - box_h);
        for (int y = 0; y < box_h; y++) {
            uint8_t* row = fb + (oy + y) * DST_W + ox;
            for (int x = 0; x < box_w; x++) row[x] = box_idx;
        }
        // Diagonal stripe for additional contrast.
        for (int i = 0; i < DST_H; i++) {
            const int x = (i + (t * 3)) % DST_W;
            fb[i * DST_W + x] = (uint8_t)((t + 64 + i) & 0xFF);
        }

        // Write 64x40 fb directly to matrix — no 5:1 downscale needed for smoke test.
        for (int y = 0; y < DST_H; y++) {
            for (int x = 0; x < DST_W; x++) {
                uint8_t idx = fb[y * DST_W + x];
                m->set_pixel(x, y, esphome::hub75_matrix::Color(pal[idx*3], pal[idx*3+1], pal[idx*3+2]));
            }
        }
        m->swap_buffers();
        esp_task_wdt_reset();

        int64_t now = esp_timer_get_time();
        if (now - last_log_us >= 1000000) {
            ESP_LOGI(TAG, "smoke running (t=%d)", t);
            last_log_us = now;
        }

        t++;
        vTaskDelay(pdMS_TO_TICKS(33));  // ~30 fps
    }
}

}  // namespace duke3d
}  // namespace esphome

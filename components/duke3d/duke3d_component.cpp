#include "duke3d_component.h"
#include "esphome/components/hub75_matrix/hub75_matrix.h"
#include "esphome/core/log.h"
#include "esp_task_wdt.h"
#include "esphome/components/sd_card/sd_card.h"
#include "esphome/components/hud/hud.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "esp32_hal.h"
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

    ESP_LOGI(TAG, "Free heap before task create: %u bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "Free internal heap: %u bytes",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    const int stack = smoke_test_ ? SMOKE_TASK_STACK : TASK_STACK;
    // Allocate task stack from PSRAM — internal DRAM is fragmented after WiFi+SD+HUB75 DMA.
    // PSRAM has ~220KB free; internal only has ~72KB in small fragments.
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
        smoke_test_ ? smoke_task : game_task,
        smoke_test_ ? "duke3d_smoke" : "duke3d",
        stack,
        this,
        5,      // priority — below HUB-75 ISR (which must be highest)
        &task_handle_,
        1,      // Core 1
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore FAILED (rc=%d) — not enough RAM?", rc);
        mark_failed();
        return;
    }
    ESP_LOGI(TAG, "Duke3D %s task started on Core 1 (stack=%d)", smoke_test_ ? "smoke" : "game", stack);
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

    // Engine argv: -game_dir /sdcard/duke3d /nm(no music) /ns(no sound) /dDEMO1.DMO
    // game_dir default is already patched to /sdcard/duke3d in cache.c, but passing
    // -game_dir here keeps checkcommandline consistent for any other path resolution.
    char* argv[] = {
        (char*)"duke3d",
        (char*)"-game_dir", (char*)"/sdcard/duke3d",
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

    // Register with TWDT; we feed it via platform_blit_frame → esp_task_wdt_reset().
    esp_task_wdt_add(nullptr);

    // 320x200 framebuffer — same size as the real engine's SDL surface.
    // Allocated from PSRAM (same as SDL_CreateRGBSurface does in the real game).
    // platform_blit_frame() will downscale this 5:1 to 64x40 exactly as the game does.
    constexpr int SRC_W = 320;
    constexpr int SRC_H = 200;
    constexpr size_t FB_SIZE  = SRC_W * SRC_H;  // 64000 bytes
    constexpr size_t PAL_SIZE = 256 * 3;

    // Try PSRAM first; fall back to internal only if PSRAM init failed.
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

    // Vivid 3-segment spectrum: R→G, G→B, B→R — every index is a bright saturated colour.
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

    // Start at t=85 → palette[85]=(0,255,0)=GREEN, immediately distinct from
    // the hub75 driver's red/blue diagnostic pattern shown before we take over.
    int t = 85;
    int64_t last_log_us = esp_timer_get_time();
    while (true) {
        // Fill background using a real engine routine from fixedPoint_math.c.
        const uint8_t bg = (uint8_t)(t & 0xFF);
        const uint32_t raw = (uint32_t)bg | ((uint32_t)bg << 8) | ((uint32_t)bg << 16) | ((uint32_t)bg << 24);
        clearbufbyte(fb, FB_SIZE, (int32_t)raw);

        // Moving box in complementary colour (~180° opposite on the palette wheel).
        // In 320x200 space → downscales to ~20x12 on the 64x40 display.
        const uint8_t box_idx = (uint8_t)((t + 128) & 0xFF);
        const int box_w = 100, box_h = 60;
        const int ox = (t * 10) % (SRC_W - box_w);
        const int oy = (t * 5)  % (SRC_H - box_h);
        for (int y = 0; y < box_h; y++) {
            uint8_t* row = fb + (oy + y) * SRC_W + ox;
            for (int x = 0; x < box_w; x++) row[x] = box_idx;
        }
        // Diagonal stripe across full 320x200.
        for (int i = 0; i < SRC_H; i++) {
            const int x = (i + (t * 15)) % SRC_W;
            fb[i * SRC_W + x] = (uint8_t)((t + 64 + i) & 0xFF);
        }

        // Hand off to the same pipeline the real engine uses:
        //   render_frame() → 5:1 downscale to 64x40
        //   global_hud->render() → HUD overlay on rows 40-63
        //   swap_buffers()
        //   esp_task_wdt_reset()
        platform_blit_frame(fb, pal);

        int64_t now = esp_timer_get_time();
        if (now - last_log_us >= 1000000) {
            ESP_LOGI(TAG, "smoke running (t=%d, fb_psram=%d)", t,
                     esp_ptr_external_ram(fb) ? 1 : 0);
            last_log_us = now;
        }

        t++;
        vTaskDelay(pdMS_TO_TICKS(33));  // ~30 fps
    }
}

}  // namespace duke3d
}  // namespace esphome

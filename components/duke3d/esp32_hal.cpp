#include "esp32_hal.h"
#include "renderer.h"
#include "esphome/components/hub75_matrix/hub75_matrix.h"
#include "esphome/components/hud/hud.h"
#include "esphome/components/sd_card/sd_card.h"
#include "esphome/components/i2s_audio/i2s_audio.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>

// global_hud is defined in duke3d_component.cpp and set during Duke3DComponent::setup().
// It is declared extern here so platform_blit_frame can overlay the HUD each frame.
extern esphome::hud::Hud* global_hud;

extern volatile bool g_wifi_window_requested;

// ---------------------------------------------------------------------------
// spi_lcd shim — replaces engine/components/SDL/spi_lcd.c
//
// The engine's SDL layer (SDL_video.c) calls:
//   spi_lcd_init()                      — once at startup
//   spi_lcd_send_boarder(pixels, border) — once per frame from SDL_Flip()
//
// SDL_SetColors() stores the palette as byte-swapped RGB565 in lcdpal[256].
// spi_lcd_send_boarder receives a 64x40 8-bit indexed framebuffer (native
// matrix resolution) and blits it directly to the HUB75 matrix.
// ---------------------------------------------------------------------------
// Diagnostic counters defined in tiles.c — read and reset each frame.
extern "C" volatile int32_t diag_tile_loads;
extern "C" volatile int32_t diag_tile_bytes;
extern "C" volatile int64_t diag_tile_us;

extern "C" {

// Palette filled by engine's SDL_SetColors(): byte-swapped RGB565.
int16_t lcdpal[256] = {};

void spi_lcd_init() {}   // no-op: we use HUB75, not SPI LCD
void spi_lcd_clear() {}  // no-op: clearing is done implicitly by swap_buffers

void spi_lcd_send_boarder(uint16_t *scr, int /*border*/) {
    auto *m = esphome::hub75_matrix::global_hub75;
    if (!m) return;

    static int frame_count = 0;
    static int64_t last_frame_us = 0;

    // Snapshot and reset tile-load diagnostics accumulated since last frame.
    int32_t tile_loads = diag_tile_loads;
    int32_t tile_bytes = diag_tile_bytes;
    int64_t tile_us    = diag_tile_us;
    diag_tile_loads = 0;
    diag_tile_bytes = 0;
    diag_tile_us    = 0;

    // Convert byte-swapped RGB565 palette → RGB888 for render_frame.
    uint8_t pal[256 * 3];
    for (int i = 0; i < 256; i++) {
        uint16_t v      = (uint16_t)lcdpal[i];
        uint16_t rgb565 = (uint16_t)((v >> 8) | ((v & 0xFF) << 8));  // un-swap
        pal[i * 3 + 0]  = (uint8_t)((rgb565 >> 11) << 3);            // R 5→8 bit
        pal[i * 3 + 1]  = (uint8_t)(((rgb565 >> 5) & 0x3F) << 2);   // G 6→8 bit
        pal[i * 3 + 2]  = (uint8_t)((rgb565 & 0x1F) << 3);          // B 5→8 bit
    }

    // scr is void* cast to uint16_t* by SDL_video.c, but the data is a
    // 64x40 8-bit indexed framebuffer — reinterpret as uint8_t*.
    const uint8_t *fb = reinterpret_cast<const uint8_t *>(scr);

    int64_t t_blit_start = esp_timer_get_time();
    if (!esphome::hub75_matrix::hub75_boot_splash_hold_active()) {
        render_frame(*m, fb, pal);  // blit 64x40 directly to rows 0-39
    }
    int64_t t_blit_us = esp_timer_get_time() - t_blit_start;

    if (global_hud) global_hud->render(*m);  // overlay rows 40-63
    m->swap_buffers();
    esp_task_wdt_reset();

    // Per-frame performance report every 30 frames.
    ++frame_count;
    int64_t now = esp_timer_get_time();
    int64_t total_frame_us = (last_frame_us > 0) ? (now - last_frame_us) : 0;
    last_frame_us = now;

    if (frame_count % 30 == 0) {
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
        int fps = (total_frame_us > 0) ? (int)(1000000 / total_frame_us) : 0;
        printf("[F%d] fps≈%d  frame=%lldms  SD: %d loads %d bytes in %lldms  blit=%lldus  stack=%u\n",
               frame_count, fps,
               (long long)total_frame_us / 1000,
               tile_loads, tile_bytes, (long long)tile_us / 1000,
               (long long)t_blit_us,
               (unsigned)hwm);
    }

    if (g_wifi_window_requested) {
        printf("[duke3d] suspending for WiFi window\n");
        vTaskSuspend(NULL);
        printf("[duke3d] resumed after WiFi window\n");
    }
}

} // extern "C"

extern "C" void platform_blit_frame(const uint8_t* src, const uint8_t* pal) {
    auto* m = esphome::hub75_matrix::global_hub75;
    if (!m) return;

    if (!esphome::hub75_matrix::hub75_boot_splash_hold_active()) {
        render_frame(*m, src, pal);  // writes rows 0-39
    }

    if (global_hud) global_hud->render(*m);  // overlays rows 40-63

    m->swap_buffers();

    // Feed the TWDT every frame — the game loop runs for minutes without
    // returning to ESPHome's main task, so we must reset here, not between demos.
    esp_task_wdt_reset();

    if (g_wifi_window_requested) {
        printf("[duke3d] suspending for WiFi window\n");
        vTaskSuspend(NULL);
        printf("[duke3d] resumed after WiFi window\n");
    }
}

extern "C" FILE* platform_open_file(const char* rel_path, const char* mode) {
    auto* sd = esphome::sd_card::global_sd_card;
    if (!sd || !sd->is_mounted()) return nullptr;
    return sd->open(rel_path, mode);
}

extern "C" void platform_audio_write(const int16_t* pcm, int n) {
    auto* audio = esphome::i2s_audio::global_i2s;
    // n is int16_t sample count (both channels). write_pcm expects bytes.
    if (audio) audio->write_pcm(pcm, n * sizeof(int16_t));
}

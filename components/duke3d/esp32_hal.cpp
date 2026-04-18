#include "esp32_hal.h"
#include "renderer.h"
#include "esphome/components/hub75_matrix/hub75_matrix.h"
#include "esphome/components/hud/hud.h"
#include "esphome/components/sd_card/sd_card.h"
#include "esphome/components/i2s_audio/i2s_audio.h"
#include "mv_stream.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdio>
#include <stdint.h>

// global_hud is defined in duke3d_component.cpp and set during Duke3DComponent::setup().
// It is declared extern here so platform_blit_frame can overlay the HUD each frame.
extern esphome::hud::Hud* global_hud;

extern volatile bool g_wifi_window_requested;

static std::atomic<unsigned> g_audio_output_percent{50u};

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
        static uint32_t last_stream_und = 0, last_stream_ps = 0;
        uint32_t su = MV_StreamUnderrunTotal();
        uint32_t sp = MV_StreamPrefetchShortTotal();
        unsigned su_d = (unsigned)(su - last_stream_und);
        unsigned sp_d = (unsigned)(sp - last_stream_ps);
        last_stream_und = su;
        last_stream_ps = sp;

        UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
        int fps = (total_frame_us > 0) ? (int)(1000000 / total_frame_us) : 0;
        char snd[56] = "";
        if (su_d > 0U || sp_d > 0U) {
            snprintf(snd, sizeof(snd), "  sound: und+%u sh+%u", su_d, sp_d);
        }
        const uint32_t caps_psram =
            (uint32_t)MALLOC_CAP_SPIRAM;  // same as initengine printf in engine.c
        const uint32_t caps_psram8 =
            (uint32_t)(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        const uint32_t caps_int =
            (uint32_t)(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t psram_free = heap_caps_get_free_size(caps_psram);
        const size_t psram_largest = heap_caps_get_largest_free_block(caps_psram);
        const size_t psram8_free = heap_caps_get_free_size(caps_psram8);
        const size_t psram8_largest = heap_caps_get_largest_free_block(caps_psram8);
        const size_t int_free = heap_caps_get_free_size(caps_int);
        // printf("[F%d] fps≈%d  frame=%lldms  SD: %ld loads %ld bytes in %lldms  blit=%lldus  stack=%u%s\n",
        //        frame_count, fps,
        //        (long long)total_frame_us / 1000,
        //        (long)tile_loads, (long)tile_bytes, (long long)tile_us / 1000,
        //        (long long)t_blit_us,
        //        (unsigned)hwm, snd);
        // printf("[mem] psram_free=%zu psram_largest_blk=%zu  psram8_free=%zu psram8_largest_blk=%zu  int_free=%zu"
        //        "  (psram_* = initengine caps; tile cache malloc needs contiguous ≤ psram8_largest_blk)\n",
        //        psram_free, psram_largest, psram8_free, psram8_largest, int_free);
        (void)fps; (void)psram_free; (void)psram_largest; (void)psram8_free; (void)psram8_largest; (void)int_free;
        (void)hwm; (void)snd; (void)t_blit_us; (void)tile_loads; (void)tile_bytes; (void)tile_us;
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

extern "C" void platform_set_audio_output_percent(unsigned percent) {
    if (percent > 100u)
        percent = 100u;
    g_audio_output_percent.store(percent, std::memory_order_relaxed);
}

extern "C" FILE* platform_open_file(const char* rel_path, const char* mode) {
    auto* sd = esphome::sd_card::global_sd_card;
    if (!sd || !sd->is_mounted()) return nullptr;
    return sd->open(rel_path, mode);
}

extern "C" void platform_audio_write(const int16_t* pcm, int n) {
    auto* audio = esphome::i2s_audio::global_i2s;
    if (!audio || n <= 0) return;
    // pcm is mono at 11025 Hz; I2S is configured stereo, so duplicate L=R.
    // n is at most MixBufferSize=256 samples; 512 int16 stereo = 1 KB stack.
    // Gain from ESPHome `duke3d.audio_output_percent` (0–100).
    if (n > 256) n = 256;
    const unsigned pct = g_audio_output_percent.load(std::memory_order_relaxed);
    int16_t stereo[512];
    for (int i = 0; i < n; i++) {
        int32_t s = ((int32_t)pcm[i] * (int32_t)pct) / 100;
        if (s > 32767)
            s = 32767;
        else if (s < -32768)
            s = -32768;
        int16_t o = (int16_t)s;
        stereo[i * 2]     = o;
        stereo[i * 2 + 1] = o;
    }
    audio->write_pcm(stereo, n * 2 * (int)sizeof(int16_t));
}

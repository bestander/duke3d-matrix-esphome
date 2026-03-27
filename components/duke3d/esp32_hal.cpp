#include "esp32_hal.h"
#include "renderer.h"
#include "esphome/components/hub75_matrix/hub75_matrix.h"
#include "esphome/components/hud/hud.h"
#include "esphome/components/sd_card/sd_card.h"
#include "esphome/components/i2s_audio/i2s_audio.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>

// global_hud is defined in duke3d_component.cpp and set during Duke3DComponent::setup().
// It is declared extern here so platform_blit_frame can overlay the HUD each frame.
extern esphome::hud::Hud* global_hud;

// ---------------------------------------------------------------------------
// spi_lcd shim — replaces engine/components/SDL/spi_lcd.c
//
// The engine's SDL layer (SDL_video.c) calls:
//   spi_lcd_init()                      — once at startup
//   spi_lcd_send_boarder(pixels, border) — once per frame from SDL_Flip()
//
// SDL_SetColors() stores the palette as byte-swapped RGB565 in lcdpal[256].
// spi_lcd_send_boarder receives a 320x200 8-bit indexed framebuffer and
// downscales it to 64x40 before writing to the HUB75 matrix.
// ---------------------------------------------------------------------------
extern "C" {

// Palette filled by engine's SDL_SetColors(): byte-swapped RGB565.
int16_t lcdpal[256] = {};

void spi_lcd_init() {}   // no-op: we use HUB75, not SPI LCD
void spi_lcd_clear() {}  // no-op: clearing is done implicitly by swap_buffers

void spi_lcd_send_boarder(uint16_t *scr, int /*border*/) {
    auto *m = esphome::hub75_matrix::global_hub75;
    if (!m) return;

    // Log stack watermark every 30 frames to catch stack exhaustion early.
    static int frame_count = 0;
    if (++frame_count % 30 == 0) {
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
        printf("frame %d: stack_hwm=%u words free\n", frame_count, (unsigned)hwm);
    }

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
    // 320x200 8-bit indexed framebuffer — reinterpret as uint8_t*.
    const uint8_t *fb = reinterpret_cast<const uint8_t *>(scr);
    render_frame(*m, fb, pal);           // downscale 320x200 → 64x40 rows 0-39
    if (global_hud) global_hud->render(*m);  // overlay rows 40-63
    m->swap_buffers();
    esp_task_wdt_reset();
}

} // extern "C"

extern "C" void platform_blit_frame(const uint8_t* src, const uint8_t* pal) {
    auto* m = esphome::hub75_matrix::global_hub75;
    if (!m) return;

    render_frame(*m, src, pal);      // writes rows 0-39

    if (global_hud) global_hud->render(*m);  // overlays rows 40-63

    m->swap_buffers();

    // Feed the TWDT every frame — the game loop runs for minutes without
    // returning to ESPHome's main task, so we must reset here, not between demos.
    esp_task_wdt_reset();
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

#include "hub75_matrix.h"
#include "esphome/core/log.h"
// ESPHome redefines ESP_LOGI/ESP_LOGW/etc to use esphome::esp_log_printf_.
// Bring it into file scope so the Hub75 header's inline functions can resolve it.
using esphome::esp_log_printf_;
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include "ESP32-HUB75-MatrixPanel-I2S-DMA.h"
// On ESP32-S3 the DMA framebuffer sits in internal SRAM which is accessed
// through the D-cache.  Per-pixel writes stay in cache until evicted; the
// GDMA reads from SRAM via the AHB bus and sees stale data unless we
// explicitly write all dirty cache lines back to SRAM first.
#include "rom/cache.h"

namespace esphome {
namespace hub75_matrix {

static const char* TAG = "hub75";
Hub75Matrix* global_hub75 = nullptr;
char hub75_init_status[128] = "not started";
static MatrixPanel_I2S_DMA* matrix_lib = nullptr;

// -----------------------------------------------------------------------
// Adafruit Matrix Portal S3 — HUB-75 GPIO assignments.
// Source: https://github.com/jnthas/clockwise/issues/75 (confirmed working)
//
// Note: GPIO 35–38 are used here for HUB-75 data lines. On the Matrix
// Portal S3 these pins are wired to the LCD parallel interface; the S3's
// PSRAM is accessed via internal die connections, NOT these GPIOs.
// -----------------------------------------------------------------------
static const HUB75_I2S_CFG::i2s_pins MATRIX_PINS = {
    .r1  = 42, .g1  = 41, .b1  = 40,
    .r2  = 38, .g2  = 39, .b2  = 37,
    .a   = 45, .b   = 36, .c   = 48, .d = 35, .e = 21,
    .lat = 47, .oe  = 14, .clk = 2
};
// Two 64x32 panels stacked vertically to form 64x64, wired in a chain (daisy-chain
// in series). Virtual canvas = 128x32 with CHAIN_LEN=2, PANEL_HEIGHT=32 (1/16 scan).
// Physical layout (confirmed by diagnostic):
//   Panel 2 (virtual x=64..127) = top physical panel, normally oriented
//   Panel 1 (virtual x=0..63)   = bottom physical panel, physically 180° rotated
// Logical 64x64 → virtual 128x32 transform in swap_buffers():
//   ly <  32 (top logical)     → panel 2: vx=lx+64,  vy=ly       (no rot needed)
//   ly >= 32 (bottom logical)  → panel 1: vx=63-lx,  vy=63-ly    (compensate 180° rot)
static const int PANEL_WIDTH  = 64;
static const int PANEL_HEIGHT = 32;
static const int CHAIN_LEN    = 2;

void Hub75Matrix::setup() {
    const size_t buf_size = WIDTH * HEIGHT * sizeof(Color);
    size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snprintf(hub75_init_status, sizeof(hub75_init_status),
             "spiram_free=%u internal_free=%u", spiram_free, internal_free);
    ESP_LOGI(TAG, "PSRAM free: %u, internal free: %u", spiram_free, internal_free);

    back_buf_ = (Color*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!back_buf_) {
        ESP_LOGE(TAG, "PSRAM alloc failed (%u bytes) — falling back to internal RAM", buf_size);
        back_buf_ = (Color*)heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!back_buf_) {
            snprintf(hub75_init_status, sizeof(hub75_init_status),
                     "FAIL: no RAM (spiram=%u internal=%u)", spiram_free, internal_free);
            ESP_LOGE(TAG, "Internal RAM alloc also failed");
            mark_failed();
            return;
        }
        snprintf(hub75_init_status, sizeof(hub75_init_status), "back_buf=internal_ram");
        ESP_LOGW(TAG, "Using internal RAM for back buffer");
    }
    for (int i = 0; i < WIDTH * HEIGHT; i++) back_buf_[i] = Color{};

    HUB75_I2S_CFG cfg(PANEL_WIDTH, PANEL_HEIGHT, CHAIN_LEN,
                      const_cast<HUB75_I2S_CFG::i2s_pins&>(MATRIX_PINS));
    cfg.clkphase = false;
    cfg.driver   = HUB75_I2S_CFG::SHIFTREG;

    snprintf(hub75_init_status, sizeof(hub75_init_status), "calling begin()");
    matrix_lib = new MatrixPanel_I2S_DMA(cfg);
    if (!matrix_lib->begin()) {
        snprintf(hub75_init_status, sizeof(hub75_init_status), "FAIL: begin() returned false");
        ESP_LOGE(TAG, "MatrixPanel_I2S_DMA::begin() failed");
        delete matrix_lib;
        matrix_lib = nullptr;
        mark_failed();
        return;
    }
    matrix_lib->setBrightness8(64); // 1/16 scan is 2x brighter than 1/32; halve brightness
    // Diagnostic: top logical half RED, bottom logical half BLUE.
    // Left edge: bright white dot at every 8th row (y=0,8,16,...,56) — count them to
    // identify which logical rows appear on which physical panel.
    // HUD will overwrite logical ly=40..63, so only ly=32..39 stays blue on the panel.
    for (int ly = 0; ly < HEIGHT; ly++) {
        Color bg = (ly < 32) ? Color{180, 0, 0} : Color{0, 0, 180};
        for (int lx = 0; lx < WIDTH; lx++) {
            // White left-edge markers every 8 rows, 2 pixels wide
            Color c = (lx < 2 && (ly % 8) == 0) ? Color{255, 255, 255} : bg;
            back_buf_[ly * WIDTH + lx] = c;
            int vx = (ly >= 32) ? (63 - lx)  : (lx + 64);
            int vy = (ly >= 32) ? (63 - ly)  : ly;
            matrix_lib->drawPixelRGB888(vx, vy, c.r, c.g, c.b);
        }
    }
    // Flush D-cache so GDMA sees the pixel data we just wrote to internal SRAM.
    Cache_WriteBack_All();

    global_hub75 = this;
    snprintf(hub75_init_status, sizeof(hub75_init_status), "OK");
    ESP_LOGI(TAG, "HUB-75 64x64 initialized");
}

void Hub75Matrix::set_pixel(int x, int y, Color c) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    back_buf_[y * WIDTH + x] = c;
}

void Hub75Matrix::fill(Color c) {
    for (int i = 0; i < WIDTH * HEIGHT; i++) back_buf_[i] = c;
}

void Hub75Matrix::swap_buffers() {
    // U-chain transform: maps logical 64x64 back buffer → physical 128x32 DMA canvas.
    // Bottom half (ly>=32) → panel 1 (physical bottom, normal): vx=lx,     vy=ly-32
    // Top half    (ly< 32) → panel 2 (physical top, 180° rot): vx=127-lx,  vy=31-ly
    for (int ly = 0; ly < HEIGHT; ly++) {
        for (int lx = 0; lx < WIDTH; lx++) {
            Color& c = back_buf_[ly * WIDTH + lx];
            int vx = (ly >= 32) ? (63 - lx)  : (lx + 64);
            int vy = (ly >= 32) ? (63 - ly)  : ly;
            matrix_lib->drawPixelRGB888(vx, vy, c.r, c.g, c.b);
        }
    }
    // Flush D-cache so GDMA sees the writes we just made to internal SRAM.
    Cache_WriteBack_All();
}

}  // namespace hub75_matrix
}  // namespace esphome

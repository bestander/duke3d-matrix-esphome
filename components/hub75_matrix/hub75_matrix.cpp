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
// Two 64x32 panels stacked vertically to form 64x64.
// The E address pin (GPIO 21) selects top (E=0) vs bottom (E=1) panel.
// CHAIN_LEN=1: 64 pixels per scan row, with E bit cycling through both panels.
// The library drives ROWS_PER_FRAME=32 scan addresses; each address drives row y (RGB1)
// and row y+32 (RGB2) simultaneously, covering all 64 physical rows.
static const int PANEL_WIDTH  = 64;
static const int PANEL_HEIGHT = 64;
static const int CHAIN_LEN    = 1;

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
    matrix_lib->setBrightness8(128);
    // Diagnostic: 8 colour bands (8 rows each) to identify which virtual y-range
    // maps to which physical panel / row position.
    //   y= 0.. 7  RED        y= 8..15  orange
    //   y=16..23  YELLOW     y=24..31  GREEN
    //   y=32..39  CYAN       y=40..47  BLUE  (HUD area – overwritten by hud.cpp)
    //   y=48..55  MAGENTA    y=56..63  WHITE
    static const struct { uint8_t r,g,b; } BANDS[8] = {
        {255,   0,   0}, // RED
        {255, 128,   0}, // orange
        {255, 255,   0}, // YELLOW
        {  0, 255,   0}, // GREEN
        {  0, 255, 255}, // CYAN
        {  0,   0, 255}, // BLUE
        {255,   0, 255}, // MAGENTA
        {255, 255, 255}, // WHITE
    };
    for (int y = 0; y < HEIGHT; y++) {
        auto& b = BANDS[y / 8];
        for (int x = 0; x < WIDTH; x++) {
            back_buf_[y * WIDTH + x] = Color{b.r, b.g, b.b};
            matrix_lib->drawPixelRGB888(x, y, b.r, b.g, b.b);
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
    // Copies the back buffer into the library's DMA buffer pixel by pixel.
    // NOT a zero-copy swap — the library's DMA scan task reads its internal
    // buffer while this loop writes, producing a brief (~4096 pixel) tearing
    // window per frame. At 25fps on a 64x64 LED matrix this is acceptable.
    //
    // For a true double-buffer swap: check if your version of
    // ESP32-HUB75-MatrixPanel-I2S-DMA exposes flipDMABuffer() and use it.
    for (int y = 0; y < HEIGHT; y++)
        for (int x = 0; x < WIDTH; x++) {
            Color& c = back_buf_[y * WIDTH + x];
            matrix_lib->drawPixelRGB888(x, y, c.r, c.g, c.b);
        }
    // Flush D-cache so GDMA sees the writes we just made to internal SRAM.
    Cache_WriteBack_All();
}

}  // namespace hub75_matrix
}  // namespace esphome

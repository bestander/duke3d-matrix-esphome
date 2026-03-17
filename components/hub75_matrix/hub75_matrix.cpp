#include "hub75_matrix.h"
#include "esphome/core/log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include "ESP32-HUB75-MatrixPanel-I2S-DMA.h"

namespace esphome {
namespace hub75_matrix {

static const char* TAG = "hub75";
Hub75Matrix* global_hub75 = nullptr;
static MatrixPanel_I2S_DMA* matrix_lib = nullptr;

// -----------------------------------------------------------------------
// ALL GPIO NUMBERS BELOW ARE -1 (UNSET) AND MUST BE FILLED IN BEFORE
// FLASHING. Look up the Adafruit Matrix Portal S3 schematic:
//   https://learn.adafruit.com/adafruit-matrix-portal-s3/pinouts
//
// FORBIDDEN: Do NOT use GPIO35, 36, 37, 38 — they are the Octal-SPI PSRAM
// bus pins and will silently corrupt PSRAM access if used as GPIO.
//
// The MatrixPanel_I2S_DMA::begin() call below will assert / return false
// if any required pin is still -1, giving a clear error message.
// -----------------------------------------------------------------------
// Compile-time safety net: causes a build error until real GPIO numbers are filled in.
// Once you have confirmed the real pin numbers from the Adafruit schematic,
// delete the three lines below (#if 1 / #error / #endif).
#if 1
#error "ACTION REQUIRED: Replace -1 placeholders in MATRIX_PINS with real GPIO numbers from the Adafruit Matrix Portal S3 schematic, then delete this #error block."
#endif
static const HUB75_I2S_CFG::i2s_pins MATRIX_PINS = {
    .r1  = -1, .g1  = -1, .b1  = -1,  // TODO: from schematic
    .r2  = -1, .g2  = -1, .b2  = -1,  // TODO: from schematic
    .a   = -1, .b   = -1, .c   = -1, .d = -1, .e = -1,  // TODO
    .lat = -1, .oe  = -1, .clk = -1   // TODO
};
// Two 64x32 panels chained to form 64x64.
// If panels are chained horizontally (128x32), swap PANEL_WIDTH/HEIGHT/CHAIN.
static const int PANEL_WIDTH  = 64;
static const int PANEL_HEIGHT = 32;
static const int CHAIN_LEN    = 2;

void Hub75Matrix::setup() {
    const size_t buf_size = WIDTH * HEIGHT * sizeof(Color);
    back_buf_ = (Color*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!back_buf_) {
        ESP_LOGE(TAG, "Failed to allocate back buffer in PSRAM");
        return;
    }
    memset(back_buf_, 0, buf_size);

    HUB75_I2S_CFG cfg(PANEL_WIDTH, PANEL_HEIGHT, CHAIN_LEN,
                      const_cast<HUB75_I2S_CFG::i2s_pins&>(MATRIX_PINS));
    cfg.clkphase = false;
    cfg.driver   = HUB75_I2S_CFG::SHIFTREG;

    matrix_lib = new MatrixPanel_I2S_DMA(cfg);
    if (!matrix_lib->begin()) {
        ESP_LOGE(TAG, "MatrixPanel_I2S_DMA::begin() failed");
        return;
    }
    matrix_lib->setBrightness8(128);
    matrix_lib->clearScreen();

    global_hub75 = this;
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
}

}  // namespace hub75_matrix
}  // namespace esphome

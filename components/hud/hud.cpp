#include "hud.h"
#include "font_5x7.h"
#include <cstring>
#include <cstdio>

#ifndef DUKE3D_HOST_TEST
#  include "esphome/core/log.h"
#  include "esp_timer.h"
static const char* TAG = "hud";
#endif

namespace esphome {
namespace hud {

// Global pointer used by Duke3DComponent to wire the HUD into platform_blit_frame.
esphome::hud::Hud* global_hud_instance = nullptr;

// 3×5 tiny font. Each entry is 3 column bytes; bit 0 = top row.
// Indices: 0-9 = digits, 10 = ':', 11 = 'H', 12 = 'L', 13 = ' ', 14 = '-'
static const uint8_t TINY_FONT[][3] = {
    {31, 17, 31}, // 0  XXX / X.X / X.X / X.X / XXX
    { 0, 31,  0}, // 1  .X. / .X. / .X. / .X. / .X.
    {29, 21, 23}, // 2  XXX / ..X / XXX / X.. / XXX
    {21, 21, 31}, // 3  XXX / ..X / XXX / ..X / XXX
    { 7,  4, 31}, // 4  X.. / X.. / XXX / ..X / ..X   (top 3 rows left + full right)
    {23, 21, 29}, // 5  XXX / X.. / XXX / ..X / XXX
    {31, 21, 29}, // 6  XXX / X.. / XXX / X.X / XXX
    { 1,  1, 31}, // 7  XXX / ..X / ..X / ..X / ..X
    {31, 21, 31}, // 8  XXX / X.X / XXX / X.X / XXX
    {23, 21, 31}, // 9  XXX / X.X / XXX / ..X / XXX
    { 0, 10,  0}, // ':' — two center dots (rows 1,3)
    {31,  4, 31}, // 'H' — full sides + center row
    {31, 16, 16}, // 'L' — full left + bottom right
    { 0,  0,  0}, // ' ' — blank
    { 4,  4,  4}, // '-' — center horizontal line (row 2)
};

static int tiny_index(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == ':') return 10;
    if (c == 'H') return 11;
    if (c == 'L') return 12;
    if (c == '-') return 14;
    return 13;  // space / fallback
}

#ifndef DUKE3D_HOST_TEST
void Hud::setup() {
    global_hud_instance = this;
    ESP_LOGI(TAG, "HUD initialized");
}
void Hud::loop() {
    // Phase 5+: Duke3D's game_task on Core 1 drives render+swap; don't double-swap.
    if (game_running_) return;
    auto* m = esphome::hub75_matrix::global_hub75;
    if (m) {
        render(*m);
        m->swap_buffers();
    }
}
#endif

void Hud::set_time(int h, int m) {
    std::lock_guard<std::mutex> lk(data_mutex_);
    hour_ = h; minute_ = m;
}

void Hud::set_min_temp(float c) {
    std::lock_guard<std::mutex> lk(data_mutex_);
    min_temp_ = c;
}

void Hud::set_max_temp(float c) {
    std::lock_guard<std::mutex> lk(data_mutex_);
    max_temp_ = c;
}

void Hud::set_water_temp(float c) {
    std::lock_guard<std::mutex> lk(data_mutex_);
    water_temp_ = c;
}

void Hud::set_tide_high(const std::string& t) {
    std::lock_guard<std::mutex> lk(data_mutex_);
    strncpy(tide_high_, t.c_str(), sizeof(tide_high_) - 1);
    tide_high_[sizeof(tide_high_) - 1] = '\0';
}

void Hud::set_tide_low(const std::string& t) {
    std::lock_guard<std::mutex> lk(data_mutex_);
    strncpy(tide_low_, t.c_str(), sizeof(tide_low_) - 1);
    tide_low_[sizeof(tide_low_) - 1] = '\0';
}

void Hud::set_ble_connected(bool connected) {
    std::lock_guard<std::mutex> lk(data_mutex_);
    ble_connected_ = connected;
}

void Hud::draw_char(MatrixType& d, int x, int y, int idx, uint8_t r, uint8_t g, uint8_t b) {
    for (int col = 0; col < 5; col++) {
        uint8_t bits = FONT_5X7[idx][col];
        for (int row = 0; row < 7; row++)
            if (bits & (1 << row))
                d.set_pixel(x + col, y + row, ColorType(r, g, b));
    }
}

void Hud::draw_tiny_char(MatrixType& d, int x, int y, int idx, uint8_t r, uint8_t g, uint8_t b) {
    for (int col = 0; col < 3; col++) {
        uint8_t bits = TINY_FONT[idx][col];
        for (int row = 0; row < 5; row++)
            if (bits & (1 << row))
                d.set_pixel(x + col, y + row, ColorType(r, g, b));
    }
}

void Hud::render(MatrixType& d) {
    int h, m;
    float t_min, t_max, t_water;
    char tide_h[6], tide_l[6];
    bool ble_connected;
    {
        std::lock_guard<std::mutex> lk(data_mutex_);
        h = hour_; m = minute_;
        t_min   = min_temp_;
        t_max   = max_temp_;
        t_water = water_temp_;
        memcpy(tide_h, tide_high_, 6);
        memcpy(tide_l, tide_low_,  6);
        ble_connected = ble_connected_;
    }

    // Clear HUD band (dark blue background)
    for (int y = HUD_TOP; y < HUD_TOP + HUD_HEIGHT; y++)
        for (int x = 0; x < 64; x++)
            d.set_pixel(x, y, ColorType(0, 0, 0));

    // ── Band 0 (y=HUD_TOP+0): Clock "HH:MM" | water "~NN°C" ──────────────
    char clock_str[6];
    snprintf(clock_str, sizeof(clock_str), "%02d:%02d", h, m);
    int cx = 2;
    for (int i = 0; clock_str[i]; i++, cx += 6)
        draw_char(d, cx, HUD_TOP, font_index(clock_str[i]), 200, 200, 255);

    // water: ~ + value + ° + C  (right side, x=34)
    draw_char(d, 34, HUD_TOP, FONT_IDX_WAVE, 80, 160, 255);
    char water_str[5];
    snprintf(water_str, sizeof(water_str), "%d", (int)t_water);
    int wx = 40;
    for (int i = 0; water_str[i]; i++, wx += 6)
        draw_char(d, wx, HUD_TOP, font_index(water_str[i]), 80, 180, 255);
    draw_char(d, wx, HUD_TOP, FONT_IDX_DEGREE, 80, 180, 255);

    // BLE status icon — bottom-right corner (x=61, y=57, icon rows y=57-63).
    // Connected: solid blue. Searching: blink at 2 Hz.
    bool draw_ble = ble_connected || ((esp_timer_get_time() / 500000LL) % 2 == 0);
    if (draw_ble)
        draw_char(d, 61, 57, FONT_IDX_BLE, 0, 100, 255);

    // ── Band 1 (y=HUD_TOP+8): ↓min°  ↑max° ──────────────────────────────
    draw_char(d, 2, HUD_TOP + 8, FONT_IDX_DOWN, 100, 180, 255);
    char min_str[5];
    snprintf(min_str, sizeof(min_str), "%d", (int)t_min);
    int lx = 8;
    for (int i = 0; min_str[i]; i++, lx += 6)
        draw_char(d, lx, HUD_TOP + 8, font_index(min_str[i]), 100, 180, 255);
    draw_char(d, lx, HUD_TOP + 8, FONT_IDX_DEGREE, 100, 180, 255);

    draw_char(d, 32, HUD_TOP + 8, FONT_IDX_UP, 255, 160, 80);
    char max_str[5];
    snprintf(max_str, sizeof(max_str), "%d", (int)t_max);
    int ux = 38;
    for (int i = 0; max_str[i]; i++, ux += 6)
        draw_char(d, ux, HUD_TOP + 8, font_index(max_str[i]), 255, 160, 80);
    draw_char(d, ux, HUD_TOP + 8, FONT_IDX_DEGREE, 255, 160, 80);

    // ── Band 2 (y=HUD_TOP+18): tide times in 3×5 tiny font ───────────────
    // "H HH:MM" at x=0, "L HH:MM" at x=32
    // Each tiny char = 3px wide + 1px gap = 4px per char
    const int ty = HUD_TOP + 18;  // 2px margin inside last 8-row band

    draw_tiny_char(d, 0, ty, tiny_index('H'), 80, 200, 255);
    // 4px gap then 5-char "HH:MM"
    int thx = 8;
    for (int i = 0; tide_h[i] && i < 5; i++, thx += 4)
        draw_tiny_char(d, thx, ty, tiny_index(tide_h[i]), 80, 200, 255);

    draw_tiny_char(d, 32, ty, tiny_index('L'), 220, 150, 60);
    int tlx = 40;
    for (int i = 0; tide_l[i] && i < 5; i++, tlx += 4)
        draw_tiny_char(d, tlx, ty, tiny_index(tide_l[i]), 220, 150, 60);
}

}  // namespace hud
}  // namespace esphome

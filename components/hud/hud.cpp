#include "hud.h"
#include "font_5x7.h"
#include "weather_icons.h"
#include <cstring>
#include <cstdio>

#ifndef DUKE3D_HOST_TEST
#  include "esphome/core/log.h"
static const char* TAG = "hud";
#endif

namespace esphome {
namespace hud {

// Global pointer used by Duke3DComponent to wire the HUD into platform_blit_frame.
esphome::hud::Hud* global_hud_instance = nullptr;

#ifndef DUKE3D_HOST_TEST
void Hud::setup() {
    global_hud_instance = this;
    ESP_LOGI(TAG, "HUD initialized");
}
void Hud::loop() {
    // Phase 1-3: HUD owns the full render+swap cycle (Duke3D not yet running).
    // Phase 5+: Duke3D's game_task on Core 1 calls render() and swap_buffers();
    // we must NOT also call swap_buffers() here or we get a concurrent DMA write race.
    // Duke3DComponent::setup() calls set_game_running(true) to disable our swap.
    if (game_running_) return;
    auto* m = esphome::hub75_matrix::global_hub75;
    if (m) {
        render(*m);
        m->swap_buffers();
    }
}
#endif

void Hud::set_temperature(float c) {
    std::lock_guard<std::mutex> lk(data_mutex_);
    temperature_ = c;
}

void Hud::set_condition(const std::string& s) {
    std::lock_guard<std::mutex> lk(data_mutex_);
    strncpy(condition_, s.c_str(), sizeof(condition_) - 1);
    condition_[sizeof(condition_) - 1] = '\0';
}

void Hud::set_time(int h, int m) {
    std::lock_guard<std::mutex> lk(data_mutex_);
    hour_ = h; minute_ = m;
}

void Hud::draw_char(MatrixType& d, int x, int y, int font_idx, uint8_t r, uint8_t g, uint8_t b) {
    for (int col = 0; col < 5; col++) {
        uint8_t bits = FONT_5X7[font_idx][col];
        for (int row = 0; row < 7; row++)
            if (bits & (1 << row))
                d.set_pixel(x + col, y + row, ColorType(r, g, b));
    }
}

void Hud::draw_icon(MatrixType& d, int x, int y, const char* cond) {
    const WeatherIcon& icon = icon_for_condition(cond);
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            if (icon.rows[row] & (1 << (7 - col)))
                d.set_pixel(x + col, y + row, ColorType(100, 180, 255));
}

void Hud::render(MatrixType& d) {
    float temp; char cond[64]; int h, m;
    {
        std::lock_guard<std::mutex> lk(data_mutex_);
        temp = temperature_;
        strncpy(cond, condition_, sizeof(cond));
        h = hour_; m = minute_;
    }

    // Clear HUD band (dark blue background)
    for (int y = HUD_TOP; y < HUD_TOP + HUD_HEIGHT; y++)
        for (int x = 0; x < 64; x++)
            d.set_pixel(x, y, ColorType(0, 0, 20));

    // Row 1: weather icon (x=0) + temperature "22 deg C" (x=10)
    draw_icon(d, 0, HUD_TOP, cond);
    char temp_str[8];
    snprintf(temp_str, sizeof(temp_str), "%d", (int)temp);
    int tx = 10;
    for (int i = 0; temp_str[i]; i++, tx += 6)
        draw_char(d, tx, HUD_TOP + 1, font_index(temp_str[i]), 255, 200, 0);
    draw_char(d, tx,     HUD_TOP + 1, FONT_IDX_DEGREE, 255, 200, 0);
    draw_char(d, tx + 6, HUD_TOP + 1, FONT_IDX_C,      255, 200, 0);

    // Row 2: clock "HH:MM" centered
    char clock_str[6];
    snprintf(clock_str, sizeof(clock_str), "%02d:%02d", h, m);
    int cx = (64 - 5 * 6) / 2;  // 5 chars x 6px wide, centered
    for (int i = 0; clock_str[i]; i++, cx += 6)
        draw_char(d, cx, HUD_TOP + 13, font_index(clock_str[i]), 200, 200, 255);
}

}  // namespace hud
}  // namespace esphome

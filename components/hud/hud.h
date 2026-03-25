#pragma once

#ifndef DUKE3D_HOST_TEST
#  include "esphome/core/component.h"
#  include "../hub75_matrix/hub75_matrix.h"
   namespace esphome { namespace hud {
   using MatrixType = hub75_matrix::Hub75Matrix;
   using ColorType  = hub75_matrix::Color;
   }}
#else
#  include "hub75_mock.h"
   namespace esphome { namespace hud {
   using MatrixType = Hub75Matrix;
   using ColorType  = Color;
   struct Component { virtual void setup(){} virtual void loop(){} };
   }}
#endif

#include <mutex>
#include <string>

namespace esphome {
namespace hud {

// Renders into rows 40-63 (HUD_TOP..63) of the 64x64 display.
// Layout (3 × 8-pixel bands):
//   Band 0 (y=40-47): clock "HH:MM" (left) | water temp "~NN°C" (right)
//   Band 1 (y=48-55): air temp range "↓NN°  ↑NN°"
//   Band 2 (y=56-63): tide "H HH:MM  L HH:MM" (3×5 tiny font)
class Hud
#ifndef DUKE3D_HOST_TEST
    : public Component
#endif
{
public:
    static const int HUD_TOP    = 40;
    static const int HUD_HEIGHT = 24;

#ifndef DUKE3D_HOST_TEST
    void setup() override;
    void loop() override;
    float get_setup_priority() const override { return setup_priority::DATA; }
    void set_game_running(bool v) { game_running_ = v; }
#endif

    void set_time(int hour, int minute);           // thread-safe
    void set_min_temp(float celsius);              // 8h air temp min
    void set_max_temp(float celsius);              // 8h air temp max
    void set_water_temp(float celsius);            // current water temperature
    void set_tide_high(const std::string& hhmm);  // "HH:MM"
    void set_tide_low(const std::string& hhmm);   // "HH:MM"

    void render(MatrixType& display);

private:
    int   hour_ = 0, minute_ = 0;
    float min_temp_   = 0.0f;
    float max_temp_   = 0.0f;
    float water_temp_ = 0.0f;
    char  tide_high_[6] = "--:--";
    char  tide_low_[6]  = "--:--";
    std::mutex data_mutex_;
    bool game_running_ = false;

    void draw_char(MatrixType& d, int x, int y, int font_idx,
                   uint8_t r, uint8_t g, uint8_t b);
    void draw_tiny_char(MatrixType& d, int x, int y, int tiny_idx,
                        uint8_t r, uint8_t g, uint8_t b);
};

}  // namespace hud
}  // namespace esphome

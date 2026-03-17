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
// Layout:
//   Row 40-50: temperature "22 deg C" with weather icon (left)
//   Row 53-63: clock "HH:MM" centered
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
    // Uses global_hub75 (set in hub75_matrix::setup()) — no explicit wiring needed.
    // set_game_running(true) is called by Duke3DComponent::setup() so that
    // Hud::loop() stops driving swap_buffers() (Duke3D's game task takes over).
    void set_game_running(bool v) { game_running_ = v; }
#endif

    void set_temperature(float celsius);           // thread-safe (Core 0 -> Core 1)
    void set_condition(const std::string& cond);   // thread-safe
    void set_time(int hour, int minute);           // thread-safe

    void render(MatrixType& display);

private:
    float temperature_ = 0.0f;
    char  condition_[64] = {};
    int   hour_ = 0, minute_ = 0;
    std::mutex data_mutex_;
    bool game_running_ = false;  // true once Duke3D takes over swap_buffers()

    void draw_char(MatrixType& d, int x, int y, int font_idx, uint8_t r, uint8_t g, uint8_t b);
    void draw_icon(MatrixType& d, int x, int y, const char* condition);
};

}  // namespace hud
}  // namespace esphome

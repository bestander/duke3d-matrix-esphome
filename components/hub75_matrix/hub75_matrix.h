#pragma once
#include "esphome/core/component.h"
#include <cstdint>

namespace esphome {
namespace hub75_matrix {

struct Color {
    uint8_t r, g, b;
    Color() : r(0), g(0), b(0) {}
    Color(uint8_t r, uint8_t g, uint8_t b) : r(r), g(g), b(b) {}
};

// Thread-safety contract:
//   set_pixel() and fill() write to the back buffer only (back_buf_).
//   swap_buffers() copies back_buf_ into the library's internal DMA buffer
//   pixel by pixel — NOT an atomic swap (brief tearing window exists per frame).
//   set_pixel() and fill() are safe to call from any task while swap_buffers()
//   is not running; do not call them concurrently with swap_buffers().
class Hub75Matrix : public Component {
public:
    static const int WIDTH  = 64;
    static const int HEIGHT = 64;

    void setup() override;
    void loop() override {}
    float get_setup_priority() const override { return setup_priority::HARDWARE; }

    void set_pixel(int x, int y, Color c);
    void fill(Color c);
    void swap_buffers();  // call once per rendered frame

private:
    Color* back_buf_ = nullptr;
};

extern Hub75Matrix* global_hub75;  // set in setup(); used by duke3d renderer
extern char hub75_init_status[128]; // human-readable init result for diagnostics

// True for a few seconds after hub75_arm_boot_splash_hold() — game blit skips rows 0..39.
bool hub75_boot_splash_hold_active();
// Call from game_task immediately before duke3d_main() (not from Hub75::setup — WiFi bootstrap
// can delay the engine long past a timer started at panel init).
void hub75_arm_boot_splash_hold(uint32_t duration_ms);

}  // namespace hub75_matrix
}  // namespace esphome

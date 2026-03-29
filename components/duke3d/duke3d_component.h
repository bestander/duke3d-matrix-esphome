#pragma once
#include "esphome/core/component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace duke3d {

class Duke3DComponent : public Component {
public:
    void setup() override;
    void loop() override;
    float get_setup_priority() const override { return setup_priority::LATE; }

    const char* current_demo() const { return current_demo_; }
    void set_smoke_test(bool v)  { smoke_test_  = v; }
    void set_usb_gamepad(bool v) { usb_gamepad_ = v; }
    void set_tile_cache(bool v)  { tile_cache_  = v; }
    void set_pause_wifi(bool v)          { pause_wifi_ = v; }
    void set_wifi_interval_s(uint32_t v) { wifi_interval_s_ = v; }

private:
    enum class WifiWindowState { STOPPED, REQUESTING_SUSPEND, WIFI_UP };

    TaskHandle_t task_handle_ = nullptr;
    char current_demo_[32] = "DEMO1.DMO";
    bool smoke_test_    = false;
    bool usb_gamepad_   = false;
    bool tile_cache_    = true;
    bool pause_wifi_          = false;
    uint32_t wifi_interval_s_     = 3600;
    WifiWindowState wifi_state_   = WifiWindowState::STOPPED;
    uint32_t wifi_window_start_s_ = 0;
    uint32_t last_wifi_window_s_  = 0;
    static constexpr uint32_t WIFI_WINDOW_DURATION_S = 20;

    static void game_task(void* arg);
    static void smoke_task(void* arg);
    static const int TASK_STACK       = 20480;  // 20 KB — HWM shows ~9 KB used; 11 KB margin
    static const int SMOKE_TASK_STACK =  8192;  // 8KB — smoke task is simple, no engine calls
};

}  // namespace duke3d
}  // namespace esphome

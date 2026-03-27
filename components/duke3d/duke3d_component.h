#pragma once
#include "esphome/core/component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace duke3d {

class Duke3DComponent : public Component {
public:
    void setup() override;
    void loop() override {}
    float get_setup_priority() const override { return setup_priority::LATE; }

    const char* current_demo() const { return current_demo_; }
    void set_smoke_test(bool v) { smoke_test_ = v; }

private:
    TaskHandle_t task_handle_ = nullptr;
    char current_demo_[32] = "DEMO1.DMO";
    bool smoke_test_ = false;

    static void game_task(void* arg);
    static void smoke_task(void* arg);
    static const int TASK_STACK       = 65536;  // 64KB — deep call stack for demo load
    static const int SMOKE_TASK_STACK =  8192;  // 8KB — smoke task is simple, no engine calls
};

}  // namespace duke3d
}  // namespace esphome

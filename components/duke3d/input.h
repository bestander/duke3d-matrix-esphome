#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "ble_gamepad.h"

enum class InputEvent : uint8_t {
    NONE = 0,
    MOVE_FORWARD, MOVE_BACK,
    STRAFE_LEFT, STRAFE_RIGHT,
    TURN_LEFT, TURN_RIGHT,
    FIRE, USE,
    OPEN_MAP, MENU_TOGGLE,
};

// Initialize input subsystem. Call from duke3d_component setup().
void input_init();

// Push from task context (USB HID callback, button handler task).
void input_push(InputEvent evt);

// Push from hardware ISR context only (GPIO interrupt).
void input_push_from_isr(InputEvent evt);

// Pop next event. Returns InputEvent::NONE if queue is empty.
// Called from game task (Core 1) each tick.
InputEvent input_pop();

// Held-button state — updated from USB HID reports every ~8-16 ms.
// Use this for movement/turning (held actions); use input_pop() for
// one-shot events like menu navigation.
// Thread-safe: written by USB host task via usb_gamepad, read by game task.
static inline GamepadState input_get_state() {
    return ble_gamepad_get_state();
}

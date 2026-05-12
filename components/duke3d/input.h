#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef struct {
    bool forward;
    bool back;
    bool turn_left;
    bool turn_right;
    bool strafe_left;
    bool strafe_right;
    bool fire;
    bool use;
    bool open_map;
    bool menu;
} GamepadState;

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

// Push from task context (button handler task).
void input_push(InputEvent evt);

// Push from hardware ISR context only (GPIO interrupt).
void input_push_from_isr(InputEvent evt);

// Pop next event. Returns InputEvent::NONE if queue is empty.
// Called from game task (Core 1) each tick.
InputEvent input_pop();

// Held-button state for movement/turning actions.
// BLE/gamepad input stack is disabled, so this currently returns all-false state.
static inline GamepadState input_get_state() {
    return GamepadState{};
}

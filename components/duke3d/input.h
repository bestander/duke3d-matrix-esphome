#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

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

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// BLE HID gamepad input — receives raw HID reports from the ESPHome
// ble_client sensor lambda and exposes a held-button state for the game task.
//
// Call ble_gamepad_push_report() from the ESPHome sensor lambda each time
// a new HID notification arrives. The function logs bit changes for each byte
// so you can map the controller layout without a scope.
//
// Call ble_gamepad_get_state() from the game task each tick to read the
// current held-button snapshot (thread-safe via atomic).
// ---------------------------------------------------------------------------

typedef struct {
    bool forward;       // left stick / D-pad up
    bool back;          // left stick / D-pad down
    bool turn_left;     // left stick / D-pad left
    bool turn_right;    // left stick / D-pad right
    bool strafe_left;   // left shoulder (LB)
    bool strafe_right;  // right shoulder (RB)
    bool fire;          // button South (A / Cross)
    bool use;           // button East  (B / Circle)
    bool open_map;      // button West  (X / Square)
    bool menu;          // Start / Menu
} GamepadState;

// Push a raw HID report (from BLE notify callback). Logs per-byte bit changes
// and updates the held-button state used by ble_gamepad_get_state().
void ble_gamepad_push_report(const uint8_t *data, size_t len);

// Returns a snapshot of the current held state.
// Thread-safe: written by NimBLE host task, read by game task.
GamepadState ble_gamepad_get_state(void);

#ifdef __cplusplus
}
#endif

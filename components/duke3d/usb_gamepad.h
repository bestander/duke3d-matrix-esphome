#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// USB gamepad input — ESP32-S3 USB OTG host, HID class driver.
//
// Mode selection (checked once at usb_gamepad_start() time):
//   - If the BOOT button (GPIO 0) is held LOW: debug mode.
//     USB OTG host is NOT initialised; the USB Serial JTAG peripheral
//     remains active so you can debug over USB-C as normal.
//   - Otherwise: USB OTG host starts, HID gamepad enumeration runs,
//     USB Serial JTAG is disconnected (use UART or WiFi logs instead).
//
// Returns false if debug mode was detected (USB host NOT started).
// Returns true  if USB host started successfully.
// ---------------------------------------------------------------------------
bool usb_gamepad_start(void);
void usb_gamepad_stop(void);

// Whether debug mode was activated (BOOT button held at start).
bool usb_gamepad_debug_mode(void);

// ---------------------------------------------------------------------------
// Button state — updated every HID report (~8-16 ms).
// Read this from the game task each tick for held-button actions.
// Thread-safe: written by USB host task, read by game task.
// ---------------------------------------------------------------------------
typedef struct {
    bool forward;       // D-pad / left stick up
    bool back;          // D-pad / left stick down
    bool turn_left;     // D-pad left
    bool turn_right;    // D-pad right
    bool strafe_left;   // left shoulder
    bool strafe_right;  // right shoulder
    bool fire;          // button South (A / Cross)
    bool use;           // button East  (B / Circle)
    bool open_map;      // button West  (X / Square)
    bool menu;          // Start
} GamepadState;

// Returns a snapshot of the current held state.
GamepadState usb_gamepad_get_state(void);

// ---------------------------------------------------------------------------
// HID report layout (configurable at compile time via defines).
//
// Defaults match most generic USB gamepads (iBuffalo, cheap PS2 clones):
//   Byte 0  : left stick / D-pad X  (0x00=left, 0x7F=centre, 0xFF=right)
//   Byte 1  : left stick / D-pad Y  (0x00=up,   0x7F=centre, 0xFF=down)
//   Byte 4  : buttons  0-7  (bit 0 = South/A, bit 1 = East/B,
//                             bit 2 = West/X,  bit 3 = North/Y,
//                             bit 4 = LB,      bit 5 = RB)
//   Byte 5  : buttons  8-15 (bit 0 = Select, bit 1 = Start)
//
// Override at build time via target_compile_definitions in CMakeLists.txt.
// ---------------------------------------------------------------------------
#ifndef GPJOY_AXIS_X_BYTE
#  define GPJOY_AXIS_X_BYTE   0
#endif
#ifndef GPJOY_AXIS_Y_BYTE
#  define GPJOY_AXIS_Y_BYTE   1
#endif
#ifndef GPJOY_AXIS_DEAD_LO
#  define GPJOY_AXIS_DEAD_LO  64    // < this → negative direction
#endif
#ifndef GPJOY_AXIS_DEAD_HI
#  define GPJOY_AXIS_DEAD_HI  192   // > this → positive direction
#endif
#ifndef GPJOY_BTN_BYTE0
#  define GPJOY_BTN_BYTE0     4
#endif
#ifndef GPJOY_BTN_FIRE_BIT
#  define GPJOY_BTN_FIRE_BIT  0    // bit in GPJOY_BTN_BYTE0
#endif
#ifndef GPJOY_BTN_USE_BIT
#  define GPJOY_BTN_USE_BIT   1
#endif
#ifndef GPJOY_BTN_MAP_BIT
#  define GPJOY_BTN_MAP_BIT   2
#endif
#ifndef GPJOY_BTN_BYTE1
#  define GPJOY_BTN_BYTE1     5
#endif
#ifndef GPJOY_BTN_LB_BIT
#  define GPJOY_BTN_LB_BIT    4    // bit in GPJOY_BTN_BYTE0 — strafe left
#endif
#ifndef GPJOY_BTN_RB_BIT
#  define GPJOY_BTN_RB_BIT    5    // bit in GPJOY_BTN_BYTE0 — strafe right
#endif
#ifndef GPJOY_BTN_MENU_BIT
#  define GPJOY_BTN_MENU_BIT  1    // bit in GPJOY_BTN_BYTE1 — Start
#endif

#ifdef __cplusplus
}
#endif

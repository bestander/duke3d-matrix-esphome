# BLE Gamepad Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the unused USB HID host with a `ble_gamepad` C++ module that logs raw HID report bit changes from a BLE gamepad; game continues running in demo mode with no controller input.

**Architecture:** A new `ble_gamepad.cpp/h` replaces `usb_gamepad.cpp/h` with the same `GamepadState` interface. The ESPHome BLE sensor lambda forwards raw HID report bytes to `ble_gamepad_push_report()`, which logs bit-level changes. `ble_gamepad_get_state()` returns all-zeroes for now. All USB host code and config are removed.

**Tech Stack:** ESP-IDF 5.2.1, ESPHome, FreeRTOS, PlatformIO via ESPHome build pipeline.

---

## File Map

| Action | File | Responsibility |
|---|---|---|
| Create | `components/duke3d/ble_gamepad.h` | Public API: `GamepadState`, `push_report`, `get_state` |
| Create | `components/duke3d/ble_gamepad.cpp` | Diagnostic bit-diff logging; zeroed `get_state` |
| Modify | `components/duke3d/input.h` | Swap `usb_gamepad.h` include + `get_state` call |
| Modify | `components/duke3d/duke3d_component.h` | Remove `set_usb_gamepad` setter and `usb_gamepad_` member |
| Modify | `components/duke3d/duke3d_component.cpp` | Remove `#include "usb_gamepad.h"` and `if (usb_gamepad_)` block |
| Modify | `components/duke3d/CMakeLists.txt` | Swap source file; remove `usb` from REQUIRES |
| Delete | `components/duke3d/usb_gamepad.cpp` | — |
| Delete | `components/duke3d/usb_gamepad.h` | — |
| Modify | `components/duke3d/__init__.py` | Remove `CONF_USB_GAMEPAD` option |
| Modify | `esphome.yaml` | Remove `usb_gamepad: false`; replace sensor lambda |

---

## Task 1: Create `ble_gamepad.h`

**Files:**
- Create: `components/duke3d/ble_gamepad.h`

- [ ] **Step 1: Write the header**

```c
// components/duke3d/ble_gamepad.h
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

// Called from ESPHome BLE sensor lambda on each HID Report notification.
// Logs raw hex dump + per-bit changes vs previous report.
void ble_gamepad_push_report(const uint8_t *data, size_t len);

// Returns zeroed state — game runs in demo mode until mapping is implemented.
GamepadState ble_gamepad_get_state(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Commit**

```bash
git add components/duke3d/ble_gamepad.h
git commit -m "feat(input): add ble_gamepad.h — GamepadState + push_report/get_state API"
```

---

## Task 2: Create `ble_gamepad.cpp`

**Files:**
- Create: `components/duke3d/ble_gamepad.cpp`

- [ ] **Step 1: Write the implementation**

```cpp
// components/duke3d/ble_gamepad.cpp
#include "ble_gamepad.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ble_gamepad";

// Buffer to detect changes between consecutive reports (max 32 bytes)
static uint8_t s_prev[32];
static size_t  s_prev_len = 0;

// Axis dead-zone thresholds — bytes 0-3 treated as axes (heuristic)
static const uint8_t AXIS_DEAD_LO = 0x40;
static const uint8_t AXIS_DEAD_HI = 0xC0;

static bool is_axis_byte(size_t idx) { return idx < 4; }

void ble_gamepad_push_report(const uint8_t *data, size_t len) {
    if (!data || len == 0) return;
    if (len > 32) len = 32;

    // 1. Raw hex dump
    char hex[97];  // 32 bytes * 3 chars + null
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 3 < sizeof(hex); i++) {
        snprintf(hex + pos, 4, "%02X ", data[i]);
        pos += 3;
    }
    if (pos > 0) hex[pos - 1] = '\0';
    ESP_LOGI(TAG, "raw (%zu B): %s", len, hex);

    // 2. Per-byte analysis
    for (size_t i = 0; i < len; i++) {
        uint8_t cur  = data[i];
        uint8_t prev = (i < s_prev_len) ? s_prev[i] : 0;
        if (cur == prev) continue;

        if (is_axis_byte(i)) {
            bool was_lo  = prev < AXIS_DEAD_LO;
            bool now_lo  = cur  < AXIS_DEAD_LO;
            bool was_hi  = prev > AXIS_DEAD_HI;
            bool now_hi  = cur  > AXIS_DEAD_HI;
            bool was_ctr = !was_lo && !was_hi;
            bool now_ctr = !now_lo && !now_hi;

            if (now_lo  && !was_lo)  ESP_LOGI(TAG, "axis byte[%zu] LOW    (0x%02X → 0x%02X)", i, prev, cur);
            if (now_hi  && !was_hi)  ESP_LOGI(TAG, "axis byte[%zu] HIGH   (0x%02X → 0x%02X)", i, prev, cur);
            if (now_ctr && !was_ctr) ESP_LOGI(TAG, "axis byte[%zu] CENTER (0x%02X → 0x%02X)", i, prev, cur);
        } else {
            uint8_t changed = cur ^ prev;
            for (int b = 0; b < 8; b++) {
                if (changed & (1 << b)) {
                    bool pressed = (cur & (1 << b)) != 0;
                    ESP_LOGI(TAG, "byte[%zu] bit%d  %s  (0x%02X → 0x%02X)",
                             i, b, pressed ? "PRESSED " : "RELEASED", prev, cur);
                }
            }
        }
    }

    // 3. Store for next comparison
    memcpy(s_prev, data, len);
    s_prev_len = len;
}

GamepadState ble_gamepad_get_state(void) {
    GamepadState st = {};
    return st;
}
```

- [ ] **Step 2: Commit**

```bash
git add components/duke3d/ble_gamepad.cpp
git commit -m "feat(input): ble_gamepad — diagnostic bit-diff logging, zeroed get_state"
```

---

## Task 3: Update `input.h`

**Files:**
- Modify: `components/duke3d/input.h`

- [ ] **Step 1: Replace the full file content**

```cpp
// components/duke3d/input.h
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

// Push from task context (BLE callback, button handler task).
void input_push(InputEvent evt);

// Push from hardware ISR context only (GPIO interrupt).
void input_push_from_isr(InputEvent evt);

// Pop next event. Returns InputEvent::NONE if queue is empty.
// Called from game task (Core 1) each tick.
InputEvent input_pop();

// Held-button state — updated from BLE HID reports.
// Use this for movement/turning (held actions); use input_pop() for
// one-shot events like menu navigation.
static inline GamepadState input_get_state() {
    return ble_gamepad_get_state();
}
```

- [ ] **Step 2: Commit**

```bash
git add components/duke3d/input.h
git commit -m "feat(input): wire input_get_state to ble_gamepad_get_state"
```

---

## Task 4: Update `duke3d_component.h` and `duke3d_component.cpp`

**Files:**
- Modify: `components/duke3d/duke3d_component.h:21,43`
- Modify: `components/duke3d/duke3d_component.cpp:20,149-156`

- [ ] **Step 1: Remove `set_usb_gamepad` and `usb_gamepad_` from `duke3d_component.h`**

Remove this line from the public setters block:
```cpp
    void set_usb_gamepad(bool v) { usb_gamepad_ = v; }
```

Remove this line from the private members block:
```cpp
    bool usb_gamepad_   = false;
```

- [ ] **Step 2: Remove USB gamepad code from `duke3d_component.cpp`**

Remove this include (line 20):
```cpp
#include "usb_gamepad.h"
```

Remove this block (lines 149-156):
```cpp
    if (usb_gamepad_) {
        bool started = usb_gamepad_start();
        if (started) {
            ESP_LOGI(TAG, "USB gamepad host started (hold BOOT button at power-on to switch to debug mode)");
        } else {
            ESP_LOGW(TAG, "USB debug mode active — connect USB-C for JTAG/CDC logging");
        }
    }
```

- [ ] **Step 3: Commit**

```bash
git add components/duke3d/duke3d_component.h components/duke3d/duke3d_component.cpp
git commit -m "feat(input): remove USB gamepad option from duke3d_component"
```

---

## Task 5: Update `CMakeLists.txt` and delete USB gamepad files

**Files:**
- Modify: `components/duke3d/CMakeLists.txt:40,56`
- Delete: `components/duke3d/usb_gamepad.cpp`
- Delete: `components/duke3d/usb_gamepad.h`

- [ ] **Step 1: Update `CMakeLists.txt`**

In the `idf_component_register(SRCS ...)` block, replace:
```cmake
        "usb_gamepad.cpp"
```
with:
```cmake
        "ble_gamepad.cpp"
```

In the `REQUIRES` line, remove `usb` from the list. The line should become:
```cmake
    REQUIRES hub75_matrix hud sd_card i2s_audio freertos esp_task_wdt esp_wifi wifi driver
```

- [ ] **Step 2: Delete the USB gamepad source files**

```bash
git rm components/duke3d/usb_gamepad.cpp components/duke3d/usb_gamepad.h
```

- [ ] **Step 3: Commit**

```bash
git add components/duke3d/CMakeLists.txt
git commit -m "feat(input): replace usb_gamepad with ble_gamepad in build; remove usb host dep"
```

---

## Task 6: Update `__init__.py`

**Files:**
- Modify: `components/duke3d/__init__.py`

- [ ] **Step 1: Remove `CONF_USB_GAMEPAD` constant, schema entry, and codegen call**

Remove this line:
```python
CONF_USB_GAMEPAD             = "usb_gamepad"
```

Remove this line from `CONFIG_SCHEMA`:
```python
    cv.Optional(CONF_USB_GAMEPAD,              default=False): cv.boolean,
```

Remove this line from `to_code`:
```python
    cg.add(var.set_usb_gamepad(config[CONF_USB_GAMEPAD]))
```

- [ ] **Step 2: Commit**

```bash
git add components/duke3d/__init__.py
git commit -m "feat(input): remove usb_gamepad config option from ESPHome schema"
```

---

## Task 7: Update `esphome.yaml`

**Files:**
- Modify: `esphome.yaml`

- [ ] **Step 1: Remove `usb_gamepad: false` from the `duke3d:` block**

Remove this line from the `duke3d:` section:
```yaml
  usb_gamepad: false   # false = keep USB as JTAG/CDC debug interface
```

- [ ] **Step 2: Replace the sensor lambda body**

The `ble_hid_report_sensor` sensor lambda currently spans about 40 lines of parsing. Replace the entire `lambda: |-` block with:

```yaml
    lambda: |-
      extern "C" void ble_gamepad_push_report(const uint8_t *data, size_t len);
      ble_gamepad_push_report(x.data(), x.size());
      return NAN;
```

The `extern "C"` forward declaration is needed because the lambda is compiled in ESPHome's generated `main.cpp` which does not automatically include `ble_gamepad.h`. The function is linked from the duke3d component.

Also update the `logger:` section — replace the current `ble_hid: INFO` log level entry:
```yaml
    ble_hid: INFO
```
with:
```yaml
    ble_gamepad: INFO
```

- [ ] **Step 3: Commit**

```bash
git add esphome.yaml
git commit -m "feat(input): wire BLE sensor lambda to ble_gamepad_push_report"
```

---

## Task 8: Build verification

- [ ] **Step 1: Compile**

```bash
esphome compile esphome.yaml
```

Expected: build succeeds with no errors referencing `usb_gamepad`, `usb_host`, or missing symbols.

If you see `undefined reference to 'ble_gamepad_push_report'`: verify `ble_gamepad.cpp` is listed in `CMakeLists.txt` SRCS and the `extern "C"` forward declaration is in the lambda.

If you see `error: 'usb_gamepad_get_state' was not declared`: verify `input.h` was updated to include `ble_gamepad.h`.

- [ ] **Step 2: Flash and observe logs**

```bash
esphome run esphome.yaml
```

Expected boot sequence in serial log:
```
I (xxxx) ble_client: Connected to 83:25:03:36:47:6B
I (xxxx) ble_gamepad: Connected: 83:25:03:36:47:6B
```

Press a button on the gamepad. Expected log lines:
```
I (xxxx) ble_gamepad: raw (8 B): 7F 7F 7F 7F 10 00 00 00
I (xxxx) ble_gamepad: byte[4] bit4  PRESSED   (0x00 → 0x10)
I (xxxx) ble_gamepad: byte[4] bit4  RELEASED  (0x10 → 0x00)
```

Move a stick. Expected:
```
I (xxxx) ble_gamepad: axis byte[0] LOW    (0x7F → 0x00)
I (xxxx) ble_gamepad: axis byte[0] CENTER (0x00 → 0x7F)
```

Game should continue running in demo/attract mode with no input.

- [ ] **Step 3: Commit if any last fixups were needed**

```bash
git add -p
git commit -m "fix(input): <describe any fixups>"
```

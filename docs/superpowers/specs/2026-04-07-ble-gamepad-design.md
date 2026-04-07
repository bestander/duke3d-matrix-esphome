# BLE Gamepad Integration — Design Spec

**Date:** 2026-04-07  
**Phase:** Diagnostic / exploratory — log HID report layout; no game input mapping yet.

---

## Goal

Replace the unused USB HID host subsystem with a BLE HID gamepad. The BLE stack is already running via ESPHome (`esp32_ble`, `ble_client`). This phase wires raw HID report notifications into a new `ble_gamepad` C++ module that logs each button press/release so the report layout can be mapped. The game runs normally (demo/attract mode) with no controller input until mapping is added in a follow-up.

---

## Files Changed

### Deleted
- `components/duke3d/usb_gamepad.cpp`
- `components/duke3d/usb_gamepad.h`

### New
- `components/duke3d/ble_gamepad.h` — public API
- `components/duke3d/ble_gamepad.cpp` — diagnostic bit-diff logging; `get_state()` returns zeroes

### Modified
| File | Change |
|---|---|
| `CMakeLists.txt` | Swap `usb_gamepad.cpp` → `ble_gamepad.cpp`; remove `usb` from `REQUIRES` |
| `input.h` | `#include "ble_gamepad.h"`; `input_get_state()` calls `ble_gamepad_get_state()` |
| `duke3d_component.h` | Remove `set_usb_gamepad(bool)` and `usb_gamepad_` member |
| `duke3d_component.cpp` | Remove `#include "usb_gamepad.h"` and `if (usb_gamepad_)` block |
| `__init__.py` | Remove `CONF_USB_GAMEPAD` and `set_usb_gamepad()` call |
| `esphome.yaml` | Remove `usb_gamepad: false`; simplify sensor lambda to call `ble_gamepad_push_report` |

---

## API

```c
// ble_gamepad.h
void ble_gamepad_push_report(const uint8_t *data, size_t len);
GamepadState ble_gamepad_get_state(void);
```

`GamepadState` struct is kept identical to the one previously in `usb_gamepad.h` (forward-compatible with future mapping).

---

## Diagnostic Logging (`push_report`)

Called from the ESPHome BLE sensor lambda on every HID Report notification.

1. **Raw hex dump** — one log line with all bytes (same as current lambda).
2. **Bit-diff logging** — compares report against previous (`static prev[32]`). For each byte that changed, logs each flipped bit:
   ```
   BLE byte[4] bit2  PRESSED   (0x00 → 0x04)
   BLE byte[4] bit2  RELEASED  (0x04 → 0x00)
   ```
3. **Axis dead-zone logging** — bytes 0–3 (likely axes) only log when the value crosses `0x40` or `0xC0` thresholds, avoiding log flooding during stick movement.
4. Stores the new report as `prev[]` for next comparison.

`get_state()` returns a zeroed `GamepadState` — the game sees no input and runs in demo mode.

---

## ESPHome YAML — Sensor Lambda

Replace the current parsing lambda body with:

```cpp
ble_gamepad_push_report(x.data(), x.size());
return NAN;
```

Everything else in the `ble_client` / `esp32_ble` / `esp32_ble_tracker` blocks stays as-is.

---

## RAM Budget

No sdkconfig changes required.

| Item | Effect |
|---|---|
| BLE controller | ~60-80KB internal DRAM — already running, no new cost |
| BLE Nimble host | `CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST: "y"` + `CONFIG_BT_BLE_DYNAMIC_ENV_MEMORY: "y"` already set — host buffers go to PSRAM (531KB heap available, BLE uses ~20-50KB) |
| USB host removed | Frees ~30-50KB internal DRAM (lib overhead + 4KB task stack) |
| **Net** | **Internal DRAM improves; PSRAM unaffected** |

---

## What's Not In Scope

- Button mapping (follow-up, configured in `esphome.yaml`)
- Game input wiring (`ble_gamepad_get_state()` returning real state)
- Reconnect / pairing logic beyond what ESPHome `ble_client` already handles

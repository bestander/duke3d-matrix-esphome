# Pi Pico USB HID Host Bridge

This folder contains firmware for a Raspberry Pi Pico that acts as a
USB HID host (keyboard/gamepad) and forwards Duke3D key events over UART to
the ESP32 firmware.

## UART protocol

The ESP32 UART bridge expects newline-delimited ASCII:

- `SC,<hex_scancode>,<0|1>`
  - `0|1 = 1` key press, `0` key release
  - Example: `SC,0x11,1` (W down), `SC,0x11,0` (W up)

Optional keepalive command:

- `PING` -> Pico replies `PONG`

## Wiring (3.3V logic)

- Pico `UART0 TX` -> ESP32 `pico_uart_rx_pin`
- Pico `UART0 RX` -> ESP32 `pico_uart_tx_pin` (optional, only needed for `PING/PONG`)
- Pico `GND` -> ESP32 `GND`

## Build

The sample under `usb_hid_uart_bridge` uses Pico SDK + TinyUSB host.

This project uses the standard Pico CMake pattern (`pico_sdk_import.cmake`)
like `pi-pico-usb-uart-host`.
Default board is set to `pico_w`.

Typical flow (macOS/Linux):

1. Install Pico SDK toolchain.
2. Clone Pico SDK once (example path):
   - `mkdir -p ~/pico && cd ~/pico`
   - `git clone https://github.com/raspberrypi/pico-sdk.git`
   - `cd pico-sdk && git submodule update --init`
3. Set `PICO_SDK_PATH`:
   - `export PICO_SDK_PATH=$HOME/pico/pico-sdk`
4. Build:
   - `cd pico/usb_hid_uart_bridge`
   - `mkdir -p build && cd build`
   - `cmake ..`
   - `cmake --build .`
   - Optional override for non-W Pico: `cmake -DPICO_BOARD=pico ..`
5. Flash:
   - Hold `BOOTSEL` on Pico while plugging USB.
   - Copy `usb_hid_uart_bridge.uf2` from `build/` to `RPI-RP2`.

## Runtime logging

The firmware emits UART logs (same UART as `SC,...` frames), including:

- `[usb] mount ...` / `[usb] umount ...`
- `[hid] mount ...` and raw HID report bytes
- `[bridge] hb ...` heartbeat and host status

This is useful when tuning controller mappings.

## Notes

- Current mapping supports:
  - USB keyboard HID (boot keyboard reports)
  - USB gamepad HID for report format observed as `len=8` with report ID `0x01`
- Gamepad mapping is intentionally simple first-pass (D-pad/left-stick/buttons
  to Duke movement/fire/use/map/menu). Keep raw HID logs on to tune per-controller.

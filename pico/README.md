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

Bridge traffic (`SC,...`, `CMD,...`, `PONG`) uses **UART0** on **GP0 TX / GP1 RX** at **115200 8N1**.

By default **`PICO_BRIDGE_DEBUG=0`**: no **`[hid]`** UART dumps (keeps **`SC,…`** bandwidth free). Set **`-DPICO_BRIDGE_DEBUG=1`** in CMake when tuning a pad; dumps print **`[hid] gp_bits=…`** plus raw bytes **only when decoded inputs change**. Boot line **`[bridge] usb_hid_uart_bridge ready`** still emits when debug is on. The ESP UART parser **ignores** any line whose first character is `[`, so protocol frames stay clean.

On the ESP, **`[hid]`** lines are accepted but **not** logged (`input.cpp`) so **`pico_uart`** stays readable.

Stdio remains disabled on UART/USB; debug dumps use `uart_write_blocking` only.

USB serial (CDC) stays **off** (USB is used as HID host for the gamepad).

## Gamepad calibration (YAML)

In `esphome.yaml`, `duke3d.pico_gamepad_map` lists each physical control with:

- **`report:`** — optional sample `01 …` hex (documentation only; Pico decode is fixed for this pad layout).
- **`action:`** — Duke binding token (`arrow_up`, `shoot`, `jump`, `escape`, `none`, …). See `components/duke3d/__init__.py` (`DUKE_SCAN_BY_ACTION`).

Every **`esphome compile`** / **`esphome run`** regenerates **`components/duke3d/pico_gamepad_generated.h`**. Rebuild the Pico UF2 afterwards so firmware matches YAML.

Allowed `action:` values: `none`, `arrow_up`, `arrow_down`, `arrow_left`, `arrow_right`, `strafe_mod`, `open`, `jump`, `crouch`, `shoot`, `next_weapon`, `inventory`, `inventory_next`, `escape`.

## Notes

- USB keyboard HID still maps WASD/arrows/Ctrl/Space/Tab/Escape as before.
  Some Adafruit boards expose the **same** 8-byte gamepad payload on an HID interface TinyUSB reports as Boot Keyboard; the bridge detects MatrixPortal-shaped payloads (`report_id==1`, byte `[2]` not zero — unlike HID boot keyboards where `[2]` is reserved `0`) and runs them through the gamepad decoder only. Without that, nibbles were misread as HID keys (**phantom “arrow right” when pressing B**).
- USB gamepad (`report_id==1`, 8 bytes), MatrixPortal calibration:
  - Cross digital on bytes `[3]`/`[4]` → arrows Up/Down/Left/Right (move / turn).
  - LB byte6 bit2 → Duke **Strafe** (Left Alt default).
  - RB byte6 bit3 → **Open** (Space).
  - Z byte6 bit0 → **Jump** (Duke default **A** key / `sc_A`).
  - C byte6 bit1 → **Crouch** (Duke default **Z** key / `sc_Z`).
  - A `(byte[5]^0x0F)==0x40` → **Fire** (Ctrl).
  - B `(byte[5]^0x0F)==0x20` → **Next weapon** (`'` default).
  - X `(byte[5]^0x0F)==0x80` → **Inventory** (Enter default).
  - **Y** `(byte[5]^0x0F)==0x10` → **inventory item next** (`]` default); actual key comes from YAML `action:` → `pico_gamepad_generated.h`.
  - Start `(byte[6]&0x20)` → **Escape** by default (`action: escape`).
  - Physical **star** / **dash** / **heart**: `action: none` — no UART keys (dash/heart are not decoded on the wire).

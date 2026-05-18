# Pi Pico USB HID Host Bridge

Firmware for Raspberry Pi Pico as USB HID host, forwarding **device-neutral** events over UART. **Duke3D bindings live only on the ESP** (`components/duke3d/pico_uart_bridge_maps.h`, regenerated from `esphome.yaml`).

## UART protocol

Newline-terminated ASCII (**115200 8N1**, Pico UART0 GP0 TX / GP1 RX):

### Gamepad (raw HID snapshot on the wire)

- **`GR,<len>,<hex>`** — when the gamepad HID report changes, Pico sends one line: payload length in bytes, comma, then **lowercase** hex (no spaces), e.g. `GR,8,017f7f7f7f0f0000\n`. The ESP decodes MatrixPortal-style 8-byte layout and maps edges to Duke using `duke3d.pico_gamepad_map` (logical names: `cross_up`, `cross_down`, … — see YAML).

### Keyboard (USB HID usage in key slot)

- **`KB,<hid_hex>,<0|1>`** — key from the 6-byte boot keycode array (e.g. **`KB,0x29,1`** = Escape down). ESP maps a small fixed subset to Duke per `pico_uart_bridge_maps.h`.

### Other

- **`PING`** → Pico replies **`PONG\n`**
- **`[bridge] …`**, **`[hid] …`** — optional debug (see `PICO_BRIDGE_DEBUG`). ESP may filter **`[hid]`** for log noise.

Start with YAML **`action: record_session`**: after ESP decodes **`GR,...`**, a **start** press triggers kiosk reload when **`pico_uart_bridge_maps.h`** sets **`PICO_UART_BRIDGE_DROP_HID_KEYBOARD_ESCAPE`** (same rule as ESC-drop for composite pads).

## Wiring (3.3V logic)

- Pico `UART0 TX` → ESP32 `pico_uart_rx_pin`
- Pico `UART0 RX` → ESP32 `pico_uart_tx_pin` (optional; for `PING`/`PONG`)
- Pico `GND` → ESP32 `GND`

## Build

Uses Pico SDK + TinyUSB host (`pico_sdk_import.cmake`). Default **`PICO_BOARD=pico_w`**; override if needed.

Typical flow:

1. Install Pico SDK toolchain; set **`PICO_SDK_PATH`**
2. `cd pico/usb_hid_uart_bridge && mkdir -p build && cd build && cmake .. && cmake --build .`
3. BOOTSEL + copy **`usb_hid_uart_bridge.uf2`**

Firmware is **self-contained** — no include path into `components/duke3d`.

## Runtime logging

Bridge traffic uses **UART0**. With **`PICO_BRIDGE_DEBUG=1`**, verbose **`[hid]`** lines appear. On boot the Pico sends **`[bridge] v1 codecs=GR,KB+PING+PONG`** (or similar) once.

## YAML / ESP codegen

`duke3d.pico_gamepad_map` **`action:`** tokens still define Duke bindings; **`esphome compile`** writes **`components/duke3d/pico_uart_bridge_maps.h`** (`pico_uart_gp_logical_to_duke_scancode`, HID→Duke for **`KB,...`**, **`PICO_UART_BRIDGE_DROP_HID_KEYBOARD_ESCAPE`** when **`start` → `record_session`** so composite pads cannot open the menu via HID Escape).

Panel **dash / heart** share HID byte **[6] bit 0x10**; the ESP treats edges as **`star/heart`** (`ESP_LOGI` `star/heart (vendor)`, no Duke binding unless you extend maps).

## MatrixPortal report layout

Pico forwards **opaque** snapshots only. Fixed 8-byte **`01 | … | b5 b6`** hat and face row is decoded on the ESP (`pico_uart_vendor_hid_decode.*`). No Duke scancodes on the wire.

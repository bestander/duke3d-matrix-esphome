# NimBLE Migration Plan

## Objective

Replace ESPHome's BlueDroid-based `ble_client` stack with a custom component driving
ESP-IDF NimBLE directly. Expected PSRAM recovery: **~130–180KB**, which — with the
current budget of 329KB at `initengine` — raises available PSRAM after `Initcache`
from ~67KB to ~200KB, pushing enough palookup tables to `kkmalloc` that the tile
cache has room for actual tiles.

---

## Prerequisites (fix before NimBLE work)

Fix **Bug 2** from the current crash first (independent of NimBLE): in `tiles.c`, the
`do_scale=0` OOM fallback silently loads a full-size tile that does not fit in the
cache.

- **Option A** (safe): when `tmp_buf` OOM and downscaling is needed, read and
  downscale per-row using an `orig_w`-byte stack scratch. Eliminates the need for
  a 64KB heap allocation.
- **Option B** (simple): skip loading the tile (render black) rather than crashing.
  Acceptable for a crash that would happen only while PSRAM is still tight.

Either fix is a single-function change in `loadtile()`.

---

## Step 1 — sdkconfig changes in `esphome.yaml`

### Remove all BlueDroid keys

```yaml
# Remove these entirely:
CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST: "y"
CONFIG_BT_BLE_DYNAMIC_ENV_MEMORY: "y"
CONFIG_BT_CTRL_BLE_MAX_ACT: "2"
CONFIG_BT_CTRL_SCAN_DUPL_CACHE_SIZE: "20"
CONFIG_BT_ACL_CONNECTIONS: "2"
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM: "4"   # added for BT headroom — re-evaluate
CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM: "6"
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM: "24"
```

### Add NimBLE keys

```yaml
CONFIG_BT_NIMBLE_ENABLED: "y"
CONFIG_BT_NIMBLE_MAX_CONNECTIONS: "1"
CONFIG_BT_NIMBLE_ROLE_CENTRAL: "y"
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL: "n"
CONFIG_BT_NIMBLE_ROLE_BROADCASTER: "n"
CONFIG_BT_NIMBLE_ROLE_OBSERVER: "n"
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL: "y"   # route NimBLE heap to PSRAM
CONFIG_BT_NIMBLE_NVS_PERSIST: "n"               # no bonding state to persist
CONFIG_BT_NIMBLE_SM_LEGACY: "y"                 # legacy pairing for Xbox just-works
CONFIG_BT_NIMBLE_SM_SC: "n"                     # disable secure connections (not needed)
CONFIG_BT_NIMBLE_LOG_LEVEL: "1"                 # WARNING only
```

---

## Step 2 — Remove ESPHome BLE blocks from `esphome.yaml`

Remove entirely:

- `esp32_ble:` block
- `esp32_ble_tracker:` block
- `ble_client:` block (with `on_connect`, `on_disconnect` lambdas)
- The `sensor:` entry with `platform: ble_client` (the `ble_hid_report_sensor`)

The HUD `set_ble_connected()` calls that were in those lambdas move into the NimBLE
component's connect/disconnect callbacks.

---

## Step 3 — New file: `components/duke3d/nimble_gamepad.h`

Minimal C-compatible header:

```c
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/* Call once from Duke3DComponent::setup(), after WiFi bootstrap, before game task starts. */
void nimble_gamepad_init(void);

#ifdef __cplusplus
}
#endif
```

---

## Step 4 — New file: `components/duke3d/nimble_gamepad.cpp`

### State machine

```
IDLE ──start──> SCANNING ──found_mac──> CONNECTING
                                           │
                                     GAP_CONNECT
                                           │
                                     DISC_SERVICES ──found_0x1812──> DISC_CHARS
                                                                           │
                                                                    found_0x2A4D
                                                                           │
                                                                    WRITE_CCCD ──success──> CONNECTED
                                                                                               │
                                                                                     notifications → ble_gamepad_push_report()
                                                                                               │
                                                                                         GAP_DISCONNECT
                                                                                               │
                                                                                           SCANNING (restart)
```

### Key NimBLE API calls

| Operation | NimBLE call |
|-----------|-------------|
| Init stack | `nimble_port_init()` |
| Start host task | `nimble_port_freertos_init(nimble_host_task)` |
| Register GAP callback | `ble_gap_event_listener_register()` |
| Start scan | `ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc_params, gap_event_cb, NULL)` |
| Stop scan | `ble_gap_disc_cancel()` |
| Connect | `ble_gap_connect(own_addr_type, &peer_addr, BLE_HS_FOREVER, NULL, gap_event_cb, NULL)` |
| Discover services | `ble_gattc_disc_svc_by_uuid(conn_handle, &svc_uuid, disc_svc_cb, NULL)` |
| Discover characteristics | `ble_gattc_disc_chrs_by_uuid(conn_handle, svc_start, svc_end, &chr_uuid, disc_chr_cb, NULL)` |
| Enable notifications (CCCD) | `ble_gattc_write_flat(conn_handle, cccd_handle, &notify_val, 2, write_cb, NULL)` |
| Receive notifications | handled in `gap_event_cb` via `BLE_GAP_EVENT_NOTIFY_RX` |

### Pairing (Xbox controller uses just-works)

```c
ble_hs_cfg.sm_io_cap  = BLE_SM_IO_CAP_NO_IO;
ble_hs_cfg.sm_bonding = 0;   // no persistent bonding needed
ble_hs_cfg.sm_mitm    = 0;
ble_hs_cfg.sm_sc      = 0;
```

### Thread safety

`ble_gamepad_push_report()` writes an `std::atomic<uint32_t>` — safe to call from
the NimBLE host task without additional locking.

### HUD status

Call `esphome::hud::global_hud_display->set_ble_connected(true/false)` from
connect/disconnect events. Use a forward-declared extern or pass a callback pointer
from `Duke3DComponent::setup()` to avoid circular includes.

---

## Step 5 — Update `CMakeLists.txt`

```cmake
# SRCS: keep ble_gamepad.cpp (HID parser), add:
"nimble_gamepad.cpp"

# REQUIRES: replace usb/bt references with NimBLE component
REQUIRES hub75_matrix hud sd_card i2s_audio freertos esp_task_wdt esp_wifi wifi driver bt
```

The ESP-IDF `bt` component covers NimBLE when `CONFIG_BT_NIMBLE_ENABLED=y`.

---

## Step 6 — Update `duke3d_component.cpp`

Add one call in `setup()` after WiFi bootstrap, before spawning the game task:

```cpp
#include "nimble_gamepad.h"
// ...
nimble_gamepad_init();
```

Remove any remaining references to `esp32_ble`, `ble_client`, or BlueDroid init.

---

## Step 7 — Update `__init__.py` (optional but clean)

Expose the target MAC as a config key so it is not hardcoded in C++:

```python
CONF_BLE_GAMEPAD_MAC = "ble_gamepad_mac"
cv.Optional(CONF_BLE_GAMEPAD_MAC, default=""): cv.string
```

Pass it to `var.set_ble_gamepad_mac(config[CONF_BLE_GAMEPAD_MAC])` and use in
`nimble_gamepad_init()`. Otherwise keep the substitution in `esphome.yaml` and pass
via a `#define` in codegen.

---

## Expected memory state post-NimBLE

| Stage | PSRAM free |
|---|---|
| Boot | ~580KB (+~120KB recovered vs current) |
| After BLE init (NimBLE) | ~480KB |
| After `Initcache` (262KB) | ~218KB |
| After `kkmalloc` palookup (26 of 32 tables) | ~5KB |
| Tile cache free for tiles | ~213KB (vs current 57KB) |
| 6 palookup tables in tile cache (6×8207=49KB) | 49KB locked |

Logo tile (64KB) fits with 164KB to spare. Audio stream buffers may still use internal
RAM fallback — acceptable given reduced BT internal RAM usage vs BlueDroid.

---

## Risks

| Risk | Mitigation |
|---|---|
| Xbox controller requires secure pairing on some firmware versions | Set `sm_bonding=1` + `NVS_PERSIST=y` if connection drops immediately after connect |
| NimBLE host task stack size | Start with 4KB, increase if stack overflow watchdog fires |
| NimBLE + WiFi radio coexistence | Same coex layer as BlueDroid; no extra work needed |
| `pause_wifi` shuts down radio — does it affect NimBLE? | BLE uses the BT controller, not WiFi driver; `esp_wifi_stop()` does not affect BT controller |
| ESPHome OTA may re-enable BLE components if yaml is regenerated | Keep all BLE components removed from yaml |
| GATT service/char handle ordering varies by controller firmware | Use UUID-based discovery (not fixed handles) — already in the plan above |

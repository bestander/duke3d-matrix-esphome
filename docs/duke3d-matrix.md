# Duke3D on ESP32-S3 LED Matrix

**Project:** `duke3d-matrix-esphome`  
**Merged from:** design spec (2026-03-17), implementation plan (2026-03-17), PSRAM plan (2026-03-28), GRP splash plan (2026-03-29)

This document is the single source of truth for goals, architecture, and memory strategy. Pin numbers and `esphome.yaml` are authoritative for wiring; narrative below may reference **Adafruit Matrix Portal S3** as the intended panel while the checked-in board may be `esp32-s3-devkitc-1` during bring-up.

---

## Overview

Port Duke Nukem 3D to an ESP32-S3 driving a 64×64 HUB-75 LED matrix, wrapped as an ESPHome project for Home Assistant. The display runs the Duke3D engine (demo playback by default). The panel is split into two zones:

```
┌──────────────────────┐  ↑
│                      │  │
│    Duke3D demo       │  40 px  (game: 320×200 → 64×40, aspect-preserving)
│    64 × 40           │  │
│                      │  ↓
├──────────────────────┤
│  time · weather HUD  │  24 px  (HA-fed widgets)
│    64 × 24           │  │
└──────────────────────┘
```

Sound is optional via I2S (e.g. MAX98357A). Work proceeds in phases: scaffold → display → HUD → SD → engine → input → audio.

---

## Reference projects

| Project | Relevance |
|---------|-----------|
| [bestander/duke3d-matrix](https://github.com/bestander/duke3d-matrix) | Original concept: Duke3D on LED matrix |
| [jkirsons/Duke3D](https://github.com/jkirsons/Duke3D) | ESP-IDF Duke3D port, SD GRP, GPL2 — engine basis |
| [DynaMight1124/retro-go](https://github.com/DynaMight1124/retro-go/releases/tag/Quake%26Duke) | ESP32-S3 + PSRAM achieves playable Duke3D framerates |

---

## Hardware

### Bill of materials (target setup)

| Component | Part | Interface |
|-----------|------|------------|
| Main board | Adafruit Matrix Portal S3 (#5778) | — |
| LED panels | 2× 64×32 HUB-75 chained → 64×64 | HUB-75 |
| SD card | SPI breakout | SPI |
| Audio (optional) | I2S amp (e.g. MAX98357A) | I2S |

### MCU (Matrix Portal S3 reference)

- **MCU:** ESP32-S3 dual-core @ 240 MHz  
- **Flash:** 8 MB (typical)  
- **PSRAM:** 8 MB Octal-SPI on Portal; **current `esphome.yaml` may use a dev kit with 2 MB quad PSRAM** — memory tables below call out both contexts.  
- **HUB-75:** Dedicated connector on Portal  
- **USB:** USB-C (power; OTG possible for gamepad work)  

Confirm GPIO mapping from the [Adafruit Matrix Portal S3](https://learn.adafruit.com/adafruit-matrix-portal-s3) schematic before trusting pad labels. ESP32-S3 GPIO35–38 are reserved for Octal PSRAM and must not be used for peripherals.

### Pin allocation (conceptual)

| Signal | Notes |
|--------|--------|
| HUB-75 | Dedicated (Portal) — no breakout pins consumed |
| SD SPI | MOSI, MISO, SCK, CS — see `esphome.yaml` |
| I2S | BCLK, LRCLK, DIN; DIN may share SD MOSI with CS discipline |
| Future input | USB OTG or GPIO buttons |

> **Shared MOSI/DIN:** When DIN and SD MOSI share a pin, de-assert SD CS before any I2S transaction; only one peripheral drives the line at a time.

---

## Repository layout

```
duke3d-matrix-esphome/
  esphome.yaml
  secrets.yaml.template
  partitions.csv
  libs/
    ESP32-HUB75-MatrixPanel-I2S-DMA/    # submodule
  components/
    hub75_matrix/                        # 64×64 double-buffered HUB-75
    hud/                                 # rows 40–63
    sd_card/
    duke3d/
      duke3d_component.{h,cpp}
      renderer/
      platform/                          # HAL, input shims
      engine/                            # Duke3D (submodule / fork)
    i2s_audio/
  test/                                  # host-side tests where present
  docs/
    README.md
    duke3d-matrix.md                     # this file
```

---

## Software architecture

### Core allocation

```
Core 0  — ESPHome: WiFi, HA API, OTA, sensors
Core 1  — HUB-75 scan (high priority), Duke3D game task (pinned), I2S if used
```

### Application model

On boot, the engine runs in demo playback mode (looping `.DMO` files from the GRP). The HUD draws the bottom band each frame using the latest HA-fed state (mutex-protected where needed).

```
Boot → Duke3D demo loop (Core 1)
         → render game to rows 0–39 → HUD rows 40–63 → swap_buffers()

Core 0 → HA / WiFi → updates shared HUD state
```

### Home Assistant integration (examples)

Wire entities in `esphome.yaml` to match your HA install. Typical patterns:

| Area | Direction | Purpose |
|------|-----------|---------|
| Time | HA → device | Clock for HUD |
| Temperature / forecast | HA → device | HUD |
| Tide or custom text | HA → device | HUD lines |
| Demo / status | device → HA | Optional `text_sensor` for debug |

---

## Component interfaces (sketch)

```cpp
// hub75_matrix — back buffer writes; scan ISR reads front; swap_buffers() per frame
class Hub75Matrix {
    void set_pixel(int x, int y, Color c);
    void fill(Color c);
    void swap_buffers();
};

// hud — rows 40–63 only
class Hud {
    void set_time(int h, int m);
    void set_min_temp(float c);  // etc.
    void render(Hub75Matrix& display);
};

// duke3d
class Duke3DComponent {
    bool start();
    void stop();
    const char* current_demo();
    void inject_input(InputEvent evt);  // when input enabled
};
```

---

## Memory

### Internal SRAM (order of magnitude)

Small allocations and stacks stay in internal DRAM; large buffers use PSRAM when `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` and heap caps permit.

### PSRAM — two deployment contexts

**Design target (e.g. Matrix Portal 8 MB):** large headroom for engine heaps, tile cache, and WiFi.

**Current dev kit (often 2 MB quad PSRAM):** budgets are tight. Static engine BSS in PSRAM, HUB75 DMA, SD/FatFS, and **WiFi stacks** compete with **Initcache** (tile cache). Observed ballpark at game start (order of magnitude, not a guarantee on every build):

| Item | Notes |
|------|--------|
| Total PSRAM | ~2048 KB on 2 MB parts |
| Static BSS (EXT_RAM_ATTR) | Large (engine arrays) |
| WiFi / TCP | ~350 KB when up — major lever for freeing heap |
| HUB75 DMA | ~128 KB (typical) |
| Game task stack | Shrink from 64 KB toward ~20 KB if high-water mark allows |
| `MAXSPRITESONSCREEN` | Reducing 1024 → 128 saves tens of KB of BSS on a 64×40 panel |

### Flash partitions

See `partitions.csv` — typical layout: NVS, OTA data, dual app slots, FAT for assets/logs.

### MicroSD layout

```
/sdcard/duke3d/DUKE3D.GRP     game data (GRP)
/sdcard/duke3d/LOADSCR.RGB    optional boot splash cache (see below)
```

Optional `TCACHE.BIN` (or similar) may be built on first boot to reduce GRP seek load.

### Boot splash (GRP cache)

On startup, `hub75_matrix` builds or loads a **64×40×3 RGB888** splash from `DUKE3D.GRP` (same aspect as the game strip). Implementation lives in `components/hub75_matrix/hub75_matrix.cpp` (`grp_title_splash_build_cache_if_needed`).

| Item | Detail |
|------|--------|
| **Output** | `/sdcard/duke3d/LOADSCR.RGB` (fallback `/sdcard/LOADSCR.RGB`), 8.3-safe names |
| **Header** | 8-byte magic `SPLASH05`, 4-byte LE `grp_size`, 4 reserved (0); then 7680 bytes RGB |
| **Tile choice** | Prefer picnums in order: **3281** (LOADSCREEN), 2456, 2492, 2493; else first 320×200 tile in the GRP |
| **ART layout** | Pixel data is **column-major** (`index = x * height + y`), matching `tiles.c` |
| **Palettes** | `PALETTE.DAT` for game / LOADSCREEN; `LOOKUP.DAT` extracts **titlepal** / **drealms** for menu-style tiles when needed |
| **Atomic write** | `LOADSCR.TMP` then `remove` destination + `rename`; FAT often returns `EEXIST` on replace-rename |
| **Task** | Built on a dedicated FreeRTOS task so `loopTask` stack is not exhausted |

Logs: `grp_splash: …`, `hub75: splash source=sd_cache` (or compiled/solid if no SD cache).

Do not set `splash_image:` in ESPHome if you want SD-only splash; a generated header would override when cache is missing.

---

## Rendering pipeline

```
320×200 palettized framebuffer
  → palette → RGB
  → downscale to 64×40 (aspect-preserving)
  → rows 0–39 of HUB-75 back buffer
  → Hud::render() rows 40–63
  → swap_buffers()
```

Target framerate: ≥20 fps early port; 25–30 fps after optimization.

---

## Phased roadmap (summary)

| Phase | Deliverable | Exit criterion (summary) |
|-------|-------------|----------------------------|
| 1 | ESPHome scaffold, partitions, WiFi/HA/OTA | Device in HA; OTA works |
| 2 | `hub75_matrix` 64×64 double-buffered | Smooth animation on panels |
| 3 | `hud` bottom band | Time/sensors update from HA |
| 4 | `sd_card` SPI FAT | Can read `/duke3d/DUKE3D.GRP` |
| 5 | Duke3D demo loop + renderer + component | Continuous demo ≥20 fps; HUD visible |
| 6 | Input (USB HID or GPIO) | Menus playable with chosen input |
| 7 | `i2s_audio` | Sound audible |

Detailed step-by-step checklists with full code snippets were folded into the repository during implementation; use git history if you need the original long-form plan.

---

## PSRAM budget: WiFi pause and tile cache

**Goal:** Grow Initcache (e.g. toward 512 KB) so Atomic Edition levels hit SD less often, while still allowing periodic HA sync.

**Approach (conceptual):**

1. **Stack:** Reduce game task stack if `uxTaskGetStackHighWaterMark()` shows large margin (e.g. 64 KB → 20 KB).  
2. **BSS:** Lower `MAXSPRITESONSCREEN` if safe for the visible sprite count on 64×40.  
3. **WiFi:** `esp_wifi_stop()` before large Initcache allocation frees PSRAM; optional **cooperative suspend** from `platform_blit_frame()` lets Core 0 run a short WiFi window (HA reconnect, sensors) while the last frame holds on screen.

**Config knobs (see `components/duke3d/__init__.py` and `esphome.yaml`):** e.g. `tile_cache`, `pause_wifi`, grace interval before stopping WiFi, debounce for sync windows.

**Verification:** Serial logs for Initcache size, `fps`/`stack` lines, tilecache messages, and WiFi window logs; confirm no watchdog resets during long runs.

---

## Key risks and mitigations

| Risk | Mitigation |
|------|------------|
| PSRAM pressure | WiFi pause, stack/BSS tuning, tile cache file |
| Game vs HUB-75 timing | Separate cores; scan task priority |
| SD vs I2S pin sharing | CS / timing discipline |
| Task watchdog | Periodic `esp_task_wdt_reset()` in game loop or appropriate TWDT config |
| Missing GRP | Fail startup with visible error |

---

## Out of scope

- Multiplayer networking  
- Full save-state gameplay (demo-focused path)  
- Custom PCB / enclosure  
- External music streaming (in-engine audio only)

---

## Build and flash

```bash
pip install esphome
cp secrets.yaml.template secrets.yaml   # fill in
esphome config esphome.yaml
esphome compile esphome.yaml
esphome upload esphome.yaml
```

Use `esphome boards esp32` if the board name in YAML needs to match your module.

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

## Audio system

### Hardware

An I2S mono amplifier (e.g. MAX98357A) is wired to three GPIO pins configured in `esphome.yaml` under the `i2s_audio:` block:

| Signal | GPIO (Matrix Portal S3) |
|--------|------------------------|
| BCLK   | A0 (GPIO12)            |
| LRCLK  | RX (GPIO8)             |
| DIN    | TX (GPIO18)            |

### Software stack

```
Duke3D game code
  sounds.c  ──┐
  xyzsound()  │  FX_PlayVOC3D / FX_PlayWAV3D
              ▼
   Apogee Multivoc  (fx_man.c, multivoc.c, mv_mix.c, ll_man.c, pitch.c)
        │  MV_ServiceVoc() — software mixing of up to 32 voices
        │  produces 16-bit mono PCM at 11025 Hz, 256 samples/buffer
        ▼
    dsl.c  — ESP32 audio driver (FreeRTOS task, Core 1, priority 4)
        │  vTaskDelay(23 ms) → portENTER_CRITICAL → MV_ServiceVoc
        │  → portEXIT_CRITICAL → platform_audio_write()
        ▼
    esp32_hal.cpp  (platform_audio_write)
        │  duplicates mono samples L=R into 512-byte stereo buffer
        ▼
    i2s_audio component  (i2s_write, ticks_to_wait=0, non-blocking)
        ▼
    I2S DMA → amplifier
```

### Audio driver (dsl.c)

`dsl.c` replaces the original SDL_OpenAudio-based driver. It spawns a single FreeRTOS task pinned to Core 1 at priority 4 (below the game task at priority 5). Every ~23 ms it:

1. Acquires a `portMUX` critical section (serialises voice-list access with the game task on the same core).
2. Calls `MV_ServiceVoc()` to mix the next 256-sample page into `MV_MixBuffer`.
3. Releases the critical section.
4. Calls `platform_audio_write()` with the freshly-mixed page.

`DisableInterrupts`/`RestoreInterrupts` — originally used to mask the DOS hardware timer ISR — are implemented with the same `portMUX`, so `MV_PlayVoice`/`MV_StopVoice` calls from the game task are atomic with the mixing loop.

The `vTaskDelay(23 ms)` strategy means the audio task naturally gets CPU time during SD card reads (game task blocked on SPI DMA) delivering near-continuous audio with zero spinning.

### Mixing parameters

| Parameter | Value | Source |
|-----------|-------|--------|
| Sample rate | 11025 Hz | `MixRate` default in `config.c`; matches I2S driver config |
| Voices | 32 | `NumVoices` default in `config.c` |
| Channels | 1 (mono) | `NumChannels`; samples duplicated L=R in `platform_audio_write` |
| Bits | 16 | `NumBits` |
| Buffer size | 256 samples | `MixBufferSize` in Multivoc |
| I2S DMA | 8 × 256-sample buffers = 8192 B ≈ 370 ms | `dma_buf_count=8, dma_buf_len=256` in `i2s_audio.cpp` |

### Source files compiled

The Apogee Multivoc library is included by `pre_build.py` via CMake `file(GLOB_RECURSE)` from `engine/components/audiolib/`. The following source files are compiled:

| File | Purpose |
|------|---------|
| `fx_man.c` | FX_* public API |
| `multivoc.c` | Software voice mixer |
| `mv_mix.c` | Inner mixing loops |
| `ll_man.c` | Low-level manager |
| `pitch.c` | Pitch shifting |
| `dsl.c` | ESP32 audio driver (FreeRTOS task) |
| `user.c` | `USER_CheckParameter` — returns false on non-DOS |

Excluded from the build (DOS/ISA hardware drivers, OPL2/MIDI music, duplicate symbols):
`adlibfx`, `al_midi`, `awe32`, `blaster`, `debugio`, `dma`, `dpmi`, `gus*`, `irq`, `leetimbr`, `midi`, `mpu401`, `music`, `nomusic`, `pas16`, `sndscape`, `sndsrc`, `task_man`, `gmtimbre`, `myprint`, `usrhooks`.

### Music stubs

OPL2/MIDI music is not ported. `audiolib_stubs.c` provides no-op implementations of all `MUSIC_*` symbols. The game is launched with `/nm` to skip music. `PlayMusic()` is also stubbed. `music.h` is intentionally **not** included in stubs — it transitively includes `duke3d.h` which defines (not just declares) large global arrays, causing duplicate symbols and ~1.6 MB DRAM overflow.

### PSRAM memory layout for audio

Duke3D has up to 600 sound definitions. The engine lazily loads sound files on first play (`loadsound()` in `sounds.c`, called when `Sound[num].ptr == 0`).

**Sound buffers live in PSRAM heap** (not the tile cache). `loadsound()` uses `heap_caps_malloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` directly. `clearsoundlocks()` — called at every level transition and menu entry — frees all sound buffers that are not currently playing (`Sound[num].num == 0`), returning PSRAM to the heap. Sounds reload from SD on next play.

Tile cache is capped at **256 KB** (reduced from 512 KB) to leave ~200 KB of PSRAM heap for concurrent sound buffers (~25 × 8 KB sounds). Tile eviction is more frequent as a result but the tile cache aging/LRU mechanism handles this transparently.

### Bug: `ERROR: CACHE SPACE ALL LOCKED UP!` (fixed)

**Symptom:** Game crashed mid-demo with `ERROR: CACHE SPACE ALL LOCKED UP!` after ~90 seconds of play with audio enabled.

**Root cause:** Sound buffers were originally allocated from the tile cache via `allocache(&Sound[num].ptr, l, &Sound[num].lock)`. The engine uses `Sound[num].lock >= 200` as a "do not evict" marker, incrementing the lock each time a sound plays (`Sound[num].lock++`). In `allocache()`:

```c
if (*cac[zz].lock >= 200) { daval = 0x7fffffff; break; }
```

Any cache region containing even a single lock≥200 block is marked maximally expensive to evict. As sounds accumulated during a demo level, their lock≥200 blocks fragmented the 448 KB cache such that no contiguous region large enough for a new tile was free of permanently-locked blocks.

**Fix:** Sound buffers were removed from the tile cache entirely. `loadsound()` now uses `heap_caps_malloc(MALLOC_CAP_SPIRAM)`. `clearsoundlocks()` explicitly frees non-playing sounds with `heap_caps_free`. The tile cache is now exclusively for tiles and can never be locked up by sound data.

---

## Key risks and mitigations

| Risk | Mitigation |
|------|------------|
| PSRAM pressure | WiFi pause, stack/BSS tuning, tile cache file |
| Game vs HUB-75 timing | Separate cores; scan task priority |
| SD vs I2S pin sharing | CS / timing discipline |
| Task watchdog | Periodic `esp_task_wdt_reset()` in game loop or appropriate TWDT config |
| Sound/tile cache contention | Sounds use dedicated PSRAM heap; tile cache is tiles-only |
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

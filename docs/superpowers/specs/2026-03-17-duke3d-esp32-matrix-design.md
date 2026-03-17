# Duke3D ESP32-S3 LED Matrix — Design Spec

**Date:** 2026-03-17
**Status:** Approved (GPIO pad numbers to be confirmed from Adafruit schematic before Phase 1)
**Project:** duke3d-matrix-esphome

---

## Overview

Port Duke Nukem 3D to an Adafruit Matrix Portal S3 (ESP32-S3) driving a 64×64 HUB-75 LED matrix, wrapped as an ESPHome project for Home Assistant integration. The display is always on and always running the Duke3D engine in demo playback mode — looping through the prerecorded `.DMO` demo files bundled in the GRP. The screen is split into two permanent zones:

```
┌──────────────────────┐  ↑
│                      │  │
│    Duke3D demo       │  40 px  (game rendering, aspect-preserving 320×200 → 64×40)
│    64 × 40           │  │
│                      │  ↓
├──────────────────────┤
│                      │  │
│  time · weather HUD  │  24 px  (HA-fed widgets)
│    64 × 24           │  │
│                      │  ↓
└──────────────────────┘
```

Weather and time are pushed from Home Assistant and rendered permanently in the bottom 14-pixel HUD band. Sound is output via an I2S Class D amplifier. The project is built in phases, starting with the ESPHome scaffold and display, then incrementally adding the game engine, input (for switching demos or interacting), and audio.

---

## Reference Projects

| Project | Relevance |
|---|---|
| [bestander/duke3d-matrix](https://github.com/bestander/duke3d-matrix) | Original concept: Duke3D on LED matrix, Raspberry Pi + eduke32 |
| [jkirsons/Duke3D](https://github.com/jkirsons/Duke3D) | ESP-IDF Duke3D port, 4MB PSRAM min, SD card GRP, GPL2 — primary engine basis |
| [DynaMight1124/retro-go](https://github.com/DynaMight1124/retro-go/releases/tag/Quake%26Duke) | Confirms ESP32-S3 + 8MB PSRAM achieves 25–30fps with Duke3D |

---

## Hardware

### Bill of Materials

| Component | Part | Interface |
|---|---|---|
| Main board | Adafruit Matrix Portal S3 (#5778) | — |
| LED panels | 2× 64×32 HUB-75 P-series, chained vertically → 64×64 | HUB-75 (built-in connector) |
| SD card | Adafruit MicroSD Breakout Board+ (#254) | SPI (4 GPIO pins) |
| Audio amp | Adafruit I2S 3W Class D Amplifier MAX98357A (#3006) | I2S (3 GPIO pins) |
| Speaker | 4Ω or 8Ω small speaker | Terminal block on amp |

### Matrix Portal S3 Specs

- **MCU:** ESP32-S3, dual-core Xtensa LX7 @ 240MHz
- **Flash:** 8MB
- **PSRAM:** 8MB (Octal-SPI)
- **HUB-75:** Dedicated 2×10 IDC connector, hardware-accelerated scanning
- **USB:** USB-C (power + USB OTG capable for future joystick phase)
- **GPIO breakout:** 6 pins available (A0–A5 / GPIO1–GPIO6 per Adafruit pinout); UART0 TX/RX (GPIO43/44) are on a separate debug header and do not consume breakout pins
- **Initial flashing:** Via USB-C using `esphome upload` or `esptool.py` with the device in bootloader mode (hold BOOT, press RESET). Subsequent updates via OTA.

### Pin Allocation

| Signal | Breakout pad | Notes |
|---|---|---|
| HUB-75 matrix | dedicated IDC | No breakout pins consumed |
| SD MOSI | A0 | |
| SD MISO | A1 | |
| SD SCK | A2 | |
| SD CS | A3 | |
| I2S BCLK | A4 | |
| I2S LRCLK | A5 | |
| I2S DIN | A0 | **Shared with SD MOSI** — safe: both are output-only signals, electrically isolated by SD chip-select. Only one peripheral drives the pin at a time. |
| Future joystick | USB OTG | Existing USB-C port |

> **Important:** The Matrix Portal S3 breakout exposes pads A0–A5. The exact ESP32-S3 GPIO numbers behind each pad **must be confirmed** from the [Adafruit Matrix Portal S3 schematic](https://learn.adafruit.com/adafruit-matrix-portal-s3) before writing `esphome.yaml`. GPIO35–38 on the ESP32-S3 are reserved for the internal Octal-SPI PSRAM bus and must **not** be used. The shared MOSI/DIN pad requires SD CS de-asserted before any I2S transaction.

---

## Software Architecture

### Repository Structure

```
duke3d-matrix-esphome/
  esphome.yaml                  # top-level ESPHome config
  partitions.csv                # custom partition table
  components/
    hub75_matrix/               # HUB-75 custom component (64×64, double-buffered)
    duke3d/                     # Duke3D engine + ESP32-S3 HAL
      engine/                   # forked jkirsons/Duke3D engine
      platform/                 # ESP32-S3 HAL (SD, audio, input shims)
      renderer/                 # framebuffer → rows 0-39 of HUB-75 scaler
    hud/                        # time + weather HUD band (rows 50-63)
    sd_card/                    # SPI FATFS wrapper
    i2s_audio/                  # MAX98357A I2S output
  docs/
    superpowers/specs/          # design specs
```

### Core Allocation

```
Core 0  (ESPHome / WiFi / FreeRTOS scheduler)
  └─ ESPHome main task: WiFi, HA native API, OTA, sensor updates

Core 1  (real-time)
  ├─ HUB-75 scan task        priority: highest  ~2kHz refresh
  ├─ Duke3D game task         priority: high     pinned to Core 1
  └─ I2S audio task           priority: high     DMA-driven
```

### Application Model

The device runs a single permanent loop — there is no idle state. On boot, the Duke3D engine starts in demo playback mode and loops continuously through the demo files in the GRP. The HUD band renders independently on every frame using the latest weather/time data received from HA.

```
Boot
 │
 ▼
Duke3D demo loop (always running, Core 1)
 │   plays DEMO1.DMO → DEMO2.DMO → DEMO3.DMO → repeat
 │
 ├─ renders top 50px of each frame → HUB-75 back buffer
 │
 └─ HUD task overlays bottom 14px (time + weather) before swap_buffers()

Core 0: WiFi / ESPHome / HA API
 │
 └─ receives weather + time updates from HA → shared state (mutex-protected)
```

### Home Assistant Integration Points

| ESPHome entity | Direction | Purpose |
|---|---|---|
| `sensor.weather_temperature` | HA → device | Current temperature (°C), shown in HUD |
| `sensor.weather_condition` | HA → device | Weather condition string/icon, shown in HUD |
| `time.homeassistant` | HA → device | NTP-synced time for HUD clock |
| `sensor.duke3d_demo` | device → HA | Currently playing demo file name |

---

## Component Interfaces

```cpp
// hub75_matrix — owns the physical display
// Thread-safety contract:
//   set_pixel() and fill() always write to the BACK buffer only.
//   The HUB-75 scan ISR reads the FRONT buffer only.
//   Both are safe to call from any task without a mutex.
//   swap_buffers() is the only synchronization point; it atomically
//   promotes the back buffer to front. Call once per rendered frame.
class Hub75Matrix {
    void set_pixel(int x, int y, Color c);
    void fill(Color c);
    void swap_buffers();          // atomic, ISR-safe double-buffer swap
};

// hud — renders time + weather widgets into the bottom 24 rows (y=40..63)
// Called once per game frame, after Duke3D writes the top 40 rows.
class Hud {
    void set_temperature(float celsius);   // called from ESPHome/Core 0, mutex-protected
    void set_condition(const std::string& icon_key);
    void render(Hub75Matrix& display);     // writes only to rows 40–63
};

// duke3d — game component, always running in demo playback mode
class Duke3DComponent {
    bool start();                 // loads GRP from SD, starts demo loop on Core 1; returns false if GRP missing
    void stop();                  // signals shutdown, joins task
    const char* current_demo();   // returns name of currently playing demo file (e.g. "DEMO1.DMO")
    void inject_input(InputEvent evt);  // reserved for Phase 6 (input control)
};

// sd_card — thin FATFS wrapper
class SdCard {
    esp_err_t mount();
    FILE* open(const char* path, const char* mode);
};

// i2s_audio — PCM audio sink
class I2SAudio {
    esp_err_t init(int bclk_pin, int lrclk_pin, int din_pin);
    void write_pcm(const int16_t* buf, size_t num_samples);
};
```

---

## Memory Layout

### Internal SRAM (~384KB usable heap)

| Region | Size | Contents |
|---|---|---|
| ESPHome main task stack | ~8KB | WiFi, HA API, OTA |
| Duke3D game task stack | ~16KB | Game loop, engine calls |
| HUB-75 ISR stack | ~4KB | Scan interrupt handler |
| I2S audio task stack | ~4KB | DMA callback |
| WiFi / LwIP / TLS | ~150KB | TCP stack, TLS handshake buffers |
| ESP-IDF system / FreeRTOS | ~64KB | Scheduler, timers, misc |
| Free headroom | ~138KB | Malloc for small allocations |

All large allocations (game data, framebuffers, audio ring buffer) are directed to PSRAM via `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`.

### PSRAM (8MB)

| Region | Size | Contents |
|---|---|---|
| Duke3D heap | ~4MB | Engine state, map data, textures in-flight |
| HUB-75 framebuffers | ~32KB | 2× 64×64×3 bytes (double-buffered) |
| Audio ring buffer | ~64KB | I2S DMA chunks |
| Weather/clock assets | ~16KB | Fonts, icon bitmaps |
| Free headroom | ~3.8MB | Stack, future use |

### Flash Partition Table

| Partition | Type | Size | Purpose |
|---|---|---|---|
| nvs | NVS | 16KB | WiFi credentials, ESPHome config |
| otadata | OTA data | 8KB | OTA slot selector |
| app0 (OTA_0) | app | 3MB | Main firmware |
| app1 (OTA_1) | app | 3MB | OTA update target |
| fatfs | data | 1.5MB | ESPHome internals / logs |

### MicroSD Layout (FAT32)

```
/duke3d/
  DUKE3D.GRP          shareware v1.3d (~11MB)
/duke3d_full/
  DUKE3D.GRP          registered / Atomic v1.5 (~26MB, optional)
```

---

## Rendering Pipeline

Duke3D renders internally at 320×200 in an 8-bit palettized framebuffer. The output is fixed to the top 50 rows of the 64×64 panel; the bottom 14 rows are reserved for the HUD.

```
320×200 engine framebuffer (8-bit, palette-indexed)
    │
    ▼ palette lookup: 256-color → RGB888
    │
    ▼ nearest-neighbor downscale to 64×40 (aspect-preserving: 320/200 = 1.6 = 64/40)
    │
    ▼ write to rows 0–39 of HUB-75 back buffer
    │
    ▼ Hud::render() writes rows 40–63 (time + weather widgets, 24px)
    │
    ▼ swap_buffers() → full 64×64 frame visible
```

The 64×40 game zone is aspect-preserving (320/200 = 64/40 = 1.6). The bottom 24px HUD band uses the remaining space, giving enough room for a clock row and a weather row.

Target: 25–30fps minimum (confirmed achievable on ESP32-S3 + 8MB PSRAM by retro-go prior art). Phase 5 exit criterion is ≥20fps (acceptable during initial port); 25–30fps is the post-optimization target confirmed achievable without hardware changes.

---

## Development Phases

### Phase 1 — ESPHome Scaffold
- `esphome.yaml` targeting Matrix Portal S3, `framework: esp-idf`
- Custom partition table
- WiFi, HA native API, OTA working
- Stub custom components compiling
- **Exit criteria:** Device appears in HA, OTA update succeeds

### Phase 2 — HUB-75 LED Matrix
- `hub75_matrix` custom component with double-buffered 64×64 driver
- Test pattern and color cycling on boot
- **Exit criteria:** Smooth animation visible on both chained panels

### Phase 3 — HUD Band
- `hud` component rendering time and weather widgets into the bottom 24 rows (y=40–63)
- Two-row layout: clock row (~10px) + weather row (~10px) with small bitmap font and weather icons
- HA sensor → HUD pipeline working; HUD renders over a test pattern in the top 40 rows
- **Exit criteria:** Time updates every second, weather condition and temperature update on HA push; top 40 rows unaffected

### Phase 4 — SD Card
- `sd_card` SPI FATFS component
- GRP file detection and basic file streaming API
- **Exit criteria:** Can open and read bytes from `/duke3d/DUKE3D.GRP`

### Phase 5 — Duke3D Demo Playback
- Fork `jkirsons/Duke3D` engine into `components/duke3d/engine/`
- Replace ILI9341 framebuffer output with HUB-75 renderer (rows 0–39 only)
- Replace SD/audio HAL with ESP-IDF components
- Enable Duke3D demo playback mode: loops DEMO1.DMO → DEMO2.DMO → DEMO3.DMO from GRP
- Wrap as ESPHome custom component with Core 1 pinned task; starts automatically on boot
- HUD renders rows 50–63 on each frame after game renderer
- `sensor.duke3d_demo` publishes current demo filename to HA
- **Exit criteria:** Demo loop runs continuously at ≥20fps; HUD visible in bottom band; no manual intervention needed after boot

### Phase 6 — Input
- Decide between USB OTG HID gamepad (via USB-C OTG adapter) or GPIO button panel before starting Phase 5 so `inject_input()` has a concrete implementation to test with
- `InputEvent` enum must cover: `MOVE_FORWARD`, `MOVE_BACK`, `STRAFE_LEFT`, `STRAFE_RIGHT`, `TURN_LEFT`, `TURN_RIGHT`, `FIRE`, `USE`, `OPEN_MAP`, `MENU_TOGGLE`
- USB HID path: ESP-IDF USB Host HID class driver → map HID usage IDs to `InputEvent`
- GPIO path: 10 buttons on breakout-accessible GPIO pins → ISR → `InputEvent` queue
- **Exit criteria:** Can navigate Duke3D menus and complete a room using chosen input method

### Phase 7 — Sound
- `i2s_audio` component driving MAX98357A
- Duke3D audio output routed through I2S DMA
- **Exit criteria:** In-game sound effects and music audible through speaker

---

## Key Risks & Mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| 8MB PSRAM insufficient for Duke3D | Low | Confirmed by retro-go port on identical hardware |
| ESPHome build pipeline incompatible with Duke3D C code | Medium | jkirsons port is pure ESP-IDF; ESPHome `framework: esp-idf` compiles standard C/C++ |
| HUB-75 scan task starved by game loop | Medium | Pinned tasks on separate cores; HUB-75 task at highest priority |
| SD card SPI conflicts with I2S (shared MOSI/DIN pin) | Low | SD CS de-asserted before any I2S transaction; only one peripheral active at a time |
| ESPHome TWDT resets device during game loop | High | ESPHome enables the task watchdog timer by default; the Duke3D game task must call `esp_task_wdt_reset()` each frame or disable TWDT for that task via `esp_task_wdt_delete(NULL)` |
| Missing or unreadable GRP file on boot | Medium | `Duke3DComponent::start()` returns `false` if SD mount fails or `/duke3d/DUKE3D.GRP` is absent; device displays error message across full 64×64 matrix until SD is reinserted and device reboots |
| ESPHome constraints block game loop in later phases | Low | Architecture designed to migrate to pure IDF (Option B) without rewriting components |

---

## Out of Scope

- Multiplayer networking
- Interactive gameplay save states (demo playback is stateless)
- Custom PCB / housing design
- Music streaming from external source (Duke3D MIDI/OPL from GRP only)

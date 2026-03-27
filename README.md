# Duke3D Matrix ESPHome

Duke Nukem 3D running on a 64×64 HUB75 LED matrix, integrated into Home Assistant via ESPHome.

Inspired by [bestander/duke3d-matrix](https://github.com/bestander/duke3d-matrix) — a Raspberry Pi version of the same idea. This project reimplements it entirely on an ESP32-S3 microcontroller so it can run standalone, without a Pi, and connect directly to Home Assistant.

---

## What it does

- Plays the Duke Nukem 3D demo loop continuously on a 64×64 RGB LED matrix
- Overlays a live weather + tide HUD on the bottom 24 rows (pulled from Home Assistant)
- Managed by ESPHome: OTA updates, API, WiFi, sensor subscriptions
- Audio output via I2S amplifier (MAX98357A)

![Display layout](docs/display-layout.png)

### Display layout

```
┌────────────────────────────────────────────┐
│                                            │
│         Duke3D rendered scene              │  rows 0-39
│         (320×200 → scaled to 64×40)        │
│                                            │
├────────────────────────────────────────────┤
│  20:31          ~18°C                      │  row 40-47  clock | water temp
│  ↓18°  ↑19°                               │  row 48-55  air temp range
│  H --:--   L 21:53                        │  row 56-63  tide times
└────────────────────────────────────────────┘
```

---

## Hardware

| Part | Details |
|------|---------|
| Board | [Adafruit Matrix Portal S3](https://www.adafruit.com/product/5778) |
| SoC | ESP32-S3, dual-core 240 MHz, 8 MB flash, 2 MB Octal PSRAM |
| Display | Two 64×32 HUB75 panels chained as a 64×64 display |
| Storage | MicroSD card (SPI via Matrix Portal S3 GPIO strip) |
| Audio | MAX98357A I2S amplifier module |
| Power | 5V via USB-C or barrel jack |

### GPIO assignment (Matrix Portal S3)

| Signal | GPIO | Connector |
|--------|------|-----------|
| SD MOSI | 3 | A1 |
| SD MISO | 9 | A2 |
| SD CLK | 10 | A3 |
| SD CS | 11 | A4 |
| I2S BCLK | 12 | A0 (JST) |
| I2S LRCLK | 8 | RX |
| I2S DIN | 18 | TX |

HUB75 data/control lines are wired directly through the Matrix Portal S3's built-in HUB75 connector.

---

## Architecture

```
ESPHome (Core 0)                    Duke3D engine (Core 1)
──────────────────────              ───────────────────────────────────
WiFi / HA API                       duke3d_main()
Sensor subscriptions                  └─ drawrooms() / game loop
HUD data updates                      └─ SDL_Flip()
OTA                                       └─ spi_lcd_send_boarder()
                                              └─ render_frame()   320×200 → 64×40
                                              └─ hud.render()     rows 40-63
                                              └─ hub75.swap_buffers()
```

The Duke3D engine runs as a dedicated FreeRTOS task pinned to Core 1 with a 64 KB PSRAM stack. Core 0 runs the standard ESPHome loop. The HUD is written by Core 1 per-frame; sensor data flows from Core 0 via a mutex-protected struct.

### ESPHome components

| Component | Role |
|-----------|------|
| `hub75_matrix` | Drives the two-panel HUB75 chain; exposes `set_pixel` / `swap_buffers` |
| `hud` | Renders clock, temperatures, tide times into rows 40–63 |
| `duke3d` | Starts the engine task; exposes `current_demo()` as a text sensor |
| `sd_card` | Mounts the FAT SD card at `/sdcard`; checks for `DUKE3D.GRP` |
| `i2s_audio` | Streams PCM audio to the MAX98357A amplifier |

---

## SD card setup

```
/sdcard/duke3d/
  DUKE3D.GRP    ← required game data file
  GAME.CON      ← optional (uses built-in if absent)
  USER.CON      ← optional
```

The game data is from Duke Nukem 3D Shareware 1.3 (free) or the full Atomic Edition (v1.5 or lower). The full game can be purchased at [zoom-platform.com](https://www.zoom-platform.com/#store-duke-nukem-3d-atomic-edition).

---

## Building and flashing

### Prerequisites

- [ESPHome](https://esphome.io) 2024.11+
- PlatformIO (installed automatically by ESPHome)
- A `secrets.yaml` (copy from `secrets.yaml.template` and fill in)

### Clone with submodules

```bash
git clone --recurse-submodules https://github.com/bestander/duke3d-matrix-esphome.git
cd duke3d-matrix-esphome
cp secrets.yaml.template secrets.yaml
# edit secrets.yaml
```

### Flash

```bash
esphome run esphome.yaml
```

OTA updates work after the first flash.

---

## Memory layout

The ESP32-S3's 2 MB PSRAM is split between the PSRAM BSS segment (static arrays) and the PSRAM heap (tile cache + dynamic allocs). Getting everything to fit required reducing several engine limits compared to the original jkirsons/Duke3D port (which targets 4 MB PSRAM):

| Limit | Upstream | This fork | PSRAM BSS saved |
|-------|----------|-----------|-----------------|
| MAXSPRITES | 4096 | 2048 | −248 KB |
| MAXWALLS | 8192 | 4096 | −128 KB |
| MAXWALLSB | 2048 | 1024 | −66 KB |

After reductions, the PSRAM heap pool sits at ~540 KB, of which ~393 KB goes to the tile cache.

| Region | Usage |
|--------|-------|
| Internal DRAM | ~128 KB used (39.7% of 320 KB) — WiFi, DMA, FreeRTOS |
| PSRAM BSS | ~1.4 MB — engine arrays (hittype, sprite, wall, sector, etc.) |
| PSRAM heap | ~540 KB pool: 393 KB tile cache + 64 KB task stack + palettes |

---

## Submodule forks

Both library submodules are pinned to forks with fixes needed for this specific target:

| Submodule | Fork | Change |
|-----------|------|--------|
| Duke3D engine | [bestander/Duke3D](https://github.com/bestander/Duke3D) | 2MB PSRAM adaptations, ESP32-S3 alloc fixes, SD path |
| HUB75 library | [bestander/ESP32-HUB75-MatrixPanel-DMA](https://github.com/bestander/ESP32-HUB75-MatrixPanel-DMA) | Fix `gdma_transfer_config_t` version guard (IDF 5.1→5.3) |

The HUB75 fix has been submitted as a PR to the upstream library.

---

## Home Assistant sensors

Configure entity IDs in `esphome.yaml` to match your HA setup:

| Sensor | Entity | Description |
|--------|--------|-------------|
| `sensor.outdoor_temperature_low` | Air temp min | 8-hour forecast low |
| `sensor.outdoor_temperature_high` | Air temp max | 8-hour forecast high |
| `sensor.water_temperature` | Water temp | Current water temperature |
| `sensor.high_tide_time` | High tide | Format `HH:MM` |
| `sensor.low_tide_time` | Low tide | Format `HH:MM` |

---

## Legal

"Duke Nukem" is a registered trademark of Apogee Software, Ltd. (3D Realms).
"Duke Nukem 3D" copyright 1996–2003 3D Realms. All trademarks and copyrights reserved.
Game data files are not included and must be obtained separately.

The engine code is based on [jkirsons/Duke3D](https://github.com/jkirsons/Duke3D) which is based on [fabiensanglard/chocolate_duke3D](https://github.com/fabiensanglard/chocolate_duke3D). See individual component licenses in the engine submodule.

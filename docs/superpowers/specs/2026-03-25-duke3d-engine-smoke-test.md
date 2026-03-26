# Duke3D Engine-Dependent Smoke Test (No GRP)

**Date:** 2026-03-25  
**Status:** Draft (implementation in progress)

## Goal

Provide a `duke3d.smoke_test` mode that:

- Builds and links against **real Duke3D engine code** in `components/duke3d/engine/`
- Runs without `DUKE3D.GRP` / SD card
- Renders an obvious animated test pattern into the **top 64×40** game area while leaving the bottom **64×24 HUD** intact

## Non-Goals

- Running the full Duke3D engine loop without assets
- Supporting demo playback without GRP
- Exercising the engine's original `main()` or SDL/SPI-LCD subsystem

## Design

### Configuration

In `esphome.yaml`:

```yaml
duke3d:
  id: game_engine
  smoke_test: true
```

Default is `false` (normal behavior).

### Runtime Behavior

When `smoke_test: true`:

- `Duke3DComponent` starts a dedicated FreeRTOS task on Core 1.
- Each frame:
  - Calls a **real engine function** `clearbufbyte()` (from `engine/components/Engine/fixedPoint_math.c`) to mutate a 320×200 8-bit framebuffer.
  - Draws simple moving shapes into the framebuffer.
  - Calls `platform_blit_frame(fb, pal)` which:
    - Downscales to 64×40 (rows 0–39)
    - Calls `Hud::render()` for rows 40–63
    - Swaps buffers to the HUB75 panel

When `smoke_test: false`:

- Existing behavior remains: require GRP on SD and run the demo loop.

### Why this proves "engine dependency"

The smoke loop executes object code that originates from the Duke engine tree, demonstrating:

- Engine headers compile under ESPHome/ESP-IDF
- Engine C objects link successfully
- Engine routines can run in the live firmware task and participate in the render loop

## Success Criteria

- `esphome compile esphome.yaml` succeeds with `smoke_test: true`
- Device boots without SD/GRP and shows a stable animated pattern in the top zone
- HUD remains visible in bottom 24 pixels


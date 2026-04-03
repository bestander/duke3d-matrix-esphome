# Plan: OPL2 music emulation (ESP32-S3 / ESPHome Duke3D)

## Goal

Play Duke Nukem 3D **background music from the GRP** (e.g. `GRABBAG.HMI`) without **pre-rendered `.raw` files** on SD. Target approach: **software OPL2** (Yamaha YM3812) driven by the game’s **AdLib/OPL music path**, mixed into the existing **11025 Hz mono** I2S pipeline alongside Multivoc SFX (same stage as `platform_audio_write` today).

## Why this instead of SD PCM

- Music in the GRP is **HMI / MIDI-style data**, not PCM; desktop builds use SDL_mixer or AdLib hardware.
- A port such as [next-hack/MGM240_DukeNukem3D](https://github.com/next-hack/MGM240_DukeNukem3D) already ships **OPL2 emulation** on a constrained MCU; ESP32-S3 has more headroom but shares constraints (CPU vs game + display + SD).

## Current codebase anchors

| Piece | Location / notes |
|--------|------------------|
| Music today | `components/duke3d/audiolib_stubs.c` — no-op `MUSIC_*` / `PlayMusic`; game launched with **`/nm`**. |
| Mix site | `dsl.c` — Multivoc `MV_ServiceVoc()` then `platform_audio_write` (I2S). OPL output would mix **here** before I2S. |
| AdLib MIDI (DOS) | `components/duke3d/engine/components/audiolib/al_midi.c` — OPL2/OPL3 register logic tied to **port I/O** (not in ESP build). |
| GRP file access | `kopen4load` / `kread` in `filesystem.c` — can load lumps by 12-char name |
| Volume / scaling | `MusicVolume` (in-game); **`platform_set_audio_output_percent`** scales **all** I2S output today. |

## High-level architecture

1. **OPL2 core** — Pick a **portable, license-compatible** emulator (e.g. Nuked OPL3 in OPL2 mode, DosBox OPL, `emu8950`, etc.). Criteria: **deterministic**, **no heap churn** in the render hot path, acceptable **CPU at ~11 kHz mono output** (or slightly higher internal rate + decimation).
2. **Register backend** — Replace DOS `outp(0x388, …)` in the AdLib path with **writes to the emulated chip** (same register semantics as `al_midi.c` expects).
3. **Music loader** — On `MUSIC_PlaySong` / `PlayMusic`: `kopen4load` the requested lump (e.g. `GRABBAG.HMI`) into **PSRAM** (or stream if size is huge—measure shareware vs atomic GRP).
4. **Sequencer** — Either:
   - **Reuse** Chocolate’s HMI → AdLib driver stack with a **software OPL** backend, or
   - **Port** a minimal path from a reference (e.g. MGM240 fork) that already feeds OPL2 from Duke music files.
5. **Audio glue** — Each audio tick, advance the sequencer (if time-based) and **OPL2-generate** `n` mono samples, then **sum** into the int16 buffer **before** `platform_audio_write` (respect `MusicVolume`; optional separate music trim in HAL if needed).
6. **Threading** — Music state vs game task vs `dsl.c` audio task: avoid long critical sections during OPL render.

## Phases (suggested order)

### Phase 0 — Spike (offline or on-device test task)

- Build **OPL2 core only** in isolation: fixed register smoke test → sine-like output.
- Measure **MCycles/sample** on ESP32-S3 @ 240 MHz at **11.025 kHz** output.

### Phase 1 — PCM bridge

- Implement `opl2_render(int16_t *mono, int n_samples)` and feed **silence** or **test tone** from I2S callback to validate CPU budget **with game running**.

### Phase 2 — Load lump from GRP

- Add a small helper: given `"GRABBAG.HMI"`, `kopen4load` + read into PSRAM; **no playback** yet.

### Phase 3 — Wire AdLib MIDI to emulated OPL2

- Map `al_midi.c` (or subset) **hardware I/O** to **emu register writes**.
- Confirm **one track** (menu / Grabbag) produces non-silent PCM.

### Phase 4 — Integrate with `MUSIC_*` API

- Replace **`audiolib_stubs.c` music stubs** (or add a new `esp_opl_music.c`) so `MUSIC_PlaySong` / `PlayMusic` drive the **OPL path** instead of no-ops; stop relying on **`/nm`** for music-off (or keep `/nm` as a user toggle).
- Optional: ESPHome `duke3d` flag to enable OPL music vs silent stubs.

### Phase 5 — Hardening

- **Loop** semantics match original `PlayMusic` loop flag.
- **Stop / pause / volume** match existing `MusicVolume` behavior.
- **Flash/RAM budget** documented; optional **instrument bank** in flash vs GRP.

## Risks and mitigations

| Risk | Mitigation |
|------|------------|
| CPU starvation (FPS drops) | Lower internal OPL rate + quality tradeoff; render music every N samples; profile with Hub75 + SD |
| RAM for full HMI in memory | Stream parse or cap supported file size; compare lump sizes in shareware vs atomic |
| License of OPL core | Choose core with **GPL-compatible** or **permissive** license matching the project |
| HMI quirks | Use MGM240 or Chocolate reference behavior; add golden-ear compare on PC first |

## References

- [MGM240_DukeNukem3D](https://github.com/next-hack/MGM240_DukeNukem3D) — OPL2 music on embedded Chocolate Duke port.
- [next-hack article](https://next-hack.com/index.php/2025/11/14/duke-nukem-3d-on-the-arduino-nano-matter-board-only-256-kb-ram/) — implementation context.
- Internal: `docs/music-streaming-proposal.md` (PCM-on-SD design note), `al_midi.c`, `audiolib_stubs.c`.

## Open decisions

- Which **OPL2 core** to vendor (size vs accuracy).
- Whether to keep **`.raw` fallback** permanently for CI or demos without GRP music parsing.
- **Stereo vs mono**: hardware is mono; OPL stereo image can be **downmixed** early.

---

*Status: shelved — SD PCM stream (`esp_music_stream`) was removed; music is stubbed again until OPL2. Last updated: 2026-04-03.*

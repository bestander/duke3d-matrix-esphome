# Demo recording on Start press (rotating demo0–9.dmo)

> **Status:** Design approved 2026-05-29. Next step: implementation plan (writing-plans).
> **Approach:** A — the engine *pulls* the demo write path from an ESP hook at `opendemowrite()` time.

## 1. Goal

When the hardware **Start** button is pressed and the resulting **live game session** begins,
record that session to a `.dmo` file on the SD card. Recorded files use the engine's native
names `demo0.dmo … demo9.dmo` and rotate across those 10 slots, replacing the **oldest** file
once all 10 exist. Recorded demos automatically join the attract/kiosk playback pool.

### Captured product decisions

1. **What is recorded:** the *existing* live Start session, unchanged — Episode 1, **random**
   level, easiest skill (`/v1 /lN /s0`). We only add recording to it.
2. **Naming + pool:** native `demo0.dmo`–`demo9.dmo`; they feed back into the attract loop
   (`pick_random_demo_dmo` already globs `*.dmo`), so the box replays the player's own sessions.
3. **Rotation:** fill the **lowest empty** index first; once all 10 exist, overwrite the file
   with the **oldest modification time** (ties → lowest index).
4. **Finalization:** *all* endings save a playable demo — level-complete, manual quit, the
   10-s-after-death auto-restart, and pressing Start again mid-session.

### Non-goals

- Changing the live-play warp itself (still random E1 level, easiest skill).
- Changing the cold-boot attract behavior (still random `.dmo` playback).
- Multiplayer / multi-mode recording.
- Any UI for browsing/exporting recordings.

## 2. Current behavior (verified against code)

- **Start press** → latch polled per frame in `spi_lcd_send_boarder` (`esp32_hal.cpp`) →
  `duke_jump_out_with_reason(DUKE_EXIT_START_PLAY_E1L1)` → `game_task` sets `run_live_next`
  and runs `argv_live` = `duke3d -game_dir /sdcard/duke3d /nm /v1 /lN /s0` (**no `/r`**).
- **Engine recording** (`/r` → `ud.m_recstat = 1`, `game.c` case `'r'`): at `enterlevel`
  (`premap.c`) `ud.recstat = ud.m_recstat`, then `if (ud.recstat==1 && !MODE_RESTART) opendemowrite()`.
- **`opendemowrite()`** (`game.c:8761`) is **broken on this device**: it hardcodes `demo1.dmo`
  and builds the path as `sprintf(buf, "%s\\%s", getGameDir(), d)` into a **16-byte** buffer
  (`/sdcard/duke3d\demo1.dmo` is 24 chars → overflow; `\` is the wrong separator). Engine-native
  recording has effectively never run here.
- **`closedemowrite()`** (`game.c:8835`) finalizes the header (`totalreccnt` at offset 0). Called
  on `MODE_EOL` (`game.c:8558`) and `gameexit` (`game.c:2427`). It also calls the existing hook
  `dukesp_maybe_jump_after_demo_write_closed()`.
- **Attract loop** (`duke3d_component.cpp` `game_task`): `pick_random_demo_dmo()` globs `*.dmo`
  on SD and plays a random one via `/d<NAME>`.
- **Death kiosk reset:** `dukesp_player_death_tick()` (`engine_main_shim.c`) calls `esp_restart()`
  ~10 s after the live player dies — a hard reboot that currently bypasses `closedemowrite()`.
- **Reload exit:** `duke_jump_out_with_reason()` → `longjmp` → shim `setjmp` block runs
  `SoundShutdown()/Shutdown()/uninitgroupfile()` — also bypasses `closedemowrite()`.
- **No `get_fattime` override** exists in the project; FatFs timestamp validity is unverified.

## 3. Architecture

### 3.1 New unit: `demo_recorder` (`components/duke3d/demo_recorder.{h,cpp}`)

Owns all rotation logic. No engine dependencies (depends only on `stat`/`fopen` against
`/sdcard/duke3d`). Public surface:

```c
// Returns true and fills `buf` with the full path of the slot to record into
// (e.g. "/sdcard/duke3d/demo3.dmo"); false if recording is not armed.
bool demo_recorder_next_path(char* buf, size_t len);

// Arm/disarm recording. Armed only for the live Start run; demo/attract runs are never armed.
void demo_recorder_arm(bool on);
```

**Slot selection (`demo_recorder_next_path`):**
1. For `i` in `0..9`, `stat("/sdcard/duke3d/demo<i>.dmo")`.
2. If any index is missing → choose the **lowest missing** index.
3. Else → choose the index with the **oldest `st_mtime`**; ties → lowest index.
4. **Fallback** (mtimes unreliable — all equal or at the 1980 epoch): use a persisted
   round-robin counter in `/sdcard/duke3d/demorec.idx` (single byte/int, `0..9`), incremented
   modulo 10 each write.

`demo_recorder_arm` is set `true` by `game_task` right before launching the live run and `false`
after it returns (so demo playback runs never record).

### 3.2 Engine seam (the single `game.c` edit)

New C hook in `dukesp_hooks.h`, implemented in `engine_main_shim.c` (delegates to `demo_recorder`):

```c
// Fill `buf` with the demo write path; return nonzero if recording is armed (use the path),
// zero to fall back to default behavior.
int dukesp_demo_write_path(char* buf, size_t len);
```

`opendemowrite()` is changed to:
```c
char fullpathdemofilename[64];          // was 16 — overflow fix
if (dukesp_demo_write_path(fullpathdemofilename, sizeof fullpathdemofilename)) {
    // armed: use the rotated path as-is
} else if (getGameDir()[0] != '\0') {
    snprintf(fullpathdemofilename, sizeof fullpathdemofilename, "%s/%s", getGameDir(), d); // '/' fix
} else {
    snprintf(fullpathdemofilename, sizeof fullpathdemofilename, "%s", d);
}
```
Behavior-preserving when the hook declines; the buffer/separator bug is repaired either way.

## 4. Data flow

```
Pico Start latch → spi_lcd_send_boarder → duke_jump_out_with_reason(DUKE_EXIT_START_PLAY_E1L1)
game_task: run_live_next = true
   → demo_recorder_arm(true)
   → argv_live gains "/r"   (duke3d -game_dir /sdcard/duke3d /nm /v1 /lN /s0 /r)
   → duke3d_main(argv_live)
engine: enterlevel → ud.recstat==1 → opendemowrite()
        → dukesp_demo_write_path() → demo_recorder_next_path() → /sdcard/duke3d/demoN.dmo
        → record() per tic until closedemowrite()
after run: demo_recorder_arm(false)
attract: pick_random_demo_dmo() globs *.dmo (now includes demoN.dmo) → /d<NAME>
```

`argv_live` grows from 7 to 8 args (plus the `nullptr`); `argc_run` updated to match.

## 5. Finalization on every ending

`closedemowrite()` must run (guarded by `if (ud.recstat == 1)`) on the two abrupt paths it
currently misses:

1. **Any `longjmp` exit (Start pressed again, etc.):** in `engine_main_shim.c`, inside the
   `setjmp`-return block, call `closedemowrite()` **before** `SoundShutdown()/Shutdown()`. The
   display mutex is already released there (`SDL_ReleaseDisplayMutexIfHeld()` ran in
   `duke_jump_out_with_reason`), so `closedemowrite`'s internal `SDL_LockDisplay()` is safe.
2. **Death auto-restart:** in `dukesp_player_death_tick()`, call `closedemowrite()` immediately
   before `esp_restart()`.

Natural endings (`MODE_EOL`, `gameexit`) already finalize — unchanged. Because the file is named
correctly at open time (Approach A), every path yields a correctly-named, finalized, playable
`demoN.dmo` — no post-hoc rename needed.

## 6. Configuration

Add a boolean `record_demos` to the `duke3d:` schema (`components/duke3d/__init__.py`), default
`true`. When `false`, `game_task` never arms the recorder and never appends `/r` (the feature
compiles in but is dormant). Surface it in `esphome.yaml` with a comment.

## 7. File touch list

| File | Change |
|------|--------|
| `components/duke3d/demo_recorder.h` / `.cpp` | **New** rotation unit (slot selection + mtime/round-robin fallback). |
| `components/duke3d/dukesp_hooks.h` | Declare `int dukesp_demo_write_path(char*, size_t)`. |
| `components/duke3d/engine_main_shim.c` | Implement the hook (delegate to `demo_recorder`); add `closedemowrite()` to the `setjmp`-return block; add `closedemowrite()` before `esp_restart()` in `dukesp_player_death_tick`. |
| `components/duke3d/engine/components/Game/game.c` | `opendemowrite()` uses the hook; fix buffer size + `/` separator. |
| `components/duke3d/duke3d_component.cpp` | Arm/disarm recorder around the live run; append `/r` to `argv_live`; bump `argc_run`. |
| `components/duke3d/duke3d_component.h` / `__init__.py` | `record_demos` config flag + setter. |
| `esphome.yaml` | Document `record_demos`. |
| `funct.h` | (If needed) ensure `closedemowrite` is declared for the shim TU. |

## 8. Risks / watchouts

| # | Risk | Mitigation |
|---|------|-----------|
| R1 | **Case-insensitive FatFs:** `demo1.dmo` aliases shipped `DEMO1.DMO`. | Intended: fill-empty counts shipped files as present; oldest-mtime overwrites them first (1980 mtimes). Recordings gradually replace shipped demos — consistent with "join the attract pool". |
| R2 | **mtime unreliable** (no `get_fattime` override; WiFi/time may be off mid-game). | Fall back to persisted round-robin `/sdcard/duke3d/demorec.idx` when mtimes are equal/at-epoch. Verify/wire `get_fattime` to system time during implementation. |
| R3 | **`closedemowrite` re-entrancy / lock order** on the longjmp path. | Insert finalize only after `SDL_ReleaseDisplayMutexIfHeld()` (already done in `duke_jump_out_with_reason`); guard with `ud.recstat==1`; `closedemowrite` does its own lock/unlock. |
| R4 | **Death-restart timing:** `esp_restart()` truncates I/O. | `closedemowrite()` `fseek`+`fwrite`+`fclose` completes before `esp_restart()`; finalize is a few small writes. |
| R5 | **SD contention / `fopen` failure** during gameplay. | Skip recording silently (engine's existing early-return contract); game continues. |
| R6 | **`argv_live` arg-count drift.** | Update `argc_run` (7→8) wherever `argv_live` is launched; keep the `nullptr` terminator. |
| R7 | **Recorder armed leaking into a demo run** would record over demo playback. | `demo_recorder_arm(false)` after the live run returns; only the `run_live_next` branch arms. |

## 9. Test checklist

- [ ] Start press → live E1 level → on quit, `/sdcard/duke3d/demoN.dmo` exists and is **playable**
      back in the attract loop (no header corruption; `totalreccnt` nonzero).
- [ ] First 10 recordings fill `demo0..demo9` in lowest-empty order (minus any pre-existing).
- [ ] 11th recording overwrites the oldest by mtime (or round-robin if mtimes unreliable).
- [ ] **Death path:** die, wait 10 s → device restarts AND the in-progress demo is finalized/playable.
- [ ] **Re-press path:** press Start again mid-level → reload AND the partial demo is finalized/playable.
- [ ] `record_demos: false` → no `/r`, no files written, live play unaffected.
- [ ] Repeated record→play cycles (~10×) keep PSRAM stable (no leak from the added I/O).
- [ ] Non-armed demo/attract runs never create or modify `demoN.dmo`.

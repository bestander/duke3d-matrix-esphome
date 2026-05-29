# Start button → soft reboot → Episode 1 Level 1 (playable)

> **Implementation status (verified against code, May 2026):** Phases 1–2 are **already shipped**.
> `DUKE_EXIT_START_PLAY_E1L1` (`duke_reload.h`), `argv_live` + `run_live_e1l1_next` (`duke3d_component.cpp`),
> the per‑frame latch poll in `spi_lcd_send_boarder` (`esp32_hal.cpp`), and the `play_live` action / schema
> (`__init__.py`, `esphome.yaml`, `pico_uart_bridge_maps.h`) all exist. Read §8 for **verified** risks before
> editing — several original plan assumptions were proven wrong by the code (e.g. the skill is **0‑based**, so
> easiest is `/s0`, now fixed in `argv_live`). When the plan and the code disagree, trust the code.

This document describes the **target behavior**, how the **existing stack** implements “soft reboot”, and a **phased implementation plan** so we avoid ad‑hoc guesses.

**Captured product decisions**

1. **Cold boot:** random `.dmo` demo (existing `pick_random_demo_dmo`).
2. **After playable session normal exit** (`main` returns 0): resume random demo kiosk (`game_task` loop `continue`).
3. **Difficulty warp:** **Piece of Cake / easiest = `/s0`** (shipped). `atol(c)%5` has **no `-1`** (`game.c` case `'s'`), so the number *is* the 0‑based skill: `/s0` = easiest, `/s1` = Let's Rock. See §3/§8 R1.

---

## 1. Goals and non‑goals

### Goals

- **Hardware Start** (Pico UART: vendor **`GR,...`** bitmask → logical `"start"` in `input.cpp`, not Duke keyboard yet) triggers a **cooperative reload** of the Duke engine **`duke3d_main()`**.
- Immediately after reload, Duke **enters a real playable game session** — **Episode 1, Level 1** (warp), **not** demo playback (`/d…`), **not** demo recording (`/r`), **not** random demo picker.
- **HID keyboard Escape** handling stays consistent with today: firmware still **drops** KB Escape when **`start`** is configured for “alternate meaning” (`PICO_UART_BRIDGE_DROP_HID_KEYBOARD_ESCAPE`), so pads that pretend Start==ESC keep working.

### Non‑goals (explicitly defer)

- **Random demo kiosk**, **recording session `/er`**, **`pick_random_demo_dmo` UX** — out of scope for this pass except where we replace or detach the legacy branch.
- **HA / WiFi** policy during playable session (only note interactions if any).
- **Renaming Pico UF2 protocol** — none required.

---

## 2. Current architecture (as of repo)

```
Pico HID host → UART line → ESP input.cpp
    vendor GR bitmask edge → logical name "start" (among others)

When YAML maps start → record_session:
    - pico_uart_bridge_maps.h: #define PICO_UART_BRIDGE_DROP_HID_KEYBOARD_ESCAPE 1
    - input.cpp (vendor + KB paths): latch s_pico_uart_start_press_requested
    - esp32_hal.cpp spi_lcd_send_boarder(): if latch → duke_jump_out_with_reason(DUKE_EXIT_START_RECORD_SESSION)

engine_main_shim.c:
    duke3d_main() arms setjmp; duke_jump_out_with_reason(reason) → longjmp
    → Shutdown(); uninitgroupfile(); return reason (never reaches main()'s tail)

duke3d_component.cpp game_task():
    for (;;):
      build argv → duke3d_main(...)
      rc == DUKE_EXIT_START_RECORD_SESSION → kiosk_record_boot = true → next loop uses argv_record ( /r … /er )
      rc == DUKE_EXIT_RECORDING_SESSION_DONE → continue (back to demo argv)
      else → break (task exits)
```

**Soft reboot today** = **`longjmp` + `Shutdown` + heap/file teardown + immediate next `duke3d_main()`** in **`game_task`**. Not `esp_restart()`.

---

## 3. Engine command‑line contract (from `game.c`)

Relevant switches (see **`comlinehelp()`** ~7060):

| Flag | Effect |
|------|--------|
| **`-game_dir PATH`** | Data directory (already used) |
| **`/nm`** | No music (already used kiosk‑style) |
| **`/v#`** | Volume/ep **1–4**. Parser sets **`ud.warp_on = 1`** and **`ud.volume_number = # - 1`**. **`/v1`** → Episode 1. |
| **`/l##`** | Level **1–11**. Sets **`warp_on = 1`** and **`level_number = atol(c)-1`**. **`/l1`** → first map of episode. |
| **`/s#`** | Skill = **`atol(c)%5`** with **NO `-1`** (`game.c:7400`, unlike `/v` and `/l`). So the number **is** the 0‑based `ud.player_skill`: **`/s0` = Piece of Cake (easiest)**, `/s1` = Let's Rock, `/s2` = Come Get Some (the omit‑default, `game.c:7102`), `/s3` = Damn I'm Good, `/s4` = +respawn monsters (`game.c:7401‑7402`). Monster spawn gate is `sp->lotag > ud.player_skill` → **higher number = more monsters = harder**. The earlier "avoid `/s0`" note was **wrong**: `/s0` is the correct value for easiest. |
| **`/dFILE`** | Demo **playback** — **avoid** on the “live play” path. |
| **`/r`** | Demo **recording** — **avoid** unless we revisit recording kiosk. |
| **`/er`** | ESP hook **`dukesp_set_kiosk_demo_record(1)`** — only relevant with **`closedemowrite()`** kiosk exit — **avoid** unless recording returns. |

**Warp path:** when **`warp_on == 1`** **after startup**, **`main()`** skips **`Logo()`** and runs **`newgame(volume, level, skill)`** then **`enterlevel(MODE_GAME)`** (~8448–8491).

**Contrast:** **`/v` alone or `/l` alone** each set **`warp_on = 1`**, but the comment implies **both** should be supplied for sane warps; **`argv_record`** already passes **`/v1`** and **`/l1`** alongside **`/r`**.

---

## 4. Target behavior specification

### 4.1 Single source of truth

- **Cold boot**: keep **playing a demo first** unchanged *for Phase 1* **or** switch cold boot straight to **`/v1/l1`** if product decision says so — **decide explicitly in Phase 0** before coding.

### 4.2 On Start press during demo (or splash)

1. Cooperative exit with a **distinct return code** (rename **`DUKE_EXIT_START_RECORD_SESSION`** or add **`DUKE_EXIT_PLAY_E1L1`** and deprecate duplicate semantics — see §6).
2. **`game_task` next iteration** uses **`argv_play`** resembling:

```
duke3d -game_dir /sdcard/duke3d /nm /v1 /l1 /s<X>
```

   - **No** `/d…`, **no** `/r`, **no** `/er` unless we re‑introduce recording later.
3. After **`duke3d_main`** returns **`0`** (normal quit), **current code `break`s and deletes `game_task`**. Decide one of:

   **A.** Accept that **quitting Duke ends the firmware game thread** until full device reboot/recreate component (minimal change).  

   **B.** Treat **`rc == 0`** as **`continue`** restoring **demo argv** idle loop (**needs product sign‑off**).

Document the choice in Implementation Phase 3.

---

## 5. File‑level touch list (expected)

| File | Change |
|------|--------|
| **`duke_reload.h`** | New/renamed exit reason constants; clarify comments (remove “recording session only” implication if Start means play). |
| **`components/duke3d/duke3d_component.cpp`** | Replace **`kiosk_record_boot`/`argv_record`** branch with **`argv_play`** (ep1 L1 warp). Optionally remove **`pick_random_demo_dmo`** from **Start-triggered path only** — **not necessarily** cold boot depending on Phase 0. Clean up **`DUKE_EXIT_RECORDING_SESSION_DONE`** if `/er` path unused temporarily. |
| **`esp32_hal.cpp`** | Use new constant name once defined; **`#ifdef` / codegen flag** unchanged (still keyed off “special Start meaning”). |
| **`input.cpp` / `input.h`** | Optional rename **`s_pico_uart_start_press`** → neutral **`warm_reboot_requested`**; comment update. Functional behavior stays: **latch Start once**. |
| **`__init__.py` + YAML** | Rename **`record_session`** to something like **`warm_play`** or **`new_game`** for **`start`** so semantics match product; **`PICO_UART_BRIDGE_DROP_HID_ESC`** predicate must remain true whenever Start is repurposed (**not “escape Duke action”**). |
| **`pico_uart_bridge_maps.h`** | Regenerated **`esphome compile`** after YAML/schema change (**start** stays **`return -1`** in logical map unless we someday map Start directly to a KB scan — not needed). |

**Pico firmware:** no protocol change (`GR` bitmask for Start unchanged).

---

## 6. Phased rollout plan

### Phase 0 — Decide product defaults (≤15 min discussion)

1. **Cold boot:** demo playback **versus** warp straight into E1L1?  
2. **After playable session normal exit (`rc == 0`):** end task (**A**) or return to demo loop (**B**)?
3. **Skill:** **`/s1`** through **`/s4`** literal for Phase 2.

### Phase 1 — Mechanics only (minimal diff)

1. **`argv_play`** = `{ "duke3d", "-game_dir", "/sdcard/duke3d", "/nm", "/v1", "/l1", "/s2", nullptr }` (adjust skill per Phase 0).  
2. On **`DUKE_EXIT_START_*`**, **`kiosk_record_boot = false`**; use **`argv_play`** instead **`argv_record`**.  
3. **Remove**/disable **`DUKE_EXIT_RECORDING_SESSION_DONE`** branch **until** recording returns (or **`continue`** with **`argv_play`** if `/er` still fires — ideally it won’t).

**Verify manually:** Launch demo → press Start → **no** **`DEMO2.DMO` /playback** artifacts in log; **`newgame` path** enters **MOVEMENT** playable state.

### Phase 2 — YAML/schema clarity

1. Extend **`DUKE_GAMEPAD_ACTIONS`** with **`warm_play`** (or **`play_e1l1`**) **`record_session` validation** adjusts: only **`record_session`** allowed on **`start`** *if recording returns* — for now **`start` → warm_play**.  
2. Map **`warm_play`** to **same codegen** **`DROP_ESC`** **`#define`**.  
3. Update **`esphome.yaml`** comment block + regenerate header.

### Phase 3 — Lifecycle polish

1. If **B**, branch **`rc == 0`** → **`continue`** with **`argv_demo`** (or deterministic **`DEMO1.DMO`**).  
2. If **A**, add **log line** **`"Duke main returned %d — game task exiting"`** so field debug is obvious.  
3. Optional: unify exit reasons into an **enum‑like** **`uint8_t`** in **`duke_reload.h`** consumed by **`game_task` switch**.

---

## 7. Test checklist

- [ ] **Demo running** → **Start once** → **single** cooperative reload (**no panic**, **heap stable** across second **`Startup()`**)
- [ ] Spawn in **E1L1 playable** (**not demo camera**, **`ud.recstat != 2`**) — confirm in log: `Live play: warp E1L1 …` then `[duke3d_main] … enter argc=7 argv[1]=-game_dir`, and **no** `/d…` (`Random demo …`) line.
- [ ] ⚠️ **HID ESC is repurposed, not dropped:** with `start.action: play_live` a **real keyboard's Escape becomes a Start press** (reload), so it does **not** open the Duke menu. Verify a keyboard ESC triggers a reload, and that the **`star`** button (→ scancode `0x01`) is what opens/closes menus instead.
- [ ] **`star`** still maps **`escape`** (YAML) and reaches the menu mid‑game (this is the only menu‑ESC path while `play_live` is active) — regress after any schema regen.
- [ ] **Skill check:** confirm log shows `/v1 /l1 /s0` and E1L1 plays at *Piece of Cake* (sparsest enemy count). Regression guard: ensure nobody "fixes" `/s0` to `/s1`.
- [ ] **Repeated reloads:** press Start ~10× (demo→play→quit→demo…) and watch `heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)` stay flat (no fragmentation leak from `longjmp` teardown).

---

## 8. Risks / watchouts

These are **verified against the current code** (file:line where useful). The first two are real defects/foot‑guns, not hypotheticals.

### 8.1 Verified defects / surprising behavior

| # | Risk (what goes wrong) | Why | How to avoid / fix |
|---|------------------------|-----|--------------------|
| **R1** | **Skill number is 0‑based, easy to off‑by‑one.** Using `/s1` yields *Let's Rock*, not *Piece of Cake*. **Fixed:** `argv_live` now uses **`/s0`** (easiest). | `game.c:7400` does `ud.player_skill = atol(c)%5` with **no `-1`** (contrast `/v`,`/l` which subtract 1). Spawn gate `sp->lotag > ud.player_skill` ⇒ higher = harder. | Keep `/s0` for easiest. Don't "correct" it to `/s1` assuming 1‑based numbering. Comments updated in `duke3d_component.cpp`, `esphome.yaml`, §3. Re‑verify by enemy count in E1L1. |
| **R2** | **Real keyboard Escape can no longer open the Duke menu.** A USB keyboard's ESC triggers a *reload into E1L1* instead. | With `start.action: play_live`, `PICO_UART_BRIDGE_DROP_HID_KEYBOARD_ESCAPE=1`; `input.cpp` (`parse_kb_line` branch) converts **every** HID ESC into `pico_uart_note_start_press_uart("HID ESC")` (a Start latch), never a Duke ESC scancode. | This is **intended** for composite pads that emit Start as ESC — but it is a global trade‑off. Document that the **`star`** button (scancode `0x01`) is the only menu key while `play_live` is active. You cannot simultaneously use a real keyboard's ESC for menus *and* ESC‑emulating pads for Start. Fix the old §7 wording that implied ESC still reaches menus. |
| **R3** | **Pressing Start during live play restarts the level.** | The latch is polled **every frame** in `spi_lcd_send_boarder` (`esp32_hal.cpp:55`); a second press → `DUKE_EXIT_START_PLAY_E1L1` → `game_task` sets `run_live_e1l1_next` → reloads E1L1. | Acceptable as a "panic restart"; **document it**. If undesired, gate the latch on demo/game mode (e.g. only honor Start when `g_mode & DUKE3D_MODE_DEMO`) before calling `duke_jump_out_with_reason`. |
| **R4** | **Re‑binding `start.action` away from `play_live` silently disables Start entirely** — no warp *and* no key. | `DROP_*` becomes `0`, so the latch set **and** its poll in `spi_lcd_send_boarder`/`input.cpp` are `#if`‑compiled out; and `start` maps to scancode `-1` in `pico_uart_bridge_maps.h` (no Duke key). | Keep `start.action: play_live`. If Start should instead send a real Duke key, add an explicit scancode action in `DUKE_SCAN_BY_ACTION` and don't rely on the latch path. Consider a schema warning in `__init__.py` if `start` is neither special nor mapped. |

### 8.2 Lifecycle / memory / timing

| # | Risk | Why | How to avoid |
|---|------|-----|--------------|
| **R5** | **Reload crash + PSRAM leak (OBSERVED, partially fixed).** First Start press aborted with `BUFFER TOO BIG TO FIT IN CACHE` in `loadpalette`→`allocache`. Separately, PSRAM free at `initengine` dropped **358 KB (cold) → 158 KB (reload)** = ~200 KB leaked per run. | **Crash:** `uninitengine()` did `transluc = NULL`, but `transluc` is a *static* buffer pointer (`draw.c:transluc_storage`). The next `loadpalette()` then hit `if (transluc==NULL) allocache(...)` **before** `initcache()` runs (`cachesize==0`) → abort. **Leak:** `palookup[]` entries are `kkmalloc`'d (`engine.c:3592,8225`, 8 KB each) but `uninitengine` only NULLs them; the demo touches many palettes. | **Crash FIXED:** removed the `transluc = NULL;` line in `uninitengine` (`engine.c`) so the static pointer survives reloads; `loadpalette` re-reads the table into the existing buffer. **Leak (still open):** free the `kkmalloc`'d `palookup[i]` in `uninitengine` (bounds-check against `[pic, pic+cachesize)` so cache-resident ones aren't double-freed). Add the repeated-reload heap test (§7). |
| **R6** | **Latch poll moved out of the blit path would drop presses.** | `duke_jump_out_with_reason` is a **no‑op unless `duke_reload_armed`** (set inside `duke3d_main`, `engine_main_shim.c:37`). `input_take_pico_uart_start_press()` *clears* the latch via `exchange`. Polling while unarmed clears without acting. | Keep the poll inside `spi_lcd_send_boarder` / `platform_blit_frame`, which only run during `main()` (armed). A Start pressed **between** engine runs (during `tilecache_open` / 5 s splash) safely stays latched and fires on the next run's first frame. |
| **R7** | **`setjmp`/`longjmp` must stay on one task.** | Both `duke3d_main` (setjmp) and `spi_lcd_send_boarder` (longjmp) run on the **game task / Core 1**; the latch is *set* from the Pico UART task (Core 0) via an atomic. | Don't call `duke_jump_out_with_reason` from the UART task or `loop()`. Keep the cross‑core hand‑off to the `std::atomic<bool>` latch only. |
| **R8** | **WiFi‑window suspend delays a Start press.** | `vTaskSuspend(NULL)` in the blit path runs **after** the Start poll; while suspended for an HA sync the press waits, firing on resume. | Harmless with current YAML (`wifi_ha_sync: false` ⇒ no mid‑game window). If re‑enabled, expect up to one WiFi window of latency on a Start press. |
| **R9** | **`/warp_on` only partially set** skips the warp and lands in menus/Logo. | Both `/v` and `/l` each set `warp_on=1` (`game.c:7382,7394`); the warp branch (`~8447`) then runs `newgame`+`enterlevel`. | Always pass **`/v1` and `/l1` together** plus an explicit `/s#` — `argv_live` already does. Don't drop either. |
| **R10** | **Watchdog trip during a slow first level load.** | With `tile_cache: false` (current YAML) E1L1 loads tiles from flash/GRP; `esp_task_wdt_reset()` is only called once a frame renders in `spi_lcd_send_boarder`. | TWDT is 60 s (`CONFIG_ESP_TASK_WDT_TIMEOUT_S`) and INT_WDT 15 s — fine for L1. Watch logs if larger maps are ever warped to. |
| **R11** | **Dead `/er` branch.** `DUKE_EXIT_RECORDING_SESSION_DONE` is still handled but unreachable. | Nothing passes `/er`; `dukesp_reset_for_new_engine_run()` clears the kiosk flag each run (`engine_main_shim.c:78`). | Harmless. Remove only if cleaning up; keep if the recording kiosk may return. |
| **R12** | **SD busy teardown vs init race.** | `tilecache_close()` after each run, `tilecache_open()` before the next — ordering matters. | Preserve the `close` (post‑`duke3d_main`) / `open` (pre‑run) ordering already in `game_task`. |
| **R13** | **Reverb buffer OOM crash (OBSERVED, fixed).** Warping into a level whose sounds use reverb (e.g. E1L4) aborted with `StoreProhibited`, `EXCVADDR=0` in `check_buffer`→`MV_FPReverb` (audio pump task, Core 0). | `check_buffer()` did `malloc(~57 KB double[])` then `memset()` with **no NULL check**; under late-load PSRAM pressure the malloc returned NULL. Latent bug only reached now that warps actually load (R5 unblocked it). | **FIXED:** `mvreverb.c check_buffer()` now keeps the old buffer on `realloc` failure and leaves `delay=0` on first-alloc failure (logged once) so `MV_FPReverb()`'s existing `delay==0` guard skips reverb instead of crashing. NOTE: skipped reverb is a symptom of tight PSRAM late in level load — worth profiling `makepalookup`/sound cache growth if it recurs. |

### 8.3 Stale plan assumptions (now resolved in code — do not "re‑decide")

- **Phase 0‑B is already implemented:** `rc == 0` does **`continue`** back to the random‑demo kiosk (`duke3d_component.cpp:357`), not `break`. The §4.2/§9 "end task vs return to demo" question is settled to **B**.
- **Cold boot still plays a demo** (`pick_random_demo_dmo`), unchanged. §4.1 question is settled.
- The exit constant was **renamed** to `DUKE_EXIT_START_PLAY_E1L1`; `DUKE_EXIT_START_RECORD_SESSION` is now just a deprecated alias (`duke_reload.h:14`).

---

## 9. Open questions for you (quick answers unblock Phase 0)

1. ~~After the player exits Duke from **the in‑game Quit** path…~~ **Resolved in code:** returns to **random‑demo autoplay** (`rc == 0` → `continue`, `duke3d_component.cpp:357`).
2. ~~**Cold boot**: still **demo first**?~~ **Resolved in code:** yes, cold boot plays a random `.dmo`.
3. ~~**Default skill**~~ **Resolved in code:** `argv_live` uses **`/s0` = Piece of Cake (easiest)**. Parse is `atol(c)%5` with no `-1`, so the number is the 0‑based skill. (See R1.)

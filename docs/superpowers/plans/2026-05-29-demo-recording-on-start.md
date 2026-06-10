# Demo Recording On Start Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Record every live Start-triggered Duke3D session to rotating SD-card files `demo0.dmo` through `demo9.dmo`, then replay those files through the existing attract loop.

**Architecture:** Add a focused `demo_recorder` C++ unit with a C ABI for arming and path selection. The Duke engine's `opendemowrite()` pulls the write path through a `dukesp_demo_write_path()` hook, so the recording is named correctly at file creation time. `game_task` adds `/r` only for Start-triggered live runs when `record_demos` is enabled, and the shim finalizes recordings before abrupt restarts/reloads.

**Tech Stack:** ESPHome custom component, ESP-IDF 5.2.1, C/C++17, FatFs mounted at `/sdcard`, host-side CMake tests, ESPHome compile pipeline.

---

## File Map

| Action | File | Responsibility |
|---|---|---|
| Create | `components/duke3d/demo_recorder.h` | C ABI for recorder arming, slot selection helper, and rotated write-path generation. |
| Create | `components/duke3d/demo_recorder.cpp` | Slot scan, lowest-empty/oldest-mtime selection, unreliable-mtime round-robin fallback, path formatting. |
| Create | `test/duke3d/test_demo_recorder.cpp` | Host tests for slot selection and mtime fallback behavior. |
| Modify | `test/CMakeLists.txt` | Build the new host test with `demo_recorder.cpp`. |
| Modify | `components/duke3d/CMakeLists.txt` | Add `demo_recorder.cpp` to the ESP-IDF component sources. |
| Modify | `components/duke3d/dukesp_hooks.h` | Declare `dukesp_demo_write_path(char*, size_t)`. |
| Modify | `components/duke3d/engine_main_shim.c` | Implement the hook; finalize recordings before longjmp teardown and death `esp_restart()`. |
| Modify | `components/duke3d/engine/components/Game/game.c` | Fix `opendemowrite()` path construction and ask the ESP hook for the rotated path. |
| Modify | `components/duke3d/duke3d_component.h` | Add `record_demos_` member and setter. |
| Modify | `components/duke3d/duke3d_component.cpp` | Arm/disarm recorder around live runs; add `/r` to live argv when enabled; update arg count. |
| Modify | `components/duke3d/__init__.py` | Add `record_demos` schema option and codegen setter. |
| Modify | `esphome.yaml` | Document and enable `record_demos`. |

Git note: inspect `git diff` at each checkpoint. Do not create commits during execution unless the user explicitly asks.

---

## Task 1: Add Host-Tested Slot Selection

**Files:**
- Create: `components/duke3d/demo_recorder.h`
- Create: `components/duke3d/demo_recorder.cpp`
- Create: `test/duke3d/test_demo_recorder.cpp`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Write the failing host test**

Create `test/duke3d/test_demo_recorder.cpp`:

```cpp
#include <cassert>
#include <ctime>

#include "demo_recorder.h"

static DemoRecorderSlot empty_slot() {
    DemoRecorderSlot s{};
    s.exists = 0;
    s.mtime = 0;
    return s;
}

static DemoRecorderSlot existing_slot(long long mtime) {
    DemoRecorderSlot s{};
    s.exists = 1;
    s.mtime = mtime;
    return s;
}

static void test_lowest_missing_slot_wins() {
    DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT];
    for (int i = 0; i < DEMO_RECORDER_SLOT_COUNT; ++i)
        slots[i] = existing_slot(1000 + i);
    slots[3] = empty_slot();
    slots[7] = empty_slot();

    assert(demo_recorder_choose_slot(slots, 9) == 3);
}

static void test_oldest_mtime_wins_when_full() {
    DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT];
    for (int i = 0; i < DEMO_RECORDER_SLOT_COUNT; ++i)
        slots[i] = existing_slot(2000 + i);
    slots[6].mtime = 100;

    assert(demo_recorder_choose_slot(slots, 9) == 6);
}

static void test_ties_choose_lowest_index() {
    DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT];
    for (int i = 0; i < DEMO_RECORDER_SLOT_COUNT; ++i)
        slots[i] = existing_slot(2000);
    slots[4].mtime = 100;
    slots[8].mtime = 100;

    assert(demo_recorder_choose_slot(slots, 9) == 4);
}

static void test_equal_mtimes_use_fallback_slot_when_full() {
    DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT];
    for (int i = 0; i < DEMO_RECORDER_SLOT_COUNT; ++i)
        slots[i] = existing_slot(315532800);  // 1980-01-01 Unix time

    assert(demo_recorder_choose_slot(slots, 5) == 5);
}

int main() {
    test_lowest_missing_slot_wins();
    test_oldest_mtime_wins_when_full();
    test_ties_choose_lowest_index();
    test_equal_mtimes_use_fallback_slot_when_full();
    return 0;
}
```

- [ ] **Step 2: Register the test and verify it fails**

Modify `test/CMakeLists.txt`:

```cmake
add_executable(test_demo_recorder
    duke3d/test_demo_recorder.cpp
    ${CMAKE_SOURCE_DIR}/../components/duke3d/demo_recorder.cpp
)
target_include_directories(test_demo_recorder PRIVATE
    ${CMAKE_SOURCE_DIR}/../components/duke3d
)
target_compile_definitions(test_demo_recorder PRIVATE DUKE3D_HOST_TEST)
```

Run:

```bash
cmake -S test -B test/build && cmake --build test/build --target test_demo_recorder
```

Expected: FAIL because `components/duke3d/demo_recorder.h` does not exist yet.

- [ ] **Step 3: Add the recorder header**

Create `components/duke3d/demo_recorder.h`:

```cpp
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEMO_RECORDER_SLOT_COUNT 10

typedef struct {
    int exists;
    long long mtime;
} DemoRecorderSlot;

// Pure helper used by host tests and by the SD-card scanner.
// Returns a slot in [0, 9].
int demo_recorder_choose_slot(const DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT],
                              int fallback_slot);

// Returns nonzero and fills buf with /sdcard/duke3d/demoN.dmo when armed.
// Returns zero when not armed or when buf is too small.
int demo_recorder_next_path(char *buf, size_t len);

// C ABI because engine_main_shim.c calls this from C.
void demo_recorder_arm(int enabled);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 4: Add the minimal implementation for the pure helper**

Create `components/duke3d/demo_recorder.cpp`:

```cpp
#include "demo_recorder.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

namespace {

static int s_armed = 0;

static constexpr const char *kDemoDir = "/sdcard/duke3d";
static constexpr const char *kRoundRobinPath = "/sdcard/duke3d/demorec.idx";
static constexpr long long kFatEpoch1980UnixSeconds = 315532800LL;

bool all_slots_exist(const DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT]) {
    for (int i = 0; i < DEMO_RECORDER_SLOT_COUNT; ++i) {
        if (!slots[i].exists)
            return false;
    }
    return true;
}

bool mtimes_unreliable(const DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT]) {
    if (!all_slots_exist(slots))
        return false;

    long long min_mtime = slots[0].mtime;
    long long max_mtime = slots[0].mtime;
    for (int i = 1; i < DEMO_RECORDER_SLOT_COUNT; ++i) {
        if (slots[i].mtime < min_mtime)
            min_mtime = slots[i].mtime;
        if (slots[i].mtime > max_mtime)
            max_mtime = slots[i].mtime;
    }

    return min_mtime == max_mtime || max_mtime <= kFatEpoch1980UnixSeconds;
}

int clamp_slot(int slot) {
    if (slot < 0)
        return 0;
    if (slot >= DEMO_RECORDER_SLOT_COUNT)
        return slot % DEMO_RECORDER_SLOT_COUNT;
    return slot;
}

}  // namespace

int demo_recorder_choose_slot(const DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT],
                              int fallback_slot) {
    if (!slots)
        return 0;

    for (int i = 0; i < DEMO_RECORDER_SLOT_COUNT; ++i) {
        if (!slots[i].exists)
            return i;
    }

    if (mtimes_unreliable(slots))
        return clamp_slot(fallback_slot);

    int best = 0;
    for (int i = 1; i < DEMO_RECORDER_SLOT_COUNT; ++i) {
        if (slots[i].mtime < slots[best].mtime)
            best = i;
    }
    return best;
}

void demo_recorder_arm(int enabled) {
    s_armed = enabled ? 1 : 0;
}

int demo_recorder_next_path(char *buf, size_t len) {
    (void)buf;
    (void)len;
    return 0;
}
```

- [ ] **Step 5: Run the host test**

Run:

```bash
cmake -S test -B test/build && cmake --build test/build --target test_demo_recorder && ./test/build/test_demo_recorder
```

Expected: PASS.

---

## Task 2: Implement SD Slot Scanning And Round-Robin Fallback

**Files:**
- Modify: `components/duke3d/demo_recorder.cpp`
- Modify: `test/duke3d/test_demo_recorder.cpp`

- [ ] **Step 1: Add tests for fallback slot clamping**

Append to `test/duke3d/test_demo_recorder.cpp` before `main()`:

```cpp
static void test_fallback_slot_is_clamped() {
    DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT];
    for (int i = 0; i < DEMO_RECORDER_SLOT_COUNT; ++i)
        slots[i] = existing_slot(315532800);

    assert(demo_recorder_choose_slot(slots, -1) == 0);
    assert(demo_recorder_choose_slot(slots, 10) == 0);
    assert(demo_recorder_choose_slot(slots, 13) == 3);
}
```

Call it from `main()`:

```cpp
int main() {
    test_lowest_missing_slot_wins();
    test_oldest_mtime_wins_when_full();
    test_ties_choose_lowest_index();
    test_equal_mtimes_use_fallback_slot_when_full();
    test_fallback_slot_is_clamped();
    return 0;
}
```

Run:

```bash
cmake --build test/build --target test_demo_recorder && ./test/build/test_demo_recorder
```

Expected: PASS (this should already pass after Task 1).

- [ ] **Step 2: Replace the stub `demo_recorder_next_path` with real SD logic**

Update the bottom half of `components/duke3d/demo_recorder.cpp`:

```cpp
int read_round_robin_slot() {
    FILE *f = fopen(kRoundRobinPath, "rb");
    if (!f)
        return 0;

    int slot = 0;
    const int ok = fscanf(f, "%d", &slot);
    fclose(f);
    return ok == 1 ? clamp_slot(slot) : 0;
}

void write_round_robin_slot(int next_slot) {
    FILE *f = fopen(kRoundRobinPath, "wb");
    if (!f)
        return;
    fprintf(f, "%d\n", clamp_slot(next_slot));
    fclose(f);
}

void scan_slots(DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT]) {
    for (int i = 0; i < DEMO_RECORDER_SLOT_COUNT; ++i) {
        char path[64];
        snprintf(path, sizeof(path), "%s/demo%d.dmo", kDemoDir, i);

        struct stat st {};
        if (stat(path, &st) == 0) {
            slots[i].exists = 1;
            slots[i].mtime = static_cast<long long>(st.st_mtime);
        } else {
            slots[i].exists = 0;
            slots[i].mtime = 0;
        }
    }
}
```

Then replace `demo_recorder_next_path`:

```cpp
int demo_recorder_next_path(char *buf, size_t len) {
    if (!s_armed || !buf || len == 0)
        return 0;

    DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT];
    scan_slots(slots);

    const int fallback = read_round_robin_slot();
    const int slot = demo_recorder_choose_slot(slots, fallback);

    const int written = snprintf(buf, len, "%s/demo%d.dmo", kDemoDir, slot);
    if (written < 0 || static_cast<size_t>(written) >= len)
        return 0;

    if (all_slots_exist(slots) && mtimes_unreliable(slots))
        write_round_robin_slot(slot + 1);

    return 1;
}
```

- [ ] **Step 3: Run host test again**

Run:

```bash
cmake --build test/build --target test_demo_recorder && ./test/build/test_demo_recorder
```

Expected: PASS.

---

## Task 3: Register `demo_recorder.cpp` In The Firmware Build

**Files:**
- Modify: `components/duke3d/CMakeLists.txt`

- [ ] **Step 1: Add the source file**

Modify the `idf_component_register(SRCS ...)` block:

```cmake
idf_component_register(
    SRCS
        "duke3d_component.cpp"
        "renderer.cpp"
        "esp32_hal.cpp"
        "tilecache.cpp"
        "input.cpp"
        "flash_tiles.cpp"
        "demo_recorder.cpp"
        "engine_main_shim.c"
        "audiolib_stubs.c"
        "pico_uart_vendor_hid_decode.c"
        ${ENGINE_SRCS}
        ${GAME_SRCS}
        ${SDL_SRCS}
        ${AUDIOLIB_SRCS}
```

- [ ] **Step 2: Run the host test and inspect the diff**

Run:

```bash
cmake --build test/build --target test_demo_recorder && ./test/build/test_demo_recorder
git diff -- components/duke3d/CMakeLists.txt components/duke3d/demo_recorder.* test/CMakeLists.txt test/duke3d/test_demo_recorder.cpp
```

Expected: host test PASS; diff shows only the new recorder unit, test registration, and firmware source registration.

---

## Task 4: Add The Engine Hook And Finalization Points

**Files:**
- Modify: `components/duke3d/dukesp_hooks.h`
- Modify: `components/duke3d/engine_main_shim.c`

- [ ] **Step 1: Declare the write-path hook**

In `components/duke3d/dukesp_hooks.h`, add after `dukesp_set_kiosk_demo_record`:

```c
/** Demo recording: fill `buf` with the rotated SD-card write path.
 * Returns nonzero when recording is armed and the path should be used. */
int dukesp_demo_write_path(char *buf, size_t len);
```

Also add `#include <stddef.h>` near the top of the header:

```c
#include <stddef.h>
```

- [ ] **Step 2: Implement the hook in the shim**

In `components/duke3d/engine_main_shim.c`, add the include:

```c
#include "demo_recorder.h"
```

Add this function near the existing ESP kiosk hook functions:

```c
int dukesp_demo_write_path(char *buf, size_t len) {
    return demo_recorder_next_path(buf, len);
}
```

- [ ] **Step 3: Finalize before longjmp teardown**

In the `setjmp` return block of `duke3d_main`, insert `closedemowrite()` before `SoundShutdown()`:

```c
        /* If the live run was recording, finalize the .dmo before teardown. */
        closedemowrite();
        SoundShutdown();
        Shutdown();
```

This is safe because `closedemowrite()` is a no-op unless `ud.recstat == 1`.

- [ ] **Step 4: Finalize before death restart**

In `dukesp_player_death_tick`, insert before `esp_restart()`:

```c
        closedemowrite();
        esp_restart();
```

- [ ] **Step 5: Run a firmware compile smoke check**

Run:

```bash
esphome compile esphome.yaml
```

Expected: likely FAIL at this point because `game.c` has not yet declared/used the new hook and `argv_live` still lacks `/r`; if it fails only due to those known next tasks, continue. If it fails due to missing `closedemowrite`, check that `components/duke3d/engine/components/Game/funct.h` still declares it and that `engine_main_shim.c` includes `funct.h`.

---

## Task 5: Make `opendemowrite()` Use The Rotated Path

**Files:**
- Modify: `components/duke3d/engine/components/Game/game.c`

- [ ] **Step 1: Replace the broken path construction**

In `opendemowrite()`, replace:

```c
	char  fullpathdemofilename[16];
```

with:

```c
    char fullpathdemofilename[64];
```

Then replace the `getGameDir()` path-building block:

```c
	// Are we loading a TC?
	if(getGameDir()[0] != '\0'){
		// Yes
		sprintf(fullpathdemofilename, "%s\\%s", getGameDir(), d);
	}
	else{
		// No 
		sprintf(fullpathdemofilename, "%s", d);
	}
```

with:

```c
    if (dukesp_demo_write_path(fullpathdemofilename, sizeof(fullpathdemofilename))) {
        /* ESP live-session recorder supplied the rotated SD-card path. */
    } else if (getGameDir()[0] != '\0') {
        snprintf(fullpathdemofilename, sizeof(fullpathdemofilename), "%s/%s", getGameDir(), d);
    } else {
        snprintf(fullpathdemofilename, sizeof(fullpathdemofilename), "%s", d);
    }
```

`game.c` already includes `dukesp_hooks.h`, so no new include should be needed.

- [ ] **Step 2: Run compile smoke check**

Run:

```bash
esphome compile esphome.yaml
```

Expected: compilation should progress past `opendemowrite()`. If it now fails because `demo_recorder.cpp` needs ESP-specific headers guarded for host tests, fix those guards in `demo_recorder.cpp` without changing the public API.

---

## Task 6: Add The `record_demos` Config Flag

**Files:**
- Modify: `components/duke3d/duke3d_component.h`
- Modify: `components/duke3d/__init__.py`
- Modify: `esphome.yaml`

- [ ] **Step 1: Add component state and setter**

In `components/duke3d/duke3d_component.h`, add the setter near the existing boolean setters:

```cpp
    void set_record_demos(bool v) { record_demos_ = v; }
```

Add the member near the other booleans:

```cpp
    bool record_demos_ = true;
```

- [ ] **Step 2: Add schema and codegen**

In `components/duke3d/__init__.py`, add a constant near the other `CONF_*` names:

```python
CONF_RECORD_DEMOS            = "record_demos"
```

Add it to `CONFIG_SCHEMA` near `CONF_SMOKE_TEST`:

```python
        cv.Optional(CONF_RECORD_DEMOS, default=True): cv.boolean,
```

Add it to `to_code` after `set_smoke_test`:

```python
    cg.add(var.set_record_demos(config[CONF_RECORD_DEMOS]))
```

- [ ] **Step 3: Document the option in YAML**

In `esphome.yaml`, under the `duke3d:` block near `smoke_test`, add:

```yaml
  # Record Start-triggered live sessions to rotating /sdcard/duke3d/demo0.dmo ... demo9.dmo.
  record_demos: true
```

- [ ] **Step 4: Run ESPHome config/compile check**

Run:

```bash
esphome config esphome.yaml
esphome compile esphome.yaml
```

Expected: config accepts `record_demos`; compile may still fail until `argv_live` wiring is done in the next task, but no Python schema/codegen errors should remain.

---

## Task 7: Arm Recording And Add `/r` To Live Runs

**Files:**
- Modify: `components/duke3d/duke3d_component.cpp`

- [ ] **Step 1: Include the recorder header**

Near the other local includes, add:

```cpp
#include "demo_recorder.h"
```

- [ ] **Step 2: Add `/r` to `argv_live`**

Change `argv_live` from:

```cpp
    char* argv_live[] = {
        (char*) "duke3d",
        (char*) "-game_dir",
        (char*) "/sdcard/duke3d",
        (char*) "/nm",
        (char*) "/v1",
        level_arg,
        (char*) "/s0",
        nullptr,
    };
```

to:

```cpp
    char* argv_live[] = {
        (char*) "duke3d",
        (char*) "-game_dir",
        (char*) "/sdcard/duke3d",
        (char*) "/nm",
        (char*) "/v1",
        level_arg,
        (char*) "/s0",
        (char*) "/r",
        nullptr,
    };
```

- [ ] **Step 3: Track whether the current run is live**

Inside the `for (;;)`, add a local flag next to `argc_run`:

```cpp
        bool live_run = false;
```

In the `run_live_next` branch, set it and update `argc_run`:

```cpp
            live_run = true;
            argc_run = self->record_demos_ ? 8 : 7;
            argv_run = argv_live;
            ESP_LOGI(TAG, "Live play: warp E1 random level easiest (/v1 %s /s0%s)",
                     level_arg, self->record_demos_ ? " /r" : "");
```

The demo branch remains unchanged.

- [ ] **Step 4: Arm/disarm around `duke3d_main()`**

Immediately before `duke3d_main(argc_run, argv_run)`:

```cpp
        demo_recorder_arm(live_run && self->record_demos_);
        const int rc = duke3d_main(argc_run, argv_run);
        demo_recorder_arm(0);
```

Remove the old single-line `const int rc = duke3d_main(argc_run, argv_run);`.

- [ ] **Step 5: Update Start log message**

Change:

```cpp
            ESP_LOGI(TAG, "Start — next engine run is live E1 random level (easiest)");
```

to:

```cpp
            ESP_LOGI(TAG, "Start - next engine run is live E1 random level (easiest%s)",
                     self->record_demos_ ? ", recording" : "");
```

- [ ] **Step 6: Run compile check**

Run:

```bash
esphome compile esphome.yaml
```

Expected: PASS or a small compile error tied directly to the new recorder/argv wiring. Fix compile errors before moving on.

---

## Task 8: Full Verification

**Files:**
- Inspect all modified files.

- [ ] **Step 1: Run host tests**

Run:

```bash
cmake -S test -B test/build && cmake --build test/build && ./test/build/test_demo_recorder
```

Expected: all targets build and `test_demo_recorder` exits 0.

- [ ] **Step 2: Run ESPHome validation and compile**

Run:

```bash
esphome config esphome.yaml
esphome compile esphome.yaml
```

Expected: both commands complete successfully. The compile should regenerate `components/duke3d/pico_uart_bridge_maps.h`; inspect it and keep it only if content changed due to current schema/codegen.

- [ ] **Step 3: Inspect the exact diff**

Run:

```bash
git diff -- components/duke3d/demo_recorder.h components/duke3d/demo_recorder.cpp test/duke3d/test_demo_recorder.cpp test/CMakeLists.txt components/duke3d/CMakeLists.txt components/duke3d/dukesp_hooks.h components/duke3d/engine_main_shim.c components/duke3d/engine/components/Game/game.c components/duke3d/duke3d_component.h components/duke3d/duke3d_component.cpp components/duke3d/__init__.py esphome.yaml
```

Expected: diff is limited to the approved feature.

- [ ] **Step 4: Manual hardware checklist after flashing**

Flash and verify on device:

```bash
esphome run esphome.yaml
```

Manual expected results:

1. Start press from attract/demo mode enters a live E1 random level and logs `/r`.
2. Quitting the live game creates or updates `/sdcard/duke3d/demoN.dmo`.
3. The generated `.dmo` appears in the attract pool and can be replayed.
4. First recordings fill the lowest missing slot before replacing older files.
5. Pressing Start again mid-level finalizes the partial recording before reload.
6. Death auto-restart finalizes the recording before `esp_restart()`.
7. Setting `record_demos: false` removes `/r` from the live argv and writes no files.

---

## Self-Review

- Spec coverage: the plan covers the new recorder unit, rotated `demo0`-`demo9` naming, fill-empty then oldest-mtime selection, round-robin fallback, engine hook, `/r` live argv, `record_demos`, and all requested finalization paths.
- Placeholder scan: no `TBD`, `TODO`, or vague "add handling" steps remain; each code-changing task includes concrete snippets and verification commands.
- Type consistency: the spec's `bool` API is adapted to a C ABI (`int` return and `int enabled`) because `engine_main_shim.c` is C. The public names remain consistent across tasks: `demo_recorder_choose_slot`, `demo_recorder_next_path`, `demo_recorder_arm`, and `dukesp_demo_write_path`.

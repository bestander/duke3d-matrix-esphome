#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Returned by duke3d_main() when the ESP reload path was triggered (historic name; unused for random demos now). */
#define DUKE_EXIT_RELOAD_RANDOM_DEMO 42

/** Pico Start pressed: cooperative reload; `game_task` next runs playable E1L1 warp (see `duke3d_component.cpp`). */
#define DUKE_EXIT_START_PLAY_E1L1 43

/** @deprecated Prefer DUKE_EXIT_START_PLAY_E1L1 (was misnamed during kiosk recording experiments). */
#define DUKE_EXIT_START_RECORD_SESSION DUKE_EXIT_START_PLAY_E1L1

/** Kiosk `/er`: demo recorder finalized `.dmo` — cooperative return to demo loop (optional legacy path). */
#define DUKE_EXIT_RECORDING_SESSION_DONE 44

/**
 * Cooperative exit from the running engine via longjmp to `duke3d_main` scope.
 * `reason` becomes the return code of duke3d_main().
 */
void duke_jump_out_with_reason(int reason);

#ifdef __cplusplus
}
#endif

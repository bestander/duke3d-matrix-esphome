#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Returned by duke3d_main() when the ESP reload path was triggered (historic name; unused for random demos now). */
#define DUKE_EXIT_RELOAD_RANDOM_DEMO 42

/** Pico Start / UART requested a kiosk recording boot (warm engine restart). */
#define DUKE_EXIT_START_RECORD_SESSION 43

/** Kiosk `/er`: demo recorder just finalized `.dmo` — return ESP shell loop to resume random demo playback. */
#define DUKE_EXIT_RECORDING_SESSION_DONE 44

/**
 * Cooperative exit from the running engine via longjmp to `duke3d_main` scope.
 * `reason` becomes the return code of duke3d_main().
 */
void duke_jump_out_with_reason(int reason);

#ifdef __cplusplus
}
#endif

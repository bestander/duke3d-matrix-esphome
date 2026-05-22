#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Clear ESP kiosk hooks before each `main()` inside `duke3d_main()` (static game state survives between runs). */
void dukesp_reset_for_new_engine_run(void);

/** Set when argv contains `/er` — after a kiosk demo recording is finalized, jump back to the ESP shell loop. */
void dukesp_set_kiosk_demo_record(int enabled);

/** If kiosk recording is armed, cooperative exit via longjmp — call only after releasing display/file locks appropriate for jump. */
void dukesp_maybe_jump_after_demo_write_closed(void);

#ifdef __cplusplus
}
#endif

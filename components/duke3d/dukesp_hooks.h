#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Kiosk/demo recorder hooks from game.c; implementations in engine_main_shim.c */
void dukesp_reset_for_new_engine_run(void);
void dukesp_set_kiosk_demo_record(int enabled);
void dukesp_maybe_jump_after_demo_write_closed(void);

#ifdef __cplusplus
}
#endif

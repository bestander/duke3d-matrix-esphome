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

/** Kiosk: call once per rendered game frame with whether the LIVE local player is currently
 *  dead (pass 0 during demos/menus/while alive). Once the player has been dead continuously
 *  for ~10s the device is hard-restarted (esp_restart) so the attract/demo loop comes back. */
void dukesp_player_death_tick(int player_is_dead);

#ifdef __cplusplus
}
#endif

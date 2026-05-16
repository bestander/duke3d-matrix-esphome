#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Returned by duke3d_main() when the ESP bridge requested a new random demo (.dmo). */
#define DUKE_EXIT_RELOAD_RANDOM_DEMO 42

/** Cooperative exit from the engine main loop (called during SDL flip / frame output). */
void duke_jump_out_for_demo_reload(void);

#ifdef __cplusplus
}
#endif

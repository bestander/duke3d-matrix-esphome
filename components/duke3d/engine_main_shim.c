/*
 * engine_main_shim.c
 *
 * The Duke3D engine entry point is main() in game.c. ESP-IDF uses app_main()
 * so main() is free. duke3d_component.cpp (when DUKE3D_ENGINE_PRESENT is defined)
 * calls duke3d_main(); this shim bridges it to the engine's main().
 */
#include <setjmp.h>

#include "SDL_video.h"
#include "dukesp_hooks.h"
#include "duke_reload.h"

extern int main(int argc, char **argv);

static jmp_buf duke_reload_jbuf;
static volatile int duke_reload_armed;
static volatile int duke_jump_reason;

int duke3d_main(int argc, char **argv)
{
    duke_reload_armed = 1;
    dukesp_reset_for_new_engine_run();

    if (setjmp(duke_reload_jbuf) != 0) {
        int reason = duke_jump_reason;
        duke_reload_armed = 0;
        return reason;
    }

    int rc = main(argc, argv);
    duke_reload_armed = 0;
    return rc;
}

void duke_jump_out_with_reason(int reason)
{
    if (!duke_reload_armed)
        return;
    duke_jump_reason = reason;
    SDL_ReleaseDisplayMutexIfHeld();
    longjmp(duke_reload_jbuf, 1);
}

/* ---- ESP kiosk demo hooks (linked with game.c; kept in shim so ESPHome always compiles one TU). ---- */

static int s_kiosk_exit_after_demo_write;

void dukesp_reset_for_new_engine_run(void) {
    s_kiosk_exit_after_demo_write = 0;
}

void dukesp_set_kiosk_demo_record(int enabled) {
    s_kiosk_exit_after_demo_write = enabled ? 1 : 0;
}

void dukesp_maybe_jump_after_demo_write_closed(void) {
    if (!s_kiosk_exit_after_demo_write)
        return;
    duke_jump_out_with_reason(DUKE_EXIT_RECORDING_SESSION_DONE);
}

/*
 * engine_main_shim.c
 *
 * The Duke3D engine entry point is main() in game.c. ESP-IDF uses app_main()
 * so main() is free. duke3d_component.cpp (when DUKE3D_ENGINE_PRESENT is defined)
 * calls duke3d_main(); this shim bridges it to the engine's main().
 */
#include <setjmp.h>

#include "SDL_video.h"
#include "duke_reload.h"

extern int main(int argc, char **argv);

static jmp_buf duke_reload_jbuf;
static volatile int duke_reload_armed;

int duke3d_main(int argc, char **argv)
{
    duke_reload_armed = 1;
    if (setjmp(duke_reload_jbuf) != 0) {
        duke_reload_armed = 0;
        return DUKE_EXIT_RELOAD_RANDOM_DEMO;
    }
    int rc = main(argc, argv);
    duke_reload_armed = 0;
    return rc;
}

void duke_jump_out_for_demo_reload(void)
{
    if (!duke_reload_armed) {
        return;
    }
    SDL_ReleaseDisplayMutexIfHeld();
    longjmp(duke_reload_jbuf, 1);
}

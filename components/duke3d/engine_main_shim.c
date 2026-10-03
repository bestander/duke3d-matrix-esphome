/*
 * engine_main_shim.c
 *
 * The Duke3D engine entry point is main() in game.c. ESP-IDF uses app_main()
 * so main() is free. duke3d_component.cpp (when DUKE3D_ENGINE_PRESENT is defined)
 * calls duke3d_main(); this shim bridges it to the engine's main().
 */
#include <setjmp.h>
#include <stdio.h>

#include "SDL_video.h"
#include "demo_recorder.h"
#include "dukesp_hooks.h"
#include "duke_reload.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "funct.h"

extern void Shutdown(void);
extern void uninitgroupfile(void);
extern void kclose(int32_t handle);
extern int recfilep;
extern uint8_t which_demo;

extern int main(int argc, char **argv);

static jmp_buf duke_reload_jbuf;
static volatile int duke_reload_armed;
static volatile int duke_jump_reason;

static int64_t shim_diag_ms(void)
{
    return (int64_t)(esp_timer_get_time() / 1000);
}

/* Free everything a run allocated, or the next duke3d_main() OOMs loading GRP. */
static void duke_teardown_run(void)
{
    /* Attract playback leaves recfilep open. longjmp skips playback()'s
     * kclose, and the next live run fopen()s a .dmo on the same volume.
     * FatFs deadlocks or faults if that read handle is still open. */
    if (recfilep >= 0) {
        kclose(recfilep);
        recfilep = -1;
    }
    /* If the live run was recording, finalize the .dmo before teardown. */
    closedemowrite();
    SoundShutdown();
    Shutdown();
    printf("[duke3d_main] t=%lldms Shutdown() done\n", (long long)shim_diag_ms());
    fflush(stdout);
    uninitgroupfile();
    printf("[duke3d_main] t=%lldms uninitgroupfile() done, internal free=%u psram free=%u\n",
           (long long)shim_diag_ms(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    fflush(stdout);

    /* Engine reloads still leak and fragment a few KB per run. Reboot between
     * demos before the next Startup() hits an allocation failure mid-init. */
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_blk  = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    const size_t int_free   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (psram_free < 250 * 1024 || psram_blk < 96 * 1024 || int_free < 42 * 1024) {
        printf("[duke3d_main] heap low (psram free=%u blk=%u internal=%u) — esp_restart()\n",
               (unsigned)psram_free, (unsigned)psram_blk, (unsigned)int_free);
        fflush(stdout);
        esp_restart();
    }
}

int duke3d_main(int argc, char **argv)
{
    printf("[duke3d_main] t=%lldms enter argc=%d argv[1]=%s\n",
           (long long)shim_diag_ms(), argc, (argc > 1 && argv[1]) ? argv[1] : "(null)");
    fflush(stdout);

    duke_reload_armed = 1;
    dukesp_reset_for_new_engine_run();

    if (setjmp(duke_reload_jbuf) != 0) {
        int reason = duke_jump_reason;
        duke_reload_armed = 0;
        printf("[duke3d_main] t=%lldms longjmp reason=%d — Shutdown()+uninitgroupfile()\n",
               (long long)shim_diag_ms(), reason);
        fflush(stdout);
        duke_teardown_run();
        return reason;
    }

    int rc = main(argc, argv);
    duke_reload_armed = 0;
    printf("[duke3d_main] t=%lldms main() returned %d — teardown\n",
           (long long)shim_diag_ms(), rc);
    fflush(stdout);
    duke_teardown_run();
    return rc;
}

void duke_jump_out_with_reason(int reason)
{
    if (!duke_reload_armed)
        return;
    duke_jump_reason = reason;
    printf("[duke3d_main] t=%lldms jump_out reason=%d — release display mtx if held\n",
           (long long)shim_diag_ms(), reason);
    fflush(stdout);
    SDL_ReleaseDisplayMutexIfHeld();
    longjmp(duke_reload_jbuf, 1);
}

/* ---- ESP kiosk demo hooks (linked with game.c; kept in shim so ESPHome always compiles one TU). ---- */

static int s_kiosk_exit_after_demo_write;

/* Timestamp (ms) the live player was first observed dead this run, 0 = not dead. */
static int64_t s_player_dead_since_ms;

/* Hard-restart the device this long after the live player dies. */
#define DUKE_DEATH_RESTART_MS 10000

void dukesp_reset_for_new_engine_run(void) {
    /* game.c globals keep their values across in-process engine restarts.
     * playback() only honours /dFILE while which_demo == 1. */
    which_demo = 1;
    s_kiosk_exit_after_demo_write = 0;
    s_player_dead_since_ms = 0;
}

void dukesp_player_death_tick(int player_is_dead) {
    if (!player_is_dead) {
        /* Alive / not live play (demo, menu) — disarm. Lets a respawn cancel the restart. */
        s_player_dead_since_ms = 0;
        return;
    }

    int64_t now = shim_diag_ms();
    if (s_player_dead_since_ms == 0) {
        s_player_dead_since_ms = now;
        printf("[duke3d] live player died — restarting device in %d s unless respawned\n",
               DUKE_DEATH_RESTART_MS / 1000);
        fflush(stdout);
        return;
    }

    if (now - s_player_dead_since_ms >= DUKE_DEATH_RESTART_MS) {
        printf("[duke3d] player dead %d s — esp_restart()\n", DUKE_DEATH_RESTART_MS / 1000);
        fflush(stdout);
        closedemowrite();
        esp_restart();
    }
}

void dukesp_set_kiosk_demo_record(int enabled) {
    s_kiosk_exit_after_demo_write = enabled ? 1 : 0;
}

int dukesp_demo_write_path(char *buf, size_t len) {
    return demo_recorder_next_path(buf, len);
}

void dukesp_maybe_jump_after_demo_write_closed(void) {
    if (!s_kiosk_exit_after_demo_write)
        return;
    duke_jump_out_with_reason(DUKE_EXIT_RECORDING_SESSION_DONE);
}

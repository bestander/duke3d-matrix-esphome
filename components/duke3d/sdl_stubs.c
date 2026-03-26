/*
 * sdl_stubs.c
 *
 * Replaces SDL_event.c and SDL_audio.c for the ESP32-S3 HUB75 build:
 *
 *   SDL_event.c  — excluded because it targets ODROID-GO GPIO hardware.
 *                  Input is not needed for demo-playback mode (/dDEMO1.DMO).
 *
 *   SDL_audio.c  — excluded because it uses IDF 4 DAC-mode I2S APIs removed
 *                  in IDF 5.  Real audio goes through platform_audio_write().
 *
 * All stub implementations are no-ops or minimal safe returns.
 */
#include <string.h>
#include "SDL_event.h"   /* SDL_Event, SDL_PollEvent declaration, inputInit, keyMode */
#include "SDL_audio.h"   /* SDL_AudioSpec, SDL_AudioCVT, audio function declarations */

/* ---- SDL_event.c replacements ---- */

int keyMode = 0;

void inputInit(void) {
    /* GPIO ISR input replaced by input.cpp; demo mode needs no events. */
}

int SDL_PollEvent(SDL_Event *event) {
    (void)event;
    /* Demo playback (/dDEMO1.DMO) does not require real input events. */
    return 0;
}

/* ---- SDL_audio.c replacements ---- */
/* Game layer uses dummy_audiolib.c; real I2S audio via platform_audio_write(). */

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained) {
    (void)desired;
    if (obtained) memset(obtained, 0, sizeof(*obtained));
    return 0;
}

void SDL_PauseAudio(int pause_on) { (void)pause_on; }
void SDL_CloseAudio(void) {}

int SDL_BuildAudioCVT(SDL_AudioCVT *cvt,
                      Uint16 src_format, Uint8 src_channels, int src_rate,
                      Uint16 dst_format, Uint8 dst_channels, int dst_rate) {
    (void)src_format; (void)src_channels; (void)src_rate;
    (void)dst_format; (void)dst_channels; (void)dst_rate;
    if (cvt) cvt->len_mult = 1;
    return 0;
}

int SDL_ConvertAudio(SDL_AudioCVT *cvt) { (void)cvt; return 0; }
void SDL_LockAudio(void)   {}
void SDL_UnlockAudio(void) {}

/*
 * audiolib_stubs.c
 *
 * FX_* symbols are now provided by the real Apogee Multivoc audiolib
 * (fx_man.c, multivoc.c, etc.) compiled via AUDIOLIB_SRCS in CMakeLists.txt.
 *
 * This file provides only:
 *   - MUSIC_* stubs  (OPL2/MIDI music is not ported; game launched with /nm)
 *   - PlayMusic stub (called by sounds.c; no-op without music support)
 *
 * IMPORTANT: Do NOT include music.h here.  music.h -> duke3d.h defines
 * (not just declares) large global arrays; including it creates duplicate
 * symbols and overflows DRAM by ~1.6 MB.
 */

#include <inttypes.h>

/* Minimal MUSIC types without pulling in duke3d.h */
#define cdecl  /* empty on non-DOS */
enum { MUSIC_Ok = 0, MUSIC_Error = -1 };
typedef struct {
    uint32_t tickposition;
    uint32_t milliseconds;
    unsigned int measure;
    unsigned int beat;
    unsigned int tick;
} songposition;

/* ---- MUSIC stubs (music disabled via /nm) ---- */

int MUSIC_ErrorCode = 0;  /* MUSIC_Ok */

char *MUSIC_ErrorString(int ErrorNumber) { (void)ErrorNumber; return (char *)""; }
int   MUSIC_Init(int SoundCard, int Address) { (void)SoundCard; (void)Address; return MUSIC_Ok; }
int   MUSIC_Shutdown(void) { return MUSIC_Ok; }
void  MUSIC_SetMaxFMMidiChannel(int channel) { (void)channel; }
void  MUSIC_SetVolume(int volume) { (void)volume; }
void  MUSIC_SetMidiChannelVolume(int channel, int volume) { (void)channel; (void)volume; }
void  MUSIC_ResetMidiChannelVolumes(void) {}
int   MUSIC_GetVolume(void) { return 0; }
void  MUSIC_SetLoopFlag(int loopflag) { (void)loopflag; }
int   MUSIC_SongPlaying(void) { return 0; }
void  MUSIC_Continue(void) {}
void  MUSIC_Pause(void) {}
int   MUSIC_StopSong(void) { return MUSIC_Ok; }
int   MUSIC_PlaySong(char *songData, int loopflag) { (void)songData; (void)loopflag; return MUSIC_Ok; }
void  MUSIC_SetContext(int context) { (void)context; }
int   MUSIC_GetContext(void) { return 0; }
void  MUSIC_SetSongTick(uint32_t PositionInTicks) { (void)PositionInTicks; }
void  MUSIC_SetSongTime(uint32_t milliseconds) { (void)milliseconds; }
void  MUSIC_SetSongPosition(int measure, int beat, int tick) {
    (void)measure; (void)beat; (void)tick;
}
void  MUSIC_GetSongPosition(songposition *pos) { (void)pos; }
void  MUSIC_GetSongLength(songposition *pos) { (void)pos; }
int   MUSIC_FadeVolume(int tovolume, int milliseconds) {
    (void)tovolume; (void)milliseconds; return MUSIC_Ok;
}
int   MUSIC_FadeActive(void) { return 0; }
void  MUSIC_StopFade(void) {}
void  MUSIC_RerouteMidiChannel(int channel,
                               int cdecl (*function)(int event, int c1, int c2)) {
    (void)channel; (void)function;
}
void  MUSIC_RegisterTimbreBank(unsigned char *timbres) { (void)timbres; }

void  PlayMusic(char *filename) {
    (void)filename;
    /* Music disabled (/nm); no-op. */
}

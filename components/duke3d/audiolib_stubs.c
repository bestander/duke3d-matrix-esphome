/*
 * audiolib_stubs.c
 *
 * dummy_audiolib.c has all its content commented out, leaving no FX_* / MUSIC_*
 * symbols defined.  This file provides all the stubs needed by the game layer.
 *
 * The game is run with /nm (no music) and /ns (no sound), so none of these
 * functions will actually play audio.
 *
 * IMPORTANT: Do NOT include music.h here.  music.h includes duke3d.h which
 * defines (not merely declares) large global arrays.  Including it here would
 * create duplicate symbol definitions and overflow DRAM by ~1.6 MB.
 * Instead we forward-declare only the types we need.
 */
#include "fx_man.h"
#include <inttypes.h>

/* Minimal MUSIC types — duplicating only what we need, without duke3d.h */
#define cdecl  /* empty on non-DOS */
enum { MUSIC_Ok = 0, MUSIC_Error = -1 };
typedef struct {
    uint32_t tickposition;
    uint32_t milliseconds;
    unsigned int measure;
    unsigned int beat;
    unsigned int tick;
} songposition;

/* ---- FX (sound effects) stubs ---- */

char *FX_ErrorString(int ErrorNumber) {
    (void)ErrorNumber; return (char *)"";
}
int   FX_SetupCard(int SoundCard, fx_device *device) {
    (void)SoundCard; (void)device; return 1;
}
int   FX_GetBlasterSettings(fx_blaster_config *blaster) {
    (void)blaster; return 1;
}
int   FX_SetupSoundBlaster(fx_blaster_config blaster, int *MaxVoices, int *MaxSampleBits, int *MaxChannels) {
    (void)blaster; (void)MaxVoices; (void)MaxSampleBits; (void)MaxChannels; return 1;
}
int   FX_Init(int SoundCard, int numvoices, int numchannels, int samplebits, unsigned mixrate) {
    (void)SoundCard; (void)numvoices; (void)numchannels; (void)samplebits; (void)mixrate;
    return FX_Ok;
}
int   FX_Shutdown(void) { return 1; }
int   FX_SetCallBack(void (*function)(int32_t)) { (void)function; return FX_Ok; }
void  FX_SetVolume(int volume) { (void)volume; }
int   FX_GetVolume(void) { return 1; }

void  FX_SetReverseStereo(int setting) { (void)setting; }
int   FX_GetReverseStereo(void) { return 1; }
void  FX_SetReverb(int reverb) { (void)reverb; }
void  FX_SetFastReverb(int reverb) { (void)reverb; }
int   FX_GetMaxReverbDelay(void) { return 0; }
int   FX_GetReverbDelay(void) { return 1; }
void  FX_SetReverbDelay(int delay) { (void)delay; }

int   FX_VoiceAvailable(int priority) { (void)priority; return 1; }
int   FX_EndLooping(int handle) { (void)handle; return 1; }
int   FX_SetPan(int handle, int vol, int left, int right) {
    (void)handle; (void)vol; (void)left; (void)right; return 1;
}
int   FX_SetPitch(int handle, int pitchoffset) { (void)handle; (void)pitchoffset; return 1; }
int   FX_SetFrequency(int handle, int frequency) { (void)handle; (void)frequency; return 1; }

int   FX_PlayVOC(uint8_t *ptr, int pitchoffset, int vol, int left, int right,
                 int priority, uint32_t callbackval) {
    (void)ptr; (void)pitchoffset; (void)vol; (void)left; (void)right;
    (void)priority; (void)callbackval; return FX_Ok;
}
int   FX_PlayLoopedVOC(uint8_t *ptr, int32_t loopstart, int32_t loopend,
                       int32_t pitchoffset, int32_t vol, int32_t left, int32_t right,
                       int32_t priority, uint32_t callbackval) {
    (void)ptr; (void)loopstart; (void)loopend; (void)pitchoffset; (void)vol;
    (void)left; (void)right; (void)priority; (void)callbackval; return FX_Ok;
}
int   FX_PlayWAV(uint8_t *ptr, int pitchoffset, int vol, int left, int right,
                 int priority, uint32_t callbackval) {
    (void)ptr; (void)pitchoffset; (void)vol; (void)left; (void)right;
    (void)priority; (void)callbackval; return FX_Ok;
}
int   FX_PlayLoopedWAV(uint8_t *ptr, int32_t loopstart, int32_t loopend,
                       int32_t pitchoffset, int32_t vol, int32_t left, int32_t right,
                       int32_t priority, uint32_t callbackval) {
    (void)ptr; (void)loopstart; (void)loopend; (void)pitchoffset; (void)vol;
    (void)left; (void)right; (void)priority; (void)callbackval; return FX_Ok;
}
int   FX_PlayVOC3D(uint8_t *ptr, int32_t pitchoffset, int32_t angle, int32_t distance,
                   int32_t priority, uint32_t callbackval) {
    (void)ptr; (void)pitchoffset; (void)angle; (void)distance;
    (void)priority; (void)callbackval; return FX_Ok;
}
int   FX_PlayWAV3D(uint8_t *ptr, int pitchoffset, int angle, int distance,
                   int priority, uint32_t callbackval) {
    (void)ptr; (void)pitchoffset; (void)angle; (void)distance;
    (void)priority; (void)callbackval; return FX_Ok;
}
int   FX_PlayRaw(uint8_t *ptr, uint32_t length, uint32_t rate,
                 int32_t pitchoffset, int32_t vol, int32_t left, int32_t right,
                 int32_t priority, uint32_t callbackval) {
    (void)ptr; (void)length; (void)rate; (void)pitchoffset; (void)vol;
    (void)left; (void)right; (void)priority; (void)callbackval; return FX_Ok;
}
int   FX_PlayLoopedRaw(uint8_t *ptr, uint32_t length, char *loopstart,
                       char *loopend, uint32_t rate, int32_t pitchoffset, int32_t vol,
                       int32_t left, int32_t right, int32_t priority, uint32_t callbackval) {
    (void)ptr; (void)length; (void)loopstart; (void)loopend; (void)rate;
    (void)pitchoffset; (void)vol; (void)left; (void)right;
    (void)priority; (void)callbackval; return FX_Ok;
}
int32_t FX_Pan3D(int handle, int angle, int distance) {
    (void)handle; (void)angle; (void)distance; return FX_Ok;
}
int32_t FX_SoundActive(int32_t handle) { (void)handle; return 1; }
int32_t FX_SoundsPlaying(void) { return 0; }
int32_t FX_StopSound(int handle) { (void)handle; return 1; }
int32_t FX_StopAllSounds(void) { return 1; }
int32_t FX_StartDemandFeedPlayback(void (*function)(char **ptr, uint32_t *length),
                                   int32_t rate, int32_t pitchoffset, int32_t vol,
                                   int32_t left, int32_t right,
                                   int32_t priority, uint32_t callbackval) {
    (void)function; (void)rate; (void)pitchoffset; (void)vol; (void)left;
    (void)right; (void)priority; (void)callbackval; return 1;
}
int   FX_StartRecording(int MixRate, void (*function)(char *ptr, int length)) {
    (void)MixRate; (void)function; return 1;
}
void  FX_StopRecord(void) {}

/* ---- MUSIC stubs ---- */

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
    /* Music disabled (/nm flag); no-op. */
}

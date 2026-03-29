#pragma once
#include <stdint.h>

// MODE_DEMO from duke3d.h — do not request HA sync during recorded demo playback.
#define DUKE3D_MODE_DEMO 2

#ifdef __cplusplus
extern "C" {
#endif

// Called from engine after a level map is loaded (enterlevel). Used to run a
// cooperative WiFi window on Core 0 (HA refresh) while the game task suspends.
void duke3d_notify_level_enter(uint8_t g_mode);

#ifdef __cplusplus
}
#endif

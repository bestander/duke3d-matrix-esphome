#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEMO_RECORDER_SLOT_COUNT 10

typedef struct {
    int exists;
    long long mtime;
} DemoRecorderSlot;

// Pure helper used by host tests and by the SD-card scanner.
// Returns a slot in [0, 9].
int demo_recorder_choose_slot(const DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT],
                              int fallback_slot);

// Returns nonzero and fills buf with /sdcard/duke3d/demoN.dmo when armed.
// Returns zero when not armed or when buf is too small.
int demo_recorder_next_path(char *buf, size_t len);

// C ABI because engine_main_shim.c calls this from C.
void demo_recorder_arm(int enabled);

#ifdef __cplusplus
}
#endif

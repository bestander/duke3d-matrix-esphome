#include "demo_recorder.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

namespace {

static int s_armed = 0;

static constexpr const char *kDemoDir = "/sdcard/duke3d";
static constexpr const char *kRoundRobinPath = "/sdcard/duke3d/demorec.idx";
static constexpr long long kFatEpoch1980UnixSeconds = 315532800LL;

bool all_slots_exist(const DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT]) {
    for (int i = 0; i < DEMO_RECORDER_SLOT_COUNT; ++i) {
        if (!slots[i].exists)
            return false;
    }
    return true;
}

bool mtimes_unreliable(const DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT]) {
    if (!all_slots_exist(slots))
        return false;

    long long min_mtime = slots[0].mtime;
    long long max_mtime = slots[0].mtime;
    for (int i = 1; i < DEMO_RECORDER_SLOT_COUNT; ++i) {
        if (slots[i].mtime < min_mtime)
            min_mtime = slots[i].mtime;
        if (slots[i].mtime > max_mtime)
            max_mtime = slots[i].mtime;
    }

    return min_mtime == max_mtime || min_mtime <= kFatEpoch1980UnixSeconds;
}

int clamp_slot(int slot) {
    if (slot < 0)
        return 0;
    if (slot >= DEMO_RECORDER_SLOT_COUNT)
        return slot % DEMO_RECORDER_SLOT_COUNT;
    return slot;
}

int read_round_robin_slot() {
    FILE *f = fopen(kRoundRobinPath, "rb");
    if (!f)
        return 0;

    int slot = 0;
    const int ok = fscanf(f, "%d", &slot);
    fclose(f);
    return ok == 1 ? clamp_slot(slot) : 0;
}

void write_round_robin_slot(int next_slot) {
    FILE *f = fopen(kRoundRobinPath, "wb");
    if (!f)
        return;
    fprintf(f, "%d\n", clamp_slot(next_slot));
    fclose(f);
}

void scan_slots(DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT]) {
    for (int i = 0; i < DEMO_RECORDER_SLOT_COUNT; ++i) {
        char path[64];
        snprintf(path, sizeof(path), "%s/demo%d.dmo", kDemoDir, i);

        struct stat st {};
        if (stat(path, &st) == 0) {
            slots[i].exists = 1;
            slots[i].mtime = static_cast<long long>(st.st_mtime);
        } else {
            slots[i].exists = 0;
            slots[i].mtime = 0;
        }
    }
}

}  // namespace

int demo_recorder_choose_slot(const DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT],
                              int fallback_slot) {
    if (!slots)
        return 0;

    for (int i = 0; i < DEMO_RECORDER_SLOT_COUNT; ++i) {
        if (!slots[i].exists)
            return i;
    }

    if (mtimes_unreliable(slots))
        return clamp_slot(fallback_slot);

    int best = 0;
    for (int i = 1; i < DEMO_RECORDER_SLOT_COUNT; ++i) {
        if (slots[i].mtime < slots[best].mtime)
            best = i;
    }
    return best;
}

void demo_recorder_arm(int enabled) {
    s_armed = enabled ? 1 : 0;
}

int demo_recorder_next_path(char *buf, size_t len) {
    if (!s_armed || !buf || len == 0)
        return 0;

    DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT];
    scan_slots(slots);

    const int fallback = read_round_robin_slot();
    const int slot = demo_recorder_choose_slot(slots, fallback);

    const int written = snprintf(buf, len, "%s/demo%d.dmo", kDemoDir, slot);
    if (written < 0 || static_cast<size_t>(written) >= len)
        return 0;

    if (all_slots_exist(slots) && mtimes_unreliable(slots))
        write_round_robin_slot(slot + 1);

    return 1;
}

#include <cassert>
#include <ctime>

#include "demo_recorder.h"

static DemoRecorderSlot empty_slot() {
    DemoRecorderSlot s{};
    s.exists = 0;
    s.mtime = 0;
    return s;
}

static DemoRecorderSlot existing_slot(long long mtime) {
    DemoRecorderSlot s{};
    s.exists = 1;
    s.mtime = mtime;
    return s;
}

static void test_lowest_missing_slot_wins() {
    DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT];
    for (int i = 0; i < DEMO_RECORDER_SLOT_COUNT; ++i)
        slots[i] = existing_slot(1000 + i);
    slots[3] = empty_slot();
    slots[7] = empty_slot();

    assert(demo_recorder_choose_slot(slots, 9) == 3);
}

static void test_oldest_mtime_wins_when_full() {
    DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT];
    for (int i = 0; i < DEMO_RECORDER_SLOT_COUNT; ++i)
        slots[i] = existing_slot(1700000000 + i);
    slots[6].mtime = 1600000000;

    assert(demo_recorder_choose_slot(slots, 9) == 6);
}

static void test_ties_choose_lowest_index() {
    DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT];
    for (int i = 0; i < DEMO_RECORDER_SLOT_COUNT; ++i)
        slots[i] = existing_slot(1700000000);
    slots[4].mtime = 1600000000;
    slots[8].mtime = 1600000000;

    assert(demo_recorder_choose_slot(slots, 9) == 4);
}

static void test_equal_mtimes_use_fallback_slot_when_full() {
    DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT];
    for (int i = 0; i < DEMO_RECORDER_SLOT_COUNT; ++i)
        slots[i] = existing_slot(315532800);  // 1980-01-01 Unix time

    assert(demo_recorder_choose_slot(slots, 5) == 5);
}

static void test_fat_epoch_mtimes_use_fallback_slot_when_full() {
    DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT];
    for (int i = 0; i < DEMO_RECORDER_SLOT_COUNT; ++i)
        slots[i] = existing_slot(315532800 + i);

    assert(demo_recorder_choose_slot(slots, 7) == 7);
}

static void test_fallback_slot_is_clamped() {
    DemoRecorderSlot slots[DEMO_RECORDER_SLOT_COUNT];
    for (int i = 0; i < DEMO_RECORDER_SLOT_COUNT; ++i)
        slots[i] = existing_slot(315532800);

    assert(demo_recorder_choose_slot(slots, -1) == 0);
    assert(demo_recorder_choose_slot(slots, 10) == 0);
    assert(demo_recorder_choose_slot(slots, 13) == 3);
}

int main() {
    test_lowest_missing_slot_wins();
    test_oldest_mtime_wins_when_full();
    test_ties_choose_lowest_index();
    test_equal_mtimes_use_fallback_slot_when_full();
    test_fat_epoch_mtimes_use_fallback_slot_when_full();
    test_fallback_slot_is_clamped();
    return 0;
}

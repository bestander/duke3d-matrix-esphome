#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Step 1 — call from setup() / main ESPHome task (internal-RAM stack).
 * esp_partition_mmap() disables caches during mapping and asserts the
 * calling stack is not in PSRAM; the game task uses a PSRAM stack so
 * mmap must happen here before the game task is created.
 * Returns 0 on success, -1 if partition missing or magic invalid.
 */
int flash_tiles_premap(void);

/*
 * Step 2 — call after initengine() + loadpics(), from any task.
 * Uses the pointer established by flash_tiles_premap() to populate
 * waloff[], picsiz[], and tiles[].lock for all flash-mapped tiles.
 * Returns tile count, or -1 if flash_tiles_premap() was not called.
 */
int flash_tiles_init(void);

#ifdef __cplusplus
}
#endif

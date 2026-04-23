#include "flash_tiles.h"

#include "esp_log.h"
#include "esp_partition.h"
#include <string.h>
#include <stdint.h>

extern "C" {
#include "tiles.h"   // waloff[], tiles[], tile_t
#include "engine.h"  // picsiz[]
}

static const char *TAG = "flash_tiles";

// TCACHE05 layout — must match make_tile_bin.py
#define FT_MAXTILES  9216
#define FT_MAGIC     "TCACHE05"
#define FT_ABSENT    0xFFFFFFFFu
// entry bits: [31:28]=log2(w)  [27:24]=log2(h)  [23:0]=byte offset
#define FT_OFF(e)    ((e) & 0x00FFFFFFu)
#define FT_LW(e)     (((e) >> 28) & 0xFu)
#define FT_LH(e)     (((e) >> 24) & 0xFu)

static esp_partition_mmap_handle_t s_mmap_handle;
static const void *s_mmap_base = NULL;  // set by flash_tiles_premap()

// Must be called from a task with internal-RAM stack (e.g. ESPHome setup()).
// esp_partition_mmap disables caches during mapping and asserts the calling
// stack is not in PSRAM — the game task uses PSRAM stack so this must run first.
int flash_tiles_premap(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "tiles");
    if (!part) {
        ESP_LOGW(TAG, "tiles partition not found");
        return -1;
    }

    esp_err_t err = esp_partition_mmap(part, 0, part->size,
                                       ESP_PARTITION_MMAP_DATA,
                                       &s_mmap_base, &s_mmap_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap failed: %d", err);
        s_mmap_base = NULL;
        return -1;
    }

    if (memcmp(s_mmap_base, FT_MAGIC, 8) != 0) {
        ESP_LOGE(TAG, "bad magic — reflash tiles.bin");
        esp_partition_munmap(s_mmap_handle);
        s_mmap_base = NULL;
        return -1;
    }

    ESP_LOGI(TAG, "partition '%s' mapped at %p (0x%x bytes)",
             part->label, s_mmap_base, (unsigned)part->size);
    return 0;
}

// Called from game.c after initengine() + loadpics().
// Uses s_mmap_base set by flash_tiles_premap() — no flash ops, safe from PSRAM stack.
int flash_tiles_init(void)
{
    if (!s_mmap_base) {
        ESP_LOGE(TAG, "flash_tiles_premap() was not called");
        return -1;
    }

    const uint32_t *etab = (const uint32_t *)((const uint8_t *)s_mmap_base + 16);
    int count = 0;
    for (int i = 0; i < FT_MAXTILES; i++) {
        uint32_t e = etab[i];
        if (e == FT_ABSENT)
            continue;
        waloff[i]     = (uint8_t *)s_mmap_base + FT_OFF(e);
        picsiz[i]     = (uint8_t)(FT_LW(e) | (FT_LH(e) << 4));
        tiles[i].lock = 255;
        // Sky tiles (89-95) are stored with height truncated to TC_MAX_DIM rows.
        // The wall renderer uses picsiz-based column stride (1<<(picsiz>>4)), so the
        // truncated layout is correct. But dim.height (used as stride by the column
        // rasterizer when height is non-power-of-2) must match the stored row count.
        switch (i) {
            case 89: case 90: case 91: case 92: case 93: case 95:
                tiles[i].dim.height = (int16_t)(1u << FT_LH(e));
                break;
            default: break;
        }
        count++;
    }

    ESP_LOGI(TAG, "%d tiles mapped into waloff[]", count);
    return count;
}

# Flash-Mapped Tile Cache Plan

## Overview

Move tile pixel data from the SD card into a dedicated SPI flash partition and
memory-map it directly into the CPU address space via `esp_partition_mmap()`.
`waloff[tilenume]` pointers are set once at startup to point into the flash-mapped
region. `loadtile()` becomes a no-op for any tile covered by the flash build.
`initcache` and the 262KB PSRAM tile cache are reduced to a 32KB safety net.

## Memory Impact

| Allocation | Before | After |
|---|---|---|
| Tile cache (`initcache`) | 262KB PSRAM | 32KB PSRAM (safety net only) |
| palookup via `kkmalloc` | 7 of 32 tables (~57KB) | all 32 tables (~263KB) |
| palookup spill into tile cache | 25 × 8207 = 205KB locked | 0 |
| Tile cache free for tiles | 57KB | 32KB (safety net, never hit) |
| SD bus contention during render | yes | no |
| CACHE SPACE ALL LOCKED UP | possible | structurally impossible |

**Net PSRAM saving**: ~230KB freed from the heap at game time.

---

## Partition Layout Changes

### Current `partitions.csv`

```
nvs,      data, nvs,     0x9000,   0x4000,
otadata,  data, ota,     0xd000,   0x2000,
app0,     app,  ota_0,   0x10000,  0x300000,
app1,     app,  ota_1,   0x310000, 0x300000,
fatfs,    data, fat,     0x610000, 0x180000,
```

### New `partitions.csv`

```
nvs,      data, nvs,     0x9000,   0x4000,
app0,     app,  factory, 0x10000,  0x300000,
tiles,    data, 0xff,    0x310000, 0x4F0000,
```

- `otadata` removed (no OTA).
- `app1` removed (no OTA).
- `fatfs` removed (replaced by the larger `tiles` partition).
- `app0` changes subtype from `ota_0` to `factory` (standard single-app boot).
- `tiles` partition: `0x310000` → `0x800000` = **4,980,736 bytes ≈ 4.9MB**.
- App stays at 3MB for firmware growth headroom.

---

## Tile Binary Format

Reuses the existing `TCACHE04` format from `tilecache.cpp` unchanged:

```
Offset      Size         Content
0x00000     8 B          magic "TCACHE04"
0x00008     4 B          grp_size (uint32_t, little-endian)
0x0000C     4 B          reserved
0x00010     9216 × 4 B   entry table (one uint32_t per tile, index = tile number)
0x09010     variable     packed pixel data (8-bit paletted, column-major)
```

Entry encoding (packed uint32_t):
```
0xFFFFFFFF            → tile absent (waloff stays NULL)
bits[31:29] = log2(w)
bits[28:26] = log2(h)
bits[25:0]  = byte offset from start of file to pixel data
```

All tiles are pre-downscaled to max 128×128 px (same rule as TILECACHE.BIN).

---

## Step 1 — Build Tool: `tools/make_tile_bin.py`

Standalone Python 3 script. Run once on the host; re-run only if `DUKE3D.GRP`
changes. Output is `tools/tiles.bin`.

### Logic

1. Open `DUKE3D.GRP`, enumerate all `TILES*.ART` entries.
2. Parse each ART header: read tile widths/heights for tiles 0–9215.
3. For each tile with non-zero dimensions:
   a. Read raw pixel data from the ART file.
   b. Downscale: halve each axis while `dim > 128` (same algorithm as `tiles.c`).
   c. Write packed pixel data to output buffer; record offset in entry table.
4. Write output file in TCACHE04 format.
5. **Cache invalidation**: if `tools/tiles.bin` already exists and its embedded
   `grp_size` matches the current GRP file size, skip the build and exit 0.

### Usage

```bash
python3 tools/make_tile_bin.py \
    --grp /path/to/DUKE3D.GRP \
    --out tools/tiles.bin
```

### Flashing (one-time, or after GRP changes)

```bash
esptool.py --port /dev/cu.usbserial-* write_flash \
    0x310000 tools/tiles.bin
```

This is independent of firmware flashing and only needs to be repeated if the
GRP content changes.

---

## Step 2 — Runtime: `components/duke3d/flash_tiles.h`

```c
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Map the tiles flash partition and initialise waloff[] / picsiz[] for all
 * tiles present in the flash build. Call after initengine() (which zeroes
 * waloff[]), before the first render frame.
 *
 * Returns the number of tiles successfully mapped from flash.
 * Returns -1 if the partition is missing or the magic doesn't match.
 */
int flash_tiles_init(void);

#ifdef __cplusplus
}
#endif
```

---

## Step 3 — Runtime: `components/duke3d/flash_tiles.cpp`

Key responsibilities:
1. Find the `tiles` partition by label using `esp_partition_find_first()`.
2. Call `esp_partition_mmap()` with `ESP_PARTITION_MMAP_DATA` to get a
   `const void *mmap_base` pointer in the `0x3C000000` DROM region.
3. Validate the magic bytes ("TCACHE04").
4. Iterate the entry table (9216 entries × 4 bytes, at `mmap_base + 16`):
   - For absent entries (`0xFFFFFFFF`): skip — `waloff[i]` stays NULL.
   - For present entries: set `waloff[i] = (uint8_t*)mmap_base + TC_OFF(e)`.
   - Set `picsiz[i]` from the stored `log2(w)` / `log2(h)` fields.
   - Set `tiles[i].lock = 255` (permanent; `allocache` never evicts).
   - Do NOT modify `tiles[i].dim` — the ART header dimensions (game logic,
     hitboxes) are already set by `loadartfile()` and must stay unchanged.
5. Store `mmap_handle` in a static so it is never freed.

### Thread safety

`flash_tiles_init()` is called once from the game task before the render loop
starts. The audio task (Core 0) never touches `waloff[]`. No locking needed.

---

## Step 4 — `tiles.c`: early return in `loadtile()`

Add at the very top of `loadtile()`, before any other logic:

```c
void loadtile(short tilenume)
{
    if (waloff[tilenume] != NULL) return;   /* flash tile or already cached */
    ...
```

**Why this is safe:**
- Flash tiles: `waloff` pre-set to flash pointer, never NULL → returns immediately.
  Prevents any write path (`kread`, `memset`, downscale) from touching read-only
  flash memory.
- PSRAM-cached tiles (not evicted): `waloff` non-NULL → returns immediately ✓
- PSRAM-cached tiles (evicted by `allocache`): `allocache` sets `*hand = 0`,
  so `waloff` becomes NULL → loadtile proceeds to reload ✓
- First load of a tile not in flash: `waloff` is NULL → proceeds normally ✓

---

## Step 5 — Reduce `initcache` to 32KB

`initcache` cannot be removed entirely because `agecache()` asserts `cacnum >= 1`
and `makepalookup` may call `allocache` as a last-resort fallback. With NimBLE
recovering ~150KB PSRAM, all 32 palookup tables should succeed via `kkmalloc`
and `allocache` will never be hit. The 32KB tile cache exists as a safety net only.

In `engine.c` (or wherever `initcache` is called from), change the allocation:

```c
// OLD: 262KB from PSRAM heap
uint8_t *thecache = (uint8_t*)kkmalloc(CACHESIZE, ...);  // CACHESIZE = 262144
initcache(thecache, CACHESIZE);

// NEW: 32KB from PSRAM heap
#define FLASH_TILE_CACHE_SIZE (32 * 1024)
uint8_t *thecache = (uint8_t*)heap_caps_malloc(
    FLASH_TILE_CACHE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
initcache(thecache, FLASH_TILE_CACHE_SIZE);
```

The 32KB tile cache is enough for ~3 palookup table fallbacks and any
`allocatepermanenttile()` calls that might appear in future code.

---

## Step 6 — Game startup sequence

The order of calls in `Startup()` must be:

```
initengine()          ← zeros waloff[0..MAXTILES-1] at engine.c:3663
loadartfile()         ← populates tiles[i].dim from ART headers
flash_tiles_init()    ← sets waloff[i] for flash tiles, sets picsiz[i]
[... rest of Startup ...]
```

`flash_tiles_init()` must come after `initengine()` (which zeros waloff) and
after `loadartfile()` (which sets tile dimensions used for game logic).

In practice, hook it immediately after the `initengine()` call site.

---

## Step 7 — `esphome.yaml` and `__init__.py`

Add a new config key `flash_tiles` (boolean, default `false`):

```yaml
# esphome.yaml
duke3d:
  flash_tiles: true
  tile_cache: false    # SD-based tilecache superseded by flash_tiles
```

In `__init__.py`:
```python
CONF_FLASH_TILES = "flash_tiles"
cv.Optional(CONF_FLASH_TILES, default=False): cv.boolean
```

`flash_tiles: true` causes codegen to:
- Call `flash_tiles_init()` in the generated component setup sequence.
- Pass the smaller `FLASH_TILE_CACHE_SIZE` to `initcache`.

When `flash_tiles: true`, `tile_cache` is implicitly ignored (SD tilecache path
is bypassed because `loadtile()` returns early for all flash-mapped tiles).

---

## Step 8 — `CMakeLists.txt`

Add `flash_tiles.cpp` to `SRCS`. No new IDF component dependencies: the
`esp_partition` API is already available via the `esp_partition` component
included by ESP-IDF's default component set.

---

## Testing Milestones

| # | Test | Pass condition |
|---|---|---|
| 1 | Build `make_tile_bin.py` output | `tiles.bin` produced; magic = "TCACHE04"; size < 4.9MB |
| 2 | Flash `tiles.bin` to device | `esptool` write_flash completes without error |
| 3 | `flash_tiles_init()` returns > 0 | Log shows N tiles mapped from flash at startup |
| 4 | Logo screen renders | Duke3D logo appears without CACHE SPACE crash |
| 5 | E1L1 gameplay | Level loads, walls and sprites render correctly |
| 6 | PSRAM free at initengine | Should be ≥ 420KB (NimBLE + no 262KB initcache) |
| 7 | No SD reads during render | `sd_open` log tag silent during gameplay frames |
| 8 | Audio uninterrupted | No audio glitches during heavy tile scenes |
| 9 | Cache safety net unused | No "CACHE SPACE ALL LOCKED UP" across multiple levels |
| 10 | Reboot / power cycle | Flash partition persists; game loads normally after reset |

---

## Risks and Mitigations

| Risk | Details | Mitigation |
|---|---|---|
| Flash DCache eviction during render | Flash and code share the 16KB DCache; tile pixel fetches evict code lines | ICache (code) and DCache (data) are separate on ESP32-S3; no cross-eviction |
| `tiles[i].dim` vs `picsiz[i]` mismatch | ART dims (game logic) must not equal stored dims (render) | `flash_tiles_init()` only sets `picsiz`, never `tiles[i].dim` |
| `waloff` zeroed after flash init | `initengine()` zeros waloff; flash init must follow it | Explicit ordering: initengine → loadartfile → flash_tiles_init |
| Write to flash-mapped memory | Any path reaching `kread`/`memset` through waloff crashes | Early return in `loadtile()` blocks all write paths for non-NULL waloff |
| Tile absent in flash, game expects it | Some tiles have zero ART dimensions and are never drawn | waloff stays NULL; renderer already checks waloff != NULL before drawing |
| `tiles.bin` stale after GRP update | Mismatched tile data silently renders wrong pixels | `make_tile_bin.py` embeds GRP file size; re-run if size changes |
| `tiles.bin` too large for partition | Pre-downscaled tiles estimated 2–3MB; partition is 4.9MB | ~2MB headroom; verified at build step 1 |
| `agecache` crash with no initcache | `agecache` asserts `agecount >= 0` which requires `cacnum >= 1` | 32KB initcache satisfies the invariant; no full removal |

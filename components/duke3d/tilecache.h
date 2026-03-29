#pragma once
#include <stdint.h>
#include <stdbool.h>

// Native max edge length (pixels) for tiles stored in TCACHE.BIN and for the
// engine GRP load path — single policy so cache hits and misses match.
#define DUKE3D_TILE_MAX_NATIVE_EDGE 128

#ifdef __cplusplus
extern "C" {
#endif

// Hit descriptor returned by tilecache_lookup().
// w, h are the stored pixel dimensions in the cache file.
// off is the byte offset of the pixel data within TCACHE.BIN.
typedef struct {
    int      w;
    int      h;
    uint32_t off;
} TileCacheHit;

// Check if TCACHE.BIN is up-to-date with the GRP file; rebuild if not.
// Returns true on success (cache is ready). Safe to call on every boot;
// re-build only triggers when the GRP file size changes.
bool tilecache_build_if_needed(const char *grp_path, const char *cache_path);

// Open TCACHE.BIN and load the 36 KB entry table into PSRAM.
// Must be called after tilecache_build_if_needed() and before gameplay.
bool tilecache_open(const char *cache_path);

// Release cache FILE* and PSRAM entry table.
void tilecache_close(void);

// Returns 1 and fills *hit if picnum has a pre-built cache entry; 0 otherwise.
// Zero-copy: the caller is responsible for allocating the destination buffer
// and calling tilecache_read() with hit->off.
int tilecache_lookup(int picnum, TileCacheHit *hit);

// Read hit->w * hit->h bytes from the cache file at byte offset `off` into dst.
void tilecache_read(uint32_t off, void *dst, int bytes);

#ifdef __cplusplus
}
#endif

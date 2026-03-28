#include "tilecache.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

// Must match MAXTILES in build.h and MAX_TILE_DIM in tiles.c.
#define TC_MAXTILES    9216
// 64 pixels matches the HUB75 display width — the maximum detail the display
// can show for a full-width tile.  Downscaling to 32 made menus/title screens
// unrecognizable (256→32 = 64× reduction); 64 keeps quality while still
// reducing large tiles 16× vs the original GRP.
#define TC_MAX_DIM     64
// "TCBUILD!" written at start of do_build; overwritten with "TCACHE03" only
// after the entry table has been successfully committed.  A file with
// "TCBUILD!" magic is treated as corrupt by tilecache_build_if_needed().
#define TC_MAGIC_OK    "TCACHE03"   // bumped from 02: increased TC_MAX_DIM 32→64
#define TC_MAGIC_WIP   "TCBUILD!"
// File layout: [8B magic][4B grp_size][4B reserved][TC_MAXTILES*4B entries][pixel data]
#define TC_HEADER_SIZE (16 + TC_MAXTILES * 4)

// Packed uint32_t entry per tile:
//   0xFFFFFFFF           → tile absent (fall back to GRP)
//   bits[31:29] = log2(w)  (0–5 maps to dims 1–32)
//   bits[28:26] = log2(h)
//   bits[25:0]  = byte offset of pixel data in cache file (max 64 MB)
#define TC_ABSENT       0xFFFFFFFFu
#define TC_W(e)         (1u  << (((e) >> 29) & 7u))
#define TC_H(e)         (1u  << (((e) >> 26) & 7u))
#define TC_OFF(e)       ((e) & 0x03FFFFFFu)
#define TC_MAKE(lw, lh, off) \
    (((uint32_t)(lw) << 29) | ((uint32_t)(lh) << 26) | ((uint32_t)(off) & 0x03FFFFFFu))

// Runtime state — opened once at startup, stays open during gameplay.
static uint32_t *s_entries = NULL;
static FILE     *s_fp      = NULL;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int tc_ilog2(int v) {
    int r = 0;
    while (v > 1) { r++; v >>= 1; }
    return r;
}

static bool name_is_art(const char name[13]) {
    char u[14] = {};
    for (int i = 0; i < 13; i++) {
        char c = name[i];
        u[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    if (strncmp(u, "TILES", 5) != 0) return false;
    const char *dot = strrchr(u, '.');
    return dot && strcmp(dot, ".ART") == 0;
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

static bool do_build(const char *grp_path, const char *cache_path, uint32_t grp_size)
{
    printf("[tilecache] do_build: grp=%s cache=%s grp_size=%u\n",
           grp_path, cache_path, (unsigned)grp_size);

    // entry table — 36 KB; all 0xFF = absent until overwritten
    uint32_t *entries = (uint32_t *)heap_caps_malloc(
        TC_MAXTILES * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!entries) { printf("[tilecache] entries alloc failed\n"); return false; }
    memset(entries, 0xFF, TC_MAXTILES * sizeof(uint32_t));

    // temporary buffer for one full-size tile (up to 256×256 = 64 KB)
    const int TMP_CAP = 64 * 1024;
    uint8_t *tmp = (uint8_t *)heap_caps_malloc(
        TMP_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tmp) {
        heap_caps_free(entries);
        printf("[tilecache] tmp alloc failed\n");
        return false;
    }

    FILE *fp_g = fopen(grp_path,   "rb");
    FILE *fp_c = fopen(cache_path, "wb");
    if (!fp_g || !fp_c) {
        if (fp_g) fclose(fp_g);
        if (fp_c) fclose(fp_c);
        heap_caps_free(tmp); heap_caps_free(entries);
        printf("[tilecache] open failed fp_g=%p fp_c=%p errno=%d (%s)\n",
               fp_g, fp_c, errno, strerror(errno));
        return false;
    }

    // Write header with WIP magic — tilecache_build_if_needed rejects this
    // magic, so a crash mid-build will trigger a full rebuild on next boot.
    // We overwrite with TC_MAGIC_OK only after the entry table is committed.
    {
        uint8_t hdr[16] = {};
        memcpy(hdr, TC_MAGIC_WIP, 8);
        memcpy(hdr + 8, &grp_size, 4);
        fwrite(hdr, 1, 16, fp_c);
        fwrite(entries, 4, TC_MAXTILES, fp_c);  // placeholder (all 0xFF)
    }

    // Validate GRP magic
    char magic[12] = {};
    fread(magic, 1, 12, fp_g);
    if (memcmp(magic, "KenSilverman", 12) != 0) {
        printf("[tilecache] invalid GRP magic\n");
        fclose(fp_c); fclose(fp_g);
        heap_caps_free(tmp); heap_caps_free(entries);
        return false;
    }

    uint32_t num_files = 0;
    fread(&num_files, 4, 1, fp_g);
    printf("[tilecache] GRP has %u files\n", (unsigned)num_files);

    // Directory: num_files × (12-byte name + 4-byte size).
    // Allocate 13 bytes per name so we can null-terminate for string ops.
    uint32_t *sizes = (uint32_t *)heap_caps_malloc(
        num_files * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char (*names)[13] = (char (*)[13])heap_caps_malloc(
        num_files * 13, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sizes || !names) {
        if (sizes) heap_caps_free(sizes);
        if (names) heap_caps_free(names);
        fclose(fp_c); fclose(fp_g);
        heap_caps_free(tmp); heap_caps_free(entries);
        printf("[tilecache] directory alloc failed\n");
        return false;
    }
    for (uint32_t i = 0; i < num_files; i++) {
        fread(names[i], 1, 12, fp_g);   // GRP filenames are exactly 12 bytes
        names[i][12] = '\0';            // null-terminate for strncmp/strrchr
        fread(&sizes[i], 4, 1, fp_g);
    }

    uint32_t data_off  = (uint32_t)TC_HEADER_SIZE;
    uint32_t built     = 0;
    uint8_t  out[TC_MAX_DIM * TC_MAX_DIM];   // 4 KB — downsampled output (64×64)

    for (uint32_t fi = 0; fi < num_files; fi++) {
        if (!name_is_art(names[fi])) {
            fseek(fp_g, (long)sizes[fi], SEEK_CUR);
            continue;
        }

        printf("[tilecache]   %s\n", names[fi]);
        esp_task_wdt_reset();  // build can take >60s; reset per ART file

        uint32_t av = 0, an = 0, ts = 0, te = 0;
        fread(&av, 4, 1, fp_g);
        fread(&an, 4, 1, fp_g);
        fread(&ts, 4, 1, fp_g);
        fread(&te, 4, 1, fp_g);

        int32_t cnt = (av == 1 && te >= ts) ? (int32_t)(te - ts + 1) : 0;
        if (cnt <= 0 || cnt > 1024) {
            printf("[tilecache]   skip (bad hdr av=%u cnt=%d)\n", (unsigned)av, (int)cnt);
            fseek(fp_g, (long)sizes[fi] - 16, SEEK_CUR);
            continue;
        }

        uint16_t tsx[1024], tsy[1024];
        fread(tsx, 2, cnt, fp_g);
        fread(tsy, 2, cnt, fp_g);
        fseek(fp_g, cnt * 4, SEEK_CUR);  // skip picanm

        for (int32_t i = 0; i < cnt; i++) {
            int w = tsx[i], h = tsy[i];
            if (w <= 0 || h <= 0) continue;   // empty — no bytes in ART file

            int pic = (int)(ts + i);
            if (w * h > TMP_CAP || (uint32_t)pic >= TC_MAXTILES) {
                fseek(fp_g, w * h, SEEK_CUR);
                continue;
            }

            fread(tmp, 1, w * h, fp_g);

            // Compute downscaled dimensions (power-of-2 halving)
            int nw = w, nh = h;
            while (nw > TC_MAX_DIM) nw >>= 1;
            while (nh > TC_MAX_DIM) nh >>= 1;
            if (nw < 1) nw = 1;
            if (nh < 1) nh = 1;

            const uint8_t *wbuf;
            int wsz;
            if (nw == w && nh == h) {
                wbuf = tmp;
                wsz  = w * h;
            } else {
                // Nearest-neighbour, column-major (index = x*h + y)
                for (int x = 0; x < nw; x++) {
                    int sx = (x * w) / nw;
                    for (int y = 0; y < nh; y++) {
                        int sy = (y * h) / nh;
                        out[x * nh + y] = tmp[sx * h + sy];
                    }
                }
                wbuf = out;
                wsz  = nw * nh;
            }

            fwrite(wbuf, 1, wsz, fp_c);
            entries[pic] = TC_MAKE(tc_ilog2(nw), tc_ilog2(nh), data_off);
            data_off += (uint32_t)wsz;
            built++;
        }
    }

    heap_caps_free(names);
    heap_caps_free(sizes);
    heap_caps_free(tmp);

    // Commit the real entry table, then overwrite magic with TC_MAGIC_OK.
    // Both writes must succeed before the cache is considered valid.
    fseek(fp_c, 16, SEEK_SET);
    size_t wrote = fwrite(entries, 4, TC_MAXTILES, fp_c);
    heap_caps_free(entries);

    if (wrote != TC_MAXTILES) {
        printf("[tilecache] entry table write failed (%zu/%d)\n", wrote, TC_MAXTILES);
        fclose(fp_c); fclose(fp_g);
        return false;
    }

    // Stamp the valid magic — only now is the file trustworthy.
    fseek(fp_c, 0, SEEK_SET);
    fwrite(TC_MAGIC_OK, 1, 8, fp_c);
    fclose(fp_c);
    fclose(fp_g);

    printf("[tilecache] built %u tiles, %.1f KB total\n",
           (unsigned)built, (float)data_off / 1024.0f);
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool tilecache_build_if_needed(const char *grp_path, const char *cache_path)
{
    FILE *fp = fopen(grp_path, "rb");
    if (!fp) { printf("[tilecache] GRP not found: %s\n", grp_path); return false; }
    fseek(fp, 0, SEEK_END);
    uint32_t grp_size = (uint32_t)ftell(fp);
    fclose(fp);

    // Accept only TC_MAGIC_OK; TC_MAGIC_WIP means a previous build was
    // interrupted — treat as corrupt and rebuild.
    fp = fopen(cache_path, "rb");
    if (fp) {
        char magic[8] = {};
        uint32_t stored = 0;
        bool ok = (fread(magic, 1, 8, fp) == 8 &&
                   memcmp(magic, TC_MAGIC_OK, 8) == 0 &&
                   fread(&stored, 4, 1, fp) == 1 &&
                   stored == grp_size);
        fclose(fp);
        if (ok) {
            printf("[tilecache] cache up to date (grp_size=%u)\n", (unsigned)grp_size);
            return true;
        }
        printf("[tilecache] cache stale or corrupt — rebuilding\n");
    }

    printf("[tilecache] building tile cache (GRP %u B) ...\n", (unsigned)grp_size);
    return do_build(grp_path, cache_path, grp_size);
}

bool tilecache_open(const char *cache_path)
{
    tilecache_close();

    s_fp = fopen(cache_path, "rb");
    if (!s_fp) {
        printf("[tilecache] open failed: %s\n", cache_path);
        return false;
    }

    s_entries = (uint32_t *)heap_caps_malloc(
        TC_MAXTILES * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_entries) {
        printf("[tilecache] entry table alloc failed\n");
        fclose(s_fp); s_fp = NULL;
        return false;
    }

    fseek(s_fp, 16, SEEK_SET);
    if ((size_t)fread(s_entries, 4, TC_MAXTILES, s_fp) != TC_MAXTILES) {
        printf("[tilecache] entry table read failed (file too short?)\n");
        heap_caps_free(s_entries); s_entries = NULL;
        fclose(s_fp); s_fp = NULL;
        return false;
    }

    // Count cached tiles so we can verify the build actually populated entries.
    uint32_t cached = 0;
    for (int i = 0; i < TC_MAXTILES; i++) {
        if (s_entries[i] != TC_ABSENT) cached++;
    }
    printf("[tilecache] open OK: %u/%d tiles cached (%u KB entry table in PSRAM)\n",
           (unsigned)cached, TC_MAXTILES, TC_MAXTILES * 4u / 1024u);
    return true;
}

void tilecache_close(void)
{
    if (s_fp)      { fclose(s_fp);             s_fp      = NULL; }
    if (s_entries) { heap_caps_free(s_entries); s_entries = NULL; }
}

int tilecache_lookup(int picnum, TileCacheHit *hit)
{
    if (!s_entries || (unsigned)picnum >= TC_MAXTILES) return 0;
    uint32_t e = s_entries[picnum];
    if (e == TC_ABSENT) return 0;
    hit->w   = (int)TC_W(e);
    hit->h   = (int)TC_H(e);
    hit->off = TC_OFF(e);
    return 1;
}

void tilecache_read(uint32_t off, void *dst, int bytes)
{
    if (!s_fp) return;
    fseek(s_fp, (long)off, SEEK_SET);
    fread(dst, 1, (size_t)bytes, s_fp);
}

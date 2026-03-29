#include "hub75_matrix.h"
#include "esphome/core/log.h"
// ESPHome redefines ESP_LOGI/ESP_LOGW/etc to use esphome::esp_log_printf_.
// Bring it into file scope so the Hub75 header's inline functions can resolve it.
using esphome::esp_log_printf_;
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstring>
#include "ESP32-HUB75-MatrixPanel-I2S-DMA.h"
// Cache_WriteBack_All() flushed DMA dirty lines in setup() but was removed from
// swap_buffers() — calling it from Core 1 while Core 0 runs the HUB75 ISR caused
// a high-priority interrupt to dereference a NULL function pointer (InstrFetchProhibited).
#include "rom/cache.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "grp_title_splash.h"
#include <cerrno>
#include <sys/stat.h>

// Optional embedded PNG: only if hub75_splash.gen.h exists (splash_image: in yaml). Prefer SD-only.
#if defined(__has_include)
#  if __has_include("hub75_splash.gen.h")
#    include "hub75_splash.gen.h"
#    ifndef HUB75_HAS_SPLASH
#      define HUB75_HAS_SPLASH 1
#    endif
#  endif
#endif

namespace esphome {
namespace hub75_matrix {

static const char* TAG = "hub75";
Hub75Matrix* global_hub75 = nullptr;
char hub75_init_status[128] = "not started";
static MatrixPanel_I2S_DMA* matrix_lib = nullptr;

// When no generated splash header, still use 64×40 row count for SD cache and loops.
#if !defined(HUB75_HAS_SPLASH)
static constexpr int kSplashHeight = 40;
#endif

static constexpr const char* kGrpPath = "/sdcard/duke3d/DUKE3D.GRP";
// 8.3-safe paths; LOADSCR.RGB replaces older SPLASH.RGB name.
static constexpr const char* kSplashCachePath = "/sdcard/duke3d/LOADSCR.RGB";
static constexpr const char* kSplashCacheAltPath = "/sdcard/LOADSCR.RGB";
static constexpr size_t kSplashRgbBytes = 64 * 40 * 3;
// Cache file: 8-byte magic + 4-byte grp_size + 4 reserved + 64×40×3 RGB888
static const uint8_t kSplashCacheMagic[8] = {'S', 'P', 'L', 'A', 'S', 'H', '0', '5'};

static bool get_file_size_u32(const char* path, uint32_t* out_size) {
    if (!out_size) return false;
    *out_size = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    fclose(f);
    if (sz < 0) return false;
    *out_size = static_cast<uint32_t>(sz);
    return true;
}

static uint32_t read_le_u32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0])) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static bool load_sd_splash_cache_at(const char* cache_path, uint8_t* out_rgb, size_t out_len) {
    if (!out_rgb || out_len < kSplashRgbBytes) return false;

    uint32_t grp_size = 0;
    if (!get_file_size_u32(kGrpPath, &grp_size) || grp_size == 0) return false;

    FILE* f = fopen(cache_path, "rb");
    if (!f) return false;

    uint8_t header[16];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return false;
    }

    if (memcmp(header, kSplashCacheMagic, 8) != 0) {
        fclose(f);
        return false;
    }

    const uint32_t cached_grp_size = read_le_u32(&header[8]);
    const uint32_t reserved = read_le_u32(&header[12]);
    if (reserved != 0 || cached_grp_size != grp_size) {
        fclose(f);
        return false;
    }

    const size_t got = fread(out_rgb, 1, kSplashRgbBytes, f);
    fclose(f);
    return got == kSplashRgbBytes;
}

static bool load_sd_splash_cache(uint8_t* out_rgb, size_t out_len) {
    if (load_sd_splash_cache_at(kSplashCachePath, out_rgb, out_len)) return true;
    return load_sd_splash_cache_at(kSplashCacheAltPath, out_rgb, out_len);
}

// -----------------------------------------------------------------------
// Adafruit Matrix Portal S3 — HUB-75 GPIO assignments.
// Source: https://github.com/jnthas/clockwise/issues/75 (confirmed working)
//
// Note: GPIO 35–38 are used here for HUB-75 data lines. On the Matrix
// Portal S3 these pins are wired to the LCD parallel interface; the S3's
// PSRAM is accessed via internal die connections, NOT these GPIOs.
// -----------------------------------------------------------------------
static const HUB75_I2S_CFG::i2s_pins MATRIX_PINS = {
    .r1  = 42, .g1  = 41, .b1  = 40,
    .r2  = 38, .g2  = 39, .b2  = 37,
    .a   = 45, .b   = 36, .c   = 48, .d = 35, .e = 21,
    .lat = 47, .oe  = 14, .clk = 2
};
// Two 64x32 panels stacked vertically to form 64x64, wired in a chain (daisy-chain
// in series). Virtual canvas = 128x32 with CHAIN_LEN=2, PANEL_HEIGHT=32 (1/16 scan).
// Physical layout (confirmed by diagnostic):
//   Panel 2 (virtual x=64..127) = top physical panel, normally oriented
//   Panel 1 (virtual x=0..63)   = bottom physical panel, physically 180° rotated
// Logical 64x64 → virtual 128x32 transform in swap_buffers():
//   ly <  32 (top logical)     → panel 2: vx=lx+64,  vy=ly       (no rot needed)
//   ly >= 32 (bottom logical)  → panel 1: vx=63-lx,  vy=63-ly    (compensate 180° rot)
static const int PANEL_WIDTH  = 64;
static const int PANEL_HEIGHT = 32;
static const int CHAIN_LEN    = 2;

// Rows 0..kSplashHeight-1 may show boot art; hold prevents game blit until armed window ends.
static bool g_boot_splash_rows_hold_enabled_ = false;
static int64_t boot_splash_hold_until_us_ = 0;

bool hub75_boot_splash_hold_active() {
    if (!g_boot_splash_rows_hold_enabled_) return false;
    if (boot_splash_hold_until_us_ == 0) return false;
    return (int64_t)esp_timer_get_time() < boot_splash_hold_until_us_;
}

void hub75_arm_boot_splash_hold(uint32_t duration_ms) {
    if (!g_boot_splash_rows_hold_enabled_) return;
    boot_splash_hold_until_us_ =
        (int64_t)esp_timer_get_time() + (int64_t)duration_ms * 1000LL;
}

static SemaphoreHandle_t s_grp_splash_done;

static void grp_splash_worker(void * /*arg*/) {
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());
    grp_title_splash_build_cache_if_needed(kGrpPath, kSplashCachePath);
    esp_task_wdt_delete(xTaskGetCurrentTaskHandle());
    xSemaphoreGive(s_grp_splash_done);
    vTaskDelete(nullptr);
}

void Hub75Matrix::setup() {
    // SD mounts at IO (900) before HARDWARE (800). Run GRP→SD cache off loopTask — large stack + FILE
    // I/O was overflowing ESPHome's loopTask stack; dedicated task has 16KB.
    s_grp_splash_done = xSemaphoreCreateBinary();
    if (s_grp_splash_done == nullptr) {
        ESP_LOGE(TAG, "grp_splash sem failed — running cache build inline (risky)");
        grp_title_splash_build_cache_if_needed(kGrpPath, kSplashCachePath);
    } else if (xTaskCreate(grp_splash_worker, "grpSplash", 16384, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "grp_splash task create failed — running cache build inline");
        grp_title_splash_build_cache_if_needed(kGrpPath, kSplashCachePath);
    } else if (xSemaphoreTake(s_grp_splash_done, pdMS_TO_TICKS(120000)) != pdTRUE) {
        ESP_LOGE(TAG, "grp_splash task timed out (120s)");
    }

    const size_t buf_size = WIDTH * HEIGHT * sizeof(Color);
    size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snprintf(hub75_init_status, sizeof(hub75_init_status),
             "spiram_free=%u internal_free=%u", spiram_free, internal_free);
    ESP_LOGI(TAG, "PSRAM free: %u, internal free: %u", spiram_free, internal_free);

    back_buf_ = (Color*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!back_buf_) {
        ESP_LOGE(TAG, "PSRAM alloc failed (%u bytes) — falling back to internal RAM", buf_size);
        back_buf_ = (Color*)heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!back_buf_) {
            snprintf(hub75_init_status, sizeof(hub75_init_status),
                     "FAIL: no RAM (spiram=%u internal=%u)", spiram_free, internal_free);
            ESP_LOGE(TAG, "Internal RAM alloc also failed");
            mark_failed();
            return;
        }
        snprintf(hub75_init_status, sizeof(hub75_init_status), "back_buf=internal_ram");
        ESP_LOGW(TAG, "Using internal RAM for back buffer");
    }
    for (int i = 0; i < WIDTH * HEIGHT; i++) back_buf_[i] = Color{};

    HUB75_I2S_CFG cfg(PANEL_WIDTH, PANEL_HEIGHT, CHAIN_LEN,
                      const_cast<HUB75_I2S_CFG::i2s_pins&>(MATRIX_PINS));
    cfg.clkphase = false;
    cfg.driver   = HUB75_I2S_CFG::SHIFTREG;

    snprintf(hub75_init_status, sizeof(hub75_init_status), "calling begin()");
    matrix_lib = new MatrixPanel_I2S_DMA(cfg);
    if (!matrix_lib->begin()) {
        snprintf(hub75_init_status, sizeof(hub75_init_status), "FAIL: begin() returned false");
        ESP_LOGE(TAG, "MatrixPanel_I2S_DMA::begin() failed");
        delete matrix_lib;
        matrix_lib = nullptr;
        mark_failed();
        return;
    }
    matrix_lib->setBrightness8(64); // 1/16 scan is 2x brighter than 1/32; halve brightness

    const char* splash_source = "solid_fill";
    const uint8_t* splash_rgb = nullptr;
    uint8_t* splash_rgb_sd =
        static_cast<uint8_t*>(heap_caps_malloc(kSplashRgbBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (splash_rgb_sd && load_sd_splash_cache(splash_rgb_sd, kSplashRgbBytes)) {
        splash_source = "sd_cache";
        splash_rgb = splash_rgb_sd;
    }
#ifdef HUB75_HAS_SPLASH
    else {
        splash_source = "compiled_png";
        splash_rgb = kSplashRgb;
        ESP_LOGW(TAG, "using compiled splash_image — SD cache missing or stale");
    }
#endif
    g_boot_splash_rows_hold_enabled_ = (splash_rgb != nullptr);
    ESP_LOGI(TAG, "hub75: splash source=%s", splash_source);

    // Boot frame: optional 64×kSplashHeight splash (rows 0..39); rows 40..63 black for HUD/weather;
    // or full-panel dim fill when no splash art.
    for (int ly = 0; ly < HEIGHT; ly++) {
        for (int lx = 0; lx < WIDTH; lx++) {
            Color c;
            if (splash_rgb != nullptr) {
                if (ly < kSplashHeight) {
                    const size_t idx = (static_cast<size_t>(ly) * WIDTH + static_cast<size_t>(lx)) * 3;
                    c = Color{splash_rgb[idx + 0], splash_rgb[idx + 1], splash_rgb[idx + 2]};
                } else {
                    c = Color{0, 0, 0};
                }
            } else {
                c = Color{10, 12, 18};
            }
            back_buf_[ly * WIDTH + lx] = c;
            int vx = (ly >= 32) ? (63 - lx) : (lx + 64);
            int vy = (ly >= 32) ? (63 - ly) : ly;
            matrix_lib->drawPixelRGB888(vx, vy, c.r, c.g, c.b);
        }
    }
    if (splash_rgb_sd) heap_caps_free(splash_rgb_sd);

    // Flush D-cache so GDMA sees the pixel data we just wrote to internal SRAM.
    Cache_WriteBack_All();

    global_hub75 = this;
    snprintf(hub75_init_status, sizeof(hub75_init_status), "OK");
    ESP_LOGI(TAG, "HUB-75 64x64 initialized (kSplashHeight=%d)", kSplashHeight);
}

void Hub75Matrix::set_pixel(int x, int y, Color c) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    back_buf_[y * WIDTH + x] = c;
}

void Hub75Matrix::fill(Color c) {
    for (int i = 0; i < WIDTH * HEIGHT; i++) back_buf_[i] = c;
}

void Hub75Matrix::swap_buffers() {
    // U-chain transform: maps logical 64x64 back buffer → physical 128x32 DMA canvas.
    // Bottom half (ly>=32) → panel 1 (physical bottom, normal): vx=lx,     vy=ly-32
    // Top half    (ly< 32) → panel 2 (physical top, 180° rot): vx=127-lx,  vy=31-ly
    for (int ly = 0; ly < HEIGHT; ly++) {
        for (int lx = 0; lx < WIDTH; lx++) {
            Color& c = back_buf_[ly * WIDTH + lx];
            int vx = (ly >= 32) ? (63 - lx)  : (lx + 64);
            int vy = (ly >= 32) ? (63 - ly)  : ly;
            matrix_lib->drawPixelRGB888(vx, vy, c.r, c.g, c.b);
        }
    }
    // Cache_WriteBack_All() removed: calling it from Core 1 while Core 0 runs the
    // HUB75 ISR caused InstrFetchProhibited (NULL fn ptr) on Core 0. The HUB75 library
    // writes pixels via updateMatrixDMABuffer() which already handles its own coherency.
}

// --- GRP → SD splash (same TU: ESPHome only compiles hub75_matrix.cpp) ----------
static const char *const TAG_GRP_SPLASH = "grp_splash";

static uint8_t s_grp_pal6[768];
static uint8_t s_grp_titlepal6[768];
static uint8_t s_grp_drealms6[768];
static uint8_t s_grp_pal8[768];

static constexpr int kSplashPicnumPriority[] = {3281, 2456, 2492, 2493};
static int grp_splash_picnum_priority(int pic) {
    constexpr size_t n = sizeof(kSplashPicnumPriority) / sizeof(kSplashPicnumPriority[0]);
    for (size_t i = 0; i < n; i++) {
        if (pic == kSplashPicnumPriority[i])
            return static_cast<int>(i);
    }
    return -1;
}
static constexpr int kSplashOutW = 64;
static constexpr int kSplashOutH = 40;
static constexpr size_t kSplashOutRgb = static_cast<size_t>(kSplashOutW) * kSplashOutH * 3;

static uint32_t grp_splash_read_le_u32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0])) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static bool grp_splash_name_is_art(const char name[12]) {
    char u[13] = {};
    for (int i = 0; i < 12; i++) {
        char c = name[i];
        u[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
    }
    u[12] = '\0';
    if (strncmp(u, "TILES", 5) != 0) return false;
    const char *dot = strrchr(u, '.');
    return dot && strcmp(dot, ".ART") == 0;
}

static bool grp_splash_name_is_lookup_dat(const char name[12]) {
    static const char want[] = "LOOKUP.DAT";
    for (size_t i = 0; i < sizeof(want) - 1; i++) {
        char g = name[i];
        if (g >= 'a' && g <= 'z') g = static_cast<char>(g - 32);
        if (g != want[i]) return false;
    }
    for (size_t i = sizeof(want) - 1; i < 12; i++) {
        char g = name[i];
        if (g != '\0' && g != ' ') return false;
    }
    return true;
}

static bool grp_splash_name_is_palette_dat(const char name[12]) {
    // GRP names are 12 bytes and may be padded with NULs (or spaces).
    static const char want[] = "PALETTE.DAT";
    for (size_t i = 0; i < sizeof(want) - 1; i++) {
        char g = name[i];
        if (g >= 'a' && g <= 'z') g = static_cast<char>(g - 32);
        if (g != want[i]) return false;
    }
    // Remaining bytes must be padding.
    for (size_t i = sizeof(want) - 1; i < 12; i++) {
        char g = name[i];
        if (g != '\0' && g != ' ') return false;
    }
    return true;
}

static bool grp_splash_cache_hdr_ok(FILE *f_cache, uint32_t grp_size) {
    uint8_t h[16];
    if (fread(h, 1, 16, f_cache) != 16) return false;
    if (memcmp(h, kSplashCacheMagic, 8) != 0) return false;
    if (grp_splash_read_le_u32(h + 12) != 0) return false;
    return grp_splash_read_le_u32(h + 8) == grp_size;
}

static void grp_splash_pal6_to_rgb888(const uint8_t *pal6, uint8_t *pal8) {
    for (int i = 0; i < 768; i++)
        pal8[i] = static_cast<uint8_t>((static_cast<unsigned>(pal6[i]) * 255u + 31u) / 63u);
}

// LOOKUP.DAT: after lookup tables, waterpal, slimepal — then titlepal, drealms (see premap.c genspriteremaps).
static bool grp_splash_load_lookup_title_drealms(FILE *fp, long data_off, uint32_t file_size) {
    if (file_size < 2305)
        return false;
    if (fseek(fp, data_off, SEEK_SET) != 0)
        return false;
    uint8_t numl = 0;
    if (fread(&numl, 1, 1, fp) != 1)
        return false;
    uint32_t after_lookups = 1u + static_cast<uint32_t>(numl) * 257u;
    if (static_cast<uint64_t>(after_lookups) + 3072ull > static_cast<uint64_t>(file_size))
        return false;
    if (fseek(fp, data_off + static_cast<long>(after_lookups) + 1536, SEEK_SET) != 0)
        return false;
    if (fread(s_grp_titlepal6, 1, 768, fp) != 768)
        return false;
    if (fread(s_grp_drealms6, 1, 768, fp) != 768)
        return false;
    return true;
}

static void grp_splash_downscale(const uint8_t *src, int sw, int sh, const uint8_t *pal8,
                                 uint8_t *out_rgb) {
    // ART tile pixels are column-major (same as tiles.c loadtile): index = x * height + y.
    for (int oy = 0; oy < kSplashOutH; oy++) {
        int sy = (oy * sh) / kSplashOutH;
        if (sy >= sh) sy = sh - 1;
        for (int ox = 0; ox < kSplashOutW; ox++) {
            int sx = (ox * sw) / kSplashOutW;
            if (sx >= sw) sx = sw - 1;
            uint8_t idx = src[sx * sh + sy];
            size_t o = (static_cast<size_t>(oy) * kSplashOutW + static_cast<size_t>(ox)) * 3;
            out_rgb[o + 0] = pal8[idx * 3 + 0];
            out_rgb[o + 1] = pal8[idx * 3 + 1];
            out_rgb[o + 2] = pal8[idx * 3 + 2];
        }
    }
}

bool grp_title_splash_build_cache_if_needed(const char *grp_path, const char *cache_path) {
    FILE *fp = fopen(grp_path, "rb");
    if (!fp) {
        ESP_LOGW(TAG_GRP_SPLASH, "GRP open failed: %s", grp_path);
        return false;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long gsz = ftell(fp);
    fclose(fp);
    if (gsz < 0) return false;
    uint32_t grp_size = static_cast<uint32_t>(gsz);

    FILE *fc = fopen(cache_path, "rb");
    if (fc) {
        bool ok = grp_splash_cache_hdr_ok(fc, grp_size);
        fclose(fc);
        if (ok) {
            ESP_LOGI(TAG_GRP_SPLASH, "cache up to date (grp_size=%u)", static_cast<unsigned>(grp_size));
            return true;
        }
    }

    ESP_LOGI(TAG_GRP_SPLASH, "building splash cache from GRP (%u bytes) ...",
             static_cast<unsigned>(grp_size));

    fp = fopen(grp_path, "rb");
    if (!fp) return false;

    char magic[12];
    if (fread(magic, 1, 12, fp) != 12 || memcmp(magic, "KenSilverman", 12) != 0) {
        ESP_LOGE(TAG_GRP_SPLASH, "invalid GRP magic");
        fclose(fp);
        return false;
    }

    uint32_t num_files = 0;
    if (fread(&num_files, 4, 1, fp) != 1 || num_files == 0 || num_files > 4096) {
        ESP_LOGE(TAG_GRP_SPLASH, "bad GRP file count");
        fclose(fp);
        return false;
    }

    uint32_t *sizes = static_cast<uint32_t *>(
        heap_caps_malloc(num_files * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    char(*names)[12] = static_cast<char(*)[12]>(
        heap_caps_malloc(num_files * 12, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!sizes || !names) {
        if (sizes) heap_caps_free(sizes);
        if (names) heap_caps_free(names);
        fclose(fp);
        ESP_LOGE(TAG_GRP_SPLASH, "directory alloc failed");
        return false;
    }

    for (uint32_t i = 0; i < num_files; i++) {
        fread(names[i], 1, 12, fp);
        fread(&sizes[i], 4, 1, fp);
    }

    bool have_pal = false;
    bool have_lookup_pals = false;
    uint32_t data_off = static_cast<uint32_t>(ftell(fp));

    for (uint32_t fi = 0; fi < num_files; fi++) {
        esp_task_wdt_reset();
        if (sizes[fi] >= 768 && grp_splash_name_is_palette_dat(names[fi])) {
            fseek(fp, data_off, SEEK_SET);
            if (fread(s_grp_pal6, 1, 768, fp) == 768)
                have_pal = true;
        } else if (grp_splash_name_is_lookup_dat(names[fi])) {
            if (grp_splash_load_lookup_title_drealms(fp, static_cast<long>(data_off), sizes[fi]))
                have_lookup_pals = true;
            else
                ESP_LOGW(TAG_GRP_SPLASH, "LOOKUP.DAT present but parse failed (size=%u)", sizes[fi]);
        }
        data_off += sizes[fi];
    }

    if (!have_pal) {
        ESP_LOGE(TAG_GRP_SPLASH, "PALETTE.DAT not found in GRP");
        heap_caps_free(names);
        heap_caps_free(sizes);
        fclose(fp);
        return false;
    }
    if (!have_lookup_pals)
        ESP_LOGW(TAG_GRP_SPLASH, "LOOKUP.DAT missing — title/menu tiles use PALETTE.DAT (colors may be wrong)");

    uint8_t *tilebuf = static_cast<uint8_t *>(
        heap_caps_malloc(320 * 200, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!tilebuf) {
        ESP_LOGE(TAG_GRP_SPLASH, "tile buffer alloc failed");
        heap_caps_free(names);
        heap_caps_free(sizes);
        fclose(fp);
        return false;
    }

    int best_pri = 999;
    int best_pic = -1;
    int tw = 0, th = 0;
    int chosen_pic = -1;
    int fallback_pic = -1;
    data_off = 16 + num_files * 16;

    for (uint32_t fi = 0; fi < num_files; fi++) {
        esp_task_wdt_reset();
        if (!grp_splash_name_is_art(names[fi])) {
            data_off += sizes[fi];
            continue;
        }

        fseek(fp, data_off, SEEK_SET);
        uint32_t av = 0, an = 0, ts = 0, te = 0;
        fread(&av, 4, 1, fp);
        fread(&an, 4, 1, fp);
        fread(&ts, 4, 1, fp);
        fread(&te, 4, 1, fp);

        int32_t cnt = (av == 1 && te >= ts) ? static_cast<int32_t>(te - ts + 1) : 0;
        // Some ART packs can have more than 1024 tiles; allocate exact-sized arrays per file.
        if (cnt <= 0 || cnt > 8192) {
            fseek(fp, static_cast<long>(sizes[fi]) - 16, SEEK_CUR);
            data_off += sizes[fi];
            continue;
        }

        uint16_t *tsx = static_cast<uint16_t *>(
            heap_caps_malloc(static_cast<size_t>(cnt) * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        uint16_t *tsy = static_cast<uint16_t *>(
            heap_caps_malloc(static_cast<size_t>(cnt) * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!tsx || !tsy) {
            if (tsx) heap_caps_free(tsx);
            if (tsy) heap_caps_free(tsy);
            ESP_LOGE(TAG_GRP_SPLASH, "ART dims alloc failed (cnt=%d)", (int)cnt);
            heap_caps_free(tilebuf);
            heap_caps_free(names);
            heap_caps_free(sizes);
            fclose(fp);
            return false;
        }

        fread(tsx, 2, static_cast<size_t>(cnt), fp);
        fread(tsy, 2, static_cast<size_t>(cnt), fp);
        fseek(fp, cnt * 4, SEEK_CUR);

        bool found_best = false;
        for (int32_t i = 0; i < cnt; i++) {
            int w = tsx[i], h = tsy[i];
            int pic = static_cast<int>(ts + i);
            if (w <= 0 || h <= 0) continue;

            int pri = grp_splash_picnum_priority(pic);
            if (pri >= 0) {
                if (pri < best_pri) {
                    if (static_cast<size_t>(w) * static_cast<size_t>(h) > 320 * 200) {
                        ESP_LOGE(TAG_GRP_SPLASH, "splash tile too large (%d×%d)", w, h);
                        heap_caps_free(tsx);
                        heap_caps_free(tsy);
                        heap_caps_free(tilebuf);
                        heap_caps_free(names);
                        heap_caps_free(sizes);
                        fclose(fp);
                        return false;
                    }
                    if (fread(tilebuf, 1, static_cast<size_t>(w) * static_cast<size_t>(h), fp) !=
                        static_cast<size_t>(w) * static_cast<size_t>(h)) {
                        ESP_LOGE(TAG_GRP_SPLASH, "short read splash tile");
                        heap_caps_free(tsx);
                        heap_caps_free(tsy);
                        heap_caps_free(tilebuf);
                        heap_caps_free(names);
                        heap_caps_free(sizes);
                        fclose(fp);
                        return false;
                    }
                    best_pri = pri;
                    best_pic = pic;
                    tw = w;
                    th = h;
                    if (best_pri == 0)
                        found_best = true;
                } else {
                    fseek(fp, static_cast<long>(w) * static_cast<long>(h), SEEK_CUR);
                }
                continue;
            }

            const bool is_fallback_320x200 =
                (fallback_pic < 0 && best_pri == 999 && w == 320 && h == 200);
            if (!is_fallback_320x200) {
                fseek(fp, static_cast<long>(w) * static_cast<long>(h), SEEK_CUR);
                continue;
            }
            if (static_cast<size_t>(w) * static_cast<size_t>(h) > 320 * 200) {
                ESP_LOGE(TAG_GRP_SPLASH, "splash tile too large (%d×%d)", w, h);
                heap_caps_free(tsx);
                heap_caps_free(tsy);
                heap_caps_free(tilebuf);
                heap_caps_free(names);
                heap_caps_free(sizes);
                fclose(fp);
                return false;
            }
            if (fread(tilebuf, 1, static_cast<size_t>(w) * static_cast<size_t>(h), fp) !=
                static_cast<size_t>(w) * static_cast<size_t>(h)) {
                ESP_LOGE(TAG_GRP_SPLASH, "short read splash tile");
                heap_caps_free(tsx);
                heap_caps_free(tsy);
                heap_caps_free(tilebuf);
                heap_caps_free(names);
                heap_caps_free(sizes);
                fclose(fp);
                return false;
            }
            tw = w;
            th = h;
            fallback_pic = pic;
        }

        heap_caps_free(tsx);
        heap_caps_free(tsy);

        data_off += sizes[fi];
        if (found_best) break;
    }

    heap_caps_free(names);
    heap_caps_free(sizes);
    fclose(fp);

    if (best_pri < 999) {
        chosen_pic = best_pic;
    } else if (fallback_pic >= 0) {
        chosen_pic = fallback_pic;
        ESP_LOGW(TAG_GRP_SPLASH,
                 "preferred splash picnums not found; using fallback picnum %d (first 320×200)",
                 chosen_pic);
    } else {
        ESP_LOGE(TAG_GRP_SPLASH, "no splash tile (LOADSCREEN/MENUSCREEN/DREALMS/BETASCREEN / 320×200) in GRP");
        heap_caps_free(tilebuf);
        return false;
    }

    uint8_t *out_rgb = static_cast<uint8_t *>(heap_caps_malloc(kSplashOutRgb, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!out_rgb)
        out_rgb = static_cast<uint8_t *>(heap_caps_malloc(kSplashOutRgb, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!out_rgb) {
        ESP_LOGE(TAG_GRP_SPLASH, "out RGB alloc failed");
        heap_caps_free(tilebuf);
        return false;
    }

    const uint8_t *pal6 = s_grp_pal6;
    if (have_lookup_pals) {
        if (chosen_pic == 2492)
            pal6 = s_grp_drealms6;
        else if (chosen_pic == 2456 || chosen_pic == 2493)
            pal6 = s_grp_titlepal6;
    } else if (chosen_pic == 2456 || chosen_pic == 2492 || chosen_pic == 2493) {
        ESP_LOGW(TAG_GRP_SPLASH, "splash picnum %d expects LOOKUP.DAT palettes", chosen_pic);
    }
    grp_splash_pal6_to_rgb888(pal6, s_grp_pal8);
    grp_splash_downscale(tilebuf, tw, th, s_grp_pal8, out_rgb);
    heap_caps_free(tilebuf);

    const char *final_path = cache_path;
    char tmp_path[96];
    // Keep temp name 8.3-safe too.
    if (strcmp(final_path, kSplashCacheAltPath) == 0) {
        snprintf(tmp_path, sizeof(tmp_path), "/sdcard/LOADSCR.TMP");
    } else {
        snprintf(tmp_path, sizeof(tmp_path), "/sdcard/duke3d/LOADSCR.TMP");
    }

    if (strncmp(final_path, "/sdcard/duke3d/", 14) == 0) {
        const char *dir = "/sdcard/duke3d";
        if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG_GRP_SPLASH, "mkdir(%s) failed (errno=%d)", dir, errno);
        }
    }

    FILE *out = fopen(tmp_path, "wb");
    if (!out) {
        int e = errno;
        ESP_LOGE(TAG_GRP_SPLASH, "open %s for write failed (errno=%d)", tmp_path, e);
        if (e == EINVAL) {
            final_path = kSplashCacheAltPath;
            snprintf(tmp_path, sizeof(tmp_path), "/sdcard/LOADSCR.TMP");
            ESP_LOGW(TAG_GRP_SPLASH, "falling back to %s", final_path);
            out = fopen(tmp_path, "wb");
        }
        if (!out) {
            ESP_LOGE(TAG_GRP_SPLASH, "open fallback %s failed (errno=%d)", tmp_path, errno);
            heap_caps_free(out_rgb);
            return false;
        }
    }

    uint8_t hdr[16];
    memcpy(hdr, kSplashCacheMagic, 8);
    memcpy(hdr + 8, &grp_size, 4);
    uint32_t z = 0;
    memcpy(hdr + 12, &z, 4);

    bool wok = (fwrite(hdr, 1, 16, out) == 16 && fwrite(out_rgb, 1, kSplashOutRgb, out) == kSplashOutRgb);
    fclose(out);
    heap_caps_free(out_rgb);

    if (!wok) {
        ESP_LOGE(TAG_GRP_SPLASH, "write splash tmp failed");
        remove(tmp_path);
        return false;
    }

    remove(final_path);
    if (rename(tmp_path, final_path) != 0) {
        int e = errno;
        ESP_LOGE(TAG_GRP_SPLASH, "rename %s -> %s failed (errno=%d)", tmp_path, final_path, e);
        remove(tmp_path);
        return false;
    }

    ESP_LOGI(TAG_GRP_SPLASH, "wrote splash cache (picnum %d %d×%d → %d×%d RGB)", chosen_pic, tw, th,
             kSplashOutW, kSplashOutH);
    return true;
}

}  // namespace hub75_matrix
}  // namespace esphome

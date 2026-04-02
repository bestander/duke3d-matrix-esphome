#include "sd_card.h"
#include "sd_open_trace.h"
#include "esphome/core/log.h"
#include "driver/gpio.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "sdmmc_cmd.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace esphome {
namespace sd_card {

static const char* TAG = "sd_card";
const char* SdCard::MOUNT_POINT = "/sdcard";
SdCard* global_sd_card = nullptr;

void SdCard::setup() {
    sd_open_trace_set(open_trace_);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 20000;  // 20 MHz — max for SDSPI mode

    spi_bus_config_t bus = {};
    bus.mosi_io_num = mosi_;
    bus.miso_io_num = miso_;
    bus.sclk_io_num = sck_;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 32768;  // larger DMA buffer reduces per-transaction overhead

    esp_err_t ret = spi_bus_initialize(
        static_cast<spi_host_device_t>(host.slot), &bus, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        mark_failed();
        return;
    }

    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs   = static_cast<gpio_num_t>(cs_);
    dev.host_id   = static_cast<spi_host_device_t>(host.slot);

    esp_vfs_fat_sdmmc_mount_config_t mnt = {};
    mnt.format_if_mount_failed = false;
    mnt.max_files = 16;  /* streaming audio opens up to 9 concurrent fds (8 voices + 1 grp_stream) */
    mnt.allocation_unit_size = 16 * 1024;

    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &dev, &mnt, &card_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        mark_failed();
        return;
    }

    mounted_ = true;
    global_sd_card = this;
    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
    sdmmc_card_print_info(stdout, card_);

    if (benchmark_) {
        run_io_benchmark_();
    }
}

void SdCard::run_io_benchmark_() const {
    FILE* f = open("duke3d/DUKE3D.GRP", "rb");
    if (!f) {
        ESP_LOGW(TAG, "SD benchmark: duke3d/DUKE3D.GRP not found — skipped");
        return;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        ESP_LOGW(TAG, "SD benchmark: fseek end failed");
        fclose(f);
        return;
    }
    const long sz_long = ftell(f);
    if (sz_long < 8192) {
        ESP_LOGW(TAG, "SD benchmark: file too small (%ld)", sz_long);
        fclose(f);
        return;
    }
    const size_t file_sz = static_cast<size_t>(sz_long);

    constexpr size_t kChunk = 32768;
    constexpr size_t kSeqCap = 4 * 1024 * 1024;
    std::vector<uint8_t> buf(kChunk);

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return;
    }

    const size_t seq_read = std::min(file_sz, kSeqCap);
    const int64_t t_seq0 = esp_timer_get_time();
    size_t got = 0;
    while (got < seq_read) {
        const size_t want = std::min(kChunk, seq_read - got);
        const size_t n = fread(buf.data(), 1, want, f);
        if (n == 0)
            break;
        got += n;
    }
    const int64_t t_seq1 = esp_timer_get_time();
    const double seq_s = (t_seq1 - t_seq0) / 1000000.0;
    const double seq_mib = static_cast<double>(got) / (1024.0 * 1024.0);
    const double seq_mibs = seq_s > 0.0 ? seq_mib / seq_s : 0.0;
    ESP_LOGI(TAG,
             "SD bench sequential: %.2f MiB (%zu B) in %.3f s => %.2f MiB/s (fread %zu B/chunk)",
             seq_mib, got, seq_s, seq_mibs, kChunk);

    constexpr int kRandomRounds = 800;
    constexpr size_t kBlock = 512;
    if (file_sz < kBlock + 1) {
        fclose(f);
        return;
    }
    const uint32_t span = static_cast<uint32_t>(file_sz - kBlock);

    uint8_t rbuf[kBlock];
    const int64_t t_rnd0 = esp_timer_get_time();
    for (int i = 0; i < kRandomRounds; i++) {
        const long off = static_cast<long>(esp_random() % span);
        if (fseek(f, off, SEEK_SET) != 0)
            continue;
        fread(rbuf, 1, kBlock, f);
    }
    const int64_t t_rnd1 = esp_timer_get_time();
    const double rnd_s = (t_rnd1 - t_rnd0) / 1000000.0;
    const size_t rnd_bytes = static_cast<size_t>(kRandomRounds) * kBlock;
    const double rnd_mib = static_cast<double>(rnd_bytes) / (1024.0 * 1024.0);
    const double rnd_mibs = rnd_s > 0.0 ? rnd_mib / rnd_s : 0.0;
    const double ops_per_s = rnd_s > 0.0 ? static_cast<double>(kRandomRounds) / rnd_s : 0.0;
    const double us_per_op = static_cast<double>(t_rnd1 - t_rnd0) / static_cast<double>(kRandomRounds);

    ESP_LOGI(TAG,
             "SD bench random: %d x seek+%zu B read in %.3f s => %.3f MiB/s, %.0f ops/s, %.0f us/op",
             kRandomRounds, kBlock, rnd_s, rnd_mibs, ops_per_s, us_per_op);

    fclose(f);
}

FILE* SdCard::open(const char* rel_path, const char* mode) const {
    if (!mounted_) return nullptr;
    char full[256];
    snprintf(full, sizeof(full), "%s/%s", MOUNT_POINT, rel_path);
    return fopen(full, mode);
}

bool SdCard::grp_present() const {
    FILE* f = open("duke3d/DUKE3D.GRP", "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

}  // namespace sd_card
}  // namespace esphome

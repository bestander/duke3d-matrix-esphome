#include "sd_card.h"
#include "esphome/core/log.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#include <cstring>

namespace esphome {
namespace sd_card {

static const char* TAG = "sd_card";
const char* SdCard::MOUNT_POINT = "/sdcard";
SdCard* global_sd_card = nullptr;

void SdCard::setup() {
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
    mnt.max_files = 8;
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

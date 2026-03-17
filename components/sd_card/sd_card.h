#pragma once
#include "esphome/core/component.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include <cstdio>

namespace esphome {
namespace sd_card {

class SdCard : public Component {
public:
    static const char* MOUNT_POINT;  // "/sdcard"

    // Pins — set before setup() via esphome.yaml config
    void set_mosi(int p) { mosi_ = p; }
    void set_miso(int p) { miso_ = p; }
    void set_sck(int p)  { sck_  = p; }
    void set_cs(int p)   { cs_   = p; }

    void setup() override;
    float get_setup_priority() const override { return setup_priority::IO; }

    bool is_mounted() const { return mounted_; }

    // Returns nullptr if path not found; caller must fclose()
    FILE* open(const char* rel_path, const char* mode) const;

    // Returns true if /sdcard/duke3d/DUKE3D.GRP exists and is readable
    bool grp_present() const;

private:
    int mosi_ = -1, miso_ = -1, sck_ = -1, cs_ = -1;
    bool mounted_ = false;
    sdmmc_card_t* card_ = nullptr;
};

extern SdCard* global_sd_card;

}  // namespace sd_card
}  // namespace esphome

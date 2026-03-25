#include "i2s_audio.h"
#include "esphome/core/log.h"

namespace esphome {
namespace i2s_audio {

static const char* TAG = "i2s_audio";
I2SAudio* global_i2s = nullptr;

void I2SAudio::setup() {
    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate          = 44100;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = 8;
    cfg.dma_buf_len          = 64;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = true;
    cfg.fixed_mclk           = 0;

    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) {
        ESP_LOGE(TAG, "I2S driver install failed");
        mark_failed();
        return;
    }

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = I2S_PIN_NO_CHANGE;
    pins.bck_io_num   = bclk_;
    pins.ws_io_num    = lrclk_;
    pins.data_out_num = din_;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;

    if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
        ESP_LOGE(TAG, "I2S set pin failed");
        mark_failed();
        return;
    }

    initialized_ = true;
    global_i2s = this;
    ESP_LOGI(TAG, "I2S audio initialized (44100Hz stereo 16-bit)");
}

void I2SAudio::write_pcm(const int16_t* buf, size_t num_bytes) {
    if (!initialized_) return;
    size_t written;
    i2s_write(I2S_NUM_0, buf, num_bytes, &written, portMAX_DELAY);
}

}  // namespace i2s_audio
}  // namespace esphome

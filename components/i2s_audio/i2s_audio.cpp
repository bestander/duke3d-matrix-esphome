#include "i2s_audio.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace i2s_audio {

static const char* TAG = "i2s_audio";
I2SAudio* global_i2s = nullptr;

void I2SAudio::setup() {
    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate          = 11025;  // matches Multivoc MixRate
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;  // stereo; mono samples duplicated L=R
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    /* Deeper ring: absorb brief mixer / SD stalls without I2S underrun (~few ms extra latency). */
    cfg.dma_buf_count        = 12;
    cfg.dma_buf_len          = 512;  // samples per channel per DMA block; stereo 16-bit ×12×512
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
    ESP_LOGD(TAG, "I2S audio ready: BCLK=%d LRC=%d DOUT=%d (11025 Hz stereo 16-bit)",
             bclk_, lrclk_, din_);
}

void I2SAudio::write_pcm(const int16_t* buf, size_t num_bytes) {
    if (!initialized_ || num_bytes == 0) return;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(buf);
    size_t remain = num_bytes;
    /* Runs only on the audio pump task (not the game). Block briefly so DMA can
     * drain; ticks_to_wait=0 drops almost all samples when the ring is full,
     * which sounds like a short click per buffer. */
    while (remain > 0) {
        size_t n = 0;
        esp_err_t err =
            i2s_write(I2S_NUM_0, p, remain, &n, pdMS_TO_TICKS(40));
        if (err != ESP_OK)
            break;
        if (n == 0) {
            vTaskDelay(1);
            continue;
        }
        p += n;
        remain -= n;
    }
    if (remain > 0) {
        static int s_short_warn;
        if (s_short_warn < 32) {
            s_short_warn++;
            ESP_LOGW("sound_trace", "i2s_write incomplete: %u of %u bytes left",
                     (unsigned)remain, (unsigned)num_bytes);
        }
    }
}

}  // namespace i2s_audio
}  // namespace esphome

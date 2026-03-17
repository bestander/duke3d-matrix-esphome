#include "i2s_audio.h"
#include "esphome/core/log.h"

namespace esphome {
namespace i2s_audio {

static const char* TAG = "i2s_audio";
I2SAudio* global_i2s = nullptr;

void I2SAudio::setup() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    i2s_new_channel(&chan_cfg, &tx_handle_, nullptr);

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = static_cast<gpio_num_t>(bclk_),
            .ws   = static_cast<gpio_num_t>(lrclk_),
            .dout = static_cast<gpio_num_t>(din_),
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {},  // zero-init bitfield struct (all inversions disabled)
        },
    };

    if (i2s_channel_init_std_mode(tx_handle_, &std_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed");
        mark_failed();
        return;
    }
    i2s_channel_enable(tx_handle_);
    global_i2s = this;
    ESP_LOGI(TAG, "I2S audio initialized (44100Hz stereo 16-bit)");
}

void I2SAudio::write_pcm(const int16_t* buf, size_t num_bytes) {
    if (!tx_handle_) return;
    size_t written;
    i2s_channel_write(tx_handle_, buf, num_bytes, &written, portMAX_DELAY);
}

}  // namespace i2s_audio
}  // namespace esphome

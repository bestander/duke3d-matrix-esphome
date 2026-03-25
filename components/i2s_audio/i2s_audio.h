#pragma once
#include "esphome/core/component.h"
#include "driver/i2s.h"
#include <cstdint>

namespace esphome {
namespace i2s_audio {

class I2SAudio : public Component {
public:
    void set_bclk(int p)  { bclk_  = p; }
    void set_lrclk(int p) { lrclk_ = p; }
    void set_din(int p)   { din_   = p; }

    void setup() override;
    float get_setup_priority() const override { return setup_priority::IO; }

    // Write interleaved stereo 16-bit PCM.
    // num_bytes: total bytes to write (= num_frames * 2 channels * 2 bytes/sample).
    // Blocks until buffer is accepted by DMA.
    void write_pcm(const int16_t* buf, size_t num_bytes);

private:
    int bclk_ = -1, lrclk_ = -1, din_ = -1;
    bool initialized_ = false;
};

extern I2SAudio* global_i2s;

}  // namespace i2s_audio
}  // namespace esphome

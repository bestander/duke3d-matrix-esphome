#include <cassert>
#include <cstdint>
#include <cstddef>

// Verify byte-count convention: num_bytes = num_frames * channels * bytes_per_sample
void test_byte_count_for_stereo_16bit() {
    const int channels     = 2;
    const int bytes_sample = 2;         // int16_t
    const int frames       = 512;       // typical Duke3D audio chunk
    const size_t num_bytes = frames * channels * bytes_sample;
    assert(num_bytes == 2048);
}

void test_byte_count_for_mono_source_upsampled_to_stereo() {
    // Duke3D may output mono 11025Hz; host doubles to stereo 22050Hz
    const int frames       = 256;
    const size_t num_bytes = frames * 2 * 2;  // stereo, 16-bit
    assert(num_bytes == 1024);
}

int main() {
    test_byte_count_for_stereo_16bit();
    test_byte_count_for_mono_source_upsampled_to_stereo();
    return 0;
}

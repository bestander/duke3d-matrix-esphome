#define DUKE3D_HOST_TEST
#include <cassert>
#include <cstring>
#include "hub75_mock.h"

// Minimal palette for test
static const uint8_t TEST_PALETTE[256 * 3] = {};

// Forward-declare the function under test
void render_frame(Hub75Matrix& dst,
                  const uint8_t* src,   // 320x200 palette-indexed
                  const uint8_t* pal);  // 256xRGB888

void test_output_is_exactly_64x40_rows() {
    Hub75Matrix m;
    m.fill({0, 0, 255});  // blue background

    uint8_t src[320 * 200] = {};  // all palette index 0 -> black (palette is zero-filled)
    render_frame(m, src, TEST_PALETTE);

    // Rows 0-39 must have been written (will be black, not blue)
    for (int x = 0; x < 64; x++) {
        assert(m.pixels[0  * 64 + x].b == 0);  // row 0: was written (no longer blue)
        assert(m.pixels[39 * 64 + x].b == 0);  // row 39: was written
    }

    // Rows 40-63 must NOT have been touched (still blue)
    for (int x = 0; x < 64; x++)
        assert(m.pixels[40 * 64 + x].b == 255);
}

int main() {
    test_output_is_exactly_64x40_rows();
    return 0;
}

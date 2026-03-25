#include "renderer.h"

// Nearest-neighbour downscale: 320x200 -> 64x40
// x_src = x_dst * 320 / 64 = x_dst * 5
// y_src = y_dst * 200 / 40 = y_dst * 5
void render_frame(RenderTarget& dst,
                  const uint8_t* src,
                  const uint8_t* pal) {
    for (int y = 0; y < 40; y++) {
        int y_src = y * 5;
        for (int x = 0; x < 64; x++) {
            int x_src = x * 5;
            uint8_t idx = src[y_src * 320 + x_src];
            uint8_t r = pal[idx * 3 + 0];
            uint8_t g = pal[idx * 3 + 1];
            uint8_t b = pal[idx * 3 + 2];
#ifndef DUKE3D_HOST_TEST
            dst.set_pixel(x, y, esphome::hub75_matrix::Color(r, g, b));
#else
            dst.set_pixel(x, y, Color(r, g, b));
#endif
        }
    }
}

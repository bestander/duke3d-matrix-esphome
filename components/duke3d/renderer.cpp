#include "renderer.h"

// Direct 1:1 blit: 64x40 8-bit indexed -> HUB75 matrix pixels.
// The engine now renders at native matrix resolution so no downscale is needed.
void render_frame(RenderTarget& dst,
                  const uint8_t* src,
                  const uint8_t* pal) {
    for (int y = 0; y < 40; y++) {
        for (int x = 0; x < 64; x++) {
            uint8_t idx = src[y * 64 + x];
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

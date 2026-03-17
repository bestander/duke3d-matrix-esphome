#pragma once
#include <cstdint>

#ifndef DUKE3D_HOST_TEST
#  include "../../../hub75_matrix/hub75_matrix.h"
   using RenderTarget = esphome::hub75_matrix::Hub75Matrix;
#else
#  include "hub75_mock.h"
   using RenderTarget = Hub75Matrix;
#endif

// Downscale a 320x200 palette-indexed framebuffer to 64x40 and
// write it into rows 0-39 of dst. Rows 40-63 are untouched.
void render_frame(RenderTarget& dst,
                  const uint8_t* src_320x200,
                  const uint8_t* palette_rgb);  // 256 x 3 bytes (R,G,B)

#pragma once
#include <cstdint>
#include <cstring>

struct Color {
    uint8_t r, g, b;
    Color(uint8_t r=0, uint8_t g=0, uint8_t b=0) : r(r), g(g), b(b) {}
};

class Hub75Matrix {
public:
    static const int WIDTH = 64, HEIGHT = 64;
    Color pixels[HEIGHT * WIDTH] = {};

    void set_pixel(int x, int y, Color c) {
        if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT)
            pixels[y * WIDTH + x] = c;
    }
    void fill(Color c) { for (int i = 0; i < WIDTH * HEIGHT; i++) pixels[i] = c; }
    void swap_buffers() {}
};

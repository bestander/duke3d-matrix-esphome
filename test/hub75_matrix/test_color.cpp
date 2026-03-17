#include <cassert>
#include "hub75_mock.h"

void test_set_pixel_writes_correct_color() {
    Hub75Matrix m;
    m.set_pixel(10, 20, {255, 0, 128});
    assert(m.pixels[20 * 64 + 10].r == 255);
    assert(m.pixels[20 * 64 + 10].g == 0);
    assert(m.pixels[20 * 64 + 10].b == 128);
}

void test_set_pixel_out_of_bounds_does_not_crash() {
    Hub75Matrix m;
    m.set_pixel(-1, 0,  {255, 0, 0});
    m.set_pixel(0,  64, {255, 0, 0});
    m.set_pixel(64, 0,  {255, 0, 0});
    // No assertion needed — must not crash
}

void test_fill_sets_all_pixels() {
    Hub75Matrix m;
    m.fill({100, 200, 50});
    for (int i = 0; i < 64 * 64; i++) {
        assert(m.pixels[i].r == 100);
        assert(m.pixels[i].g == 200);
        assert(m.pixels[i].b == 50);
    }
}

int main() {
    test_set_pixel_writes_correct_color();
    test_set_pixel_out_of_bounds_does_not_crash();
    test_fill_sets_all_pixels();
    return 0;
}

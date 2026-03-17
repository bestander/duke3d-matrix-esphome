#define DUKE3D_HOST_TEST
#include <cassert>
#include "hub75_mock.h"
#include "hud.h"

void test_hud_does_not_touch_game_rows() {
    Hub75Matrix m;
    m.fill({255, 0, 0});  // all red

    esphome::hud::Hud hud;
    hud.set_temperature(22.5f);
    hud.set_condition("sunny");
    hud.render(m);

    // Rows 0–39 must remain untouched (all red)
    for (int y = 0; y < 40; y++)
        for (int x = 0; x < 64; x++) {
            assert(m.pixels[y * 64 + x].r == 255);
            assert(m.pixels[y * 64 + x].g == 0);
        }
}

void test_hud_draws_something_in_hud_rows() {
    Hub75Matrix m;
    m.fill({0, 0, 0});  // all black

    esphome::hud::Hud hud;
    hud.set_temperature(22.5f);
    hud.set_condition("sunny");
    hud.render(m);

    // At least one non-black pixel must appear in rows 40–63
    bool drew = false;
    for (int y = 40; y < 64 && !drew; y++)
        for (int x = 0; x < 64 && !drew; x++) {
            auto& p = m.pixels[y * 64 + x];
            if (p.r || p.g || p.b) drew = true;
        }
    assert(drew);
}

int main() {
    test_hud_does_not_touch_game_rows();
    test_hud_draws_something_in_hud_rows();
    return 0;
}

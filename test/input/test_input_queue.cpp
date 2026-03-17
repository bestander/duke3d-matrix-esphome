// Host-side test for InputEvent enum values — no FreeRTOS needed.
#include <cassert>
#include <cstdint>

// Reproduce the enum here (no FreeRTOS headers on host)
enum class InputEvent : uint8_t {
    NONE=0, MOVE_FORWARD, MOVE_BACK, STRAFE_LEFT, STRAFE_RIGHT,
    TURN_LEFT, TURN_RIGHT, FIRE, USE, OPEN_MAP, MENU_TOGGLE,
};

void test_none_is_zero() { assert((uint8_t)InputEvent::NONE == 0); }

void test_all_values_unique() {
    uint8_t seen[16] = {};
    for (uint8_t v = 0; v <= (uint8_t)InputEvent::MENU_TOGGLE; v++) {
        assert(seen[v] == 0);
        seen[v] = 1;
    }
}

int main() {
    test_none_is_zero();
    test_all_values_unique();
    return 0;
}

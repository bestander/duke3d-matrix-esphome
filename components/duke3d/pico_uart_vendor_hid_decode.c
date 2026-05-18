#include "pico_uart_vendor_hid_decode.h"

bool pico_uart_vendor_is_mp_gamepad_shape(const uint8_t *report, uint16_t len) {
    /* Same routing heuristic as TinyUSB Pico bridge: distinguish MatrixPortal gamepad blobs from HID boot keyboards. */
    return len >= 8u && report[0] == 0x01u && !(report[2] == 0u && report[1] < 0x40u);
}

static void hat_axes_to_cross_thresh(uint8_t ax, uint8_t ay, uint16_t *bits, uint8_t low_th, uint8_t high_th) {
    if (ay <= low_th) {
        *bits |= VENDOR_GP_CROSS_UP;
    }
    if (ay >= high_th) {
        *bits |= VENDOR_GP_CROSS_DOWN;
    }
    if (ax <= low_th) {
        *bits |= VENDOR_GP_CROSS_LEFT;
    }
    if (ax >= high_th) {
        *bits |= VENDOR_GP_CROSS_RIGHT;
    }
}

bool pico_uart_vendor_decode_matrixportal_buttons(const uint8_t *report, uint16_t len, uint16_t *out_bits) {
    if (!report || !out_bits) {
        return false;
    }

    uint8_t base = 0;
    if (len >= 8u && report[0] == 0x01u) {
        base = 1u;
    } else if (len < 7u) {
        return false;
    }

    if (len < (uint16_t)(base + 6u)) {
        return false;
    }

    const uint8_t ax = report[base + 2u];
    const uint8_t ay = report[base + 3u];
    const uint8_t b5 = report[base + 4u];
    const uint8_t b6 = report[base + 5u];

    uint16_t bits = 0;
    const uint8_t d5 = (uint8_t)(b5 ^ 0x0Fu);

    enum { HAT_LOW = 0x55u, HAT_HIGH = 0xAAu };

    if (d5 == 0u) {
        hat_axes_to_cross_thresh(ax, ay, &bits, HAT_LOW, HAT_HIGH);
    }

    if (d5 == 0x80u) bits |= VENDOR_GP_INV_MENU;
    if (d5 == 0x40u) bits |= VENDOR_GP_FIRE;
    if (d5 == 0x20u) bits |= VENDOR_GP_NEXT_WEAPON;
    if (d5 == 0x10u) bits |= VENDOR_GP_INV_NEXT;
    if (b6 & 0x01u) bits |= VENDOR_GP_JUMP;
    if (b6 & 0x02u) bits |= VENDOR_GP_CROUCH;
    if (b6 & 0x04u) bits |= VENDOR_GP_BUMPER_L;
    if (b6 & 0x08u) bits |= VENDOR_GP_OPEN;
    if (b6 & 0x10u) bits |= VENDOR_GP_STAR_HEART;
    if (b6 & 0x20u) bits |= VENDOR_GP_START;

    *out_bits = bits;
    return true;
}

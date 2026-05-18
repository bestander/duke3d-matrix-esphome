#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Buttons decoded from Adafruit MatrixPortal-style 8-byte gamepad HID reports.
 * (Same bit layout previously implemented on the Pico; now decoded on ESP only.)
 */
enum {
    VENDOR_GP_CROSS_UP = 1u << 0,
    VENDOR_GP_CROSS_DOWN = 1u << 1,
    VENDOR_GP_CROSS_LEFT = 1u << 2,
    VENDOR_GP_CROSS_RIGHT = 1u << 3,
    VENDOR_GP_BUMPER_L = 1u << 4,
    VENDOR_GP_FIRE = 1u << 5,
    VENDOR_GP_OPEN = 1u << 6,
    VENDOR_GP_JUMP = 1u << 7,
    VENDOR_GP_CROUCH = 1u << 8,
    VENDOR_GP_NEXT_WEAPON = 1u << 9,
    VENDOR_GP_INV_MENU = 1u << 10,
    VENDOR_GP_INV_NEXT = 1u << 11,
    VENDOR_GP_START = 1u << 12,
    /** Panel extras: YAML dash/heart (byte [6] bit 4); physical button often labeled star or heart. */
    VENDOR_GP_STAR_HEART = 1u << 13,
};

bool pico_uart_vendor_is_mp_gamepad_shape(const uint8_t *report, uint16_t len);

/** Decode report into button bitmask; false if payload too short / unknown layout. */
bool pico_uart_vendor_decode_matrixportal_buttons(const uint8_t *report, uint16_t len, uint16_t *out_bits);

#ifdef __cplusplus
}
#endif

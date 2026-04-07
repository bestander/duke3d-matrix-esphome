#pragma once
#include <cstdint>

// 5x7 bitmap font. Each entry is 5 column bytes; bit 0 = top row.
// Indices: 0-9 = digits, 10 = ':', 11 = ' ', 12 = degree, 13 = 'C'
static const uint8_t FONT_5X7[][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
    {0x00, 0x36, 0x36, 0x00, 0x00}, // ':'  (index 10)
    {0x00, 0x00, 0x00, 0x00, 0x00}, // ' '  (index 11)
    {0x00, 0x07, 0x05, 0x07, 0x00}, // 'deg' (index 12) — referenced by FONT_IDX_DEGREE
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // 'C'    (index 13)
    {0x04, 0x06, 0x3F, 0x06, 0x04}, // ↑ up   (index 14)
    {0x08, 0x18, 0x3F, 0x18, 0x08}, // ↓ down (index 15)
    {0x08, 0x04, 0x08, 0x04, 0x08}, // ~ wave (index 16)
    // BLE / Bluetooth indicator (index 17)
    // 5×5 symbol centred in the 7-row slot (1 row padding top & bottom):
    //   .X.     (rows 1-5 used; rows 0,6 blank)
    //   .XX
    //   XX.
    //   .XX
    //   .X.
    {0x08, 0x3E, 0x14, 0x00, 0x00}, // BLE icon (index 17)
};

// Use integer constants instead of char codes to avoid UTF-8/Latin-1 ambiguity.
static const int FONT_IDX_COLON  = 10;
static const int FONT_IDX_SPACE  = 11;
static const int FONT_IDX_DEGREE = 12;
static const int FONT_IDX_C      = 13;
static const int FONT_IDX_UP     = 14;
static const int FONT_IDX_DOWN   = 15;
static const int FONT_IDX_WAVE   = 16;
static const int FONT_IDX_BLE    = 17;

inline int font_index(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == ':') return FONT_IDX_COLON;
    if (c == 'C') return FONT_IDX_C;
    return FONT_IDX_SPACE;
}

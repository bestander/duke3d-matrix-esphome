#include "ble_gamepad.h"
#include "esp_log.h"
#include <atomic>
#include <cstdio>
#include <string.h>

static const char *TAG = "ble_gamepad";

// Axis deadzone thresholds — same as usb_gamepad defaults.
#define AXIS_DEAD_LO  64
#define AXIS_DEAD_HI  192

// Max report size we track for bit-diff logging.
#define MAX_REPORT_BYTES 32

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum Btn : uint32_t {
    BTN_FORWARD      = 1 << 0,
    BTN_BACK         = 1 << 1,
    BTN_TURN_LEFT    = 1 << 2,
    BTN_TURN_RIGHT   = 1 << 3,
    BTN_STRAFE_LEFT  = 1 << 4,
    BTN_STRAFE_RIGHT = 1 << 5,
    BTN_FIRE         = 1 << 6,
    BTN_USE          = 1 << 7,
    BTN_MAP          = 1 << 8,
    BTN_MENU         = 1 << 9,
};

static std::atomic<uint32_t> s_state{0};

// Previous report for bit-diff logging.
static uint8_t s_prev[MAX_REPORT_BYTES];
static size_t  s_prev_len = 0;

// ---------------------------------------------------------------------------
// Bit-diff logger — logs which bits changed in each byte of the HID report.
// On first report, logs the full raw hex so the layout can be decoded.
// ---------------------------------------------------------------------------

static void log_report_diff(const uint8_t *data, size_t len)
{
    // Log full raw hex for every report (at VERBOSE level) so nothing is lost.
    char hex[MAX_REPORT_BYTES * 3 + 1];
    size_t out = 0;
    for (size_t i = 0; i < len && i < MAX_REPORT_BYTES; i++) {
        out += snprintf(hex + out, sizeof(hex) - out, "%02X ", data[i]);
    }
    ESP_LOGV(TAG, "raw (%zu B): %s", len, hex);

    // Log bit changes at INFO level so they stand out in the log stream.
    bool any_change = false;
    size_t check_len = len < MAX_REPORT_BYTES ? len : MAX_REPORT_BYTES;
    size_t prev_check = s_prev_len < check_len ? s_prev_len : check_len;

    for (size_t i = 0; i < check_len; i++) {
        uint8_t prev = (i < prev_check) ? s_prev[i] : 0;
        uint8_t curr = data[i];
        if (prev == curr) continue;

        any_change = true;
        uint8_t changed_bits = prev ^ curr;
        char desc[64];
        int d = 0;
        for (int b = 0; b < 8; b++) {
            if (changed_bits & (1 << b)) {
                bool now_set = (curr >> b) & 1;
                d += snprintf(desc + d, sizeof(desc) - d, "b%d:%c ", b, now_set ? '1' : '0');
            }
        }
        ESP_LOGI(TAG, "byte[%zu] 0x%02X→0x%02X  %s", i, prev, curr, desc);
    }
    (void)any_change;
}

// ---------------------------------------------------------------------------
// HID report parser
//
// Layout (same as usb_gamepad defaults — verified against Xbox BLE reports):
//   Byte 0  : left stick X  (0x00=left, 0x7F=center, 0xFF=right)
//   Byte 1  : left stick Y  (0x00=up,   0x7F=center, 0xFF=down)
//   Byte 4  : buttons 0-7   (bit0=A/fire, bit1=B/use, bit2=X/map,
//                             bit4=LB/strafe_left, bit5=RB/strafe_right)
//   Byte 5  : buttons 8-15  (bit1=Start/menu)
//
// Some controllers prefix reports with a 1-byte report ID; detect by checking
// if byte 0 is a small non-zero value that looks like a report ID (< 32 and
// the report is long enough to have data after it).
// ---------------------------------------------------------------------------

static void parse_report(const uint8_t *data, size_t len)
{
    if (len < 6) return;

    size_t o = 0;
    if (len >= 7 && data[0] != 0 && data[0] < 32) {
        o = 1;  // skip report ID byte
    }
    if (len < o + 6) return;

    uint32_t bits = 0;

    uint8_t ax = data[o + 0];
    uint8_t ay = data[o + 1];
    if (ax < AXIS_DEAD_LO)  bits |= BTN_TURN_LEFT;
    if (ax > AXIS_DEAD_HI)  bits |= BTN_TURN_RIGHT;
    if (ay < AXIS_DEAD_LO)  bits |= BTN_FORWARD;
    if (ay > AXIS_DEAD_HI)  bits |= BTN_BACK;

    uint8_t b0 = data[o + 4];
    if (b0 & (1 << 0))  bits |= BTN_FIRE;
    if (b0 & (1 << 1))  bits |= BTN_USE;
    if (b0 & (1 << 2))  bits |= BTN_MAP;
    if (b0 & (1 << 4))  bits |= BTN_STRAFE_LEFT;
    if (b0 & (1 << 5))  bits |= BTN_STRAFE_RIGHT;

    uint8_t b1 = data[o + 5];
    if (b1 & (1 << 1))  bits |= BTN_MENU;

    s_state.store(bits, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ble_gamepad_push_report(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return;
    size_t safe_len = len < MAX_REPORT_BYTES ? len : MAX_REPORT_BYTES;

    log_report_diff(data, safe_len);
    parse_report(data, len);

    memcpy(s_prev, data, safe_len);
    s_prev_len = safe_len;
}

GamepadState ble_gamepad_get_state(void)
{
    uint32_t bits = s_state.load(std::memory_order_acquire);
    GamepadState st = {};
    st.forward      = bits & BTN_FORWARD;
    st.back         = bits & BTN_BACK;
    st.turn_left    = bits & BTN_TURN_LEFT;
    st.turn_right   = bits & BTN_TURN_RIGHT;
    st.strafe_left  = bits & BTN_STRAFE_LEFT;
    st.strafe_right = bits & BTN_STRAFE_RIGHT;
    st.fire         = bits & BTN_FIRE;
    st.use          = bits & BTN_USE;
    st.open_map     = bits & BTN_MAP;
    st.menu         = bits & BTN_MENU;
    return st;
}

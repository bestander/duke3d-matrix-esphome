#include "bsp/board.h"
#include "class/hid/hid.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// UART settings for ESP32 bridge.
#define BRIDGE_UART uart0
#define BRIDGE_BAUD 115200
#define BRIDGE_TX_PIN 0
#define BRIDGE_RX_PIN 1

// Duke keyboard scancodes from engine keyboard.h.
enum {
  SC_W = 0x11,
  SC_A = 0x1e,
  SC_S = 0x1f,
  SC_D = 0x20,
  SC_SPACE = 0x39,
  SC_TAB = 0x0f,
  SC_ENTER = 0x1c,
  SC_ESCAPE = 0x01,
  SC_LEFT_CTRL = 0x1d,
  SC_UP = 0x5a,
  SC_DOWN = 0x6a,
  SC_LEFT = 0x6b,
  SC_RIGHT = 0x6c,
};

typedef struct {
  uint8_t hid;
  uint8_t duke_sc;
} key_map_t;

static const key_map_t KEY_MAP[] = {
    {HID_KEY_W, SC_W},
    {HID_KEY_A, SC_A},
    {HID_KEY_S, SC_S},
    {HID_KEY_D, SC_D},
    {HID_KEY_ARROW_UP, SC_UP},
    {HID_KEY_ARROW_DOWN, SC_DOWN},
    {HID_KEY_ARROW_LEFT, SC_LEFT},
    {HID_KEY_ARROW_RIGHT, SC_RIGHT},
    {HID_KEY_SPACE, SC_SPACE},
    {HID_KEY_TAB, SC_TAB},
    {HID_KEY_ENTER, SC_ENTER},
    {HID_KEY_ESCAPE, SC_ESCAPE},
    {HID_KEY_CONTROL_LEFT, SC_LEFT_CTRL},
};

static hid_keyboard_report_t s_prev_report = {0};
static uint16_t s_gamepad_bits_prev = 0;
static void send_scancode(uint8_t duke_sc, bool pressed);

enum {
  GP_FORWARD      = 1u << 0,
  GP_BACK         = 1u << 1,
  GP_TURN_LEFT    = 1u << 2,
  GP_TURN_RIGHT   = 1u << 3,
  GP_STRAFE_LEFT  = 1u << 4,
  GP_STRAFE_RIGHT = 1u << 5,
  GP_FIRE         = 1u << 6,
  GP_USE          = 1u << 7,
  GP_MAP          = 1u << 8,
  GP_MENU         = 1u << 9,
};

static const char *hid_proto_name(uint8_t proto) {
  switch (proto) {
    case HID_ITF_PROTOCOL_NONE:
      return "none";
    case HID_ITF_PROTOCOL_KEYBOARD:
      return "keyboard";
    case HID_ITF_PROTOCOL_MOUSE:
      return "mouse";
    default:
      return "unknown";
  }
}

static void log_report_hex(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
  printf("[hid] d=%u i=%u len=%u data=", dev_addr, instance, (unsigned) len);
  uint16_t dump_len = len > 20 ? 20 : len;
  for (uint16_t i = 0; i < dump_len; ++i) {
    printf("%02X", report[i]);
    if (i + 1 < dump_len) {
      printf(" ");
    }
  }
  if (len > dump_len) {
    printf(" ...");
  }
  printf("\n");
}

static bool gamepad_hat_has_up(uint8_t hat) {
  return hat == GAMEPAD_HAT_UP || hat == GAMEPAD_HAT_UP_RIGHT || hat == GAMEPAD_HAT_UP_LEFT;
}

static bool gamepad_hat_has_down(uint8_t hat) {
  return hat == GAMEPAD_HAT_DOWN || hat == GAMEPAD_HAT_DOWN_RIGHT || hat == GAMEPAD_HAT_DOWN_LEFT;
}

static bool gamepad_hat_has_left(uint8_t hat) {
  return hat == GAMEPAD_HAT_LEFT || hat == GAMEPAD_HAT_UP_LEFT || hat == GAMEPAD_HAT_DOWN_LEFT;
}

static bool gamepad_hat_has_right(uint8_t hat) {
  return hat == GAMEPAD_HAT_RIGHT || hat == GAMEPAD_HAT_UP_RIGHT || hat == GAMEPAD_HAT_DOWN_RIGHT;
}

static void emit_gamepad_scancode(uint16_t curr_bits, uint16_t mask, uint8_t sc) {
  bool prev = (s_gamepad_bits_prev & mask) != 0;
  bool curr = (curr_bits & mask) != 0;
  if (prev != curr) {
    send_scancode(sc, curr);
  }
}

static bool map_gamepad_report_to_bits(uint8_t const *report, uint16_t len, uint16_t *out_bits) {
  if (!report || !out_bits) {
    return false;
  }

  // Observed controller format (VID:PID 0CA3:0024), 8-byte report:
  //   [0]=report_id (0x01)
  //   [1..4]=axes around 0x7F center
  //   [5]=high nibble D-pad bitmask (U=1,R=2,D=4,L=8), low nibble const 0xF
  //   [6]=buttons bitfield
  //   [7]=reserved
  if (len == 8 && report[0] == 0x01) {
    uint16_t bits = 0;

    const uint8_t axis_x = report[3];
    const uint8_t axis_y = report[4];
    const uint8_t dpad = (uint8_t)(report[5] >> 4);
    const uint8_t btn = report[6];

    // Digital D-pad from bitmask nibble.
    if (dpad & 0x1) bits |= GP_FORWARD;    // up
    if (dpad & 0x2) bits |= GP_TURN_RIGHT; // right
    if (dpad & 0x4) bits |= GP_BACK;       // down
    if (dpad & 0x8) bits |= GP_TURN_LEFT;  // left

    // Left stick fallback.
    if (axis_x < 64) bits |= GP_TURN_LEFT;
    if (axis_x > 192) bits |= GP_TURN_RIGHT;
    if (axis_y < 64) bits |= GP_FORWARD;
    if (axis_y > 192) bits |= GP_BACK;

    // First-pass button map for this controller.
    if (btn & 0x01) bits |= GP_FIRE;
    if (btn & 0x02) bits |= GP_USE;
    if (btn & 0x04) bits |= GP_MAP;
    if (btn & 0x08) bits |= GP_MENU;
    if (btn & 0x10) bits |= GP_STRAFE_LEFT;
    if (btn & 0x20) bits |= GP_STRAFE_RIGHT;

    *out_bits = bits;
    return true;
  }

  size_t off = 0;
  if (len >= sizeof(hid_gamepad_report_t) + 1) {
    // Many controllers prepend Report ID for non-boot HID gamepad reports.
    off = 1;
  }
  if (len < off + sizeof(hid_gamepad_report_t)) {
    return false;
  }

  const hid_gamepad_report_t *gp = (const hid_gamepad_report_t *)(report + off);
  uint16_t bits = 0;

  // Hat/D-pad first.
  if (gp->hat != GAMEPAD_HAT_CENTERED) {
    if (gamepad_hat_has_up(gp->hat)) bits |= GP_FORWARD;
    if (gamepad_hat_has_down(gp->hat)) bits |= GP_BACK;
    if (gamepad_hat_has_left(gp->hat)) bits |= GP_TURN_LEFT;
    if (gamepad_hat_has_right(gp->hat)) bits |= GP_TURN_RIGHT;
  }

  // Left stick fallback with deadzone.
  if (gp->x < -32) bits |= GP_TURN_LEFT;
  if (gp->x > 32) bits |= GP_TURN_RIGHT;
  if (gp->y < -32) bits |= GP_FORWARD;
  if (gp->y > 32) bits |= GP_BACK;

  // Common gamepad button mapping.
  if (gp->buttons & GAMEPAD_BUTTON_TL) bits |= GP_STRAFE_LEFT;
  if (gp->buttons & GAMEPAD_BUTTON_TR) bits |= GP_STRAFE_RIGHT;
  if (gp->buttons & GAMEPAD_BUTTON_A) bits |= GP_FIRE;
  if (gp->buttons & GAMEPAD_BUTTON_B) bits |= GP_USE;
  if (gp->buttons & GAMEPAD_BUTTON_X) bits |= GP_MAP;
  if (gp->buttons & GAMEPAD_BUTTON_START) bits |= GP_MENU;

  *out_bits = bits;
  return true;
}

static uint8_t map_hid_to_duke(uint8_t hid_key) {
  for (size_t i = 0; i < sizeof(KEY_MAP) / sizeof(KEY_MAP[0]); ++i) {
    if (KEY_MAP[i].hid == hid_key) {
      return KEY_MAP[i].duke_sc;
    }
  }
  return 0;
}

static bool report_contains_key(const hid_keyboard_report_t *report, uint8_t key) {
  if (key == 0) {
    return false;
  }
  for (uint8_t i = 0; i < 6; ++i) {
    if (report->keycode[i] == key) {
      return true;
    }
  }
  return false;
}

static void send_scancode(uint8_t duke_sc, bool pressed) {
  if (duke_sc == 0) {
    return;
  }
  char line[32];
  const int n = snprintf(line, sizeof(line), "SC,0x%02X,%d\n", duke_sc, pressed ? 1 : 0);
  if (n > 0) {
    uart_write_blocking(BRIDGE_UART, (const uint8_t *) line, (size_t) n);
  }
}

// TinyUSB host callbacks.
void tuh_mount_cb(uint8_t dev_addr) {
  uint16_t vid = 0;
  uint16_t pid = 0;
  (void) tuh_vid_pid_get(dev_addr, &vid, &pid);
  printf("[usb] mount d=%u vid=%04X pid=%04X\n", dev_addr, vid, pid);
}

void tuh_umount_cb(uint8_t dev_addr) {
  printf("[usb] umount d=%u\n", dev_addr);
  memset(&s_prev_report, 0, sizeof(s_prev_report));
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
  (void) desc_report;
  uint16_t vid = 0;
  uint16_t pid = 0;
  (void) tuh_vid_pid_get(dev_addr, &vid, &pid);
  uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);
  printf("[hid] mount d=%u i=%u vid=%04X pid=%04X proto=%s desc_len=%u\n",
         dev_addr, instance, vid, pid, hid_proto_name(proto), (unsigned) desc_len);
  tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
  printf("[hid] umount d=%u i=%u\n", dev_addr, instance);
  memset(&s_prev_report, 0, sizeof(s_prev_report));
  s_gamepad_bits_prev = 0;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
  uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);
  if (proto != HID_ITF_PROTOCOL_KEYBOARD) {
    uint16_t gp_bits = 0;
    if (map_gamepad_report_to_bits(report, len, &gp_bits)) {
      emit_gamepad_scancode(gp_bits, GP_FORWARD, SC_W);
      emit_gamepad_scancode(gp_bits, GP_BACK, SC_S);
      emit_gamepad_scancode(gp_bits, GP_TURN_LEFT, SC_LEFT);
      emit_gamepad_scancode(gp_bits, GP_TURN_RIGHT, SC_RIGHT);
      emit_gamepad_scancode(gp_bits, GP_STRAFE_LEFT, SC_A);
      emit_gamepad_scancode(gp_bits, GP_STRAFE_RIGHT, SC_D);
      emit_gamepad_scancode(gp_bits, GP_FIRE, SC_LEFT_CTRL);
      emit_gamepad_scancode(gp_bits, GP_USE, SC_SPACE);
      emit_gamepad_scancode(gp_bits, GP_MAP, SC_TAB);
      emit_gamepad_scancode(gp_bits, GP_MENU, SC_ESCAPE);
      s_gamepad_bits_prev = gp_bits;
    }
    log_report_hex(dev_addr, instance, report, len);
    tuh_hid_receive_report(dev_addr, instance);
    return;
  }
  if (len < sizeof(hid_keyboard_report_t)) {
    tuh_hid_receive_report(dev_addr, instance);
    return;
  }

  const hid_keyboard_report_t *curr = (const hid_keyboard_report_t *) report;

  // Released keys.
  for (uint8_t i = 0; i < 6; ++i) {
    uint8_t key = s_prev_report.keycode[i];
    if (key && !report_contains_key(curr, key)) {
      send_scancode(map_hid_to_duke(key), false);
    }
  }

  // Pressed keys.
  for (uint8_t i = 0; i < 6; ++i) {
    uint8_t key = curr->keycode[i];
    if (key && !report_contains_key(&s_prev_report, key)) {
      send_scancode(map_hid_to_duke(key), true);
    }
  }

  s_prev_report = *curr;
  tuh_hid_receive_report(dev_addr, instance);
}

int main(void) {
  board_init();
  stdio_init_all();
  sleep_ms(1200);
  printf("[bridge] boot\n");

  uart_init(BRIDGE_UART, BRIDGE_BAUD);
  gpio_set_function(BRIDGE_TX_PIN, GPIO_FUNC_UART);
  gpio_set_function(BRIDGE_RX_PIN, GPIO_FUNC_UART);
  printf("[bridge] uart0 tx=GP%d rx=GP%d baud=%d\n", BRIDGE_TX_PIN, BRIDGE_RX_PIN, BRIDGE_BAUD);

  bool host_ok = tuh_init(0);
  printf("[bridge] tinyusb host init=%d inited=%d\n", host_ok ? 1 : 0, tuh_inited() ? 1 : 0);

  uint32_t last_hb_ms = to_ms_since_boot(get_absolute_time());
  uint32_t last_host_ms = last_hb_ms;

  while (true) {
    tuh_task();
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if ((now_ms - last_hb_ms) >= 1000) {
      printf("[bridge] hb %lu ms\n", (unsigned long) now_ms);
      last_hb_ms = now_ms;
    }
    if ((now_ms - last_host_ms) >= 5000) {
      printf("[bridge] host inited=%d\n", tuh_inited() ? 1 : 0);
      last_host_ms = now_ms;
    }
  }
}

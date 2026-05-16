#include "bsp/board.h"
#include "class/hid/hid.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "pico_gamepad_generated.h"
#if PICO_CYW43_SUPPORTED
#include "pico/cyw43_arch.h"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef PICO_BRIDGE_DEBUG
#define PICO_BRIDGE_DEBUG 0
#endif

// UART settings for ESP32 bridge.
#define BRIDGE_UART uart0
#define BRIDGE_BAUD 115200
#define BRIDGE_TX_PIN 0
#define BRIDGE_RX_PIN 1

#if PICO_BRIDGE_DEBUG
static void bridge_debug_boot_line(void) {
  static const char msg[] = "[bridge] usb_hid_uart_bridge ready\n";
  uart_write_blocking(BRIDGE_UART, (const uint8_t *)msg, sizeof(msg) - 1);
}

/** One line when decoded buttons/hat change — avoids flooding 115200 UART (drops SC,... frames). */
static void log_hid_gamepad_line(uint8_t const *report, uint16_t len, uint16_t gp_bits) {
  uint8_t base = (len >= 8u && report[0] == 0x01u) ? 1u : 0u;
  if (len < (uint16_t)(base + 6u)) {
    return;
  }
  const uint8_t ax = report[base + 2u];
  const uint8_t ay = report[base + 3u];
  const uint8_t b5 = report[base + 4u];
  const uint8_t b6 = report[base + 5u];
  const uint8_t d5 = (uint8_t)(b5 ^ 0x0Fu);
  char buf[160];
  const int head =
      snprintf(buf, sizeof(buf), "[hid] gp_bits=0x%04X ax=%02X ay=%02X b5=%02X b6=%02X d5=%02X",
               (unsigned)gp_bits, ax, ay, b5, b6, (unsigned)d5);
  if (head < 0 || (size_t)head >= sizeof(buf)) {
    return;
  }
  if (len >= 8u) {
    snprintf(buf + (size_t)head, sizeof(buf) - (size_t)head,
             " | %02X %02X %02X %02X %02X %02X %02X %02X\n", report[0], report[1], report[2], report[3],
             report[4], report[5], report[6], report[7]);
  } else {
    snprintf(buf + (size_t)head, sizeof(buf) - (size_t)head, "\n");
  }
  uart_write_blocking(BRIDGE_UART, (const uint8_t *)buf, strlen(buf));
}
#else
static inline void bridge_debug_boot_line(void) {}
static inline void log_hid_gamepad_line(uint8_t const *report, uint16_t len, uint16_t gp_bits) {
  (void)report;
  (void)len;
  (void)gp_bits;
}
#endif

// GP2: short blink bursts on HID button presses (see send_scancode + main loop).
#define STATUS_BTN_LED_PIN 2
// GP3: continuous ~2 Hz LED blink (local firmware-alive indicator; not sent on UART).
#define HEARTBEAT_LED_PIN 3

static volatile uint32_t s_status_led_blink_until_ms;

// Duke keyboard scancodes (keyboard.h) — mapped to Duke Atomic defaults (_functio.h).
enum {
  SC_W = 0x11,
  SC_A_KEY = 0x1e,  // Duke default Jump key ("A")
  SC_S = 0x1f,
  SC_D = 0x20,
  SC_Z_KEY = 0x2c,  // Duke default Crouch key ("Z")
  SC_SPACE = 0x39,
  SC_TAB = 0x0f,
  SC_ENTER = 0x1c,
  SC_ESCAPE = 0x01,
  SC_LEFT_CTRL = 0x1d,
  SC_LEFT_ALT = 0x38,
  SC_QUOTE = 0x28,           // Next_Weapon ("'")
  SC_CLOSE_BRACKET = 0x1b,   // Inventory_Right ("]")
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
    {HID_KEY_A, SC_A_KEY},
    {HID_KEY_S, SC_S},
    {HID_KEY_D, SC_D},
    {HID_KEY_Z, SC_Z_KEY},
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
static void set_startup_led_on(void);

enum {
  GP_CROSS_UP = 1u << 0,
  GP_CROSS_DOWN = 1u << 1,
  GP_CROSS_LEFT = 1u << 2,
  GP_CROSS_RIGHT = 1u << 3,
  GP_STRAFE_MOD = 1u << 4,    // LB → Duke Strafe (Left Alt default)
  GP_FIRE = 1u << 5,
  GP_OPEN = 1u << 6,          // RB → Open (Space)
  GP_JUMP = 1u << 7,
  GP_CROUCH = 1u << 8,
  GP_NEXT_WEAPON = 1u << 9,
  GP_INV_MENU = 1u << 10,     // X → Inventory (Enter)
  GP_INV_NEXT = 1u << 11,     // physical Y (byte5) → scancode from YAML (e.g. inventory_next)
  GP_MENU_PAUSE = 1u << 12,   // Start → CMD,... or scancode from YAML
};

static void emit_gamepad_scancode(uint16_t curr_bits, uint16_t mask, uint8_t sc) {
  if (sc == 0) {
    return;
  }
  bool prev = (s_gamepad_bits_prev & mask) != 0;
  bool curr = (curr_bits & mask) != 0;
  if (prev != curr) {
    send_scancode(sc, curr);
  }
}

/** Start: either Duke scancode from YAML or UART macro CMD,RANDOM_DEMO (see PICO_START_SENDS_RANDOM_DEMO_CMD). */
static void emit_start_button_uart_or_scancode(uint16_t curr_bits, uint16_t prev_bits) {
#if PICO_START_SENDS_RANDOM_DEMO_CMD
  bool prev = (prev_bits & GP_MENU_PAUSE) != 0;
  bool curr = (curr_bits & GP_MENU_PAUSE) != 0;
  if (!prev && curr) {
    static const char cmd[] = "CMD,RANDOM_DEMO\n";
    uart_write_blocking(BRIDGE_UART, (const uint8_t *)cmd, sizeof(cmd) - 1);
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t until = now + 280;
    if (until > s_status_led_blink_until_ms) {
      s_status_led_blink_until_ms = until;
    }
  }
#else
  emit_gamepad_scancode(curr_bits, GP_MENU_PAUSE, PICO_GP_ACT_START);
#endif
}

/** Digital hat from one analog pair (horizontal ax, vertical ay). */
static void hat_axes_to_cross_bits_thresh(uint8_t ax, uint8_t ay, uint16_t *bits, uint8_t low_th, uint8_t high_th) {
  if (ay <= low_th) {
    *bits |= GP_CROSS_UP;
  }
  if (ay >= high_th) {
    *bits |= GP_CROSS_DOWN;
  }
  if (ax <= low_th) {
    *bits |= GP_CROSS_LEFT;
  }
  if (ax >= high_th) {
    *bits |= GP_CROSS_RIGHT;
  }
}

static bool map_gamepad_report_to_bits(uint8_t const *report, uint16_t len, uint16_t *out_bits) {
  if (!report || !out_bits) {
    return false;
  }

  /* MatrixPortal-style 8-byte gamepad:
   *  - With report ID:  01 | axL axL axH axH | b5 b6 pad...  (YAML “cross_*” touches axH_x, axH_y = bytes after 01)
   *  - Without ID (7 payload bytes + common): first byte may be 7F not 01 — use base=0 so UP/DOWN still land on ay.
   *
   * Face row: idle b5 low nibble 0xF → d5 = b5 ^ 0x0F == 0.
   * While any ABXY-row bit is active (d5 != 0), do not decode hat from axes — prevents ghost cross_* (e.g. B → cross_right)
   * from left-stick noise/coupling or glitches when byte [5] changes.
   *
   * We only use the YAML hat pair (two bytes before b5), never OR-merge with the other analog pair.
   */
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
    hat_axes_to_cross_bits_thresh(ax, ay, &bits, HAT_LOW, HAT_HIGH);
  }

  if (d5 == 0x80u) bits |= GP_INV_MENU;
  if (d5 == 0x40u) bits |= GP_FIRE;
  if (d5 == 0x20u) bits |= GP_NEXT_WEAPON;
  if (d5 == 0x10u) bits |= GP_INV_NEXT;
  if (b6 & 0x01u) bits |= GP_JUMP;
  if (b6 & 0x02u) bits |= GP_CROUCH;
  if (b6 & 0x04u) bits |= GP_STRAFE_MOD;
  if (b6 & 0x08u) bits |= GP_OPEN;
  if (b6 & 0x20u) bits |= GP_MENU_PAUSE;

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
  if (pressed) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t until = now + 280;
    if (until > s_status_led_blink_until_ms) {
      s_status_led_blink_until_ms = until;
    }
  }
  char line[32];
  const int n = snprintf(line, sizeof(line), "SC,0x%02X,%d\n", duke_sc, pressed ? 1 : 0);
  if (n > 0) {
    uart_write_blocking(BRIDGE_UART, (const uint8_t *) line, (size_t) n);
  }
}

static void set_startup_led_on(void) {
#if PICO_CYW43_SUPPORTED
  // Pico W onboard LED is controlled by the CYW43 chip.
  if (cyw43_arch_init() == 0) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    return;
  }
#endif
  // Fallback for boards with direct GPIO LED (e.g. Pico non-W).
  board_led_write(true);
}

// TinyUSB host callbacks.
void tuh_mount_cb(uint8_t dev_addr) {
  (void) dev_addr;
}

void tuh_umount_cb(uint8_t dev_addr) {
  (void) dev_addr;
  memset(&s_prev_report, 0, sizeof(s_prev_report));
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
  (void) desc_report;
  (void) desc_len;
  tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
  (void) dev_addr;
  (void) instance;
  memset(&s_prev_report, 0, sizeof(s_prev_report));
  s_gamepad_bits_prev = 0;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
  uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);

  /* Composite controllers sometimes expose this 8-byte layout on an interface TinyUSB labels as
   * Boot Keyboard. Feeding those bytes through the keyboard decoder treats nibbles as HID keycodes
   * (phantom Arrow Right → Duke SC_RIGHT / ESP label cross_right) while the gamepad decoder would
   * correctly see face buttons (e.g. B → next_weapon).
   *
   * HID boot keyboard after Report ID 0x01 uses modifiers in [1] and reserved [2]==0.
   * MatrixPortal axes idle ~0x7F in [1]/[2]; treat “looks like keyboard boot idle/modifiers only” as NOT gamepad.
   */
  const bool mp_gamepad_shape =
      (len >= 8u && report[0] == 0x01u && !(report[2] == 0u && report[1] < 0x40u));
  const bool use_gamepad_decoder = (proto != HID_ITF_PROTOCOL_KEYBOARD) || mp_gamepad_shape;

  if (use_gamepad_decoder) {
    uint16_t gp_bits = 0;
    if (map_gamepad_report_to_bits(report, len, &gp_bits)) {
#if PICO_BRIDGE_DEBUG
      /* Logging every HID poll (~100Hz+) fills 115200 baud — SC,... lines get dropped on the wire. */
      if (gp_bits != s_gamepad_bits_prev) {
        log_hid_gamepad_line(report, len, gp_bits);
      }
#endif
      emit_gamepad_scancode(gp_bits, GP_CROSS_UP, PICO_GP_ACT_CROSS_UP);
      emit_gamepad_scancode(gp_bits, GP_CROSS_DOWN, PICO_GP_ACT_CROSS_DOWN);
      emit_gamepad_scancode(gp_bits, GP_CROSS_LEFT, PICO_GP_ACT_CROSS_LEFT);
      emit_gamepad_scancode(gp_bits, GP_CROSS_RIGHT, PICO_GP_ACT_CROSS_RIGHT);
      emit_gamepad_scancode(gp_bits, GP_STRAFE_MOD, PICO_GP_ACT_BUMPER_L);
      emit_gamepad_scancode(gp_bits, GP_OPEN, PICO_GP_ACT_BUMPER_R);
      emit_gamepad_scancode(gp_bits, GP_JUMP, PICO_GP_ACT_Z);
      emit_gamepad_scancode(gp_bits, GP_CROUCH, PICO_GP_ACT_C);
      emit_gamepad_scancode(gp_bits, GP_FIRE, PICO_GP_ACT_A);
      emit_gamepad_scancode(gp_bits, GP_NEXT_WEAPON, PICO_GP_ACT_B);
      emit_gamepad_scancode(gp_bits, GP_INV_MENU, PICO_GP_ACT_X);
      emit_gamepad_scancode(gp_bits, GP_INV_NEXT, PICO_GP_ACT_Y);
      emit_start_button_uart_or_scancode(gp_bits, s_gamepad_bits_prev);
      s_gamepad_bits_prev = gp_bits;
      tuh_hid_receive_report(dev_addr, instance);
      return;
    }
    if (mp_gamepad_shape) {
      /* Known vendor shape — never reinterpret as keyboard (same phantom-arrow failure mode). */
      tuh_hid_receive_report(dev_addr, instance);
      return;
    }
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
  set_startup_led_on();
  // UART0: bridge frames via uart_write_blocking. Optional "[hid]" / "[bridge]" lines are ESP-ignored.
  uart_init(BRIDGE_UART, BRIDGE_BAUD);
  gpio_set_function(BRIDGE_TX_PIN, GPIO_FUNC_UART);
  gpio_set_function(BRIDGE_RX_PIN, GPIO_FUNC_UART);
#if PICO_BRIDGE_DEBUG
  bridge_debug_boot_line();
#endif
  sleep_ms(1200);

  (void) tuh_init(0);
  gpio_init(STATUS_BTN_LED_PIN);
  gpio_set_dir(STATUS_BTN_LED_PIN, GPIO_OUT);
  gpio_put(STATUS_BTN_LED_PIN, false);
  gpio_init(HEARTBEAT_LED_PIN);
  gpio_set_dir(HEARTBEAT_LED_PIN, GPIO_OUT);
  gpio_put(HEARTBEAT_LED_PIN, false);
  s_status_led_blink_until_ms = 0;

  uint32_t now_boot_ms = to_ms_since_boot(get_absolute_time());
  uint32_t last_btn_led_toggle_ms = now_boot_ms;
  uint32_t last_heartbeat_led_ms = now_boot_ms;
  bool btn_led_phase = false;
  bool heartbeat_led_phase = false;

  while (true) {
    tuh_task();
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if ((now_ms - last_heartbeat_led_ms) >= 250) {
      last_heartbeat_led_ms = now_ms;
      heartbeat_led_phase = !heartbeat_led_phase;
      gpio_put(HEARTBEAT_LED_PIN, heartbeat_led_phase);
    }

    if (now_ms < s_status_led_blink_until_ms) {
      if ((now_ms - last_btn_led_toggle_ms) >= 70) {
        last_btn_led_toggle_ms = now_ms;
        btn_led_phase = !btn_led_phase;
        gpio_put(STATUS_BTN_LED_PIN, btn_led_phase);
      }
    } else {
      gpio_put(STATUS_BTN_LED_PIN, false);
      btn_led_phase = false;
      last_btn_led_toggle_ms = now_ms;
    }
  }
}

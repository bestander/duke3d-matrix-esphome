#include "bsp/board.h"
#include "class/hid/hid.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#include "tusb.h"
#if PICO_CYW43_SUPPORTED
#include "pico/cyw43_arch.h"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef PICO_BRIDGE_DEBUG
#define PICO_BRIDGE_DEBUG 0
#endif

/* UART bridge: GR,<len>,<lowercase_hex> HID report snapshot when bytes change (gamepad-route).
 * KB,... boot keyboard usages. No vendor button decode on-device. */
#define BRIDGE_UART uart0
#define BRIDGE_BAUD 115200
#define BRIDGE_TX_PIN 0
#define BRIDGE_RX_PIN 1
#define GR_CAP 64

#if PICO_BRIDGE_DEBUG
static void bridge_debug_boot_line(void) {
  static const char msg[] = "[bridge] usb_hid_uart_bridge ready\n";
  uart_write_blocking(BRIDGE_UART, (const uint8_t *)msg, sizeof(msg) - 1);
}

static void log_hid_hex_line(uint8_t const *report, uint16_t len) {
  char buf[220];
  int head = snprintf(buf, sizeof(buf), "[hid] report_len=%u |", (unsigned)len);
  if (head < 0 || (size_t)head >= sizeof(buf)) {
    return;
  }
  for (uint16_t i = 0; i < len && head < (int)sizeof(buf) - 4; ++i) {
    head += snprintf(buf + head, sizeof(buf) - (size_t)head, " %02X", report[i]);
    if (head < 0 || (size_t)head >= sizeof(buf)) {
      return;
    }
  }
  if (head < (int)sizeof(buf) - 2) {
    buf[head++] = '\n';
    buf[head] = '\0';
    uart_write_blocking(BRIDGE_UART, (const uint8_t *)buf, (size_t)head);
  }
}
#else
static inline void bridge_debug_boot_line(void) {}
static inline void log_hid_hex_line(uint8_t const *report, uint16_t len) {
  (void)report;
  (void)len;
}
#endif

#define STATUS_BTN_LED_PIN 2
#define HEARTBEAT_LED_PIN 3

static volatile uint32_t s_status_led_blink_until_ms;

static hid_keyboard_report_t s_prev_keyboard_report = {0};
static uint8_t s_last_gr_payload[GR_CAP];
static uint16_t s_last_gr_len;

static void note_wire_activity(void) {
  uint32_t now = to_ms_since_boot(get_absolute_time());
  uint32_t until = now + 120;
  if (until > s_status_led_blink_until_ms) {
    s_status_led_blink_until_ms = until;
  }
}

/** Forward raw HID report bytes (truncated). Only sends when snapshot differs / length changes. */
static void uart_send_gr_snapshot(const uint8_t *payload, uint16_t len) {
  if (!payload || len == 0) {
    return;
  }

  uint16_t nlen = len;
  if (nlen > GR_CAP) {
    nlen = GR_CAP;
  }
  if (s_last_gr_len == nlen && memcmp(s_last_gr_payload, payload, nlen) == 0) {
    return;
  }
  memcpy(s_last_gr_payload, payload, nlen);
  s_last_gr_len = nlen;

  char line[GR_CAP * 2 + 32];
  int pos = snprintf(line, sizeof(line), "GR,%u,", (unsigned)nlen);
  if (pos < 0 || (size_t)pos >= sizeof(line)) {
    return;
  }
  for (uint16_t i = 0; i < nlen; ++i) {
    static const char *const hx = "0123456789abcdef";
    uint8_t b = payload[i];
    if ((size_t)pos + 3 > sizeof(line)) {
      break;
    }
    line[pos++] = hx[b >> 4u];
    line[pos++] = hx[b & 0xFu];
  }
  if ((size_t)pos + 2 > sizeof(line)) {
    return;
  }
  line[pos++] = '\n';
  uart_write_blocking(BRIDGE_UART, (const uint8_t *)line, (size_t)pos);
  note_wire_activity();
}

static void uart_send_kb_hid(uint8_t hid_usage, bool down) {
  char line[32];
  const int n = snprintf(line, sizeof(line), "KB,0x%02X,%d\n", (unsigned)hid_usage, down ? 1 : 0);
  if (n > 0) {
    uart_write_blocking(BRIDGE_UART, (const uint8_t *)line, (size_t)n);
  }
  note_wire_activity();
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

static void set_startup_led_on(void) {
#if PICO_CYW43_SUPPORTED
  if (cyw43_arch_init() == 0) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    return;
  }
#endif
  board_led_write(true);
}

void tuh_mount_cb(uint8_t dev_addr) {
  (void)dev_addr;
}

void tuh_umount_cb(uint8_t dev_addr) {
  (void)dev_addr;
  memset(&s_prev_keyboard_report, 0, sizeof(s_prev_keyboard_report));
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
  (void)desc_report;
  (void)desc_len;
  tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
  (void)dev_addr;
  (void)instance;
  memset(&s_prev_keyboard_report, 0, sizeof(s_prev_keyboard_report));
  memset(s_last_gr_payload, 0, sizeof(s_last_gr_payload));
  s_last_gr_len = 0;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
  uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);

  const bool mp_gamepad_shape =
      (len >= 8u && report[0] == 0x01u && !(report[2] == 0u && report[1] < 0x40u));
  const bool gamepad_route = (proto != HID_ITF_PROTOCOL_KEYBOARD) || mp_gamepad_shape;

  if (gamepad_route) {
#if PICO_BRIDGE_DEBUG
    log_hid_hex_line(report, len);
#endif
    uart_send_gr_snapshot(report, len);
    tuh_hid_receive_report(dev_addr, instance);
    return;
  }

  if (len < sizeof(hid_keyboard_report_t)) {
    tuh_hid_receive_report(dev_addr, instance);
    return;
  }

  const hid_keyboard_report_t *curr = (const hid_keyboard_report_t *)report;

  for (uint8_t i = 0; i < 6; ++i) {
    uint8_t key = s_prev_keyboard_report.keycode[i];
    if (key && !report_contains_key(curr, key)) {
      uart_send_kb_hid(key, false);
    }
  }

  for (uint8_t i = 0; i < 6; ++i) {
    uint8_t key = curr->keycode[i];
    if (key && !report_contains_key(&s_prev_keyboard_report, key)) {
      uart_send_kb_hid(key, true);
    }
  }

  s_prev_keyboard_report = *curr;
  tuh_hid_receive_report(dev_addr, instance);
}

int main(void) {
  board_init();
  set_startup_led_on();
  uart_init(BRIDGE_UART, BRIDGE_BAUD);
  gpio_set_function(BRIDGE_TX_PIN, GPIO_FUNC_UART);
  gpio_set_function(BRIDGE_RX_PIN, GPIO_FUNC_UART);
  static const char k_banner[] = "[bridge] v1 codecs=GR,KB+PING+PONG\n";
  uart_write_blocking(BRIDGE_UART, (const uint8_t *)k_banner, sizeof(k_banner) - 1);

#if PICO_BRIDGE_DEBUG
  bridge_debug_boot_line();
#endif
  sleep_ms(1200);

  (void)tuh_init(0);
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

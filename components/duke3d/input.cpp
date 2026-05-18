#include "input.h"

extern "C" {
#include "keyboard.h"
#include "pico_uart_vendor_hid_decode.h"
}

#include "pico_uart_bridge_maps.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char *const TAG_PICO = "pico_uart";

static QueueHandle_t input_queue = nullptr;
static const int QUEUE_DEPTH = 16;
static GamepadState input_state = {};
static portMUX_TYPE input_state_mux = portMUX_INITIALIZER_UNLOCKED;

static std::atomic<bool> s_pico_uart_start_press_requested{false};

struct PicoUartConfig {
    int uart_num;
    int tx_pin;
    int rx_pin;
    int baud_rate;
};

struct PicoUartStatusArg {
    uart_port_t port;
    int uart_num;
    int tx_pin;
    int rx_pin;
    int baud_rate;
};

namespace {

static std::atomic<uint32_t> s_pico_last_rx_ms{0};
static std::atomic<uint32_t> s_pico_lines_gr_interval{0};
static std::atomic<uint32_t> s_pico_lines_kb_interval{0};
static std::atomic<uint32_t> s_pico_bytes_rx_interval{0};
static std::atomic<uint32_t> s_pico_unknown_lines_interval{0};
static std::atomic<uint32_t> s_pico_lines_bracket_interval{0};

/** Last decoded vendor button mask (MatrixPortal-style); edges → logical names / Duke. */
static uint16_t s_vendor_btn_prev = 0;

/** Physical Start often arrives as MatrixPortal vendor bit (GR) or as HID Escape on the keyboard interface (KB). */
static void pico_uart_note_start_press_uart(const char *via) {
    ESP_LOGI(TAG_PICO, "start (%s)", via);
#if PICO_UART_BRIDGE_DROP_HID_KEYBOARD_ESCAPE
    /* Same YAML rule as record_session / ESC drop — avoids relying on a separate codegen -D that may miss TU flags. */
    s_pico_uart_start_press_requested.store(true, std::memory_order_release);
#endif
}

static void pico_note_rx_activity() {
    s_pico_last_rx_ms.store(xTaskGetTickCount() * portTICK_PERIOD_MS, std::memory_order_relaxed);
}

bool parse_gr_line(const char *line, uint8_t *out, size_t *out_len, size_t max_len) {
    if (line == nullptr || out == nullptr || out_len == nullptr)
        return false;
    if (strncmp(line, "GR,", 3) != 0)
        return false;
    const char *p = line + 3;
    char *endp = nullptr;
    unsigned long reclen = strtoul(p, &endp, 10);
    if (endp == p || *endp != ',')
        return false;
    if (reclen > max_len)
        return false;
    p = endp + 1;
    for (unsigned long i = 0; i < reclen; i++) {
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1]))
            return false;
        unsigned v = 0;
        if (sscanf(p, "%2x", &v) != 1)
            return false;
        out[i] = static_cast<uint8_t>(v);
        p += 2;
    }
    if (*p != '\0')
        return false;
    *out_len = static_cast<size_t>(reclen);
    return true;
}

bool parse_kb_line(const char *line, unsigned *hid_out, bool *pressed_out) {
    if (line == nullptr || hid_out == nullptr || pressed_out == nullptr)
        return false;
    unsigned hid = 0;
    int pv = 0;
    if (sscanf(line, "KB,%x,%d", &hid, &pv) != 2)
        return false;
    *hid_out = hid;
    *pressed_out = (pv != 0);
    return true;
}

void sync_held_state_from_keys() {
    GamepadState st{};
    st.forward = KB_KeyPressed(sc_W) || KB_KeyPressed(sc_UpArrow);
    st.back = KB_KeyPressed(sc_S) || KB_KeyPressed(sc_DownArrow);
    st.turn_left = KB_KeyPressed(sc_LeftArrow);
    st.turn_right = KB_KeyPressed(sc_RightArrow);
    st.strafe_left = KB_KeyPressed(sc_A);
    st.strafe_right = KB_KeyPressed(sc_D);
    st.fire = KB_KeyPressed(sc_LeftControl) || KB_KeyPressed(sc_RightControl);
    st.use = KB_KeyPressed(sc_Space);
    st.open_map = KB_KeyPressed(sc_Tab);
    st.menu = KB_KeyPressed(sc_Escape);
    input_set_state(st);
}

static void inject_logical_button_event(const char *name, bool pressed) {
    bool handled = false;
    if (strcmp(name, "star") == 0 && pressed) {
        ESP_LOGI(TAG_PICO, "star/heart (vendor)");
        handled = true;
    }
    if (!handled && strcmp(name, "start") == 0 && pressed) {
        pico_uart_note_start_press_uart("vendor");
        handled = true;
    }
    if (!handled) {
        const int32_t sc32 = pico_uart_gp_logical_to_duke_scancode(name);
        if (sc32 >= 0 && sc32 <= sc_LastScanCode) {
            KB_InjectScanCode(static_cast<int>(sc32), pressed ? 1 : 0);
            sync_held_state_from_keys();
            if (pressed)
                ESP_LOGI(TAG_PICO, "%s", name);
        } else if (sc32 < 0 && pressed) {
            ESP_LOGD(TAG_PICO, "logical %s not mapped to Duke", name);
        }
    }
}

static void vendor_emit_mask_edge(uint16_t curr, uint16_t prev, uint16_t mask, const char *logical_name) {
    const bool c = (curr & mask) != 0;
    const bool p = (prev & mask) != 0;
    if (c != p)
        inject_logical_button_event(logical_name, c);
}

/** Apply edges from decoded vendor bitmask (same layout as Pico used to expose as GP,... lines). */
static void vendor_apply_gamepad_bits(uint16_t curr_bits) {
    uint16_t prev = s_vendor_btn_prev;
    vendor_emit_mask_edge(curr_bits, prev, VENDOR_GP_CROSS_UP, "cross_up");
    vendor_emit_mask_edge(curr_bits, prev, VENDOR_GP_CROSS_DOWN, "cross_down");
    vendor_emit_mask_edge(curr_bits, prev, VENDOR_GP_CROSS_LEFT, "cross_left");
    vendor_emit_mask_edge(curr_bits, prev, VENDOR_GP_CROSS_RIGHT, "cross_right");
    vendor_emit_mask_edge(curr_bits, prev, VENDOR_GP_BUMPER_L, "bumper_l");
    vendor_emit_mask_edge(curr_bits, prev, VENDOR_GP_OPEN, "bumper_r");
    vendor_emit_mask_edge(curr_bits, prev, VENDOR_GP_JUMP, "z");
    vendor_emit_mask_edge(curr_bits, prev, VENDOR_GP_CROUCH, "c");
    vendor_emit_mask_edge(curr_bits, prev, VENDOR_GP_FIRE, "a");
    vendor_emit_mask_edge(curr_bits, prev, VENDOR_GP_NEXT_WEAPON, "b");
    vendor_emit_mask_edge(curr_bits, prev, VENDOR_GP_INV_MENU, "x");
    vendor_emit_mask_edge(curr_bits, prev, VENDOR_GP_INV_NEXT, "y");
    vendor_emit_mask_edge(curr_bits, prev, VENDOR_GP_STAR_HEART, "star");
    vendor_emit_mask_edge(curr_bits, prev, VENDOR_GP_START, "start");
    s_vendor_btn_prev = curr_bits;
}

void pico_uart_task(void *arg) {
    PicoUartConfig cfg = *static_cast<PicoUartConfig *>(arg);
    delete static_cast<PicoUartConfig *>(arg);

    const uart_port_t port = static_cast<uart_port_t>(cfg.uart_num);
    uart_config_t uart_cfg{};
    uart_cfg.baud_rate = cfg.baud_rate;
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.rx_flow_ctrl_thresh = 0;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_driver_install(port, 1024, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(port, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(port, cfg.tx_pin, cfg.rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGD(TAG_PICO, "bridge started port=%d esp_tx=GPIO%d esp_rx=GPIO%d baud=%d (Pico TX→ESP RX, Pico RX←ESP TX)",
             (int) port, cfg.tx_pin, cfg.rx_pin, cfg.baud_rate);

    constexpr size_t k_gr_max_payload = 64;
    constexpr size_t k_line_max = 8 + k_gr_max_payload * 2 + 4;

    char line_buf[k_line_max];
    size_t line_len = 0;
    uint8_t b = 0;

    while (true) {
        const int n = uart_read_bytes(port, &b, 1, pdMS_TO_TICKS(100));
        if (n <= 0)
            continue;
        s_pico_bytes_rx_interval.fetch_add((uint32_t)n, std::memory_order_relaxed);
        if (b == '\r')
            continue;

        if (b == '\n') {
            line_buf[line_len] = '\0';
            if (line_len > 0) {
                pico_note_rx_activity();

                uint8_t gr_blob[k_gr_max_payload];
                size_t gr_len = 0;
                bool pressed = false;
                unsigned hid_key = 0;

                if (parse_gr_line(line_buf, gr_blob, &gr_len, sizeof gr_blob)) {
                    s_pico_lines_gr_interval.fetch_add(1, std::memory_order_relaxed);
                    uint16_t bits = 0;
                    if (!pico_uart_vendor_decode_matrixportal_buttons(gr_blob, static_cast<uint16_t>(gr_len), &bits)) {
                        ESP_LOGD(TAG_PICO, "GR decode skip len=%u", (unsigned)gr_len);
                    } else {
                        vendor_apply_gamepad_bits(bits);
                    }
                } else if (parse_kb_line(line_buf, &hid_key, &pressed)) {
                    s_pico_lines_kb_interval.fetch_add(1, std::memory_order_relaxed);
                    if (hid_key == static_cast<unsigned>(PICO_UART_BRIDGE_HID_KEYBOARD_ESCAPE) &&
                        PICO_UART_BRIDGE_DROP_HID_KEYBOARD_ESCAPE) {
                        if (pressed) {
                            /* Composite pads expose Start as ESC; YAML drops ESC so it cannot open Duke menu. */
                            pico_uart_note_start_press_uart("HID ESC");
                        }
                    } else {
                        const int32_t sc32 = pico_uart_hid_keyboard_to_duke_scancode(hid_key);
                        if (sc32 >= 0 && sc32 <= sc_LastScanCode) {
                            KB_InjectScanCode(static_cast<int>(sc32), pressed ? 1 : 0);
                            sync_held_state_from_keys();
                        } else if (pressed) {
                            ESP_LOGD(TAG_PICO, "KB HID 0x%02X not mapped to Duke", (unsigned)hid_key);
                        }
                    }
                } else if (strcmp(line_buf, "PING") == 0) {
                    const char *pong = "PONG\n";
                    uart_write_bytes(port, pong, strlen(pong));
                    ESP_LOGD(TAG_PICO, "PING → PONG");
                } else if (line_buf[0] == '[') {
                    s_pico_lines_bracket_interval.fetch_add(1, std::memory_order_relaxed);
                    if (strncmp(line_buf, "[hid]", 5) != 0) {
                        ESP_LOGI(TAG_PICO, "%s", line_buf);
                    }
                } else {
                    s_pico_unknown_lines_interval.fetch_add(1, std::memory_order_relaxed);
                    ESP_LOGW(TAG_PICO, "unknown line: %s", line_buf);
                }
            }
            line_len = 0;
            continue;
        }

        if (line_len < sizeof(line_buf) - 1) {
            line_buf[line_len++] = static_cast<char>(b);
        } else {
            line_len = 0;
        }
    }
}

void pico_uart_status_task(void *arg) {
    auto *info = static_cast<PicoUartStatusArg *>(arg);
    const uart_port_t port = info->port;
    const int uart_num = info->uart_num;
    const int tx_pin = info->tx_pin;
    const int rx_pin = info->rx_pin;
    const int baud = info->baud_rate;
    delete info;

    vTaskDelay(pdMS_TO_TICKS(200));

    const uint32_t stale_after_ms = 3500;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        const uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        const uint32_t last_ms = s_pico_last_rx_ms.load(std::memory_order_relaxed);
        const uint32_t ago_ms = (last_ms == 0) ? UINT32_MAX : (now_ms - last_ms);
        const bool ever = last_ms != 0;
        const bool link_ok = ever && ago_ms < stale_after_ms;

        size_t pending_rx = 0;
        if (uart_get_buffered_data_len(port, &pending_rx) != ESP_OK)
            pending_rx = 0;

        const uint32_t gr_n = s_pico_lines_gr_interval.exchange(0, std::memory_order_relaxed);
        const uint32_t kb_n = s_pico_lines_kb_interval.exchange(0, std::memory_order_relaxed);
        const uint32_t bytes_n = s_pico_bytes_rx_interval.exchange(0, std::memory_order_relaxed);
        const uint32_t unk_n = s_pico_unknown_lines_interval.exchange(0, std::memory_order_relaxed);
        const uint32_t bracket_n = s_pico_lines_bracket_interval.exchange(0, std::memory_order_relaxed);

        if (!ever) {
            if (bytes_n > 0u) {
                ESP_LOGW(TAG_PICO,
                         "status: NO_FRAMED_LINES (got %u raw bytes/s, no LF-terminated line yet) | UART%d tx=%d rx=%d baud=%d | "
                         "pending_rx=%u | gr=%u kb=%u dbg=%u unk=%u — check TX/RX not swapped; Pico sends '\\n'",
                         (unsigned) bytes_n, uart_num, tx_pin, rx_pin, baud, (unsigned) pending_rx, (unsigned) gr_n,
                         (unsigned) kb_n, (unsigned) bracket_n, (unsigned) unk_n);
            } else {
                ESP_LOGD(TAG_PICO,
                         "status: NO_DATA_YET | UART%d tx=%d rx=%d baud=%d | pending_rx=%u — idle RX "
                         "(Pico GP0 TX → ESP GPIO%d RX, common GND, 115200 8N1)",
                         uart_num, tx_pin, rx_pin, baud, (unsigned) pending_rx, rx_pin);
            }
        } else if (!link_ok) {
            ESP_LOGW(TAG_PICO,
                     "status: STALE (%lums since last line) | UART%d | pending_rx=%u | last 1s: bytes=%u gr=%u kb=%u dbg=%u unk=%u",
                     (unsigned long) ago_ms, uart_num, (unsigned) pending_rx, (unsigned) bytes_n, (unsigned) gr_n,
                     (unsigned) kb_n, (unsigned) bracket_n, (unsigned) unk_n);
        } else {
            ESP_LOGD(TAG_PICO,
                     "status: OK (last line %lums ago) | UART%d | pending_rx=%u | last 1s: bytes=%u gr=%u kb=%u dbg=%u unk=%u",
                     (unsigned long) ago_ms, uart_num, (unsigned) pending_rx, (unsigned) bytes_n, (unsigned) gr_n,
                     (unsigned) kb_n, (unsigned) bracket_n, (unsigned) unk_n);
        }
    }
}

}  // namespace

void input_init() {
    input_queue = xQueueCreate(QUEUE_DEPTH, sizeof(InputEvent));
}

void input_push(InputEvent evt) {
    if (input_queue)
        xQueueSend(input_queue, &evt, 0);
}

void input_push_from_isr(InputEvent evt) {
    BaseType_t woken = pdFALSE;
    if (input_queue)
        xQueueSendFromISR(input_queue, &evt, &woken);
    portYIELD_FROM_ISR(woken);
}

InputEvent input_pop() {
    InputEvent evt = InputEvent::NONE;
    if (input_queue)
        xQueueReceive(input_queue, &evt, 0);
    return evt;
}

GamepadState input_get_state() {
    taskENTER_CRITICAL(&input_state_mux);
    GamepadState copy = input_state;
    taskEXIT_CRITICAL(&input_state_mux);
    return copy;
}

void input_set_state(const GamepadState &state) {
    taskENTER_CRITICAL(&input_state_mux);
    input_state = state;
    taskEXIT_CRITICAL(&input_state_mux);
}

void input_start_pico_uart_bridge(int uart_num, int tx_pin, int rx_pin, int baud_rate) {
    auto *cfg = new PicoUartConfig{uart_num, tx_pin, rx_pin, baud_rate};
    xTaskCreatePinnedToCore(pico_uart_task, "pico_uart_in", 4096, cfg, 5, nullptr, 0);

    auto *st = new PicoUartStatusArg();
    st->port = static_cast<uart_port_t>(uart_num);
    st->uart_num = uart_num;
    st->tx_pin = tx_pin;
    st->rx_pin = rx_pin;
    st->baud_rate = baud_rate;
    xTaskCreatePinnedToCore(pico_uart_status_task, "pico_uart_stat", 3072, st, 4, nullptr, 0);
}

bool input_take_pico_uart_start_press(void) {
    return s_pico_uart_start_press_requested.exchange(false, std::memory_order_acq_rel);
}

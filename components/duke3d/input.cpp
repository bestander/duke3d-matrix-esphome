#include "input.h"

extern "C" {
#include "keyboard.h"
}

#include "pico_uart_gamepad_labels.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

static const char *const TAG_PICO = "pico_uart";

static QueueHandle_t input_queue = nullptr;
static const int QUEUE_DEPTH = 16;
static GamepadState input_state = {};
static portMUX_TYPE input_state_mux = portMUX_INITIALIZER_UNLOCKED;

static std::atomic<bool> s_random_demo_reload_requested{false};

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

// Link / traffic stats (UART task writes, status task reads).
static std::atomic<uint32_t> s_pico_last_rx_ms{0};
static std::atomic<uint32_t> s_pico_lines_sc_interval{0};
static std::atomic<uint32_t> s_pico_bytes_rx_interval{0};
static std::atomic<uint32_t> s_pico_unknown_lines_interval{0};
static std::atomic<uint32_t> s_pico_lines_cmd_interval{0};
static std::atomic<uint32_t> s_pico_lines_bracket_interval{0};

static void pico_note_rx_activity() {
    s_pico_last_rx_ms.store(xTaskGetTickCount() * portTICK_PERIOD_MS, std::memory_order_relaxed);
}

bool parse_scancode_line(const char *line, int *scancode_out, bool *pressed_out) {
    if (line == nullptr || scancode_out == nullptr || pressed_out == nullptr) return false;
    unsigned int sc = 0;
    int pressed = 0;
    if (sscanf(line, "SC,%x,%d", &sc, &pressed) != 2) return false;
    *scancode_out = static_cast<int>(sc);
    *pressed_out = (pressed != 0);
    return true;
}

bool parse_random_demo_cmd_line(const char *line) {
    return line != nullptr && strcmp(line, "CMD,RANDOM_DEMO") == 0;
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

    // Pico "[hid] …" hex dumps can exceed 80 chars; keep room for NUL + worst-case truncation edge.
    char line_buf[192];
    size_t line_len = 0;
    uint8_t b = 0;

    while (true) {
        const int n = uart_read_bytes(port, &b, 1, pdMS_TO_TICKS(100));
        if (n <= 0) continue;
        s_pico_bytes_rx_interval.fetch_add((uint32_t) n, std::memory_order_relaxed);
        if (b == '\r') continue;

        if (b == '\n') {
            line_buf[line_len] = '\0';
            if (line_len > 0) {
                pico_note_rx_activity();
                int scancode = -1;
                bool pressed = false;
                if (parse_scancode_line(line_buf, &scancode, &pressed)) {
                    s_pico_lines_sc_interval.fetch_add(1, std::memory_order_relaxed);
                    if (scancode >= 0 && scancode <= sc_LastScanCode) {
                        KB_InjectScanCode(scancode, pressed ? 1 : 0);
                        sync_held_state_from_keys();
                        const auto sc_u = static_cast<unsigned>(scancode);
                        const char *gp_btn = pico_uart_gamepad_label_for_scancode(sc_u);
                        if (gp_btn != nullptr && pressed) {
                            ESP_LOGI(TAG_PICO, "%s", gp_btn);
                        }
                    } else {
                        ESP_LOGW(TAG_PICO, "ignored out-of-range scancode %d", scancode);
                    }
                } else if (parse_random_demo_cmd_line(line_buf)) {
                    s_pico_lines_cmd_interval.fetch_add(1, std::memory_order_relaxed);
                    s_random_demo_reload_requested.store(true, std::memory_order_release);
                    ESP_LOGI(TAG_PICO, "start");
                } else if (strcmp(line_buf, "PING") == 0) {
                    const char *pong = "PONG\n";
                    uart_write_bytes(port, pong, strlen(pong));
                    ESP_LOGD(TAG_PICO, "PING → PONG");
                } else if (line_buf[0] == '[') {
                    s_pico_lines_bracket_interval.fetch_add(1, std::memory_order_relaxed);
                    /* Pico "[hid] …" is noisy once UART decode is verified; keep "[bridge]" etc. visible. */
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

        const uint32_t sc_n = s_pico_lines_sc_interval.exchange(0, std::memory_order_relaxed);
        const uint32_t cmd_n = s_pico_lines_cmd_interval.exchange(0, std::memory_order_relaxed);
        const uint32_t bytes_n = s_pico_bytes_rx_interval.exchange(0, std::memory_order_relaxed);
        const uint32_t unk_n = s_pico_unknown_lines_interval.exchange(0, std::memory_order_relaxed);
        const uint32_t bracket_n = s_pico_lines_bracket_interval.exchange(0, std::memory_order_relaxed);

        if (!ever) {
            if (bytes_n > 0u) {
                ESP_LOGW(TAG_PICO,
                         "status: NO_FRAMED_LINES (got %u raw bytes/s, no LF-terminated line yet) | UART%d tx=%d rx=%d baud=%d | "
                         "pending_rx=%u | sc=%u cmd=%u dbg=%u unk=%u — check TX/RX not swapped; Pico sends '\\n'",
                         (unsigned) bytes_n, uart_num, tx_pin, rx_pin, baud, (unsigned) pending_rx, (unsigned) sc_n,
                         (unsigned) cmd_n, (unsigned) bracket_n, (unsigned) unk_n);
            } else {
                /* Idle RX before any Pico traffic is normal; keep at DEBUG so INFO logs stay clean. */
                ESP_LOGD(TAG_PICO,
                         "status: NO_DATA_YET | UART%d tx=%d rx=%d baud=%d | pending_rx=%u — idle RX "
                         "(Pico GP0 TX → ESP GPIO%d RX, common GND, 115200 8N1)",
                         uart_num, tx_pin, rx_pin, baud, (unsigned) pending_rx, rx_pin);
            }
        } else if (!link_ok) {
            ESP_LOGW(TAG_PICO,
                     "status: STALE (%lums since last line) | UART%d | pending_rx=%u | last 1s: bytes=%u sc=%u cmd=%u dbg=%u unk=%u",
                     (unsigned long) ago_ms, uart_num, (unsigned) pending_rx, (unsigned) bytes_n, (unsigned) sc_n,
                     (unsigned) cmd_n, (unsigned) bracket_n, (unsigned) unk_n);
        } else {
            ESP_LOGD(TAG_PICO,
                     "status: OK (last line %lums ago) | UART%d | pending_rx=%u | last 1s: bytes=%u sc=%u cmd=%u dbg=%u unk=%u",
                     (unsigned long) ago_ms, uart_num, (unsigned) pending_rx, (unsigned) bytes_n, (unsigned) sc_n,
                     (unsigned) cmd_n, (unsigned) bracket_n, (unsigned) unk_n);
        }
    }
}

}  // namespace

void input_init() {
    input_queue = xQueueCreate(QUEUE_DEPTH, sizeof(InputEvent));
}

// Safe to call from task context only (not ISR).
void input_push(InputEvent evt) {
    if (input_queue) xQueueSend(input_queue, &evt, 0);
}

// Use ONLY from hardware ISR context (GPIO interrupt handler).
void input_push_from_isr(InputEvent evt) {
    BaseType_t woken = pdFALSE;
    if (input_queue) xQueueSendFromISR(input_queue, &evt, &woken);
    portYIELD_FROM_ISR(woken);
}

InputEvent input_pop() {
    InputEvent evt = InputEvent::NONE;
    if (input_queue) xQueueReceive(input_queue, &evt, 0);
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

bool input_take_random_demo_reload_request(void) {
    return s_random_demo_reload_requested.exchange(false, std::memory_order_acq_rel);
}

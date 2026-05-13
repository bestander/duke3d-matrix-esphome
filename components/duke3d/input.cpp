#include "input.h"

extern "C" {
#include "keyboard.h"
}

#include "driver/uart.h"
#include <cstdio>
#include <cstring>

static QueueHandle_t input_queue = nullptr;
static const int QUEUE_DEPTH = 16;
static GamepadState input_state = {};
static portMUX_TYPE input_state_mux = portMUX_INITIALIZER_UNLOCKED;

namespace {

struct PicoUartConfig {
    int uart_num;
    int tx_pin;
    int rx_pin;
    int baud_rate;
};

bool parse_scancode_line(const char *line, int *scancode_out, bool *pressed_out) {
    if (line == nullptr || scancode_out == nullptr || pressed_out == nullptr) return false;
    unsigned int sc = 0;
    int pressed = 0;
    if (sscanf(line, "SC,%x,%d", &sc, &pressed) != 2) return false;
    *scancode_out = static_cast<int>(sc);
    *pressed_out = (pressed != 0);
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

    printf("[input] Pico UART bridge started on UART%d (tx=%d rx=%d baud=%d)\n", cfg.uart_num,
           cfg.tx_pin, cfg.rx_pin, cfg.baud_rate);

    char line_buf[80];
    size_t line_len = 0;
    uint8_t b = 0;

    while (true) {
        const int n = uart_read_bytes(port, &b, 1, pdMS_TO_TICKS(100));
        if (n <= 0) continue;
        if (b == '\r') continue;

        if (b == '\n') {
            line_buf[line_len] = '\0';
            if (line_len > 0) {
                int scancode = -1;
                bool pressed = false;
                if (parse_scancode_line(line_buf, &scancode, &pressed)) {
                    if (scancode >= 0 && scancode <= sc_LastScanCode) {
                        KB_InjectScanCode(scancode, pressed ? 1 : 0);
                        sync_held_state_from_keys();
                    } else {
                        printf("[input] Ignored out-of-range scancode: %d\n", scancode);
                    }
                } else if (strcmp(line_buf, "PING") == 0) {
                    const char *pong = "PONG\n";
                    uart_write_bytes(port, pong, strlen(pong));
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
}

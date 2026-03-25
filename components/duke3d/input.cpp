#include "input.h"

static QueueHandle_t input_queue = nullptr;
static const int QUEUE_DEPTH = 16;

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

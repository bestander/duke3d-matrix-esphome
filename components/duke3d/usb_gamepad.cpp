#include "usb_gamepad.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include <string.h>
#include <atomic>

static const char* TAG = "usb_gamepad";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static bool s_debug_mode = false;
static bool s_running    = false;
static TaskHandle_t s_host_task = nullptr;

static usb_host_client_handle_t s_client_hdl = nullptr;
static usb_device_handle_t      s_dev_hdl    = nullptr;
static usb_transfer_t*          s_transfer   = nullptr;
static uint8_t  s_hid_intf_num = 0xFF;
static uint8_t  s_intr_ep_addr = 0;
static uint16_t s_intr_ep_mps  = 64;

// Atomic bitmask — written by USB host task, read by game task.
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

// ---------------------------------------------------------------------------
// HID report parser (generic gamepad — axes + buttons)
// ---------------------------------------------------------------------------

static void parse_report(const uint8_t* data, int len)
{
    if (len < 6) return;

    uint32_t bits = 0;

    uint8_t ax = data[GPJOY_AXIS_X_BYTE];
    uint8_t ay = data[GPJOY_AXIS_Y_BYTE];
    if (ax < GPJOY_AXIS_DEAD_LO)  bits |= BTN_TURN_LEFT;
    if (ax > GPJOY_AXIS_DEAD_HI)  bits |= BTN_TURN_RIGHT;
    if (ay < GPJOY_AXIS_DEAD_LO)  bits |= BTN_FORWARD;
    if (ay > GPJOY_AXIS_DEAD_HI)  bits |= BTN_BACK;

    uint8_t b0 = data[GPJOY_BTN_BYTE0];
    if (b0 & (1 << GPJOY_BTN_FIRE_BIT))  bits |= BTN_FIRE;
    if (b0 & (1 << GPJOY_BTN_USE_BIT))   bits |= BTN_USE;
    if (b0 & (1 << GPJOY_BTN_MAP_BIT))   bits |= BTN_MAP;
    if (b0 & (1 << GPJOY_BTN_LB_BIT))    bits |= BTN_STRAFE_LEFT;
    if (b0 & (1 << GPJOY_BTN_RB_BIT))    bits |= BTN_STRAFE_RIGHT;

    uint8_t b1 = data[GPJOY_BTN_BYTE1];
    if (b1 & (1 << GPJOY_BTN_MENU_BIT))  bits |= BTN_MENU;

    s_state.store(bits, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Interrupt-IN transfer callback — called in client task context
// ---------------------------------------------------------------------------

static void intr_transfer_cb(usb_transfer_t* transfer)
{
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        parse_report(transfer->data_buffer, transfer->actual_num_bytes);
        // Re-arm for the next report, as long as the device is still present.
        if (s_running && s_dev_hdl) {
            usb_host_transfer_submit(transfer);
        }
    } else if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE ||
               transfer->status == USB_TRANSFER_STATUS_CANCELED) {
        // Device gone or driver stopping — do not re-submit.
        s_state.store(0, std::memory_order_release);
    } else {
        // Transient error — re-arm anyway.
        if (s_running && s_dev_hdl) {
            usb_host_transfer_submit(transfer);
        }
    }
}

// ---------------------------------------------------------------------------
// Scan config descriptor → find HID interface + interrupt-IN endpoint
// ---------------------------------------------------------------------------

static bool find_hid_intr_ep(const usb_config_desc_t* config_desc,
                              uint8_t* out_intf_num,
                              uint8_t* out_ep_addr,
                              uint16_t* out_ep_mps)
{
    // Walk all descriptors looking for an interface with class=HID that has
    // an interrupt-IN endpoint.
    const usb_standard_desc_t* desc =
        (const usb_standard_desc_t*)config_desc;
    int offset = 0;
    uint16_t total_len = config_desc->wTotalLength;

    uint8_t  cur_intf   = 0xFF;
    bool     in_hid     = false;

    while (offset < total_len) {
        if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t* intf = (const usb_intf_desc_t*)desc;
            cur_intf  = intf->bInterfaceNumber;
            in_hid    = (intf->bInterfaceClass == USB_CLASS_HID);
        } else if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && in_hid) {
            const usb_ep_desc_t* ep = (const usb_ep_desc_t*)desc;
            bool is_in  = USB_EP_DESC_GET_EP_DIR(ep);
            bool is_int = (USB_EP_DESC_GET_XFERTYPE(ep) == USB_TRANSFER_TYPE_INTR);
            if (is_in && is_int) {
                *out_intf_num = cur_intf;
                *out_ep_addr  = ep->bEndpointAddress;
                *out_ep_mps   = ep->wMaxPacketSize;
                return true;
            }
        }
        desc = usb_parse_next_descriptor(desc, total_len, &offset);
        if (!desc) break;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Client event callback — device connect / disconnect
// ---------------------------------------------------------------------------

static void client_event_cb(const usb_host_client_event_msg_t* msg, void* /*arg*/)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        uint8_t addr = msg->new_dev.address;

        if (usb_host_device_open(s_client_hdl, addr, &s_dev_hdl) != ESP_OK) {
            ESP_LOGW(TAG, "usb_host_device_open failed for addr=%d", addr);
            return;
        }

        const usb_device_desc_t* dev_desc;
        usb_host_get_device_descriptor(s_dev_hdl, &dev_desc);
        ESP_LOGI(TAG, "USB device connected: VID=%04X PID=%04X class=%d",
                 dev_desc->idVendor, dev_desc->idProduct, dev_desc->bDeviceClass);

        const usb_config_desc_t* config_desc;
        usb_host_get_active_config_descriptor(s_dev_hdl, &config_desc);

        uint8_t  intf_num = 0xFF;
        uint8_t  ep_addr  = 0;
        uint16_t ep_mps   = 64;

        if (!find_hid_intr_ep(config_desc, &intf_num, &ep_addr, &ep_mps)) {
            ESP_LOGW(TAG, "No HID interrupt-IN endpoint found — not a gamepad?");
            usb_host_device_close(s_client_hdl, s_dev_hdl);
            s_dev_hdl = nullptr;
            return;
        }

        ESP_LOGI(TAG, "HID gamepad: intf=%d ep=0x%02X mps=%d", intf_num, ep_addr, ep_mps);
        s_hid_intf_num = intf_num;
        s_intr_ep_addr = ep_addr;
        s_intr_ep_mps  = ep_mps;

        if (usb_host_interface_claim(s_client_hdl, s_dev_hdl, intf_num, 0) != ESP_OK) {
            ESP_LOGW(TAG, "usb_host_interface_claim failed");
            usb_host_device_close(s_client_hdl, s_dev_hdl);
            s_dev_hdl = nullptr;
            return;
        }

        // Allocate interrupt transfer — num_bytes must be multiple of MPS.
        const int buf_size = ep_mps;
        if (usb_host_transfer_alloc(buf_size, 0, &s_transfer) != ESP_OK) {
            ESP_LOGW(TAG, "usb_host_transfer_alloc failed");
            usb_host_interface_release(s_client_hdl, s_dev_hdl, intf_num);
            usb_host_device_close(s_client_hdl, s_dev_hdl);
            s_dev_hdl  = nullptr;
            s_transfer = nullptr;
            return;
        }

        s_transfer->device_handle    = s_dev_hdl;
        s_transfer->bEndpointAddress = ep_addr;
        s_transfer->num_bytes        = ep_mps;
        s_transfer->callback         = intr_transfer_cb;
        s_transfer->context          = nullptr;

        esp_err_t err = usb_host_transfer_submit(s_transfer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "usb_host_transfer_submit failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Gamepad polling started");
        }

    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        ESP_LOGI(TAG, "USB device gone");
        s_state.store(0, std::memory_order_release);

        if (s_transfer) {
            usb_host_transfer_free(s_transfer);
            s_transfer = nullptr;
        }
        if (s_dev_hdl) {
            if (s_hid_intf_num != 0xFF) {
                usb_host_interface_release(s_client_hdl, s_dev_hdl, s_hid_intf_num);
                s_hid_intf_num = 0xFF;
            }
            usb_host_device_close(s_client_hdl, s_dev_hdl);
            s_dev_hdl = nullptr;
        }
    }
}

// ---------------------------------------------------------------------------
// USB host library task (pinned to Core 0)
// ---------------------------------------------------------------------------

static void usb_host_task(void* /*arg*/)
{
    usb_host_config_t host_cfg = {
        .skip_phy_setup  = false,
        .intr_flags      = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb  = nullptr,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    ESP_LOGI(TAG, "USB host installed");

    usb_host_client_config_t client_cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg          = nullptr,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client_cfg, &s_client_hdl));
    ESP_LOGI(TAG, "USB client registered — waiting for gamepad");

    while (s_running) {
        // Process client events (connect/disconnect + transfer completions).
        usb_host_client_handle_events(s_client_hdl, pdMS_TO_TICKS(50));

        // Process library-level events (needed to allow device enumeration).
        uint32_t flags = 0;
        usb_host_lib_handle_events(0, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }

    // Tear down.
    if (s_transfer) {
        usb_host_transfer_free(s_transfer);
        s_transfer = nullptr;
    }
    if (s_dev_hdl) {
        if (s_hid_intf_num != 0xFF) {
            usb_host_interface_release(s_client_hdl, s_dev_hdl, s_hid_intf_num);
        }
        usb_host_device_close(s_client_hdl, s_dev_hdl);
        s_dev_hdl = nullptr;
    }
    usb_host_client_deregister(s_client_hdl);
    s_client_hdl = nullptr;
    usb_host_uninstall();
    ESP_LOGI(TAG, "USB host stopped");
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool usb_gamepad_start(void)
{
    // Check BOOT button (GPIO 0 with internal pull-up): LOW held = debug mode.
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << 0,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    int low_count = 0;
    for (int i = 0; i < 5; i++) {
        if (gpio_get_level(GPIO_NUM_0) == 0) low_count++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_debug_mode = (low_count >= 3);

    if (s_debug_mode) {
        ESP_LOGW(TAG, "BOOT button held — USB debug mode (JTAG/CDC enabled, gamepad disabled)");
        return false;
    }

    ESP_LOGI(TAG, "Starting USB OTG host (gamepad mode — USB JTAG/CDC will disconnect)");
    s_running = true;
    xTaskCreatePinnedToCore(usb_host_task, "usb_host", 4096, nullptr,
                            5, &s_host_task, 0);
    return true;
}

void usb_gamepad_stop(void)
{
    s_running = false;
    s_host_task = nullptr;
}

bool usb_gamepad_debug_mode(void)
{
    return s_debug_mode;
}

GamepadState usb_gamepad_get_state(void)
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

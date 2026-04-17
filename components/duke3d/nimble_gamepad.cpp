#include "nimble_gamepad.h"

#include "ble_gamepad.h"
#include "esphome/components/hud/hud.h"
#include "esp_log.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
}

namespace esphome {
namespace hud {
extern Hud *global_hud_instance;
}
}

namespace {

static const char *TAG = "nimble_gamepad";

static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static bool s_nimble_started = false;
static bool s_target_set = false;
static uint8_t s_target_mac_be[6] = {0};  // parsed AA:BB:CC:DD:EE:FF order

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_hid_svc_start = 0;
static uint16_t s_hid_svc_end = 0;
static uint16_t s_report_val_handle = 0;
static uint16_t s_report_cccd_handle = 0;

static const ble_uuid16_t kHidSvcUuid = BLE_UUID16_INIT(0x1812);
static const ble_uuid16_t kReportCharUuid = BLE_UUID16_INIT(0x2A4D);

static void set_hud_connected(bool connected) {
  if (esphome::hud::global_hud_instance != nullptr) {
    esphome::hud::global_hud_instance->set_ble_connected(connected);
  }
}

static bool parse_mac_be(const char *mac, uint8_t out[6]) {
  if (mac == nullptr) {
    return false;
  }
  unsigned int b[6];
  if (sscanf(mac, "%02x:%02x:%02x:%02x:%02x:%02x",
             &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
    return false;
  }
  for (int i = 0; i < 6; i++) {
    out[i] = static_cast<uint8_t>(b[i]);
  }
  return true;
}

static bool addr_matches_target(const ble_addr_t *addr) {
  if (!s_target_set || addr == nullptr) {
    return false;
  }
  // NimBLE stores address bytes little-endian in ble_addr_t::val.
  const bool little_match = memcmp(addr->val, s_target_mac_be, 6) == 0;
  const bool big_match =
      addr->val[0] == s_target_mac_be[5] && addr->val[1] == s_target_mac_be[4] &&
      addr->val[2] == s_target_mac_be[3] && addr->val[3] == s_target_mac_be[2] &&
      addr->val[4] == s_target_mac_be[1] && addr->val[5] == s_target_mac_be[0];
  return little_match || big_match;
}

static void start_scan(void);
static int gap_event_cb(struct ble_gap_event *event, void *arg);

static int cccd_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg) {
  (void)conn_handle;
  (void)attr;
  (void)arg;
  if (error->status != 0) {
    ESP_LOGW(TAG, "CCCD write failed: status=%d", error->status);
    return 0;
  }
  ESP_LOGI(TAG, "HID notifications enabled");
  return 0;
}

static int disc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg) {
  (void)arg;
  if (error->status == 0 && chr != nullptr) {
    s_report_val_handle = chr->val_handle;
    s_report_cccd_handle = static_cast<uint16_t>(chr->val_handle + 1);
    return 0;
  }
  if (error->status != BLE_HS_EDONE) {
    ESP_LOGW(TAG, "Characteristic discovery failed: status=%d", error->status);
    return 0;
  }
  if (s_report_val_handle == 0 || s_report_cccd_handle == 0) {
    ESP_LOGW(TAG, "HID report characteristic 0x2A4D not found");
    return 0;
  }
  const uint8_t notify_on[2] = {0x01, 0x00};
  int rc = ble_gattc_write_flat(conn_handle, s_report_cccd_handle, notify_on, sizeof(notify_on),
                                cccd_write_cb, nullptr);
  if (rc != 0) {
    ESP_LOGW(TAG, "Failed to write CCCD: rc=%d", rc);
  }
  return 0;
}

static int disc_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service, void *arg) {
  (void)arg;
  if (error->status == 0 && service != nullptr) {
    s_hid_svc_start = service->start_handle;
    s_hid_svc_end = service->end_handle;
    return 0;
  }
  if (error->status != BLE_HS_EDONE) {
    ESP_LOGW(TAG, "Service discovery failed: status=%d", error->status);
    return 0;
  }
  if (s_hid_svc_start == 0 || s_hid_svc_end == 0) {
    ESP_LOGW(TAG, "HID service 0x1812 not found");
    return 0;
  }
  int rc = ble_gattc_disc_chrs_by_uuid(conn_handle, s_hid_svc_start, s_hid_svc_end,
                                       &kReportCharUuid.u, disc_chr_cb, nullptr);
  if (rc != 0) {
    ESP_LOGW(TAG, "Characteristic discovery start failed: rc=%d", rc);
  }
  return 0;
}

static void start_scan(void) {
  if (!s_target_set) {
    ESP_LOGE(TAG, "BLE target MAC is not set; skipping scan");
    return;
  }

  struct ble_gap_disc_params params;
  memset(&params, 0, sizeof(params));
  params.passive = 0;
  params.itvl = 0x0010;
  params.window = 0x0010;
  params.filter_duplicates = 1;

  int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params, gap_event_cb, nullptr);
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    ESP_LOGW(TAG, "ble_gap_disc failed: rc=%d", rc);
    return;
  }
  ESP_LOGI(TAG, "Scanning for BLE gamepad...");
}

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
  (void)arg;
  switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
      if (!addr_matches_target(&event->disc.addr)) {
        return 0;
      }
      ble_gap_disc_cancel();
      int rc =
          ble_gap_connect(s_own_addr_type, &event->disc.addr, BLE_HS_FOREVER, nullptr, gap_event_cb, nullptr);
      if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_connect failed: rc=%d", rc);
        start_scan();
      }
      return 0;
    }

    case BLE_GAP_EVENT_CONNECT: {
      if (event->connect.status != 0) {
        ESP_LOGW(TAG, "Connect failed: status=%d", event->connect.status);
        start_scan();
        return 0;
      }
      s_conn_handle = event->connect.conn_handle;
      s_hid_svc_start = 0;
      s_hid_svc_end = 0;
      s_report_val_handle = 0;
      s_report_cccd_handle = 0;
      set_hud_connected(true);
      ESP_LOGI(TAG, "Connected (conn_handle=%u)", s_conn_handle);
      int rc =
          ble_gattc_disc_svc_by_uuid(s_conn_handle, &kHidSvcUuid.u, disc_svc_cb, nullptr);
      if (rc != 0) {
        ESP_LOGW(TAG, "Service discovery start failed: rc=%d", rc);
      }
      return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
      if (!event->notify_rx.indication && event->notify_rx.om != nullptr) {
        uint8_t report[64];
        uint16_t out_len = 0;
        int rc = ble_hs_mbuf_to_flat(event->notify_rx.om, report, sizeof(report), &out_len);
        if (rc == 0 && out_len > 0) {
          ble_gamepad_push_report(report, out_len);
        }
      }
      return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
      ESP_LOGW(TAG, "Disconnected: reason=%d", event->disconnect.reason);
      s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
      s_hid_svc_start = 0;
      s_hid_svc_end = 0;
      s_report_val_handle = 0;
      s_report_cccd_handle = 0;
      set_hud_connected(false);
      start_scan();
      return 0;
    }

    default:
      return 0;
  }
}

static void on_sync(void) {
  int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
  if (rc != 0) {
    ESP_LOGW(TAG, "ble_hs_id_infer_auto failed: rc=%d", rc);
    return;
  }
  start_scan();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "NimBLE host reset: reason=%d", reason); }

static void nimble_host_task(void *param) {
  (void)param;
  nimble_port_run();
  nimble_port_freertos_deinit();
}

}  // namespace

void nimble_gamepad_set_target_mac(const char *mac) {
  s_target_set = parse_mac_be(mac, s_target_mac_be);
  if (!s_target_set) {
    ESP_LOGE(TAG, "Invalid BLE MAC '%s' (expected AA:BB:CC:DD:EE:FF)", mac ? mac : "");
    return;
  }
  ESP_LOGI(TAG, "Configured BLE target MAC: %s", mac);
}

void nimble_gamepad_init(void) {
  if (s_nimble_started) {
    return;
  }
  if (!s_target_set) {
    ESP_LOGE(TAG, "nimble_gamepad_init called without target MAC");
    return;
  }

  nimble_port_init();

  ble_hs_cfg.reset_cb = on_reset;
  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
  ble_hs_cfg.sm_bonding = 0;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 0;

  set_hud_connected(false);
  nimble_port_freertos_init(nimble_host_task);
  s_nimble_started = true;
}

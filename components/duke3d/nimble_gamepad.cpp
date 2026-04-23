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

static bool s_use_uuid = false;
static ble_uuid128_t s_target_uuid = {};
static bool s_use_name = false;
static char s_target_name[64] = {};


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

static bool parse_uuid128(const char *str, ble_uuid128_t *out) {
  if (str == nullptr) return false;
  unsigned int b[16];
  // Accept XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
  if (sscanf(str,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7],
             &b[8], &b[9], &b[10], &b[11], &b[12], &b[13], &b[14], &b[15]) != 16)
    return false;
  out->u.type = BLE_UUID_TYPE_128;
  // NimBLE stores 128-bit UUIDs little-endian
  for (int i = 0; i < 16; i++) out->value[i] = static_cast<uint8_t>(b[15 - i]);
  return true;
}

// Returns true if advertisement contains target UUID. Also populates name_out and logs UUIDs.
static bool adv_matches_target(const uint8_t *data, uint8_t len,
                               char *name_out, size_t name_out_sz,
                               char *uuids_out, size_t uuids_out_sz) {
  name_out[0] = '\0';
  uuids_out[0] = '\0';
  if (data == nullptr || len == 0) return false;
  struct ble_hs_adv_fields fields;
  if (ble_hs_adv_parse_fields(&fields, data, len) != 0) return false;

  if (fields.name != nullptr && fields.name_len > 0) {
    size_t copy = fields.name_len < name_out_sz - 1 ? fields.name_len : name_out_sz - 1;
    memcpy(name_out, fields.name, copy);
    name_out[copy] = '\0';
  }

  // Build UUID log string and check for target
  bool matched = false;
  int pos = 0;
  for (int i = 0; i < fields.num_uuids16 && pos < (int)uuids_out_sz - 8; i++) {
    pos += snprintf(uuids_out + pos, uuids_out_sz - pos, "%04X ", fields.uuids16[i].value);
  }
  for (int i = 0; i < fields.num_uuids128; i++) {
    const uint8_t *v = fields.uuids128[i].value;
    if (pos < (int)uuids_out_sz - 40)
      pos += snprintf(uuids_out + pos, uuids_out_sz - pos,
                      "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X ",
                      v[15],v[14],v[13],v[12],v[11],v[10],v[9],v[8],
                      v[7],v[6],v[5],v[4],v[3],v[2],v[1],v[0]);
    if (s_use_uuid && ble_uuid_cmp(&fields.uuids128[i].u, &s_target_uuid.u) == 0) matched = true;
  }
  if (!matched && s_use_name && name_out[0] != '\0')
    matched = (strncmp(name_out, s_target_name, sizeof(s_target_name)) == 0);
  return matched;
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
  char target_str[48] = "(no uuid)";
  if (s_use_uuid) {
    const uint8_t *v = s_target_uuid.value;
    snprintf(target_str, sizeof(target_str),
             "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             v[15],v[14],v[13],v[12],v[11],v[10],v[9],v[8],
             v[7],v[6],v[5],v[4],v[3],v[2],v[1],v[0]);
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
  ESP_LOGI(TAG, "Scanning for BLE gamepad (uuid=%s name=%s)...",
           target_str, s_use_name ? s_target_name : "(none)");
}

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
  (void)arg;
  switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
      const ble_addr_t *a = &event->disc.addr;
      char found_str[18];
      snprintf(found_str, sizeof(found_str), "%02X:%02X:%02X:%02X:%02X:%02X",
               a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);

      char dev_name[32];
      char dev_uuids[120];
      bool matched = adv_matches_target(event->disc.data, event->disc.length_data,
                                        dev_name, sizeof(dev_name),
                                        dev_uuids, sizeof(dev_uuids));
      if (!matched) {
        ESP_LOGI(TAG, "Found device %s (addrtype=%d evtype=%d) name='%s' uuids=[%s]— not target",
                 found_str, a->type, event->disc.event_type, dev_name, dev_uuids);
        return 0;
      }
      ESP_LOGI(TAG, "Found target device %s — attempting connect", found_str);
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
  ESP_LOGI(TAG, "NimBLE host synced — BLE stack ready");
  int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
  if (rc != 0) {
    ESP_LOGW(TAG, "ble_hs_id_infer_auto failed: rc=%d", rc);
    return;
  }
  ESP_LOGI(TAG, "Own addr type: %d", s_own_addr_type);
  start_scan();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "NimBLE host reset: reason=%d", reason); }

static void nimble_host_task(void *param) {
  (void)param;
  nimble_port_run();
  nimble_port_freertos_deinit();
}

}  // namespace

void nimble_gamepad_set_target_name(const char *name) {
  if (name == nullptr || name[0] == '\0') return;
  strncpy(s_target_name, name, sizeof(s_target_name) - 1);
  s_target_name[sizeof(s_target_name) - 1] = '\0';
  s_use_name = true;
  ESP_LOGI(TAG, "Configured BLE target name: %s", s_target_name);
}

void nimble_gamepad_set_target_uuid(const char *uuid_str) {
  s_use_uuid = parse_uuid128(uuid_str, &s_target_uuid);
  if (!s_use_uuid) {
    ESP_LOGE(TAG, "Invalid BLE UUID '%s'", uuid_str ? uuid_str : "");
    return;
  }
  ESP_LOGI(TAG, "Configured BLE target UUID: %s", uuid_str);
}

void nimble_gamepad_init(void) {
  if (s_nimble_started) {
    ESP_LOGI(TAG, "NimBLE already started, skipping init");
    return;
  }
  ESP_LOGI(TAG, "Initializing NimBLE gamepad (uuid=%s)", s_use_uuid ? "yes" : "no");

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

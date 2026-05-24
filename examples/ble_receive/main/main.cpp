// FastJsonDL — BLE GATT server receive example
//
// Advertises as a BLE GATT server and receives a FastJsonDL JSON payload from
// a remote client (e.g. the FastJsonRenderer app).  Once the full payload has
// arrived it is passed to FastJsonDL for rendering on an e-paper display.
//
// Protocol (same wire format used by the CALE / FastJsonRenderer client):
//   1. Client writes 5 bytes:  { 0x01, len[0], len[1], len[2], len[3] }
//      where len is the total JSON payload size as a little-endian uint32_t.
//   2. Client writes N data chunks containing the raw JSON bytes.
//   3. Client writes 1 byte:   { 0x09 }  — end-of-transfer signal.
//      The server then renders the accumulated JSON and refreshes the display.
//   4. On disconnect the server restarts advertising and clears its state.
//
// Target board: ESP32-C5 (or any ESP32 variant with BLE support).
//               Adjust BB_PANEL_SENSORIA_C5 / setPanelSize() for your panel.
//
// The receive buffer is allocated from SPIRAM when available; it falls back
// to internal heap when SPIRAM is not present.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "esp_timer.h"

#include "FastEPD.h"
#include "FastJsonDL.h"

// ---------------------------------------------------------------------------
// Logging tag
// ---------------------------------------------------------------------------
static const char *TAG = "BLE_FASTJSON";

// ---------------------------------------------------------------------------
// BLE identifiers
// ---------------------------------------------------------------------------
// Service UUID: Heart-Rate (0x180D) — reused here as the transport service.
// Change to a custom 128-bit UUID in production if needed.
#define BLE_SERVICE_UUID        0x180D
#define BLE_CHAR_UUID           0x180D

#define DEVICE_NAME             "FastJsonDL"
#define GATTS_NUM_HANDLES       4
#define PREPARE_BUF_MAX_SIZE    1024

// ---------------------------------------------------------------------------
// Receive buffer
// ---------------------------------------------------------------------------
// Maximum JSON payload the device will accept over BLE (bytes).
// Increase if you need larger layouts, but keep it within available RAM.
#define JSON_BUF_MAX_SIZE       (64 * 1024)

static uint8_t  *s_json_buf      = nullptr; // Heap-allocated receive buffer
static uint32_t  s_json_buf_pos  = 0;        // Write cursor into s_json_buf
static uint32_t  s_expected_len  = 0;        // Content-length announced by client
static uint16_t  s_write_events  = 0;        // Number of WRITE_EVT calls received
static uint64_t  s_start_time    = 0;        // Timestamp of first data chunk

// ---------------------------------------------------------------------------
// EPD / FastJsonDL
// ---------------------------------------------------------------------------
static FASTEPD    *s_epaper = nullptr;
static FastJsonDL *s_dl     = nullptr;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void gatts_profile_event_handler(esp_gatts_cb_event_t event,
                                         esp_gatt_if_t        gatts_if,
                                         esp_ble_gatts_cb_param_t *param);

// ---------------------------------------------------------------------------
// GAP advertising configuration
// ---------------------------------------------------------------------------
static uint8_t s_adv_service_uuid128[16] = {
    /* LSB <-----------------------------------------------> MSB */
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xEE, 0x00, 0x00, 0x00,
};

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp        = false,
    .include_name        = true,
    .include_txpower     = false,
    .min_interval        = 0x0006,
    .max_interval        = 0x0010,
    .appearance          = 0x00,
    .manufacturer_len    = 0,
    .p_manufacturer_data = nullptr,
    .service_data_len    = 0,
    .p_service_data      = nullptr,
    .service_uuid_len    = sizeof(s_adv_service_uuid128),
    .p_service_uuid      = s_adv_service_uuid128,
    .flag                = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_data_t s_scan_rsp_data = {
    .set_scan_rsp        = true,
    .include_name        = true,
    .include_txpower     = true,
    .appearance          = 0x00,
    .manufacturer_len    = 0,
    .p_manufacturer_data = nullptr,
    .service_data_len    = 0,
    .p_service_data      = nullptr,
    .service_uuid_len    = sizeof(s_adv_service_uuid128),
    .p_service_uuid      = s_adv_service_uuid128,
    .flag                = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min      = 0x20,
    .adv_int_max      = 0x40,
    .adv_type         = ADV_TYPE_IND,
    .own_addr_type    = BLE_ADDR_TYPE_PUBLIC,
    .channel_map      = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// Track which adv config steps have completed before starting advertising.
static uint8_t s_adv_config_done = 0;
#define ADV_CONFIG_FLAG      (1 << 0)
#define SCAN_RSP_CONFIG_FLAG (1 << 1)

// ---------------------------------------------------------------------------
// GATTS profile table (single profile)
// ---------------------------------------------------------------------------
#define PROFILE_APP_ID 0

struct GattsProfileInst {
    esp_gatts_cb_t      gatts_cb;
    uint16_t            gatts_if;
    uint16_t            app_id;
    uint16_t            conn_id;
    uint16_t            service_handle;
    esp_gatt_srvc_id_t  service_id;
    uint16_t            char_handle;
    esp_bt_uuid_t       char_uuid;
    esp_gatt_perm_t     perm;
    esp_gatt_char_prop_t property;
    uint16_t            descr_handle;
    esp_bt_uuid_t       descr_uuid;
};

static GattsProfileInst s_profile_tab[1] = {
    [PROFILE_APP_ID] = {
        .gatts_cb  = gatts_profile_event_handler,
        .gatts_if  = ESP_GATT_IF_NONE,
    },
};

// ---------------------------------------------------------------------------
// Prepare-write environment (long-write support)
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t *prepare_buf;
    int      prepare_len;
} prepare_type_env_t;

static prepare_type_env_t s_prepare_write_env;

// ---------------------------------------------------------------------------
// Characteristic initial value
// ---------------------------------------------------------------------------
static uint8_t s_char_init_val[] = { 0x00, 0x00 };
static esp_gatt_char_prop_t s_char_property = 0;

static esp_attr_value_t s_gatts_char_val = {
    .attr_max_len = PREPARE_BUF_MAX_SIZE,
    .attr_len     = sizeof(s_char_init_val),
    .attr_value   = s_char_init_val,
};

// ---------------------------------------------------------------------------
// Helper: reset transfer state
// ---------------------------------------------------------------------------
static void reset_transfer_state(void)
{
    s_json_buf_pos  = 0;
    s_expected_len  = 0;
    s_write_events  = 0;
    s_start_time    = 0;
}

// ---------------------------------------------------------------------------
// Helper: render accumulated JSON buffer and refresh the display
// ---------------------------------------------------------------------------
static void render_json_and_refresh(void)
{
    if (s_json_buf_pos == 0) {
        ESP_LOGW(TAG, "render_json_and_refresh: empty buffer, skipping");
        return;
    }

    ESP_LOGI(TAG, "Rendering JSON (%lu bytes)...", (unsigned long)s_json_buf_pos);

    if (!s_dl->renderJson((const char *)s_json_buf, s_json_buf_pos)) {
        ESP_LOGE(TAG, "FastJsonDL error: %s", s_dl->getLastError());
    } else {
        ESP_LOGI(TAG, "Render OK — refreshing display");
        s_epaper->fullUpdate();
    }
}

// ---------------------------------------------------------------------------
// Long-write helpers (taken from ESP-IDF GATTS demo)
// ---------------------------------------------------------------------------
static void write_event_env(esp_gatt_if_t             gatts_if,
                             prepare_type_env_t       *env,
                             esp_ble_gatts_cb_param_t *param)
{
    esp_gatt_status_t status = ESP_GATT_OK;

    if (!param->write.need_rsp) {
        return;
    }

    if (param->write.is_prep) {
        if (env->prepare_buf == nullptr) {
            env->prepare_buf = (uint8_t *)malloc(PREPARE_BUF_MAX_SIZE);
            env->prepare_len = 0;
            if (env->prepare_buf == nullptr) {
                ESP_LOGE(TAG, "Gatt_server prep: no memory");
                status = ESP_GATT_NO_RESOURCES;
            }
        }

        if (status == ESP_GATT_OK) {
            if (param->write.offset > PREPARE_BUF_MAX_SIZE) {
                status = ESP_GATT_INVALID_OFFSET;
            } else if ((param->write.offset + param->write.len) > PREPARE_BUF_MAX_SIZE) {
                status = ESP_GATT_INVALID_ATTR_LEN;
            }
        }

        esp_gatt_rsp_t *rsp = (esp_gatt_rsp_t *)malloc(sizeof(esp_gatt_rsp_t));
        if (rsp != nullptr) {
            rsp->attr_value.len    = param->write.len;
            rsp->attr_value.handle = param->write.handle;
            rsp->attr_value.offset = param->write.offset;
            rsp->attr_value.auth_req = ESP_GATT_AUTH_REQ_NONE;
            memcpy(rsp->attr_value.value, param->write.value, param->write.len);
            esp_err_t err = esp_ble_gatts_send_response(gatts_if,
                                                         param->write.conn_id,
                                                         param->write.trans_id,
                                                         status, rsp);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "send_response error: %s", esp_err_to_name(err));
            }
            free(rsp);
        }

        if (status == ESP_GATT_OK && env->prepare_buf != nullptr) {
            memcpy(env->prepare_buf + param->write.offset,
                   param->write.value,
                   param->write.len);
            env->prepare_len += param->write.len;
        }
    } else {
        esp_ble_gatts_send_response(gatts_if,
                                     param->write.conn_id,
                                     param->write.trans_id,
                                     status, nullptr);
    }
}

static void exec_write_event_env(prepare_type_env_t       *env,
                                  esp_ble_gatts_cb_param_t *param)
{
    if (param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC) {
        esp_log_buffer_hex(TAG, env->prepare_buf, env->prepare_len);
    } else {
        ESP_LOGI(TAG, "ESP_GATT_PREP_WRITE_CANCEL");
    }

    if (env->prepare_buf != nullptr) {
        free(env->prepare_buf);
        env->prepare_buf = nullptr;
    }
    env->prepare_len = 0;
}

// ---------------------------------------------------------------------------
// GAP event handler
// ---------------------------------------------------------------------------
static void gap_event_handler(esp_gap_ble_cb_event_t event,
                               esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        s_adv_config_done &= ~ADV_CONFIG_FLAG;
        if (s_adv_config_done == 0) {
            esp_ble_gap_start_advertising(&s_adv_params);
        }
        break;

    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        s_adv_config_done &= ~SCAN_RSP_CONFIG_FLAG;
        if (s_adv_config_done == 0) {
            esp_ble_gap_start_advertising(&s_adv_params);
        }
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising start failed");
        } else {
            ESP_LOGI(TAG, "Advertising started");
        }
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising stop failed");
        } else {
            ESP_LOGI(TAG, "Advertising stopped");
        }
        break;

    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        ESP_LOGI(TAG, "Connection params updated: status=%d min=%d max=%d "
                 "interval=%d latency=%d timeout=%d",
                 param->update_conn_params.status,
                 param->update_conn_params.min_int,
                 param->update_conn_params.max_int,
                 param->update_conn_params.conn_int,
                 param->update_conn_params.latency,
                 param->update_conn_params.timeout);
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// GATTS profile event handler
// ---------------------------------------------------------------------------
static void gatts_profile_event_handler(esp_gatts_cb_event_t     event,
                                          esp_gatt_if_t            gatts_if,
                                          esp_ble_gatts_cb_param_t *param)
{
    switch (event) {

    case ESP_GATTS_REG_EVT: {
        ESP_LOGI(TAG, "REGISTER_APP_EVT status=%d app_id=%d",
                 param->reg.status, param->reg.app_id);

        s_profile_tab[PROFILE_APP_ID].service_id.is_primary          = true;
        s_profile_tab[PROFILE_APP_ID].service_id.id.inst_id          = 0x00;
        s_profile_tab[PROFILE_APP_ID].service_id.id.uuid.len         = ESP_UUID_LEN_16;
        s_profile_tab[PROFILE_APP_ID].service_id.id.uuid.uuid.uuid16 = BLE_SERVICE_UUID;

        esp_err_t err = esp_ble_gap_set_device_name(DEVICE_NAME);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set_device_name failed: %s", esp_err_to_name(err));
        }

        err = esp_ble_gap_config_adv_data(&s_adv_data);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "config_adv_data failed: %s", esp_err_to_name(err));
        }
        s_adv_config_done |= ADV_CONFIG_FLAG;

        err = esp_ble_gap_config_adv_data(&s_scan_rsp_data);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "config_scan_rsp failed: %s", esp_err_to_name(err));
        }
        s_adv_config_done |= SCAN_RSP_CONFIG_FLAG;

        esp_ble_gatts_create_service(gatts_if,
                                      &s_profile_tab[PROFILE_APP_ID].service_id,
                                      GATTS_NUM_HANDLES);
        break;
    }

    case ESP_GATTS_READ_EVT: {
        // Respond with a dummy 4-byte value — no data to expose from this server.
        esp_gatt_rsp_t rsp;
        memset(&rsp, 0, sizeof(rsp));
        rsp.attr_value.handle    = param->read.handle;
        rsp.attr_value.len       = 4;
        rsp.attr_value.value[0]  = 0x00;
        rsp.attr_value.value[1]  = 0x01;
        rsp.attr_value.value[2]  = 0x02;
        rsp.attr_value.value[3]  = 0x03;
        esp_ble_gatts_send_response(gatts_if, param->read.conn_id,
                                     param->read.trans_id, ESP_GATT_OK, &rsp);
        break;
    }

    case ESP_GATTS_WRITE_EVT: {
        s_write_events++;
        // Record start time after the content-length command (second event onward).
        if (s_write_events == 2) {
            s_start_time = esp_timer_get_time();
        }

        if (!param->write.is_prep) {
            bool is_cmd = false;

            // --- Command 0x01: content-length announcement ---
            if (s_write_events == 1
                && param->write.len == 5
                && param->write.value[0] == 0x01)
            {
                is_cmd = true;
                s_expected_len =
                    (uint32_t)param->write.value[1]
                    | ((uint32_t)param->write.value[2] << 8)
                    | ((uint32_t)param->write.value[3] << 16)
                    | ((uint32_t)param->write.value[4] << 24);
                ESP_LOGI(TAG, "0x01 content-length: %lu bytes",
                         (unsigned long)s_expected_len);

                // Validate announced size before allocating.
                if (s_expected_len == 0 || s_expected_len > JSON_BUF_MAX_SIZE) {
                    ESP_LOGE(TAG, "Invalid content-length (%lu), max=%d",
                             (unsigned long)s_expected_len, JSON_BUF_MAX_SIZE);
                    reset_transfer_state();
                    break;
                }

                // Allocate (or re-use) receive buffer.
                if (s_json_buf == nullptr) {
#if CONFIG_SPIRAM
                    s_json_buf = (uint8_t *)heap_caps_malloc(JSON_BUF_MAX_SIZE,
                                                              MALLOC_CAP_SPIRAM);
#else
                    s_json_buf = (uint8_t *)malloc(JSON_BUF_MAX_SIZE);
#endif
                    if (s_json_buf == nullptr) {
                        ESP_LOGE(TAG, "Failed to allocate receive buffer (%d bytes)",
                                 JSON_BUF_MAX_SIZE);
                        reset_transfer_state();
                        break;
                    }
                }
            }

            // --- Command 0x09: end-of-transfer ---
            if (param->write.len == 1 && param->write.value[0] == 0x09) {
                is_cmd = true;
                ESP_LOGI(TAG, "0x09 EOF received");

                uint32_t ms_receive = (uint32_t)((esp_timer_get_time() - s_start_time) / 1000);
                ESP_LOGI(TAG, "Transfer complete: %lu bytes in %lu ms",
                         (unsigned long)s_json_buf_pos, (unsigned long)ms_receive);

                render_json_and_refresh();
                reset_transfer_state();
            }

            // --- Data chunk ---
            if (!is_cmd && s_json_buf != nullptr) {
                uint32_t remaining = JSON_BUF_MAX_SIZE - s_json_buf_pos;
                uint32_t copy_len  = param->write.len;

                if (copy_len > remaining) {
                    ESP_LOGW(TAG, "Buffer overflow: truncating %lu bytes to %lu",
                             (unsigned long)copy_len, (unsigned long)remaining);
                    copy_len = remaining;
                }

                memcpy(s_json_buf + s_json_buf_pos, param->write.value, copy_len);
                s_json_buf_pos += copy_len;

                ESP_LOGD(TAG, "WRITE_EVT buf_pos=%lu/%lu",
                         (unsigned long)s_json_buf_pos,
                         (unsigned long)s_expected_len);
            }
        }

        write_event_env(gatts_if, &s_prepare_write_env, param);
        break;
    }

    case ESP_GATTS_EXEC_WRITE_EVT:
        // Long-write (prepared write) support. In practice, clients should
        // keep chunks below the negotiated MTU to avoid this path.
        ESP_LOGW(TAG, "ESP_GATTS_EXEC_WRITE_EVT: long-writes not supported; "
                 "keep chunk size <= MTU - 3 bytes");
        exec_write_event_env(&s_prepare_write_env, param);
        break;

    case ESP_GATTS_MTU_EVT:
        ESP_LOGI(TAG, "MTU negotiated: %d bytes", param->mtu.mtu);
        break;

    case ESP_GATTS_CREATE_EVT: {
        ESP_LOGI(TAG, "CREATE_SERVICE_EVT status=%d handle=%d",
                 param->create.status, param->create.service_handle);

        s_profile_tab[PROFILE_APP_ID].service_handle = param->create.service_handle;
        s_profile_tab[PROFILE_APP_ID].char_uuid.len              = ESP_UUID_LEN_16;
        s_profile_tab[PROFILE_APP_ID].char_uuid.uuid.uuid16      = BLE_CHAR_UUID;

        esp_ble_gatts_start_service(s_profile_tab[PROFILE_APP_ID].service_handle);

        s_char_property = ESP_GATT_CHAR_PROP_BIT_READ
                        | ESP_GATT_CHAR_PROP_BIT_WRITE
                        | ESP_GATT_CHAR_PROP_BIT_WRITE_NR
                        | ESP_GATT_CHAR_PROP_BIT_NOTIFY;

        esp_err_t err = esp_ble_gatts_add_char(
            s_profile_tab[PROFILE_APP_ID].service_handle,
            &s_profile_tab[PROFILE_APP_ID].char_uuid,
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            s_char_property,
            &s_gatts_char_val,
            nullptr);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "add_char failed: %s", esp_err_to_name(err));
        }
        break;
    }

    case ESP_GATTS_ADD_CHAR_EVT: {
        ESP_LOGI(TAG, "ADD_CHAR_EVT status=%d attr_handle=%d svc_handle=%d",
                 param->add_char.status,
                 param->add_char.attr_handle,
                 param->add_char.service_handle);

        s_profile_tab[PROFILE_APP_ID].char_handle             = param->add_char.attr_handle;
        s_profile_tab[PROFILE_APP_ID].descr_uuid.len          = ESP_UUID_LEN_16;
        s_profile_tab[PROFILE_APP_ID].descr_uuid.uuid.uuid16  = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

        esp_err_t err = esp_ble_gatts_add_char_descr(
            s_profile_tab[PROFILE_APP_ID].service_handle,
            &s_profile_tab[PROFILE_APP_ID].descr_uuid,
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            nullptr, nullptr);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "add_char_descr failed: %s", esp_err_to_name(err));
        }
        break;
    }

    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
        s_profile_tab[PROFILE_APP_ID].descr_handle = param->add_char_descr.attr_handle;
        ESP_LOGI(TAG, "ADD_DESCR_EVT status=%d attr_handle=%d svc_handle=%d",
                 param->add_char_descr.status,
                 param->add_char_descr.attr_handle,
                 param->add_char_descr.service_handle);
        break;

    case ESP_GATTS_START_EVT:
        ESP_LOGI(TAG, "SERVICE_START_EVT status=%d handle=%d",
                 param->start.status, param->start.service_handle);
        break;

    case ESP_GATTS_CONNECT_EVT: {
        ESP_LOGI(TAG, "CONNECT_EVT conn_id=%d remote=%02x:%02x:%02x:%02x:%02x:%02x",
                 param->connect.conn_id,
                 param->connect.remote_bda[0], param->connect.remote_bda[1],
                 param->connect.remote_bda[2], param->connect.remote_bda[3],
                 param->connect.remote_bda[4], param->connect.remote_bda[5]);

        s_profile_tab[PROFILE_APP_ID].conn_id = param->connect.conn_id;

        // Request faster connection parameters to improve throughput.
        esp_ble_conn_update_params_t conn_params = {};
        memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        conn_params.latency = 0;
        conn_params.max_int = 0x20; // 40 ms
        conn_params.min_int = 0x10; // 20 ms
        conn_params.timeout = 1000; // 10 s supervision timeout
        esp_ble_gap_update_conn_params(&conn_params);
        break;
    }

    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(TAG, "DISCONNECT_EVT reason=0x%x", param->disconnect.reason);
        reset_transfer_state();
        esp_ble_gap_start_advertising(&s_adv_params);
        break;

    case ESP_GATTS_CONF_EVT:
        if (param->conf.status != ESP_GATT_OK) {
            esp_log_buffer_hex(TAG, param->conf.value, param->conf.len);
        }
        break;

    case ESP_GATTS_UNREG_EVT:
    case ESP_GATTS_DELETE_EVT:
    case ESP_GATTS_STOP_EVT:
    case ESP_GATTS_ADD_INCL_SRVC_EVT:
    case ESP_GATTS_OPEN_EVT:
    case ESP_GATTS_CANCEL_OPEN_EVT:
    case ESP_GATTS_CLOSE_EVT:
    case ESP_GATTS_LISTEN_EVT:
    case ESP_GATTS_CONGEST_EVT:
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Top-level GATTS dispatcher — routes events to the correct profile handler
// ---------------------------------------------------------------------------
static void gatts_event_handler(esp_gatts_cb_event_t     event,
                                  esp_gatt_if_t            gatts_if,
                                  esp_ble_gatts_cb_param_t *param)
{
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            s_profile_tab[param->reg.app_id].gatts_if = gatts_if;
        } else {
            ESP_LOGE(TAG, "Reg app failed app_id=%04x status=%d",
                     param->reg.app_id, param->reg.status);
            return;
        }
    }

    for (int idx = 0; idx < 1; idx++) {
        if (gatts_if == ESP_GATT_IF_NONE
            || gatts_if == s_profile_tab[idx].gatts_if)
        {
            if (s_profile_tab[idx].gatts_cb) {
                s_profile_tab[idx].gatts_cb(event, gatts_if, param);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// app_main
// ---------------------------------------------------------------------------
extern "C" void app_main(void)
{
    // -----------------------------------------------------------------------
    // Initialise NVS — required by the BT stack.
    // -----------------------------------------------------------------------
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // -----------------------------------------------------------------------
    // Initialise EPD and FastJsonDL.
    // -----------------------------------------------------------------------
    s_epaper = new FASTEPD();

    // Replace BB_PANEL_SENSORIA_C5 with the constant that matches your panel
    // (see the BB_PANEL_* enum in FastEPD.h).
    int rc = s_epaper->initPanel(BB_PANEL_SENSORIA_C5);
    if (rc != BBEP_SUCCESS) {
        ESP_LOGE(TAG, "EPD init failed (%d)", rc);
        return;
    }

    // Adjust width, height and flags for your specific panel.
    s_epaper->setPanelSize(1280, 780, BB_PANEL_FLAG_MIRROR_X);

    s_dl = new FastJsonDL(*s_epaper);
    // Register fonts here if your JSON layouts reference them, e.g.:
    //   static const FastJsonDLFont fonts[] = { { "Ubuntu40", ubuntu40 } };
    //   s_dl->setFontRegistry(fonts, 1);

    ESP_LOGI(TAG, "EPD initialised, waiting for BLE connection");

    // -----------------------------------------------------------------------
    // Initialise and start the BT / BLE stack.
    // -----------------------------------------------------------------------
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(PROFILE_APP_ID));
    ESP_ERROR_CHECK(esp_ble_gatt_set_local_mtu(500));

    // The BLE stack is event-driven; no polling loop is required here.
    // Advertising will start automatically once the GATTS_REG_EVT has been
    // handled and the adv-data configuration completes.
    ESP_LOGI(TAG, "BLE GATT server started — device name: \"%s\"", DEVICE_NAME);
}

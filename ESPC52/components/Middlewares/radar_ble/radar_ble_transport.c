#include "radar_ble_transport.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "radar_ble_binding_config.h"

#if RADAR_BLE_BINDING_ENABLED
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"
#include "nimble/nimble_npl.h"
#endif

static const char *TAG = "radar_ble_transport";
static radar_ble_transport_status_t s_status;
static radar_ble_notify_cb_t s_notify_cb;
static void *s_notify_ctx;
#ifndef RADAR_DEBUG_RAW_FRAME
#define RADAR_DEBUG_RAW_FRAME 0
#endif

const char *radar_ble_state_name(radar_ble_state_t state)
{
    switch (state) {
    case RADAR_BLE_STATE_DISABLED: return "disabled";
    case RADAR_BLE_STATE_UNAVAILABLE: return "unavailable";
    case RADAR_BLE_STATE_SCANNING: return "scanning";
    case RADAR_BLE_STATE_CONNECTING: return "connecting";
    case RADAR_BLE_STATE_DISCOVERING: return "discovering";
    case RADAR_BLE_STATE_PROFILE_FOUND: return "profile_found";
    case RADAR_BLE_STATE_SUBSCRIBING: return "subscribing";
    case RADAR_BLE_STATE_READY: return "ready";
    case RADAR_BLE_STATE_BACKOFF: return "backoff";
    default: return "unknown";
    }
}

#if RADAR_BLE_BINDING_ENABLED
#define RADAR_BLE_SURVEY_MAX_SERVICES 12U

typedef struct {
    uint16_t start_handle;
    uint16_t end_handle;
    ble_uuid_any_t uuid;
} radar_ble_service_range_t;

static ble_uuid_any_t s_notify_uuid;
static ble_uuid_any_t s_write_uuid;
static ble_uuid_any_t s_notify_service_uuid;
static uint16_t s_conn_handle;
static uint16_t s_service_start;
static uint16_t s_service_end;
static uint16_t s_notify_handle;
static uint16_t s_write_handle;
static uint16_t s_cccd_handle;
static uint8_t s_notify_priority;
static uint8_t s_write_priority;
static bool s_write_without_response;
static uint8_t s_control_command[64];
static size_t s_control_command_length;
static uint8_t s_own_addr_type;
static uint32_t s_backoff_ms;
static uint64_t s_last_device_log_ms;
static uint64_t s_notify_summary_window_start_ms;
static uint32_t s_notify_summary_count;
static uint32_t s_notify_summary_bytes;
static uint16_t s_notify_summary_last_len;
static struct ble_npl_callout s_backoff_callout;
static bool s_callout_initialized;
static radar_ble_service_range_t s_survey_services[RADAR_BLE_SURVEY_MAX_SERVICES];
static uint8_t s_survey_count;
static uint8_t s_survey_index;

static void start_scan(void);
static int gap_event(struct ble_gap_event *event, void *arg);

static uint64_t now_ms(void)
{
    const int64_t now_us = esp_timer_get_time();
    return now_us > 0 ? (uint64_t)(now_us / 1000) : 0U;
}

static void format_address(const ble_addr_t *address, char out[18])
{
    if (address == NULL || out == NULL) return;
    (void)snprintf(out, 18U, "%02X:%02X:%02X:%02X:%02X:%02X",
                   address->val[5], address->val[4], address->val[3],
                   address->val[2], address->val[1], address->val[0]);
}

static bool binding_mac_is_nonzero(void)
{
    static const uint8_t mac[6] = RADAR_BLE_BINDING_MAC;
    for (size_t i = 0U; i < sizeof(mac); ++i) {
        if (mac[i] != 0U) return true;
    }
    return false;
}

static bool peer_matches(const ble_addr_t *addr)
{
    static const uint8_t mac[6] = RADAR_BLE_BINDING_MAC;
    if (addr == NULL || RADAR_BLE_BINDING_ADDRESS_TYPE == RADAR_BLE_ADDR_TYPE_UNSPECIFIED ||
        (RADAR_BLE_BINDING_ADDRESS_TYPE != RADAR_BLE_ADDR_TYPE_ANY &&
         addr->type != RADAR_BLE_BINDING_ADDRESS_TYPE)) {
        return false;
    }
    for (size_t i = 0U; i < sizeof(mac); ++i) {
        if (addr->val[i] != mac[sizeof(mac) - 1U - i]) return false;
    }
    return true;
}

static void schedule_retry(void)
{
    if (s_backoff_ms == 0U) s_backoff_ms = 1000U;
    else if (s_backoff_ms < 30000U) s_backoff_ms *= 2U;
    if (s_backoff_ms > 30000U) s_backoff_ms = 30000U;
    s_status.state = RADAR_BLE_STATE_BACKOFF;
    s_status.backoff_ms = s_backoff_ms;
    ESP_LOGI(TAG, "RADAR_RECONNECT local_id=%u retry_ms=%lu count=%lu",
             (unsigned int)RADAR_BLE_BINDING_LOCAL_ID,
             (unsigned long)s_backoff_ms,
             (unsigned long)s_status.reconnect_count);
    if (s_callout_initialized) {
        (void)ble_npl_callout_reset(&s_backoff_callout,
                                    ble_npl_time_ms_to_ticks32(s_backoff_ms));
    }
}

static void backoff_event(struct ble_npl_event *event)
{
    (void)event;
    start_scan();
}

static void start_scan(void)
{
    struct ble_gap_disc_params params = {0};
    params.itvl = 0x0010;
    params.window = 0x0010;
    params.filter_duplicates = 1;
    s_status.state = RADAR_BLE_STATE_SCANNING;
    ESP_LOGI(TAG, "RADAR_SCAN_START local_id=%u radar_id=%s", (unsigned int)RADAR_BLE_BINDING_LOCAL_ID,
             RADAR_BLE_BINDING_RADAR_ID);
    const int rc = ble_gap_disc(s_own_addr_type, 5000, &params, gap_event, NULL);
    if (rc != 0) schedule_retry();
}

static void format_uuid(const ble_uuid_t *uuid, char out[37])
{
    if (uuid->type == BLE_UUID_TYPE_16) {
        (void)snprintf(out, 37U, "%04X", (unsigned int)BLE_UUID16(uuid)->value);
        return;
    }
    (void)ble_uuid_to_str(uuid, out);
}

static uint8_t notify_candidate_priority(const struct ble_gatt_chr *chr)
{
    if ((chr->properties & BLE_GATT_CHR_PROP_NOTIFY) == 0U) return 0U;
    if (ble_uuid_cmp(&chr->uuid.u, BLE_UUID16_DECLARE(0xfff1)) == 0) return 2U;
    if (ble_uuid_cmp(&chr->uuid.u, BLE_UUID16_DECLARE(0xae02)) == 0) return 1U;
    return 0U;
}

static uint8_t write_candidate_priority(const struct ble_gatt_chr *chr, bool *without_response)
{
    if (ble_uuid_cmp(&chr->uuid.u, BLE_UUID16_DECLARE(0xfff2)) == 0 &&
        (chr->properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP) != 0U) {
        *without_response = true;
        return 2U;
    }
    if (ble_uuid_cmp(&chr->uuid.u, BLE_UUID16_DECLARE(0xae01)) == 0 &&
        (chr->properties & BLE_GATT_CHR_PROP_WRITE) != 0U) {
        *without_response = false;
        return 1U;
    }
    return 0U;
}

static void select_profile_characteristics(const radar_ble_service_range_t *service,
                                           const struct ble_gatt_chr *chr)
{
    const uint8_t notify_priority = notify_candidate_priority(chr);
    bool write_without_response = false;
    const uint8_t write_priority = write_candidate_priority(chr, &write_without_response);

    if (notify_priority > s_notify_priority) {
        s_notify_priority = notify_priority;
        s_service_start = service->start_handle;
        s_service_end = service->end_handle;
        s_notify_handle = chr->val_handle;
        ble_uuid_copy(&s_notify_uuid, &chr->uuid.u);
        ble_uuid_copy(&s_notify_service_uuid, &service->uuid.u);
    }
    if (write_priority > s_write_priority) {
        s_write_priority = write_priority;
        s_write_handle = chr->val_handle;
        s_write_without_response = write_without_response;
        ble_uuid_copy(&s_write_uuid, &chr->uuid.u);
    }
}

static int dsc_cb(uint16_t conn, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg);

static int start_subscription(uint16_t conn)
{
    if (s_notify_handle == 0U || s_service_end == 0U) return -1;
    s_status.state = RADAR_BLE_STATE_SUBSCRIBING;
    s_cccd_handle = 0U;
    return ble_gattc_disc_all_dscs(conn, s_notify_handle, s_service_end, dsc_cb, NULL);
}

static int subscribe_write_cb(uint16_t conn, const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (error == NULL || error->status != 0) {
        schedule_retry();
        return 0;
    }
    s_status.notify_subscribed = true;
    s_status.data_ready = true;
    s_status.state = RADAR_BLE_STATE_READY;
    s_backoff_ms = 0U;
    s_status.backoff_ms = 0U;
    ESP_LOGI(TAG, "RADAR_NOTIFY_READY local_id=%u handle=%u", (unsigned int)RADAR_BLE_BINDING_LOCAL_ID,
             (unsigned int)s_notify_handle);
    char notify_uuid[37];
    format_uuid(&s_notify_uuid.u, notify_uuid);
    ESP_LOGI(TAG, "RADAR_NOTIFY_ENABLED local_id=%u notify_uuid=%s handle=%u",
             (unsigned int)RADAR_BLE_BINDING_LOCAL_ID,
             notify_uuid,
             (unsigned int)s_notify_handle);
    ESP_LOGI(TAG, "RADAR_BLE_NOTIFY_SUBSCRIBED service=FFF0 notify=FFF1");
    ESP_LOGI(TAG, "RADAR_BLE_WAIT_DATA");
    return 0;
}

static int dsc_cb(uint16_t conn, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)arg;
    if (error == NULL) {
        schedule_retry();
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (s_cccd_handle == 0U) {
            schedule_retry();
            return 0;
        }
        const uint8_t enable_notify[2] = {1U, 0U};
        if (ble_gattc_write_flat(conn, s_cccd_handle, enable_notify, sizeof(enable_notify),
                                 subscribe_write_cb, NULL) != 0) {
            schedule_retry();
        }
        return 0;
    }
    if (error->status == 0 && dsc != NULL && chr_val_handle == s_notify_handle &&
        ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0) {
        s_cccd_handle = dsc->handle;
    }
    return 0;
}

static int survey_chr_cb(uint16_t conn, const struct ble_gatt_error *error,
                         const struct ble_gatt_chr *chr, void *arg);

static int survey_next_service(uint16_t conn)
{
    if (s_survey_index >= s_survey_count) {
        char service_uuid[37];
        char write_uuid[37];
        char notify_uuid[37];
        if (s_notify_handle == 0U || s_service_end == 0U) return -1;
        format_uuid(&s_notify_service_uuid.u, service_uuid);
        format_uuid(&s_write_uuid.u, write_uuid);
        format_uuid(&s_notify_uuid.u, notify_uuid);
        ESP_LOGI(TAG,
                 "RADAR_BLE_PROFILE_FOUND service=%s write_uuid=%s write_handle=%u "
                 "notify_uuid=%s notify_handle=%u",
                 service_uuid, s_write_handle == 0U ? "none" : write_uuid,
                 (unsigned int)s_write_handle, notify_uuid, (unsigned int)s_notify_handle);
        s_status.state = RADAR_BLE_STATE_PROFILE_FOUND;
        return start_subscription(conn);
    }
    const radar_ble_service_range_t *service = &s_survey_services[s_survey_index];
    return ble_gattc_disc_all_chrs(conn, service->start_handle, service->end_handle,
                                   survey_chr_cb, NULL);
}

static int survey_chr_cb(uint16_t conn, const struct ble_gatt_error *error,
                         const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (error == NULL) {
        schedule_retry();
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        ++s_survey_index;
        if (survey_next_service(conn) != 0) schedule_retry();
        return 0;
    }
    if (error->status == 0 && chr != NULL) {
        char uuid[37];
        ESP_LOGI(TAG, "RADAR_GATT_CHARACTERISTIC service_index=%u uuid=%s props=0x%02x",
                 (unsigned int)s_survey_index, ble_uuid_to_str(&chr->uuid.u, uuid),
                 (unsigned int)chr->properties);
        select_profile_characteristics(&s_survey_services[s_survey_index], chr);
    }
    return 0;
}

static int survey_svc_cb(uint16_t conn, const struct ble_gatt_error *error,
                         const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (error == NULL) {
        schedule_retry();
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        s_survey_index = 0U;
        if (s_survey_count == 0U || survey_next_service(conn) != 0) schedule_retry();
        return 0;
    }
    if (error->status == 0 && svc != NULL && s_survey_count < RADAR_BLE_SURVEY_MAX_SERVICES) {
        char uuid[37];
        ESP_LOGI(TAG, "RADAR_GATT_SERVICE index=%u uuid=%s start=%u end=%u",
                 (unsigned int)s_survey_count, ble_uuid_to_str(&svc->uuid.u, uuid),
                 (unsigned int)svc->start_handle, (unsigned int)svc->end_handle);
        s_survey_services[s_survey_count++] = (radar_ble_service_range_t){
            .start_handle = svc->start_handle,
            .end_handle = svc->end_handle,
            .uuid = svc->uuid,
        };
    }
    return 0;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        char address[18] = {0};
        const uint64_t timestamp = now_ms();
        if (s_last_device_log_ms == 0U || timestamp - s_last_device_log_ms >= 1000U) {
            format_address(&event->disc.addr, address);
            ESP_LOGI(TAG, "RADAR_DEVICE_FOUND mac=%s addr_type=%u rssi=%d", address,
                     (unsigned int)event->disc.addr.type, (int)event->disc.rssi);
            s_last_device_log_ms = timestamp;
        }
        if (!peer_matches(&event->disc.addr)) return 0;
        format_address(&event->disc.addr, address);
        ESP_LOGI(TAG, "RADAR_MAC_MATCH local_id=%u radar_id=%s mac=%s addr_type=%u",
                 (unsigned int)RADAR_BLE_BINDING_LOCAL_ID, RADAR_BLE_BINDING_RADAR_ID, address,
                 (unsigned int)event->disc.addr.type);
        (void)ble_gap_disc_cancel();
        s_status.state = RADAR_BLE_STATE_CONNECTING;
        if (ble_gap_connect(s_own_addr_type, &event->disc.addr, 30000, NULL, gap_event, NULL) != 0) {
            ++s_status.reconnect_count;
            schedule_retry();
        }
        return 0;
    }
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ++s_status.reconnect_count;
            schedule_retry();
            return 0;
        }
        s_conn_handle = event->connect.conn_handle;
        s_service_start = 0U;
        s_service_end = 0U;
        s_notify_handle = 0U;
        s_write_handle = 0U;
        s_cccd_handle = 0U;
        s_notify_priority = 0U;
        s_write_priority = 0U;
        s_write_without_response = false;
        s_status.connected = true;
        s_status.notify_subscribed = false;
        s_status.data_ready = false;
        s_status.state = RADAR_BLE_STATE_DISCOVERING;
        ESP_LOGI(TAG, "RADAR_CONNECTED local_id=%u conn=%u", (unsigned int)RADAR_BLE_BINDING_LOCAL_ID,
                 (unsigned int)s_conn_handle);
        s_survey_count = 0U;
        if (ble_gattc_disc_all_svcs(s_conn_handle, survey_svc_cb, NULL) != 0) schedule_retry();
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (!s_status.notify_subscribed || event->notify_rx.attr_handle != s_notify_handle) return 0;
        const uint16_t notify_length = os_mbuf_len(event->notify_rx.om);
        const uint64_t timestamp = now_ms();
        if (s_notify_summary_window_start_ms == 0U) {
            s_notify_summary_window_start_ms = timestamp;
        }
        ++s_notify_summary_count;
        s_notify_summary_bytes += notify_length;
        s_notify_summary_last_len = notify_length;
        if (timestamp - s_notify_summary_window_start_ms >= 1000U) {
            ESP_LOGI(TAG,
                     "RADAR_NOTIFY_RX_SUMMARY local_id=%u notify_count=%lu last_len=%u "
                     "bytes_per_sec=%lu",
                     (unsigned int)RADAR_BLE_BINDING_LOCAL_ID,
                     (unsigned long)s_notify_summary_count,
                     (unsigned int)s_notify_summary_last_len,
                     (unsigned long)s_notify_summary_bytes);
            s_notify_summary_window_start_ms = timestamp;
            s_notify_summary_count = 0U;
            s_notify_summary_bytes = 0U;
            s_notify_summary_last_len = 0U;
        }
        uint16_t length = notify_length;
        uint8_t copy[64];
        if (length > sizeof(copy)) length = sizeof(copy);
        if (os_mbuf_copydata(event->notify_rx.om, 0, length, copy) != 0) return 0;
        if (s_notify_cb != NULL) s_notify_cb(copy, length, s_notify_ctx);
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "RADAR_DISCONNECTED local_id=%u reason=%d", (unsigned int)RADAR_BLE_BINDING_LOCAL_ID,
                 event->disconnect.reason);
        s_status.connected = false;
        s_status.notify_subscribed = false;
        s_status.data_ready = false;
        ++s_status.reconnect_count;
        schedule_retry();
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        ++s_status.reconnect_count;
        schedule_retry();
        return 0;
    default:
        return 0;
    }
}

static void on_sync(void)
{
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        schedule_retry();
        return;
    }
    ble_npl_callout_init(&s_backoff_callout, nimble_port_get_dflt_eventq(), backoff_event, NULL);
    s_callout_initialized = true;
    start_scan();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}
#endif

int radar_ble_set_control_command(const uint8_t *data, size_t length)
{
    if (length > sizeof(s_control_command) || (data == NULL && length > 0U)) return -1;
    if (length > 0U) memcpy(s_control_command, data, length);
    s_control_command_length = length;
    return 0;
}

int radar_ble_send_control_command(void)
{
#if RADAR_BLE_BINDING_ENABLED
    if (!s_status.connected || s_write_handle == 0U ||
        ble_uuid_cmp(&s_write_uuid.u, BLE_UUID16_DECLARE(0xfff2)) != 0 ||
        s_control_command_length == 0U) {
        ESP_LOGW(TAG, "RADAR_BLE_CONTROL_COMMAND_REQUIRED local_id=%u handle=%u len=%u",
                 (unsigned int)RADAR_BLE_BINDING_LOCAL_ID, (unsigned int)s_write_handle,
                 (unsigned int)s_control_command_length);
        return -1;
    }
    ESP_LOGI(TAG, "RADAR_BLE_CONTROL_TX uuid=FFF2 len=%u",
             (unsigned int)s_control_command_length);
    if (s_write_without_response) {
        const int rc = ble_gattc_write_no_rsp_flat(s_conn_handle, s_write_handle,
                                                   s_control_command,
                                                   (uint16_t)s_control_command_length);
        if (rc != 0) return rc;
        return 0;
    }
    if (ble_gattc_write_flat(s_conn_handle, s_write_handle, s_control_command,
                             (uint16_t)s_control_command_length,
                             NULL, NULL) != 0) {
        return -1;
    }
    return 0;
#else
    return -1;
#endif
}

int radar_ble_transport_start(radar_ble_notify_cb_t callback, void *ctx)
{
    memset(&s_status, 0, sizeof(s_status));
    s_notify_cb = callback;
    s_notify_ctx = ctx;
#if !RADAR_BLE_BINDING_ENABLED
    s_status.state = RADAR_BLE_STATE_DISABLED;
    return 0;
#else
    if (!binding_mac_is_nonzero() || RADAR_BLE_BINDING_ADDRESS_TYPE == RADAR_BLE_ADDR_TYPE_UNSPECIFIED) {
        s_status.state = RADAR_BLE_STATE_UNAVAILABLE;
        ++s_status.unavailable_count;
        return -1;
    }
    s_status.configured = true;
    s_backoff_ms = 0U;
    s_callout_initialized = false;
    s_last_device_log_ms = 0U;
    s_notify_summary_window_start_ms = 0U;
    s_notify_summary_count = 0U;
    s_notify_summary_bytes = 0U;
    s_notify_summary_last_len = 0U;
    ESP_LOGI(TAG, "RADAR_BLE_INIT device_id=%s room_id=%s radar_id=%s local_id=%u gatt_discovery=all",
             RADAR_BLE_BINDING_DEVICE_ID, RADAR_BLE_BINDING_ROOM_ID, RADAR_BLE_BINDING_RADAR_ID,
             (unsigned int)RADAR_BLE_BINDING_LOCAL_ID);
    ble_hs_cfg.sync_cb = on_sync;
    const int rc = nimble_port_init();
    if (rc != ESP_OK) return rc;
    nimble_port_freertos_init(host_task);
    return 0;
#endif
}

int radar_ble_transport_write(const uint8_t *data, size_t length)
{
#if RADAR_BLE_BINDING_ENABLED
    if (data == NULL || length > UINT16_MAX || !s_status.connected || s_write_handle == 0U) return -1;
    if (s_write_without_response) {
        return ble_gattc_write_no_rsp_flat(s_conn_handle, s_write_handle, data, (uint16_t)length);
    }
    return ble_gattc_write_flat(s_conn_handle, s_write_handle, data, (uint16_t)length, NULL, NULL);
#else
    (void)data;
    (void)length;
    return -1;
#endif
}

void radar_ble_transport_stop(void)
{
    s_status.state = RADAR_BLE_STATE_DISABLED;
    s_status.connected = false;
    s_status.notify_subscribed = false;
    s_status.data_ready = false;
}

void radar_ble_transport_set_data_ready(bool ready)
{
    s_status.data_ready = ready && s_status.notify_subscribed;
    if (s_status.data_ready) s_status.state = RADAR_BLE_STATE_READY;
    else if (s_status.notify_subscribed) s_status.state = RADAR_BLE_STATE_SUBSCRIBING;
}

void radar_ble_transport_get_status(radar_ble_transport_status_t *out)
{
    if (out != NULL) *out = s_status;
}

/**
 * @file network_worker.c
 * @brief ESPS3 网络状态 worker 和 Server 上云 gate。
 *
 * 本文件属于 ESPS3 网关。WiFi callback 只投递轻量事件；network_worker 负责
 * STA 连接/重连、稳定窗口门控和 scheduler 网络状态发布。upload/command worker
 * 只在 LINK_STABLE 后访问 ESP-server，避免 C5 本地链路刚恢复时被上云请求挤占。
 */

#include "network_worker.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_stack_monitor.h"
#include "bme_cache_manager.h"
#include "cJSON.h"
#include "child_registry.h"
#include "command_router.h"
#include "device_stream_gateway.h"
#include "esp111_protocol_common.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gateway_config.h"
#include "gateway_event_reporter.h"
#include "gateway_wifi.h"
#include "local_http_server.h"
#include "network_replay_worker.h"
#include "offline_policy.h"
#include "resource_manager.h"
#include "s3_scheduler.h"
#include "sensor_aggregator.h"
#include "server_client.h"
#include "smart_home_gateway.h"

static const char *TAG = "network_worker";

#ifndef NETWORK_WORKER_QUEUE_DEPTH
#define NETWORK_WORKER_QUEUE_DEPTH 16U
#endif

#ifndef NETWORK_WORKER_WORK_QUEUE_DEPTH
#define NETWORK_WORKER_WORK_QUEUE_DEPTH 16U
#endif

#ifndef NETWORK_WORKER_TASK_STACK
#define NETWORK_WORKER_TASK_STACK 16384U
#endif

#ifndef NETWORK_WORKER_UPLOAD_TASK_STACK
#define NETWORK_WORKER_UPLOAD_TASK_STACK 12288U
#endif

#ifndef NETWORK_WORKER_COMMAND_TASK_STACK
#define NETWORK_WORKER_COMMAND_TASK_STACK 16384U
#endif

#ifndef NETWORK_WORKER_TASK_PRIORITY
#define NETWORK_WORKER_TASK_PRIORITY 5U
#endif

#ifndef NETWORK_WORKER_UPLOAD_TASK_PRIORITY
#define NETWORK_WORKER_UPLOAD_TASK_PRIORITY 4U
#endif

#ifndef NETWORK_WORKER_COMMAND_TASK_PRIORITY
#define NETWORK_WORKER_COMMAND_TASK_PRIORITY 4U
#endif

#ifndef NETWORK_WORKER_STABLE_GATE_MS
#define NETWORK_WORKER_STABLE_GATE_MS 3000U
#endif

#ifndef NETWORK_WORKER_POLL_MS
#define NETWORK_WORKER_POLL_MS 250U
#endif

#ifndef NETWORK_WORKER_SNAPSHOT_WORKER_WAIT_MS
#define NETWORK_WORKER_SNAPSHOT_WORKER_WAIT_MS 250U
#endif

#ifndef NETWORK_WORKER_SNAPSHOT_LOCK_TIMEOUT_MS
#define NETWORK_WORKER_SNAPSHOT_LOCK_TIMEOUT_MS 100U
#endif

#ifndef NETWORK_WORKER_STA_RECONNECT_ATTEMPT_TIMEOUT_MS
#define NETWORK_WORKER_STA_RECONNECT_ATTEMPT_TIMEOUT_MS 15000U
#endif

#ifndef NETWORK_WORKER_STA_ASSOC_FAILURE_BUDGET
#define NETWORK_WORKER_STA_ASSOC_FAILURE_BUDGET 5U
#endif

#ifndef NETWORK_WORKER_STA_SCAN_TIMEOUT_MS
#define NETWORK_WORKER_STA_SCAN_TIMEOUT_MS 4000U
#endif

#ifndef NETWORK_WORKER_STA_LONG_DISCONNECTED_SCAN_MS
#define NETWORK_WORKER_STA_LONG_DISCONNECTED_SCAN_MS 15000U
#endif

#ifndef NETWORK_WORKER_PENDING_STATION_TTL_MS
#define NETWORK_WORKER_PENDING_STATION_TTL_MS 30000U
#endif

#ifndef NETWORK_WORKER_SERVER_PROBE_INTERVAL_MS
#define NETWORK_WORKER_SERVER_PROBE_INTERVAL_MS 3000U
#endif

#ifndef NETWORK_WORKER_SERVER_FAIL_THRESHOLD
#define NETWORK_WORKER_SERVER_FAIL_THRESHOLD 3U
#endif

#ifndef NETWORK_WORKER_SERVER_RECOVER_THRESHOLD
#define NETWORK_WORKER_SERVER_RECOVER_THRESHOLD 2U
#endif

#ifndef NETWORK_WORKER_OFFLINE_POLICY_LOG_MS
#define NETWORK_WORKER_OFFLINE_POLICY_LOG_MS 5000U
#endif

#ifndef NETWORK_WORKER_SNAPSHOT_PRESSURE_DEPTH
#define NETWORK_WORKER_SNAPSHOT_PRESSURE_DEPTH 2U
#endif

#ifndef NETWORK_WORKER_UPLOAD_PRESSURE_DEPTH
#define NETWORK_WORKER_UPLOAD_PRESSURE_DEPTH 4U
#endif

#ifndef NETWORK_WORKER_UPLOAD_DROP_LOG_MS
#define NETWORK_WORKER_UPLOAD_DROP_LOG_MS 5000U
#endif

#ifndef NETWORK_WORKER_UPLOAD_FAIL_LOG_MS
#define NETWORK_WORKER_UPLOAD_FAIL_LOG_MS 1000U
#endif

#ifndef NETWORK_WORKER_SOFTAP_STABLE_GATE_MS
#define NETWORK_WORKER_SOFTAP_STABLE_GATE_MS 750U
#endif

typedef struct {
    network_worker_event_t event;
    network_worker_event_source_t source;
    uint32_t ip_addr;
    uint8_t disconnect_reason;
    int64_t event_time_us;
    int64_t event_time_ms;
    uint8_t ap_mac[6];
    uint8_t ap_aid;
    bool has_ap_station;
} network_worker_item_t;

typedef enum {
    NETWORK_WORKER_WORK_UPLOAD_JSON = 0,
    NETWORK_WORKER_WORK_UPLOAD_SNAPSHOT,
    NETWORK_WORKER_WORK_COMMAND_PULL,
    NETWORK_WORKER_WORK_COMMAND_ACK,
    NETWORK_WORKER_WORK_SMART_HOME_POLL,
} network_worker_work_type_t;

typedef struct {
    network_worker_work_type_t work_type;
    network_worker_server_json_type_t json_type;
    char *json_body;
    char device_id[CHILD_REGISTRY_DEVICE_ID_LEN];
    char command_id[48];
    char source[24];
    uint32_t bme_cache_sequence;
} network_worker_work_item_t;

static QueueHandle_t s_event_queue;
static QueueHandle_t s_ap_disconnect_queue;
static QueueHandle_t s_work_queue;
static QueueHandle_t s_command_queue;
static SemaphoreHandle_t s_work_queue_mutation_lock;
static SemaphoreHandle_t s_command_queue_mutation_lock;
static TaskHandle_t s_worker_task;
static TaskHandle_t s_upload_task;
static TaskHandle_t s_command_task;
static TaskHandle_t s_snapshot_task;
static SemaphoreHandle_t s_pending_station_lock;
static network_worker_link_state_t s_link_state = NETWORK_WORKER_LINK_DOWN;
static s3_scheduler_network_state_t s_last_scheduler_state = S3_SCHEDULER_NET_NOT_READY;
static int64_t s_last_network_change_ms;
static int64_t s_last_worker_stack_log_ms;
static int64_t s_last_worker_heap_log_ms;
static int64_t s_last_upload_stack_log_ms;
static int64_t s_last_upload_heap_log_ms;
static int64_t s_last_upload_drop_log_ms;
static int64_t s_last_upload_fail_log_ms;
static int64_t s_last_command_stack_log_ms;
static int64_t s_last_command_heap_log_ms;
static int64_t s_last_server_summary_log_ms;
static bool s_sta_connect_pending;
static bool s_sta_connect_request_sent;
static bool s_sta_reconnect_needed;
static int64_t s_next_sta_connect_ms;
static int64_t s_sta_connect_request_deadline_ms;
static bool s_sta_scan_pending;
static bool s_sta_scan_in_progress;
static bool s_sta_scan_avoid_current;
static int64_t s_next_sta_scan_ms;
static int64_t s_sta_scan_deadline_ms;
static int64_t s_sta_last_scan_start_ms;
static int64_t s_sta_disconnected_since_ms;
static char s_sta_scan_reason[32];
static char s_command_ack_response[SERVER_CLIENT_SMALL_BODY_BYTES];
static bool s_local_ingest_ready;
static bool s_local_http_start_requested;
static bool s_local_http_running;
static int64_t s_next_local_http_start_ms;
static int64_t s_softap_ready_since_ms;
static uint32_t s_local_http_retry_count;
static int64_t s_last_server_probe_ms;
static bool s_snapshot_upload_pending;
static bool s_command_pull_pending;
static bool s_smart_home_poll_pending;
static bool s_server_ready;
static uint32_t s_server_failure_count;
static uint32_t s_server_success_count;
static char s_server_last_error[32];
static int64_t s_last_snapshot_enqueue_ms;
static uint32_t s_low_priority_drop_count;
static uint32_t s_low_priority_coalesce_count;
static portMUX_TYPE s_snapshot_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static network_worker_snapshot_stats_t s_snapshot_stats;

typedef struct {
    bool used;
    bool device_bound;
    uint8_t mac[6];
    uint8_t aid;
    char peer_ip[16];
    char device_id[CHILD_REGISTRY_DEVICE_ID_LEN];
    int64_t connected_at_us;
} pending_softap_station_t;

static pending_softap_station_t s_pending_stations[GATEWAY_CONFIG_MAX_CHILDREN];

static void release_work_item(network_worker_work_item_t *item);
static void drop_low_priority_upload_backlog(const char *reason);
static esp_err_t enqueue_upload_work_item(const network_worker_work_item_t *item);
static void snapshot_worker_task(void *arg);

static const uint32_t s_local_http_retry_backoff_ms[] = {
    2000U,
    5000U,
    10000U,
    30000U,
};

static const uint32_t s_sta_reconnect_backoff_ms[] = {
    1000U,
    3000U,
    10000U,
    30000U,
};

/* 网络门控只看本机 uptime，避免 Server 时间未同步时影响重连/稳定窗口。 */
static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

const char *network_worker_link_state_name(network_worker_link_state_t state)
{
    switch (state) {
    case NETWORK_WORKER_LINK_DOWN:
        return "LINK_DOWN";
    case NETWORK_WORKER_LINK_UP:
        return "LINK_UP";
    case NETWORK_WORKER_LINK_IP_READY:
        return "IP_READY";
    case NETWORK_WORKER_LINK_STABLE:
        return "LINK_STABLE";
    default:
        return "UNKNOWN";
    }
}

static const char *event_name(network_worker_event_t event)
{
    switch (event) {
    case NETWORK_WORKER_EVENT_LINK_UP:
        return "LINK_UP";
    case NETWORK_WORKER_EVENT_LINK_DOWN:
        return "LINK_DOWN";
    case NETWORK_WORKER_EVENT_IP_READY:
        return "IP_READY";
    case NETWORK_WORKER_EVENT_SCAN_DONE:
        return "SCAN_DONE";
    default:
        return "UNKNOWN";
    }
}

static const char *source_name(network_worker_event_source_t source)
{
    switch (source) {
    case NETWORK_WORKER_SOURCE_SOFTAP_START:
        return "softap_start";
    case NETWORK_WORKER_SOURCE_SOFTAP_STOP:
        return "softap_stop";
    case NETWORK_WORKER_SOURCE_AP_STA_CONNECTED:
        return "ap_sta_connected";
    case NETWORK_WORKER_SOURCE_AP_STA_DISCONNECTED:
        return "ap_sta_disconnected";
    case NETWORK_WORKER_SOURCE_STA_START:
        return "sta_start";
    case NETWORK_WORKER_SOURCE_STA_CONNECTED:
        return "sta_connected";
    case NETWORK_WORKER_SOURCE_STA_STOP:
        return "sta_stop";
    case NETWORK_WORKER_SOURCE_STA_DISCONNECTED:
        return "sta_disconnected";
    case NETWORK_WORKER_SOURCE_STA_GOT_IP:
        return "sta_got_ip";
    case NETWORK_WORKER_SOURCE_STA_LOST_IP:
        return "sta_lost_ip";
    case NETWORK_WORKER_SOURCE_STA_SCAN_DONE:
        return "sta_scan_done";
    case NETWORK_WORKER_SOURCE_LOCAL_HTTP_ENABLE:
        return "local_http_enable";
    case NETWORK_WORKER_SOURCE_UNKNOWN:
    default:
        return "unknown";
    }
}

typedef enum {
    NETWORK_RECONNECT_CURRENT_RETRY = 0,
    NETWORK_RECONNECT_RESCAN_FALLBACK,
    NETWORK_RECONNECT_ASSOC_RETRY,
    NETWORK_RECONNECT_RF_TEMPORARY_LOSS,
} network_reconnect_policy_t;

static const char *network_reconnect_policy_name(network_reconnect_policy_t policy)
{
    switch (policy) {
    case NETWORK_RECONNECT_RESCAN_FALLBACK:
        return "AUTH_FAIL_OR_NO_AP";
    case NETWORK_RECONNECT_ASSOC_RETRY:
        return "ASSOC_FAIL_RETRY_CURRENT";
    case NETWORK_RECONNECT_RF_TEMPORARY_LOSS:
        return "AP_LOST_OR_RF_TEMP_LOSS";
    case NETWORK_RECONNECT_CURRENT_RETRY:
    default:
        return "CURRENT_SSID_RETRY";
    }
}

static network_reconnect_policy_t classify_reconnect_policy(uint8_t disconnect_reason)
{
    switch ((wifi_err_reason_t)disconnect_reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return NETWORK_RECONNECT_RESCAN_FALLBACK;
    case WIFI_REASON_ASSOC_FAIL:
        return NETWORK_RECONNECT_ASSOC_RETRY;
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_LEAVE:
    case WIFI_REASON_ASSOC_LEAVE:
    case WIFI_REASON_BEACON_TIMEOUT:
    case WIFI_REASON_AP_TSF_RESET:
    case WIFI_REASON_ROAMING:
        return NETWORK_RECONNECT_RF_TEMPORARY_LOSS;
    default:
        return NETWORK_RECONNECT_CURRENT_RETRY;
    }
}

static uint32_t sta_reconnect_backoff_ms(uint32_t retry_count)
{
    const size_t count = sizeof(s_sta_reconnect_backoff_ms) /
                         sizeof(s_sta_reconnect_backoff_ms[0]);
    const size_t index = retry_count > 0U ? (size_t)(retry_count - 1U) : 0U;
    return s_sta_reconnect_backoff_ms[index < count ? index : count - 1U];
}

static void log_network_epoch(const char *state, const char *reason)
{
    ESP_LOGI(TAG,
             "NETWORK_EPOCH epoch_id=%lu state=%s reason=%s sta_connected=%d sta_ip_ready=%d",
             (unsigned long)gateway_wifi_get_sta_network_epoch(),
             state != NULL ? state : "unknown",
             reason != NULL ? reason : "unknown",
             gateway_wifi_is_sta_connected() ? 1 : 0,
             gateway_wifi_is_sta_ip_ready() ? 1 : 0);
}

static bool source_affects_upstream_stability(network_worker_event_source_t source)
{
    return source == NETWORK_WORKER_SOURCE_STA_CONNECTED ||
           source == NETWORK_WORKER_SOURCE_STA_DISCONNECTED ||
           source == NETWORK_WORKER_SOURCE_STA_GOT_IP ||
           source == NETWORK_WORKER_SOURCE_STA_LOST_IP;
}

static void format_ap_station_mac(const uint8_t mac[6], char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    if (mac == NULL) {
        strlcpy(out, "<unknown>", out_size);
        return;
    }
    snprintf(out,
             out_size,
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
}

static pending_softap_station_t *find_pending_station_locked(const uint8_t mac[6], uint8_t aid)
{
    for (size_t index = 0U; index < GATEWAY_CONFIG_MAX_CHILDREN; ++index) {
        pending_softap_station_t *station = &s_pending_stations[index];
        if (!station->used) {
            continue;
        }
        if (mac != NULL && memcmp(station->mac, mac, sizeof(station->mac)) == 0) {
            return station;
        }
        if (aid != 0U && station->aid == aid) {
            return station;
        }
    }
    return NULL;
}

static pending_softap_station_t *allocate_pending_station_locked(int64_t timestamp_us)
{
    pending_softap_station_t *oldest = &s_pending_stations[0];
    for (size_t index = 0U; index < GATEWAY_CONFIG_MAX_CHILDREN; ++index) {
        pending_softap_station_t *station = &s_pending_stations[index];
        if (!station->used) {
            return station;
        }
        if (station->connected_at_us < oldest->connected_at_us) {
            oldest = station;
        }
    }
    ESP_LOGW(TAG,
             "SOFTAP_PENDING_STATION state=evict age_ms=%lld",
             (long long)((timestamp_us - oldest->connected_at_us) / 1000));
    return oldest;
}

static void prune_pending_stations(int64_t timestamp_us)
{
    if (s_pending_station_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_pending_station_lock, portMAX_DELAY);
    for (size_t index = 0U; index < GATEWAY_CONFIG_MAX_CHILDREN; ++index) {
        pending_softap_station_t *station = &s_pending_stations[index];
        if (station->used && !station->device_bound &&
            timestamp_us - station->connected_at_us >
                (int64_t)NETWORK_WORKER_PENDING_STATION_TTL_MS * 1000) {
            char mac[18] = {0};
            format_ap_station_mac(station->mac, mac, sizeof(mac));
            ESP_LOGI(TAG,
                     "SOFTAP_PENDING_STATION state=expired mac=%s aid=%u age_ms=%lld",
                     mac,
                     (unsigned int)station->aid,
                     (long long)((timestamp_us - station->connected_at_us) / 1000));
            memset(station, 0, sizeof(*station));
        }
    }
    xSemaphoreGive(s_pending_station_lock);
}

static void record_pending_station(const network_worker_item_t *item)
{
    if (item == NULL || !item->has_ap_station || s_pending_station_lock == NULL) {
        return;
    }

    char peer_ip[16] = {0};
    (void)gateway_wifi_get_ap_client_ip(item->ap_mac, peer_ip, sizeof(peer_ip));
    xSemaphoreTake(s_pending_station_lock, portMAX_DELAY);
    pending_softap_station_t *station =
        find_pending_station_locked(item->ap_mac, item->ap_aid);
    if (station == NULL) {
        station = allocate_pending_station_locked(item->event_time_us);
    }
    *station = (pending_softap_station_t){
        .used = true,
        .device_bound = false,
        .aid = item->ap_aid,
        .connected_at_us = item->event_time_us,
    };
    memcpy(station->mac, item->ap_mac, sizeof(station->mac));
    strlcpy(station->peer_ip, peer_ip, sizeof(station->peer_ip));
    char mac[18] = {0};
    format_ap_station_mac(station->mac, mac, sizeof(mac));
    ESP_LOGI(TAG,
             "SOFTAP_PENDING_STATION state=pending mac=%s ip=%s aid=%u timestamp_us=%lld",
             mac,
             station->peer_ip[0] != '\0' ? station->peer_ip : "<pending_dhcp>",
             (unsigned int)station->aid,
             (long long)station->connected_at_us);
    xSemaphoreGive(s_pending_station_lock);
}

static bool take_pending_station(const network_worker_item_t *item,
                                 pending_softap_station_t *out_station)
{
    if (item == NULL || !item->has_ap_station || out_station == NULL ||
        s_pending_station_lock == NULL) {
        return false;
    }

    bool found = false;
    xSemaphoreTake(s_pending_station_lock, portMAX_DELAY);
    pending_softap_station_t *station =
        find_pending_station_locked(item->ap_mac, item->ap_aid);
    if (station != NULL) {
        *out_station = *station;
        memset(station, 0, sizeof(*station));
        found = true;
    }
    xSemaphoreGive(s_pending_station_lock);
    return found;
}

static const char *work_name(network_worker_work_type_t work_type)
{
    switch (work_type) {
    case NETWORK_WORKER_WORK_UPLOAD_JSON:
        return "upload_json";
    case NETWORK_WORKER_WORK_UPLOAD_SNAPSHOT:
        return "upload_snapshot";
    case NETWORK_WORKER_WORK_COMMAND_PULL:
        return "command_pull";
    case NETWORK_WORKER_WORK_COMMAND_ACK:
        return "command_ack";
    case NETWORK_WORKER_WORK_SMART_HOME_POLL:
        return "smart_home_poll";
    default:
        return "unknown";
    }
}

static const char *json_type_name(network_worker_server_json_type_t type)
{
    switch (type) {
    case NETWORK_WORKER_SERVER_JSON_INGEST:
        return "ingest";
    case NETWORK_WORKER_SERVER_JSON_GATEWAY_STATE:
        return "gateway_state";
    case NETWORK_WORKER_SERVER_JSON_SYSTEM_LOG:
        return "system_log";
    case NETWORK_WORKER_SERVER_JSON_ALARM:
        return "alarm";
    default:
        return "unknown";
    }
}

static bool server_result_ok(esp_err_t ret, int status)
{
    return ret == ESP_OK && status >= 200 && status < 300;
}

static bool upload_work_is_peer_sensor(const network_worker_work_item_t *item)
{
    return item != NULL &&
           item->work_type == NETWORK_WORKER_WORK_UPLOAD_JSON &&
           item->json_type == NETWORK_WORKER_SERVER_JSON_INGEST &&
           item->device_id[0] != '\0';
}

static bool sensor_upload_cancelled(void *ctx)
{
    const char *device_id = (const char *)ctx;
    return device_id == NULL || device_id[0] == '\0' ||
           !sensor_aggregator_peer_active(device_id) ||
           !resource_manager_is_live(device_id);
}

static void device_id_from_server_json(const char *json_body,
                                       char *out,
                                       size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    out[0] = '\0';
    if (json_body == NULL || json_body[0] == '\0') {
        return;
    }

    cJSON *root = cJSON_Parse(json_body);
    if (root == NULL) {
        return;
    }
    cJSON *device_id = cJSON_GetObjectItemCaseSensitive(root,
                                                        ESP111_PROTOCOL_JSON_DEVICE_ID);
    if (cJSON_IsString(device_id) && device_id->valuestring != NULL) {
        strlcpy(out, device_id->valuestring, out_size);
    }
    cJSON_Delete(root);
}

static bool server_link_stable(void)
{
    return gateway_wifi_is_sta_connected() &&
           gateway_wifi_is_sta_ip_ready() &&
           s3_scheduler_is_server_upload_allowed() &&
           s_server_ready;
}

static bool link_stable_inputs_ready(void)
{
    return gateway_wifi_is_softap_ready() &&
           gateway_wifi_is_sta_started() &&
           gateway_wifi_is_sta_connected() &&
           gateway_wifi_is_sta_ip_ready();
}

static bool upload_work_is_low_priority(const network_worker_work_item_t *item)
{
    return item != NULL &&
           (item->work_type == NETWORK_WORKER_WORK_UPLOAD_SNAPSHOT ||
            (item->work_type == NETWORK_WORKER_WORK_UPLOAD_JSON &&
             item->json_type != NETWORK_WORKER_SERVER_JSON_INGEST));
}

static bool upload_work_is_telemetry(const network_worker_work_item_t *item)
{
    return upload_work_is_low_priority(item);
}

static bool upload_work_is_high_priority(const network_worker_work_item_t *item)
{
    return item != NULL &&
           item->work_type == NETWORK_WORKER_WORK_UPLOAD_JSON &&
           item->json_type == NETWORK_WORKER_SERVER_JSON_INGEST;
}

static void clear_upload_pending_flag(const network_worker_work_item_t *item)
{
    if (item == NULL) {
        return;
    }
    if (item->work_type == NETWORK_WORKER_WORK_UPLOAD_SNAPSHOT) {
        s_snapshot_upload_pending = false;
    } else if (item->work_type == NETWORK_WORKER_WORK_COMMAND_PULL) {
        s_command_pull_pending = false;
    } else if (item->work_type == NETWORK_WORKER_WORK_SMART_HOME_POLL) {
        s_smart_home_poll_pending = false;
    }
}

static void mark_upload_pending_flag(const network_worker_work_item_t *item)
{
    if (item == NULL) {
        return;
    }
    if (item->work_type == NETWORK_WORKER_WORK_UPLOAD_SNAPSHOT) {
        s_snapshot_upload_pending = true;
    } else if (item->work_type == NETWORK_WORKER_WORK_COMMAND_PULL) {
        s_command_pull_pending = true;
    } else if (item->work_type == NETWORK_WORKER_WORK_SMART_HOME_POLL) {
        s_smart_home_poll_pending = true;
    }
}

static bool upload_low_priority_already_pending(const network_worker_work_item_t *item)
{
    return item != NULL &&
           item->work_type == NETWORK_WORKER_WORK_UPLOAD_SNAPSHOT &&
           s_snapshot_upload_pending;
}

static bool upload_queue_under_pressure(void)
{
    return s_work_queue != NULL &&
           uxQueueMessagesWaiting(s_work_queue) >= NETWORK_WORKER_UPLOAD_PRESSURE_DEPTH;
}

static bool snapshot_queue_under_pressure(void)
{
    return s_work_queue != NULL &&
           uxQueueMessagesWaiting(s_work_queue) >= NETWORK_WORKER_SNAPSHOT_PRESSURE_DEPTH;
}

static void snapshot_stats_record_skip(void)
{
    portENTER_CRITICAL(&s_snapshot_stats_mux);
    ++s_snapshot_stats.snapshot_skip_count;
    portEXIT_CRITICAL(&s_snapshot_stats_mux);
}

static void snapshot_stats_record_upload(void)
{
    portENTER_CRITICAL(&s_snapshot_stats_mux);
    ++s_snapshot_stats.snapshot_upload_count;
    portEXIT_CRITICAL(&s_snapshot_stats_mux);
}

static void snapshot_stats_record_coalesce(void)
{
    portENTER_CRITICAL(&s_snapshot_stats_mux);
    ++s_snapshot_stats.snapshot_coalesce_count;
    portEXIT_CRITICAL(&s_snapshot_stats_mux);
}

static bool snapshot_has_higher_priority_work(void)
{
    return s_work_queue != NULL && uxQueueMessagesWaiting(s_work_queue) > 0U;
}

static void log_server_health_summary(const char *reason,
                                      esp_err_t ret,
                                      int status,
                                      bool force)
{
    const int64_t timestamp_ms = now_ms();
    if (!force && s_last_server_summary_log_ms != 0 &&
        timestamp_ms - s_last_server_summary_log_ms <
            (int64_t)NETWORK_WORKER_OFFLINE_POLICY_LOG_MS) {
        return;
    }
    s_last_server_summary_log_ms = timestamp_ms;

    ESP_LOGW(TAG,
             "offline_policy summary reason=%s last_error=%s failures=%lu successes=%lu server_ready=%d queue_depth=%u ret=%s http_status=%d",
             reason != NULL ? reason : "unknown",
             s_server_last_error[0] != '\0' ? s_server_last_error : "-",
             (unsigned long)s_server_failure_count,
             (unsigned long)s_server_success_count,
             s_server_ready ? 1 : 0,
             s_work_queue != NULL ? (unsigned int)uxQueueMessagesWaiting(s_work_queue) : 0U,
             esp_err_to_name(ret),
             status);
}

static void record_offline_policy_limited(esp_err_t ret,
                                          int status,
                                          const char *reason,
                                          bool force)
{
    if (server_result_ok(ret, status)) {
        offline_policy_record_server_result(ret, status);
        return;
    }

    const int64_t timestamp_ms = now_ms();
    const bool due = s_last_server_summary_log_ms == 0 ||
                     timestamp_ms - s_last_server_summary_log_ms >=
                         (int64_t)NETWORK_WORKER_OFFLINE_POLICY_LOG_MS;
    if (!force && !due) {
        return;
    }

    offline_policy_record_server_result(ret, status);
    log_server_health_summary(reason, ret, status, true);
}

static bool update_server_health(esp_err_t ret, int status, const char *reason)
{
    const bool ok = server_result_ok(ret, status);
    const bool was_ready = s_server_ready;

    if (ok) {
        if (s_server_success_count < UINT32_MAX) {
            ++s_server_success_count;
        }
        s_server_failure_count = 0U;
        s_server_last_error[0] = '\0';
        if (!s_server_ready &&
            s_server_success_count >= NETWORK_WORKER_SERVER_RECOVER_THRESHOLD) {
            s_server_ready = true;
        }
        record_offline_policy_limited(ret, status, reason, was_ready != s_server_ready);
    } else {
        const char *code = offline_policy_code_for_result(ret, status);
        strlcpy(s_server_last_error,
                code != NULL && code[0] != '\0' ? code : ESP111_PROTOCOL_ERROR_SERVER_UNAVAILABLE,
                sizeof(s_server_last_error));
        s_server_success_count = 0U;
        if (s_server_failure_count < UINT32_MAX) {
            ++s_server_failure_count;
        }
        if (s_server_ready &&
            s_server_failure_count >= NETWORK_WORKER_SERVER_FAIL_THRESHOLD) {
            s_server_ready = false;
        }
        record_offline_policy_limited(ret, status, reason, was_ready != s_server_ready);
    }

    if (was_ready != s_server_ready) {
        ESP_LOGI(TAG,
                 "server_ready transition %d -> %d reason=%s failures=%lu successes=%lu last_error=%s",
                 was_ready ? 1 : 0,
                 s_server_ready ? 1 : 0,
                 reason != NULL ? reason : "unknown",
                 (unsigned long)s_server_failure_count,
                 (unsigned long)s_server_success_count,
                 s_server_last_error[0] != '\0' ? s_server_last_error : "-");
        gateway_event_reporter_record_server_state(s_server_ready);
    }

    return s_server_ready;
}

static void mark_server_physical_down(const char *reason)
{
    const bool was_ready = s_server_ready;
    s_server_ready = false;
    s_server_success_count = 0U;
    s_server_failure_count = 0U;
    strlcpy(s_server_last_error,
            ESP111_PROTOCOL_ERROR_GATEWAY_OFFLINE,
            sizeof(s_server_last_error));

    if (was_ready) {
        ESP_LOGI(TAG,
                 "server_ready transition 1 -> 0 reason=%s failures=0 successes=0 last_error=%s",
                 reason != NULL ? reason : "physical_down",
                 s_server_last_error);
        gateway_event_reporter_record_server_state(false);
        record_offline_policy_limited(ESP_ERR_INVALID_STATE, 0, reason, true);
    }
}

static void log_upload_drop_summary(const char *reason, const network_worker_work_item_t *item)
{
    const int64_t timestamp_ms = now_ms();
    if (s_last_upload_drop_log_ms != 0 &&
        timestamp_ms - s_last_upload_drop_log_ms < (int64_t)NETWORK_WORKER_UPLOAD_DROP_LOG_MS) {
        return;
    }
    s_last_upload_drop_log_ms = timestamp_ms;
    ESP_LOGW(TAG,
             "upload queue drop/coalesce reason=%s type=%s json_type=%s source=%s drop_count=%lu coalesce_count=%lu depth=%u server_ready=%d",
             reason != NULL ? reason : "unknown",
             work_name(item != NULL ? item->work_type : NETWORK_WORKER_WORK_UPLOAD_JSON),
             json_type_name(item != NULL ? item->json_type : NETWORK_WORKER_SERVER_JSON_INGEST),
             item != NULL ? item->source : "-",
             (unsigned long)s_low_priority_drop_count,
             (unsigned long)s_low_priority_coalesce_count,
             s_work_queue != NULL ? (unsigned int)uxQueueMessagesWaiting(s_work_queue) : 0U,
             server_link_stable() ? 1 : 0);
}

static void log_server_upload_failed_limited(const network_worker_work_item_t *item,
                                             esp_err_t ret,
                                             int status)
{
    const int64_t timestamp_ms = now_ms();
    if (s_last_upload_fail_log_ms != 0 &&
        timestamp_ms - s_last_upload_fail_log_ms < (int64_t)NETWORK_WORKER_UPLOAD_FAIL_LOG_MS) {
        return;
    }
    s_last_upload_fail_log_ms = timestamp_ms;
    ESP_LOGW(TAG,
             "server JSON upload failed type=%s source=%s status=%d ret=%s",
             item != NULL ? json_type_name(item->json_type) : "unknown",
             item != NULL ? item->source : "-",
             status,
             esp_err_to_name(ret));
}

static void reset_stream_runtime(const char *reason)
{
    s3_scheduler_reset_stream_queue(reason);
    device_stream_gateway_reset_timestamp_baseline(NULL, reason);
}

static void reset_local_http_retry(void)
{
    s_next_local_http_start_ms = 0;
    s_local_http_retry_count = 0U;
}

static void schedule_local_http_retry(int64_t timestamp_ms,
                                      const char *retry_reason,
                                      esp_err_t ret)
{
    const size_t retry_count = sizeof(s_local_http_retry_backoff_ms) /
                               sizeof(s_local_http_retry_backoff_ms[0]);
    if (s_local_http_retry_count < UINT32_MAX) {
        ++s_local_http_retry_count;
    }
    const size_t retry_index = s_local_http_retry_count == 0U ? 0U :
        (size_t)(s_local_http_retry_count - 1U);
    const uint32_t delay_ms = s_local_http_retry_backoff_ms[
        retry_index < retry_count ? retry_index : retry_count - 1U];
    s_next_local_http_start_ms = timestamp_ms + (int64_t)delay_ms;

    ESP_LOGW(TAG,
             "local HTTP retry scheduled reason=%s attempt=%lu retry_ms=%u state=%s handle=%d ret=%s",
             retry_reason != NULL ? retry_reason : "unknown",
             (unsigned long)s_local_http_retry_count,
             (unsigned int)delay_ms,
             local_http_server_state_name(local_http_server_get_state()),
             local_http_server_has_handle() ? 1 : 0,
             esp_err_to_name(ret));
}

static bool service_local_http_server(bool softap_ready, const char *reason)
{
    if (!s_local_http_start_requested) {
        return false;
    }

    if (!softap_ready) {
        s_softap_ready_since_ms = 0;
        reset_local_http_retry();
        if (!s_local_http_running && !local_http_server_has_handle()) {
            return false;
        }

        esp_err_t stop_ret = local_http_server_stop_with_reason("softap_not_ready");
        if (stop_ret == ESP_OK) {
            s_local_http_running = false;
            ESP_LOGI(TAG,
                     "local HTTP server stopped reason=%s handle=%d",
                     reason != NULL ? reason : "softap_not_ready",
                     local_http_server_has_handle() ? 1 : 0);
        } else {
            ESP_LOGW(TAG,
                     "local HTTP server stop failed reason=%s state=%s handle=%d ret=%s",
                     reason != NULL ? reason : "softap_not_ready",
                     local_http_server_state_name(local_http_server_get_state()),
                     local_http_server_has_handle() ? 1 : 0,
                     esp_err_to_name(stop_ret));
        }
        return false;
    }

    const int64_t timestamp_ms = now_ms();
    if (s_softap_ready_since_ms == 0) {
        s_softap_ready_since_ms = timestamp_ms;
        ESP_LOGI(TAG,
                 "local HTTP start deferred reason=softap_stability_gate gate_ms=%u source=%s",
                 (unsigned int)NETWORK_WORKER_SOFTAP_STABLE_GATE_MS,
                 reason != NULL ? reason : "unknown");
        return false;
    }

    const int64_t softap_stable_for_ms = timestamp_ms - s_softap_ready_since_ms;
    if (softap_stable_for_ms < (int64_t)NETWORK_WORKER_SOFTAP_STABLE_GATE_MS) {
        return false;
    }

    if (local_http_server_is_running()) {
        s_local_http_running = true;
        reset_local_http_retry();
        return true;
    }
    s_local_http_running = false;

    if (timestamp_ms < s_next_local_http_start_ms) {
        return false;
    }

    if (local_http_server_has_handle()) {
        esp_err_t cleanup_ret = local_http_server_stop_with_reason("retry_preflight_cleanup");
        if (cleanup_ret != ESP_OK || local_http_server_has_handle()) {
            schedule_local_http_retry(timestamp_ms,
                                      "retry_preflight_cleanup_failed",
                                      cleanup_ret != ESP_OK ? cleanup_ret : ESP_FAIL);
            return false;
        }
    }

    ESP_LOGI(TAG,
             "local HTTP start attempt reason=softap_stable_gate stable_for_ms=%lld state=%s handle=%d",
             (long long)softap_stable_for_ms,
             local_http_server_state_name(local_http_server_get_state()),
             local_http_server_has_handle() ? 1 : 0);
    esp_err_t start_ret = local_http_server_start_with_reason("softap_stable_gate");
    if (start_ret == ESP_OK && local_http_server_is_running()) {
        s_local_http_running = true;
        reset_local_http_retry();
        ESP_LOGI(TAG,
                 "local HTTP server ready after SoftAP ready reason=%s",
                 reason != NULL ? reason : "unknown");
        return true;
    }

    const esp_err_t cleanup_ret = local_http_server_stop_with_reason("start_failure_cleanup");
    if (cleanup_ret != ESP_OK || local_http_server_has_handle()) {
        schedule_local_http_retry(timestamp_ms,
                                  "start_failure_cleanup_failed",
                                  cleanup_ret != ESP_OK ? cleanup_ret : ESP_FAIL);
    } else {
        schedule_local_http_retry(timestamp_ms,
                                  start_ret == ESP_OK ? "start_not_running" : "httpd_start_failed",
                                  start_ret == ESP_OK ? ESP_FAIL : start_ret);
    }
    return false;
}

static void set_local_ingest_ready(bool ready, const char *reason)
{
    if (s_local_ingest_ready == ready) {
        return;
    }

    s_local_ingest_ready = ready;
    gateway_wifi_set_net_ready_gate(ready, reason);
    if (!ready) {
        reset_stream_runtime(reason);
        mark_server_physical_down(reason);
    }
}

static bool server_available_for_gate(const char *reason)
{
    const int64_t timestamp_ms = now_ms();
    if (s_last_server_probe_ms != 0 &&
        timestamp_ms - s_last_server_probe_ms < (int64_t)NETWORK_WORKER_SERVER_PROBE_INTERVAL_MS) {
        return s_server_ready;
    }
    s_last_server_probe_ms = timestamp_ms;

    int status = 0;
    esp_err_t ret = server_client_probe_available(&status);
    if (ret == ESP_ERR_NOT_FINISHED) {
        return s_server_ready;
    }
    const bool ready = update_server_health(ret, status, reason);
    if (!server_result_ok(ret, status)) {
        log_server_health_summary(reason, ret, status, false);
    }
    return ready;
}

static void set_gateway_link_state(network_worker_link_state_t state, const char *reason)
{
    if (s_link_state == state) {
        return;
    }

    network_worker_link_state_t old_state = s_link_state;
    s_link_state = state;
    ESP_LOGI(TAG,
             "gateway_link transition %s -> %s reason=%s softap=%d sta_started=%d sta_connected=%d",
             network_worker_link_state_name(old_state),
             network_worker_link_state_name(state),
             reason != NULL ? reason : "unknown",
             gateway_wifi_is_softap_ready() ? 1 : 0,
             gateway_wifi_is_sta_started() ? 1 : 0,
             gateway_wifi_is_sta_connected() ? 1 : 0);
}

static void publish_scheduler_state(s3_scheduler_network_state_t state, const char *reason)
{
    if (s_last_scheduler_state == state) {
        return;
    }
    s_last_scheduler_state = state;

    esp_err_t ret = s3_scheduler_enqueue_network_state(state);
    if (ret != ESP_OK) {
        /* scheduler 队列满时仍直接更新状态，避免全局网络 gate 长时间停在旧值。 */
        ESP_LOGW(TAG,
                 "scheduler network state enqueue failed state=%s ret=%s",
                 s3_scheduler_network_state_name(state),
                 esp_err_to_name(ret));
        s3_scheduler_set_network_state(state);
    }
}

static void schedule_sta_connect(uint32_t delay_ms)
{
    if (!gateway_config_sta_credentials_configured()) {
        s_sta_connect_pending = false;
        s_sta_connect_request_sent = false;
        s_sta_reconnect_needed = false;
        return;
    }
    /* 只记录意图，真正调用 WiFi connect 在 worker task 中执行。 */
    s_sta_connect_pending = true;
    s_sta_connect_request_sent = false;
    s_sta_connect_request_deadline_ms = 0;
    s_next_sta_connect_ms = now_ms() + (int64_t)delay_ms;
}

static void schedule_sta_scan(bool avoid_current, uint32_t delay_ms, const char *reason)
{
    if (!gateway_config_sta_credentials_configured() || gateway_wifi_is_sta_connected()) {
        return;
    }
    if (s_sta_scan_in_progress) {
        return;
    }

    s_sta_scan_pending = true;
    s_sta_scan_avoid_current = avoid_current;
    s_next_sta_scan_ms = now_ms() + (int64_t)delay_ms;
    strlcpy(s_sta_scan_reason,
            reason != NULL ? reason : "unspecified",
            sizeof(s_sta_scan_reason));
}

static void schedule_sta_reconnect_from_disconnect(uint8_t disconnect_reason)
{
    const network_reconnect_policy_t policy = classify_reconnect_policy(disconnect_reason);
    const uint32_t retry_count = gateway_wifi_record_sta_credential_failure();
    const uint32_t backoff_ms = sta_reconnect_backoff_ms(retry_count);

    const bool rescan_fallback = policy == NETWORK_RECONNECT_RESCAN_FALLBACK ||
                                 ((policy == NETWORK_RECONNECT_ASSOC_RETRY ||
                                   policy == NETWORK_RECONNECT_CURRENT_RETRY) &&
                                  retry_count >= NETWORK_WORKER_STA_ASSOC_FAILURE_BUDGET);
    if (rescan_fallback) {
        gateway_wifi_log_sta_candidate_fallback("connect_failed");
        schedule_sta_scan(true, backoff_ms, network_reconnect_policy_name(policy));
    } else {
        schedule_sta_connect(backoff_ms);
    }
    ESP_LOGW(TAG,
             "WIFI_RECONNECT_POLICY disconnect_reason=%u policy=%s current_ssid_index=%u retry_count=%lu assoc_retry_budget=%u backoff_ms=%lu action=%s",
             (unsigned int)disconnect_reason,
             network_reconnect_policy_name(policy),
             (unsigned int)gateway_wifi_get_sta_credential_index(),
             (unsigned long)retry_count,
             (unsigned int)NETWORK_WORKER_STA_ASSOC_FAILURE_BUDGET,
             (unsigned long)backoff_ms,
             rescan_fallback ? "rescan_fallback" : "retry_current");
}

static void ensure_sta_reconnect_pending(void)
{
    if (!gateway_config_sta_credentials_configured() || !gateway_wifi_is_sta_started() ||
        gateway_wifi_is_sta_connected() || s_sta_connect_pending || s_sta_scan_pending ||
        s_sta_scan_in_progress) {
        return;
    }

    const int64_t timestamp_ms = now_ms();
    if (!gateway_wifi_has_sta_candidate()) {
        schedule_sta_scan(false, 0U, "initial_candidate_selection");
        return;
    }
    if (s_sta_disconnected_since_ms > 0 &&
        timestamp_ms - s_sta_disconnected_since_ms >=
            (int64_t)NETWORK_WORKER_STA_LONG_DISCONNECTED_SCAN_MS &&
        timestamp_ms - s_sta_last_scan_start_ms >=
            (int64_t)NETWORK_WORKER_STA_LONG_DISCONNECTED_SCAN_MS) {
        schedule_sta_scan(false, 0U, "long_sta_disconnect");
        return;
    }

    const bool reconnect_was_needed = s_sta_reconnect_needed;
    s_sta_reconnect_needed = true;
    schedule_sta_connect(s_sta_reconnect_backoff_ms[0]);
    ESP_LOGW(TAG,
             "NETWORK_RECONNECT_COMPENSATE sta_started=1 sta_connected=0 reconnect_pending=0 reconnect_needed=%d action=schedule_selected_ap delay_ms=%lu",
             reconnect_was_needed ? 1 : 0,
             (unsigned long)s_sta_reconnect_backoff_ms[0]);
}

static void service_sta_scan(void)
{
    if (s_sta_scan_in_progress) {
        if (now_ms() < s_sta_scan_deadline_ms) {
            return;
        }
        const esp_err_t ret = gateway_wifi_cancel_sta_scan();
        ESP_LOGW(TAG,
                 "WIFI_SCAN_DONE count=0 status=timeout timeout_ms=%u ret=%s",
                 (unsigned int)NETWORK_WORKER_STA_SCAN_TIMEOUT_MS,
                 esp_err_to_name(ret));
        s_sta_scan_in_progress = false;
        schedule_sta_scan(false,
                          NETWORK_WORKER_STA_LONG_DISCONNECTED_SCAN_MS,
                          "scan_timeout_retry");
        return;
    }
    if (!s_sta_scan_pending || gateway_wifi_is_sta_connected() ||
        now_ms() < s_next_sta_scan_ms) {
        return;
    }

    const esp_err_t ret = gateway_wifi_start_sta_scan();
    if (ret == ESP_OK) {
        s_sta_scan_pending = false;
        s_sta_scan_in_progress = true;
        s_sta_last_scan_start_ms = now_ms();
        s_sta_scan_deadline_ms =
            s_sta_last_scan_start_ms + (int64_t)NETWORK_WORKER_STA_SCAN_TIMEOUT_MS;
        ESP_LOGI(TAG,
                 "WIFI_SCAN_START trigger=%s timeout_ms=%u avoid_current=%d",
                 s_sta_scan_reason[0] != '\0' ? s_sta_scan_reason : "unspecified",
                 (unsigned int)NETWORK_WORKER_STA_SCAN_TIMEOUT_MS,
                 s_sta_scan_avoid_current ? 1 : 0);
        return;
    }

    s_sta_scan_pending = false;
    ESP_LOGW(TAG,
             "WIFI_SCAN_START trigger=%s status=failed ret=%s",
             s_sta_scan_reason[0] != '\0' ? s_sta_scan_reason : "unspecified",
             esp_err_to_name(ret));
    schedule_sta_scan(false,
                      NETWORK_WORKER_STA_LONG_DISCONNECTED_SCAN_MS,
                      "scan_start_retry");
}

static void service_sta_connect(void)
{
    if (!s_sta_connect_pending || gateway_wifi_is_sta_connected()) {
        return;
    }

    const int64_t timestamp_ms = now_ms();
    if (s_sta_connect_request_sent) {
        if (timestamp_ms < s_sta_connect_request_deadline_ms) {
            return;
        }
        ESP_LOGW(TAG,
                 "STA connect request timed out pending=1 action=retry_selected_ap timeout_ms=%u",
                 (unsigned int)NETWORK_WORKER_STA_RECONNECT_ATTEMPT_TIMEOUT_MS);
        s_sta_connect_pending = false;
        s_sta_connect_request_sent = false;
        s_sta_reconnect_needed = true;
        schedule_sta_reconnect_from_disconnect(WIFI_REASON_UNSPECIFIED);
        return;
    }
    if (timestamp_ms < s_next_sta_connect_ms) {
        return;
    }

    esp_err_t ret = gateway_wifi_connect_sta_current();
    if (ret == ESP_OK || ret == ESP_ERR_WIFI_STATE) {
        s_sta_connect_request_sent = true;
        s_sta_connect_request_deadline_ms =
            timestamp_ms + (int64_t)NETWORK_WORKER_STA_RECONNECT_ATTEMPT_TIMEOUT_MS;
        return;
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "STA connect request failed mode=selected_ap ret=%s",
                 esp_err_to_name(ret));
        s_sta_reconnect_needed = true;
        schedule_sta_reconnect_from_disconnect(WIFI_REASON_UNSPECIFIED);
    }
}

static void evaluate_state(const char *reason)
{
    prune_pending_stations(now_us());
    service_sta_scan();
    ensure_sta_reconnect_pending();
    service_sta_connect();

    /*
     * local ingest 只依赖 SoftAP。STA/IP/server preflight 只影响 Server upload gate，
     * 不能暂停 C5 本地 register/heartbeat/sensor/UDP stream ingest。
     */
    const bool softap_ready = gateway_wifi_is_softap_ready();
    const bool sta_started = gateway_wifi_is_sta_started();
    const bool sta_connected = gateway_wifi_is_sta_connected();
    const bool sta_ip_ready = gateway_wifi_is_sta_ip_ready();
    const bool stable_inputs_ready = link_stable_inputs_ready();
    const bool local_http_ready = service_local_http_server(softap_ready, reason);
    set_local_ingest_ready(softap_ready && local_http_ready, reason);
    if (!softap_ready) {
        mark_server_physical_down(reason);
        publish_scheduler_state(S3_SCHEDULER_NET_NOT_READY, reason);
        set_gateway_link_state(NETWORK_WORKER_LINK_DOWN, reason);
        return;
    }

    if (!sta_started) {
        mark_server_physical_down(reason);
        publish_scheduler_state(S3_SCHEDULER_NET_NOT_READY, reason);
        set_gateway_link_state(NETWORK_WORKER_LINK_UP, reason);
        return;
    }

    if (!sta_connected) {
        mark_server_physical_down(reason);
        publish_scheduler_state(S3_SCHEDULER_NET_NOT_READY, reason);
        set_gateway_link_state(NETWORK_WORKER_LINK_UP, reason);
        return;
    }

    if (!sta_ip_ready) {
        mark_server_physical_down(reason);
        publish_scheduler_state(S3_SCHEDULER_STA_CONNECTED, reason);
        set_gateway_link_state(NETWORK_WORKER_LINK_UP, reason);
        return;
    }

    if (s_last_network_change_ms <= 0) {
        s_last_network_change_ms = now_ms();
    }

    const int64_t stable_for_ms = now_ms() - s_last_network_change_ms;
    if (!stable_inputs_ready || stable_for_ms < (int64_t)NETWORK_WORKER_STABLE_GATE_MS) {
        publish_scheduler_state(S3_SCHEDULER_IP_READY, reason);
        set_gateway_link_state(NETWORK_WORKER_LINK_IP_READY, reason);
        return;
    }

    if (!server_available_for_gate(reason)) {
        ESP_LOGD(TAG, "server upload gate closed reason=%s local_ingest_ready=%d",
                 reason != NULL ? reason : "unknown",
                 gateway_wifi_is_local_ingest_ready() ? 1 : 0);
        publish_scheduler_state(S3_SCHEDULER_IP_READY, reason);
        set_gateway_link_state(NETWORK_WORKER_LINK_IP_READY, reason);
        return;
    }

    const bool entering_link_stable = s_link_state != NETWORK_WORKER_LINK_STABLE;
    publish_scheduler_state(S3_SCHEDULER_LINK_STABLE, reason);
    set_gateway_link_state(NETWORK_WORKER_LINK_STABLE, reason);
    if (entering_link_stable) {
        log_network_epoch("LINK_STABLE", reason);
    }
    network_replay_worker_request_bme_replay();
}

static bool resolve_ap_station_device(const network_worker_item_t *item,
                                      char *out_device_id,
                                      size_t out_size,
                                      char *out_peer_ip,
                                      size_t peer_ip_size)
{
    if (out_device_id == NULL || out_size == 0U) {
        return false;
    }
    out_device_id[0] = '\0';
    if (out_peer_ip != NULL && peer_ip_size > 0U) {
        out_peer_ip[0] = '\0';
    }
    if (item == NULL || !item->has_ap_station) {
        return false;
    }

    char peer_ip[16] = {0};
    if (gateway_wifi_get_ap_client_ip(item->ap_mac, peer_ip, sizeof(peer_ip)) &&
        out_peer_ip != NULL && peer_ip_size > 0U) {
        strlcpy(out_peer_ip, peer_ip, peer_ip_size);
    }

    if (child_registry_find_device_by_peer_mac(item->ap_mac,
                                               out_device_id,
                                               out_size)) {
        return true;
    }

    if (peer_ip[0] == '\0') {
        return false;
    }
    return child_registry_find_device_by_peer_ip(peer_ip, out_device_id, out_size);
}

static bool release_disconnected_session(const char *device_id,
                                         int64_t disconnected_at_us,
                                         const char *reason)
{
    resource_manager_session_view_t before = {0};
    resource_manager_session_view_t after = {0};
    if (!resource_manager_get_session(device_id, &before)) {
        return false;
    }
    (void)resource_manager_release_child_by_identity(device_id,
                                                     before.generation,
                                                     disconnected_at_us,
                                                     reason);
    return resource_manager_get_session(device_id, &after) &&
           after.generation != before.generation &&
           !resource_manager_is_live(device_id);
}

static void handle_network_event(const network_worker_item_t *item)
{
    if (item == NULL) {
        return;
    }

    const char *reason = source_name(item->source);
    if (source_affects_upstream_stability(item->source)) {
        s_last_network_change_ms = item->event_time_ms > 0 ? item->event_time_ms : now_ms();
    }

    ESP_LOGI(TAG,
             "network event=%s source=%s ip=0x%08lx disconnect_reason=%u event_time_us=%lld ap_station=%d aid=%u",
             event_name(item->event),
             reason,
             (unsigned long)item->ip_addr,
             (unsigned int)item->disconnect_reason,
             (long long)item->event_time_us,
             item->has_ap_station ? 1 : 0,
             (unsigned int)item->ap_aid);

    switch (item->source) {
    case NETWORK_WORKER_SOURCE_SOFTAP_START:
        s_softap_ready_since_ms = item->event_time_ms > 0 ? item->event_time_ms : now_ms();
        reset_local_http_retry();
        ESP_LOGI(TAG,
                 "local HTTP SoftAP ready reason=%s gate_ms=%u",
                 reason,
                 (unsigned int)NETWORK_WORKER_SOFTAP_STABLE_GATE_MS);
        break;
    case NETWORK_WORKER_SOURCE_SOFTAP_STOP:
        s_softap_ready_since_ms = 0;
        reset_local_http_retry();
        break;
    case NETWORK_WORKER_SOURCE_AP_STA_CONNECTED:
    {
        device_stream_gateway_reset_timestamp_baseline(NULL, "ap_sta_connected");
        record_pending_station(item);
        char device_id[CHILD_REGISTRY_DEVICE_ID_LEN] = {0};
        char peer_ip[16] = {0};
        char peer_mac[18] = {0};
        format_ap_station_mac(item->has_ap_station ? item->ap_mac : NULL,
                              peer_mac,
                              sizeof(peer_mac));
        if (resolve_ap_station_device(item,
                                      device_id,
                                      sizeof(device_id),
                                      peer_ip,
                                      sizeof(peer_ip))) {
            esp_err_t prep_ret = resource_manager_prepare_reconnect_at_us(
                device_id,
                item->event_time_us,
                "ap_sta_connected");
            ESP_LOGI(TAG,
                     "AP child connect mapped device_id=%s aid=%u peer_mac=%s peer_ip=%s action=restore_prepare ret=%s",
                     device_id,
                     (unsigned int)item->ap_aid,
                     peer_mac,
                     peer_ip[0] != '\0' ? peer_ip : "<unknown>",
                     esp_err_to_name(prep_ret));
        } else {
            ESP_LOGI(TAG,
                     "AP child connect unmapped aid=%u peer_mac=%s peer_ip=%s action=wait_for_identity",
                     (unsigned int)item->ap_aid,
                     peer_mac,
                     peer_ip[0] != '\0' ? peer_ip : "<unknown>");
        }
        break;
    }
    case NETWORK_WORKER_SOURCE_STA_START:
        s_sta_reconnect_needed = true;
        s_sta_disconnected_since_ms = item->event_time_ms > 0 ? item->event_time_ms : now_ms();
        schedule_sta_scan(false, 0U, "sta_start");
        break;
    case NETWORK_WORKER_SOURCE_STA_CONNECTED:
        s_sta_connect_pending = false;
        s_sta_connect_request_sent = false;
        s_sta_connect_request_deadline_ms = 0;
        s_sta_reconnect_needed = false;
        s_sta_disconnected_since_ms = 0;
        ESP_LOGI(TAG, "WIFI_CONNECT_RESULT status=sta_connected");
        server_client_on_network_epoch_changed(0U, "sta_connected_waiting_ip");
        log_network_epoch("STA_CONNECTED", reason);
        break;
    case NETWORK_WORKER_SOURCE_STA_DISCONNECTED:
        s_sta_reconnect_needed = true;
        s_sta_connect_request_sent = false;
        s_sta_connect_request_deadline_ms = 0;
        if (s_sta_disconnected_since_ms == 0) {
            s_sta_disconnected_since_ms =
                item->event_time_ms > 0 ? item->event_time_ms : now_ms();
        }
        ESP_LOGW(TAG,
                 "WIFI_CONNECT_RESULT status=disconnected disconnect_reason=%u",
                 (unsigned int)item->disconnect_reason);
        server_client_on_network_epoch_changed(0U, reason);
        schedule_sta_reconnect_from_disconnect(item->disconnect_reason);
        break;
    case NETWORK_WORKER_SOURCE_STA_GOT_IP:
        s_sta_connect_pending = false;
        s_sta_connect_request_sent = false;
        s_sta_connect_request_deadline_ms = 0;
        s_sta_reconnect_needed = false;
        s_sta_disconnected_since_ms = 0;
        s_sta_scan_pending = false;
        s_sta_scan_in_progress = false;
        gateway_wifi_reset_sta_credential_failures();
        ESP_LOGI(TAG, "WIFI_CONNECT_RESULT status=ip_ready");
        server_client_on_network_epoch_changed(gateway_wifi_get_sta_network_epoch(), reason);
        log_network_epoch("GOT_IP", reason);
        break;
    case NETWORK_WORKER_SOURCE_STA_LOST_IP:
        server_client_on_network_epoch_changed(0U, reason);
        break;
    case NETWORK_WORKER_SOURCE_STA_STOP:
        s_sta_connect_pending = false;
        s_sta_connect_request_sent = false;
        s_sta_reconnect_needed = false;
        s_sta_scan_pending = false;
        s_sta_scan_in_progress = false;
        server_client_on_network_epoch_changed(0U, reason);
        break;
    case NETWORK_WORKER_SOURCE_STA_SCAN_DONE:
    {
        if (!s_sta_scan_in_progress) {
            ESP_LOGI(TAG, "WIFI_SCAN_DONE status=ignored reason=no_scan_in_progress");
            break;
        }
        s_sta_scan_in_progress = false;
        size_t scan_count = 0U;
        esp_err_t scan_ret = gateway_wifi_collect_sta_scan_candidates(&scan_count);
        if (scan_ret == ESP_OK) {
            scan_ret = gateway_wifi_select_sta_candidate(s_sta_scan_avoid_current);
        }
        if (scan_ret == ESP_OK) {
            schedule_sta_connect(0U);
        } else {
            ESP_LOGW(TAG,
                     "WIFI_SCAN_DONE count=%u status=no_usable_candidate ret=%s",
                     (unsigned int)scan_count,
                     esp_err_to_name(scan_ret));
            schedule_sta_scan(false,
                              NETWORK_WORKER_STA_LONG_DISCONNECTED_SCAN_MS,
                              "no_known_ap");
        }
        break;
    }
    case NETWORK_WORKER_SOURCE_LOCAL_HTTP_ENABLE:
        s_local_http_start_requested = true;
        break;
    case NETWORK_WORKER_SOURCE_AP_STA_DISCONNECTED:
    {
        char device_id[CHILD_REGISTRY_DEVICE_ID_LEN] = {0};
        char peer_ip[16] = {0};
        char peer_mac[18] = {0};
        format_ap_station_mac(item->has_ap_station ? item->ap_mac : NULL,
                              peer_mac,
                              sizeof(peer_mac));
        pending_softap_station_t pending_station = {0};
        const bool was_pending = take_pending_station(item, &pending_station);
        if (was_pending && !pending_station.device_bound) {
            ESP_LOGI(TAG,
                     "PENDING_STATION_DISCONNECT mac=%s ip=%s aid=%u timestamp_us=%lld action=no_resource_release",
                     peer_mac,
                     pending_station.peer_ip[0] != '\0' ? pending_station.peer_ip : "<unknown>",
                     (unsigned int)pending_station.aid,
                     (long long)pending_station.connected_at_us);
            break;
        }
        bool resolved = false;
        if (was_pending && pending_station.device_id[0] != '\0') {
            strlcpy(device_id, pending_station.device_id, sizeof(device_id));
            strlcpy(peer_ip, pending_station.peer_ip, sizeof(peer_ip));
            resolved = true;
        } else {
            resolved = resolve_ap_station_device(item,
                                                 device_id,
                                                 sizeof(device_id),
                                                 peer_ip,
                                                 sizeof(peer_ip));
        }
        if (resolved) {
            const bool released = release_disconnected_session(device_id,
                                                                item->event_time_us,
                                                                "ap_sta_disconnected");
            if (released) {
                device_stream_gateway_reset_timestamp_baseline(device_id,
                                                               "ap_sta_disconnected");
                ESP_LOGI(TAG,
                         "RESOURCE_RELEASE_REASON=STA_DISCONNECT device=%s peer_mac=%s peer_ip=%s aid=%u",
                         device_id,
                         peer_mac,
                         peer_ip[0] != '\0' ? peer_ip : "<unknown>",
                         (unsigned int)item->ap_aid);
            }
            ESP_LOGI(TAG,
                     "AP child disconnect mapped device_id=%s aid=%u peer_mac=%s peer_ip=%s action=%s",
                     device_id,
                     (unsigned int)item->ap_aid,
                     peer_mac,
                     peer_ip[0] != '\0' ? peer_ip : "<unknown>",
                     released ? "device_release" : "stale_event_ignored");
        } else {
            ESP_LOGW(TAG,
                     "RESOURCE_RELEASE_SKIP reason=UNKNOWN_STATION peer_mac=%s peer_ip=%s aid=%u action=no_release",
                     peer_mac,
                     peer_ip[0] != '\0' ? peer_ip : "<unknown>",
                     (unsigned int)item->ap_aid);
            ESP_LOGW(TAG,
                     "AP child disconnect unmapped aid=%u peer_mac=%s peer_ip=%s action=no_release",
                     (unsigned int)item->ap_aid,
                     peer_mac,
                     peer_ip[0] != '\0' ? peer_ip : "<unknown>");
            return;
        }
        break;
    }
    default:
        break;
    }

    evaluate_state(reason);
}

static esp_err_t enqueue_to_queue(QueueHandle_t queue, const network_worker_work_item_t *item)
{
    if (item == NULL || queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(queue, item, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void drop_low_priority_upload_backlog_locked(const char *reason)
{
    if (s_work_queue == NULL) {
        return;
    }

    network_worker_work_item_t kept[NETWORK_WORKER_WORK_QUEUE_DEPTH];
    size_t kept_count = 0U;
    size_t dropped = 0U;
    network_worker_work_item_t queued = {0};
    while (xQueueReceive(s_work_queue, &queued, 0) == pdTRUE) {
        if (upload_work_is_low_priority(&queued)) {
            ++dropped;
            ++s_low_priority_drop_count;
            clear_upload_pending_flag(&queued);
            release_work_item(&queued);
            memset(&queued, 0, sizeof(queued));
            continue;
        }
        if (kept_count < NETWORK_WORKER_WORK_QUEUE_DEPTH) {
            kept[kept_count++] = queued;
        } else {
            ++dropped;
            ++s_low_priority_drop_count;
            release_work_item(&queued);
        }
        memset(&queued, 0, sizeof(queued));
    }

    for (size_t i = 0; i < kept_count; ++i) {
        if (xQueueSend(s_work_queue, &kept[i], 0) != pdTRUE) {
            ++dropped;
            ++s_low_priority_drop_count;
            release_work_item(&kept[i]);
        }
    }

    if (dropped > 0U) {
        log_upload_drop_summary(reason, NULL);
    }
}

static void drop_low_priority_upload_backlog(const char *reason)
{
    if (s_work_queue_mutation_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_work_queue_mutation_lock, portMAX_DELAY);
    drop_low_priority_upload_backlog_locked(reason);
    xSemaphoreGive(s_work_queue_mutation_lock);
}

static void coalesce_upload_work_backlog(const network_worker_work_item_t *incoming,
                                         const char *reason)
{
    if (incoming == NULL || s_work_queue == NULL || !upload_work_is_low_priority(incoming)) {
        return;
    }

    network_worker_work_item_t kept[NETWORK_WORKER_WORK_QUEUE_DEPTH];
    size_t kept_count = 0U;
    size_t coalesced = 0U;
    network_worker_work_item_t queued = {0};
    while (xQueueReceive(s_work_queue, &queued, 0) == pdTRUE) {
        bool same_kind = queued.work_type == incoming->work_type &&
                         queued.json_type == incoming->json_type;
        if (same_kind) {
            ++coalesced;
            ++s_low_priority_coalesce_count;
            clear_upload_pending_flag(&queued);
            release_work_item(&queued);
            memset(&queued, 0, sizeof(queued));
            continue;
        }
        if (kept_count < NETWORK_WORKER_WORK_QUEUE_DEPTH) {
            kept[kept_count++] = queued;
        } else {
            ++coalesced;
            ++s_low_priority_drop_count;
            release_work_item(&queued);
        }
        memset(&queued, 0, sizeof(queued));
    }

    for (size_t i = 0; i < kept_count; ++i) {
        if (xQueueSend(s_work_queue, &kept[i], 0) != pdTRUE) {
            ++s_low_priority_drop_count;
            release_work_item(&kept[i]);
        }
    }

    if (coalesced > 0U) {
        log_upload_drop_summary(reason, incoming);
    }
}

static esp_err_t enqueue_upload_work_item(const network_worker_work_item_t *item)
{
    if (item == NULL || s_work_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const bool low_priority = upload_work_is_low_priority(item);
    if (low_priority && !server_link_stable()) {
        ++s_low_priority_drop_count;
        log_upload_drop_summary("server_not_ready", item);
        return ESP_ERR_INVALID_STATE;
    }
    if (item->work_type == NETWORK_WORKER_WORK_UPLOAD_SNAPSHOT &&
        snapshot_queue_under_pressure()) {
        ++s_low_priority_coalesce_count;
        log_upload_drop_summary("snapshot_paused_queue_pressure", item);
        return ESP_ERR_INVALID_STATE;
    }
    if (low_priority && upload_low_priority_already_pending(item)) {
        ++s_low_priority_coalesce_count;
        log_upload_drop_summary("low_priority_coalesced", item);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_work_queue_mutation_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_work_queue_mutation_lock, portMAX_DELAY);
    if (upload_work_is_high_priority(item) && upload_queue_under_pressure()) {
        drop_low_priority_upload_backlog_locked("high_priority_pressure_relief");
    }
    if (!upload_work_is_high_priority(item) && !low_priority && upload_queue_under_pressure()) {
        drop_low_priority_upload_backlog_locked("medium_priority_pressure_relief");
    }
    if (low_priority) {
        coalesce_upload_work_backlog(item, "low_priority_replace_latest");
    }

    BaseType_t queued = upload_work_is_high_priority(item) ?
                            xQueueSendToFront(s_work_queue, item, 0) :
                            xQueueSend(s_work_queue, item, 0);
    if (queued != pdTRUE && upload_work_is_high_priority(item)) {
        drop_low_priority_upload_backlog_locked("high_priority_make_room");
        queued = xQueueSendToFront(s_work_queue, item, 0);
    }
    if (queued != pdTRUE) {
        if (low_priority) {
            ++s_low_priority_drop_count;
            log_upload_drop_summary("queue_full", item);
        }
        xSemaphoreGive(s_work_queue_mutation_lock);
        return ESP_ERR_TIMEOUT;
    }

    if (low_priority) {
        mark_upload_pending_flag(item);
    }
    xSemaphoreGive(s_work_queue_mutation_lock);
    return ESP_OK;
}

static esp_err_t enqueue_command_work_item(const network_worker_work_item_t *item)
{
    if (item == NULL || s_command_queue_mutation_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_command_queue_mutation_lock, portMAX_DELAY);
    if (item->work_type == NETWORK_WORKER_WORK_COMMAND_PULL && s_command_pull_pending) {
        xSemaphoreGive(s_command_queue_mutation_lock);
        return ESP_OK;
    }
    if (item->work_type == NETWORK_WORKER_WORK_SMART_HOME_POLL && s_smart_home_poll_pending) {
        xSemaphoreGive(s_command_queue_mutation_lock);
        return ESP_OK;
    }

    esp_err_t ret = enqueue_to_queue(s_command_queue, item);
    if (ret == ESP_OK &&
        (item->work_type == NETWORK_WORKER_WORK_COMMAND_PULL ||
         item->work_type == NETWORK_WORKER_WORK_SMART_HOME_POLL)) {
        mark_upload_pending_flag(item);
    }
    xSemaphoreGive(s_command_queue_mutation_lock);
    return ret;
}

static esp_err_t enqueue_command_work_item_wait(const network_worker_work_item_t *item,
                                                TickType_t ticks_to_wait)
{
    if (item == NULL || s_command_queue == NULL || s_command_queue_mutation_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_command_queue_mutation_lock, portMAX_DELAY);
    BaseType_t queued = xQueueSend(s_command_queue, item, ticks_to_wait);
    xSemaphoreGive(s_command_queue_mutation_lock);
    if (queued != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void release_work_item(network_worker_work_item_t *item)
{
    if (item == NULL) {
        return;
    }
    clear_upload_pending_flag(item);
    if (item->json_body != NULL) {
        cJSON_free(item->json_body);
        item->json_body = NULL;
    }
    if (item->bme_cache_sequence != 0U) {
        (void)bme_cache_manager_mark_in_flight(item->bme_cache_sequence, false);
    }
}

static void requeue_or_drop_work(QueueHandle_t queue,
                                 network_worker_work_item_t *item,
                                 esp_err_t reason)
{
    if (item == NULL) {
        return;
    }
    SemaphoreHandle_t mutation_lock = queue == s_work_queue ? s_work_queue_mutation_lock :
                                      queue == s_command_queue ? s_command_queue_mutation_lock :
                                                               NULL;
    if (mutation_lock != NULL) {
        xSemaphoreTake(mutation_lock, portMAX_DELAY);
    }
    BaseType_t queued = queue != NULL ? xQueueSendToFront(queue, item, 0) : pdFALSE;
    if (mutation_lock != NULL) {
        xSemaphoreGive(mutation_lock);
    }
    if (queued == pdTRUE) {
        return;
    }

    /* 队列已满时宁可丢弃本次上云工作；Server 是事实中心，S3 不在 RAM 中堆积离线队列。 */
    ESP_LOGW(TAG,
             "offline work drop type=%s json_type=%s source=%s ret=%s",
             work_name(item->work_type),
             json_type_name(item->json_type),
             item->source,
             esp_err_to_name(reason));
    release_work_item(item);
}

static esp_err_t perform_server_json(network_worker_work_item_t *item)
{
    if (item == NULL || item->json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char response[SERVER_CLIENT_SMALL_BODY_BYTES];
    int status = 0;
    esp_err_t ret = ESP_ERR_INVALID_ARG;
    bool telemetry_local_deferred = false;
    bool peer_cancelled = false;
    switch (item->json_type) {
    case NETWORK_WORKER_SERVER_JSON_INGEST:
        if (item->device_id[0] != '\0') {
            if (!sensor_aggregator_peer_active(item->device_id) ||
                !resource_manager_is_live(item->device_id)) {
                ret = ESP_ERR_INVALID_STATE;
                peer_cancelled = true;
                break;
            }
            ret = server_client_post_ingest_json_cancellable_for_device(
                item->device_id,
                item->json_body,
                response,
                sizeof(response),
                &status,
                sensor_upload_cancelled,
                item->device_id);
            peer_cancelled = ret == ESP_ERR_INVALID_STATE &&
                             (!sensor_aggregator_peer_active(item->device_id) ||
                              !resource_manager_is_live(item->device_id));
        } else {
            ret = server_client_post_ingest_json(item->json_body,
                                                 response,
                                                 sizeof(response),
                                                 &status);
        }
        break;
    case NETWORK_WORKER_SERVER_JSON_GATEWAY_STATE:
        ret = server_client_post_gateway_state_json(item->json_body, response, sizeof(response), &status);
        telemetry_local_deferred = ret == ESP_ERR_NOT_FINISHED;
        break;
    case NETWORK_WORKER_SERVER_JSON_SYSTEM_LOG:
        ret = server_client_post_system_log_json(item->json_body, response, sizeof(response), &status);
        telemetry_local_deferred = ret == ESP_ERR_NOT_FINISHED;
        break;
    case NETWORK_WORKER_SERVER_JSON_ALARM:
        ret = server_client_post_alarm_json(item->json_body, response, sizeof(response), &status);
        telemetry_local_deferred = ret == ESP_ERR_NOT_FINISHED;
        break;
    default:
        ret = ESP_ERR_INVALID_ARG;
        break;
    }

    if (!peer_cancelled) {
        if (!upload_work_is_telemetry(item) && !telemetry_local_deferred) {
            (void)update_server_health(ret, status, json_type_name(item->json_type));
        }
    }
    if (item->json_type == NETWORK_WORKER_SERVER_JSON_INGEST &&
        item->bme_cache_sequence != 0U &&
        ret == ESP_OK &&
        status >= 200 &&
        status < 300) {
        esp_err_t delete_ret = bme_cache_manager_delete_sequence(item->bme_cache_sequence);
        if (delete_ret != ESP_OK && delete_ret != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG,
                     "BME cache delete after realtime upload failed seq=%lu ret=%s",
                     (unsigned long)item->bme_cache_sequence,
                     esp_err_to_name(delete_ret));
        }
    }
    if (!peer_cancelled && !telemetry_local_deferred &&
        (ret != ESP_OK || status < 200 || status >= 300)) {
        log_server_upload_failed_limited(item, ret, status);
    }
    return ret;
}

static void process_upload_work_item(network_worker_work_item_t *item)
{
    if (item == NULL) {
        return;
    }
    if (upload_work_is_peer_sensor(item) &&
        (!sensor_aggregator_peer_active(item->device_id) ||
         !resource_manager_is_live(item->device_id))) {
        resource_manager_log_session_diagnostic(item->device_id,
                                                NULL,
                                                "upload_drop",
                                                "session_not_active");
        ESP_LOGI(TAG,
                 "sensor upload cancelled device_id=%s source=%s reason=session_not_active",
                 item->device_id,
                 item->source);
        release_work_item(item);
        return;
    }
    if (!server_link_stable()) {
        requeue_or_drop_work(s_work_queue, item, ESP_ERR_INVALID_STATE);
        return;
    }

    /* upload worker 只处理 Server-facing 工作；C5 本地 HTTP 响应早已在 S3 ingress 路径完成。 */
    esp_err_t ret = ESP_OK;
    switch (item->work_type) {
    case NETWORK_WORKER_WORK_UPLOAD_JSON:
        ret = perform_server_json(item);
        if (ret == ESP_ERR_INVALID_STATE && !server_link_stable()) {
            requeue_or_drop_work(s_work_queue, item, ret);
            return;
        }
        break;
    case NETWORK_WORKER_WORK_UPLOAD_SNAPSHOT:
        sensor_aggregator_upload_snapshot_now();
        break;
    default:
        ret = ESP_ERR_INVALID_ARG;
        break;
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "network worker work failed type=%s source=%s ret=%s",
                 work_name(item->work_type),
                 item->source,
                 esp_err_to_name(ret));
    }
    release_work_item(item);
}

static esp_err_t perform_command_ack(network_worker_work_item_t *item)
{
    if (item == NULL || item->command_id[0] == '\0' || item->json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int status = 0;
    esp_err_t ret = server_client_ack_command(item->command_id,
                                             item->json_body,
                                             s_command_ack_response,
                                             sizeof(s_command_ack_response),
                                             &status);
    if (ret == ESP_ERR_INVALID_STATE && !server_link_stable()) {
        return ret;
    }
    /* command ack 属于 Server contract，失败只记录并交给上层下一轮命令生命周期处理。 */
    (void)update_server_health(ret, status, "command_ack");
    if (ret != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG,
                 "command ack upload failed id=%s status=%d ret=%s",
                 item->command_id,
                 status,
                 esp_err_to_name(ret));
    }
    return ret;
}

static void process_command_work_item(network_worker_work_item_t *item)
{
    if (item == NULL) {
        return;
    }
    if (!server_link_stable()) {
        requeue_or_drop_work(s_command_queue, item, ESP_ERR_INVALID_STATE);
        return;
    }

    /* command/smart-home 与普通 snapshot 分开队列，避免大 JSON 上传阻塞命令 ack。 */
    esp_err_t ret = ESP_OK;
    switch (item->work_type) {
    case NETWORK_WORKER_WORK_COMMAND_PULL:
        if (command_router_has_active_peers()) {
            command_router_poll_server_pending();
        }
        break;
    case NETWORK_WORKER_WORK_COMMAND_ACK:
        ret = perform_command_ack(item);
        if (ret == ESP_ERR_INVALID_STATE && !server_link_stable()) {
            requeue_or_drop_work(s_command_queue, item, ret);
            return;
        }
        break;
    case NETWORK_WORKER_WORK_SMART_HOME_POLL:
        smart_home_gateway_poll_once();
        break;
    default:
        ret = ESP_ERR_INVALID_ARG;
        break;
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "network worker work failed type=%s source=%s ret=%s",
                 work_name(item->work_type),
                 item->source,
                 esp_err_to_name(ret));
    }
    release_work_item(item);
}

static void network_worker_task(void *arg)
{
    (void)arg;
    const bool wdt_registered = app_task_wdt_add_current(TAG, "network_worker");
    ESP_LOGI(TAG,
             "network worker started queue_depth=%u stable_gate_ms=%u",
             (unsigned int)NETWORK_WORKER_QUEUE_DEPTH,
             (unsigned int)NETWORK_WORKER_STABLE_GATE_MS);
    app_stack_monitor_log(TAG, "network_worker", "entry");

    while (1) {
        network_worker_item_t item = {0};
        if (xQueueReceive(s_ap_disconnect_queue, &item, 0) == pdTRUE ||
            xQueueReceive(s_event_queue, &item, pdMS_TO_TICKS(NETWORK_WORKER_POLL_MS)) ==
                pdTRUE) {
            handle_network_event(&item);
        } else {
            /* 周期评估用于稳定窗口到期、STA reconnect 延迟到期等无事件状态推进。 */
            evaluate_state("periodic");
        }
        app_stack_monitor_log_periodic(TAG,
                                       "network_worker",
                                       &s_last_worker_stack_log_ms,
                                       APP_STACK_MONITOR_INTERVAL_MS);
        app_heap_monitor_log_periodic(TAG,
                                      &s_last_worker_heap_log_ms,
                                      APP_HEAP_MONITOR_INTERVAL_MS);
        app_task_wdt_reset_current(wdt_registered);
    }
}

static void upload_worker_task(void *arg)
{
    (void)arg;
    const bool wdt_registered = app_task_wdt_add_current(TAG, "upload_worker");
    ESP_LOGI(TAG,
             "upload worker started queue_depth=%u server_gate=LINK_STABLE",
             (unsigned int)NETWORK_WORKER_WORK_QUEUE_DEPTH);
    app_stack_monitor_log(TAG, "upload_worker", "entry");

    while (1) {
        if (!server_link_stable()) {
            /* Server 未 ready 时不上传；清掉过期的低优先级 snapshot。 */
            drop_low_priority_upload_backlog("server_not_ready_backlog");
            app_stack_monitor_log_periodic(TAG,
                                           "upload_worker",
                                           &s_last_upload_stack_log_ms,
                                           APP_STACK_MONITOR_INTERVAL_MS);
            app_heap_monitor_log_periodic(TAG,
                                          &s_last_upload_heap_log_ms,
                                          APP_HEAP_MONITOR_INTERVAL_MS);
            app_task_wdt_delay_ms(wdt_registered, NETWORK_WORKER_POLL_MS);
            continue;
        }

        network_worker_work_item_t item = {0};
        if (xQueueReceive(s_work_queue, &item, pdMS_TO_TICKS(NETWORK_WORKER_POLL_MS)) ==
            pdTRUE) {
            process_upload_work_item(&item);
        }
        app_stack_monitor_log_periodic(TAG,
                                       "upload_worker",
                                       &s_last_upload_stack_log_ms,
                                       APP_STACK_MONITOR_INTERVAL_MS);
        app_heap_monitor_log_periodic(TAG,
                                      &s_last_upload_heap_log_ms,
                                      APP_HEAP_MONITOR_INTERVAL_MS);
        app_task_wdt_reset_current(wdt_registered);
    }
}

static void snapshot_worker_task(void *arg)
{
    (void)arg;
    const bool wdt_registered = app_task_wdt_add_current(TAG, "snapshot_worker");
    ESP_LOGI(TAG, "snapshot worker started cache=1 priority=low interval_ms=%u",
             (unsigned int)UPLOAD_SNAPSHOT_INTERVAL_MS);

    while (1) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(NETWORK_WORKER_SNAPSHOT_WORKER_WAIT_MS)) ==
            0U) {
            app_task_wdt_reset_current(wdt_registered);
            continue;
        }

        if (s_work_queue_mutation_lock == NULL) {
            snapshot_stats_record_skip();
            ESP_LOGI(TAG, "snapshot_deferred reason=mutation_lock_unavailable");
            app_task_wdt_reset_current(wdt_registered);
            continue;
        }
        if (xSemaphoreTake(s_work_queue_mutation_lock,
                           pdMS_TO_TICKS(NETWORK_WORKER_SNAPSHOT_LOCK_TIMEOUT_MS)) != pdTRUE) {
            snapshot_stats_record_skip();
            ESP_LOGI(TAG, "snapshot_deferred reason=mutation_lock_timeout");
            app_task_wdt_reset_current(wdt_registered);
            continue;
        }
        const bool pending = s_snapshot_upload_pending;
        s_snapshot_upload_pending = false;
        xSemaphoreGive(s_work_queue_mutation_lock);
        if (!pending) {
            app_task_wdt_reset_current(wdt_registered);
            continue;
        }

        if (!server_link_stable() || snapshot_has_higher_priority_work() ||
            server_client_snapshot_upload_should_skip()) {
            snapshot_stats_record_skip();
            ESP_LOGI(TAG, "snapshot_deferred reason=higher_priority_or_link_not_ready");
            app_task_wdt_reset_current(wdt_registered);
            continue;
        }

        const esp_err_t ret = sensor_aggregator_upload_snapshot_now();
        if (ret == ESP_OK) {
            snapshot_stats_record_upload();
        } else if (ret == ESP_ERR_HTTP_EAGAIN || ret == ESP_ERR_NOT_FINISHED) {
            snapshot_stats_record_skip();
            ESP_LOGI(TAG, "snapshot_deferred reason=slot_or_snapshot_lock_busy ret=%s",
                     esp_err_to_name(ret));
        } else {
            ESP_LOGW(TAG,
                     "snapshot_failed ret=%s; no retry, next interval rebuilds latest state",
                     esp_err_to_name(ret));
        }
        app_task_wdt_reset_current(wdt_registered);
    }
}

static void command_worker_task(void *arg)
{
    (void)arg;
    const bool wdt_registered = app_task_wdt_add_current(TAG, "command_worker");
    ESP_LOGI(TAG,
             "command worker started queue_depth=%u server_gate=LINK_STABLE",
             (unsigned int)NETWORK_WORKER_WORK_QUEUE_DEPTH);
    app_stack_monitor_log(TAG, "command_worker", "entry");

    while (1) {
        if (!server_link_stable()) {
            /* command 队列同样等 LINK_STABLE，避免 ack 在 STA 抖动时反复失败。 */
            app_stack_monitor_log_periodic(TAG,
                                           "command_worker",
                                           &s_last_command_stack_log_ms,
                                           APP_STACK_MONITOR_INTERVAL_MS);
            app_heap_monitor_log_periodic(TAG,
                                          &s_last_command_heap_log_ms,
                                          APP_HEAP_MONITOR_INTERVAL_MS);
            app_task_wdt_delay_ms(wdt_registered, NETWORK_WORKER_POLL_MS);
            continue;
        }

        network_worker_work_item_t item = {0};
        if (xQueueReceive(s_command_queue, &item, pdMS_TO_TICKS(NETWORK_WORKER_POLL_MS)) ==
            pdTRUE) {
            process_command_work_item(&item);
        }
        app_stack_monitor_log_periodic(TAG,
                                       "command_worker",
                                       &s_last_command_stack_log_ms,
                                       APP_STACK_MONITOR_INTERVAL_MS);
        app_heap_monitor_log_periodic(TAG,
                                      &s_last_command_heap_log_ms,
                                      APP_HEAP_MONITOR_INTERVAL_MS);
        app_task_wdt_reset_current(wdt_registered);
    }
}

esp_err_t network_worker_init(void)
{
    if (s_pending_station_lock == NULL) {
        s_pending_station_lock = xSemaphoreCreateMutex();
        if (s_pending_station_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_work_queue_mutation_lock == NULL) {
        s_work_queue_mutation_lock = xSemaphoreCreateMutex();
        if (s_work_queue_mutation_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_command_queue_mutation_lock == NULL) {
        s_command_queue_mutation_lock = xSemaphoreCreateMutex();
        if (s_command_queue_mutation_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_event_queue == NULL) {
        s_event_queue = xQueueCreate(NETWORK_WORKER_QUEUE_DEPTH, sizeof(network_worker_item_t));
        if (s_event_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_ap_disconnect_queue == NULL) {
        s_ap_disconnect_queue =
            xQueueCreate(GATEWAY_CONFIG_MAX_CHILDREN, sizeof(network_worker_item_t));
        if (s_ap_disconnect_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_work_queue == NULL) {
        s_work_queue =
            xQueueCreate(NETWORK_WORKER_WORK_QUEUE_DEPTH, sizeof(network_worker_work_item_t));
        if (s_work_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_command_queue == NULL) {
        s_command_queue =
            xQueueCreate(NETWORK_WORKER_WORK_QUEUE_DEPTH, sizeof(network_worker_work_item_t));
        if (s_command_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_worker_task == NULL) {
        BaseType_t created = xTaskCreate(network_worker_task,
                                         "network_worker",
                                         NETWORK_WORKER_TASK_STACK,
                                         NULL,
                                         NETWORK_WORKER_TASK_PRIORITY,
                                         &s_worker_task);
        if (created != pdPASS) {
            s_worker_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_upload_task == NULL) {
        BaseType_t created = xTaskCreate(upload_worker_task,
                                         "upload_worker",
                                         NETWORK_WORKER_UPLOAD_TASK_STACK,
                                         NULL,
                                         NETWORK_WORKER_UPLOAD_TASK_PRIORITY,
                                         &s_upload_task);
        if (created != pdPASS) {
            s_upload_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_command_task == NULL) {
        BaseType_t created = xTaskCreate(command_worker_task,
                                         "command_worker",
                                         NETWORK_WORKER_COMMAND_TASK_STACK,
                                         NULL,
                                         NETWORK_WORKER_COMMAND_TASK_PRIORITY,
                                         &s_command_task);
        if (created != pdPASS) {
            s_command_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_snapshot_task == NULL) {
        BaseType_t created = xTaskCreate(snapshot_worker_task,
                                         "snapshot_worker",
                                         NETWORK_WORKER_UPLOAD_TASK_STACK,
                                         NULL,
                                         NETWORK_WORKER_UPLOAD_TASK_PRIORITY - 1U,
                                         &s_snapshot_task);
        if (created != pdPASS) {
            s_snapshot_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t network_worker_enable_local_http_server(void)
{
    return network_worker_post_event(NETWORK_WORKER_EVENT_LINK_UP,
                                     NETWORK_WORKER_SOURCE_LOCAL_HTTP_ENABLE,
                                     0U,
                                     0U);
}

esp_err_t network_worker_post_event(network_worker_event_t event,
                                    network_worker_event_source_t source,
                                    uint32_t ip_addr,
                                    uint8_t disconnect_reason)
{
    if (s_event_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t event_time_us = now_us();
    network_worker_item_t item = {
        .event = event,
        .source = source,
        .ip_addr = ip_addr,
        .disconnect_reason = disconnect_reason,
        .event_time_us = event_time_us,
        .event_time_ms = event_time_us / 1000,
    };
    if (source == NETWORK_WORKER_SOURCE_AP_STA_DISCONNECTED) {
        if (xQueueSendToFront(s_event_queue, &item, 0) == pdTRUE ||
            (s_ap_disconnect_queue != NULL &&
             xQueueSend(s_ap_disconnect_queue, &item, 0) == pdTRUE)) {
            return ESP_OK;
        }
        return ESP_ERR_TIMEOUT;
    }
    return xQueueSend(s_event_queue, &item, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t network_worker_post_ap_station_event(network_worker_event_t event,
                                               network_worker_event_source_t source,
                                               uint32_t ip_addr,
                                               const uint8_t mac[6],
                                               uint8_t aid)
{
    if (s_event_queue == NULL || mac == NULL ||
        (source != NETWORK_WORKER_SOURCE_AP_STA_CONNECTED &&
         source != NETWORK_WORKER_SOURCE_AP_STA_DISCONNECTED)) {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t event_time_us = now_us();
    network_worker_item_t item = {
        .event = event,
        .source = source,
        .ip_addr = ip_addr,
        .event_time_us = event_time_us,
        .event_time_ms = event_time_us / 1000,
        .ap_aid = aid,
        .has_ap_station = true,
    };
    memcpy(item.ap_mac, mac, sizeof(item.ap_mac));
    if (source == NETWORK_WORKER_SOURCE_AP_STA_DISCONNECTED) {
        if (xQueueSendToFront(s_event_queue, &item, 0) == pdTRUE ||
            (s_ap_disconnect_queue != NULL &&
             xQueueSend(s_ap_disconnect_queue, &item, 0) == pdTRUE)) {
            return ESP_OK;
        }
        return ESP_ERR_TIMEOUT;
    }
    return xQueueSend(s_event_queue, &item, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t network_worker_bind_ap_station_identity(const char *device_id, const char *peer_ip)
{
    if (!gateway_config_child_allowed(device_id) || peer_ip == NULL || peer_ip[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_pending_station_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t mac[6] = {0};
    const bool mac_found = gateway_wifi_get_ap_client_mac(peer_ip, mac);
    xSemaphoreTake(s_pending_station_lock, portMAX_DELAY);
    pending_softap_station_t *station = mac_found ? find_pending_station_locked(mac, 0U) : NULL;
    if (station == NULL) {
        for (size_t index = 0U; index < GATEWAY_CONFIG_MAX_CHILDREN; ++index) {
            pending_softap_station_t *candidate = &s_pending_stations[index];
            if (candidate->used && strcmp(candidate->peer_ip, peer_ip) == 0) {
                station = candidate;
                break;
            }
        }
    }
    if (station == NULL) {
        xSemaphoreGive(s_pending_station_lock);
        return ESP_ERR_NOT_FOUND;
    }

    station->device_bound = true;
    strlcpy(station->device_id, device_id, sizeof(station->device_id));
    strlcpy(station->peer_ip, peer_ip, sizeof(station->peer_ip));
    if (mac_found) {
        memcpy(station->mac, mac, sizeof(station->mac));
    }
    char station_mac[18] = {0};
    format_ap_station_mac(station->mac, station_mac, sizeof(station_mac));
    ESP_LOGI(TAG,
             "SOFTAP_PENDING_STATION state=bound device=%s mac=%s ip=%s aid=%u timestamp_us=%lld",
             station->device_id,
             station_mac,
             station->peer_ip,
             (unsigned int)station->aid,
             (long long)station->connected_at_us);
    xSemaphoreGive(s_pending_station_lock);
    return ESP_OK;
}

network_worker_link_state_t network_worker_get_link_state(void)
{
    return s_link_state;
}

bool network_worker_is_server_ready(void)
{
    return s_server_ready;
}

network_worker_snapshot_stats_t network_worker_get_snapshot_stats(void)
{
    network_worker_snapshot_stats_t stats;
    portENTER_CRITICAL(&s_snapshot_stats_mux);
    stats = s_snapshot_stats;
    portEXIT_CRITICAL(&s_snapshot_stats_mux);
    return stats;
}

esp_err_t network_worker_submit_server_json(network_worker_server_json_type_t type,
                                            char *json_body,
                                            const char *source)
{
    return network_worker_submit_peer_server_json(type, json_body, NULL, source);
}

esp_err_t network_worker_submit_peer_server_json(network_worker_server_json_type_t type,
                                                 char *json_body,
                                                 const char *device_id,
                                                 const char *source)
{
    if (json_body == NULL || type > NETWORK_WORKER_SERVER_JSON_ALARM) {
        return ESP_ERR_INVALID_ARG;
    }

    network_worker_work_item_t item = {
        .work_type = NETWORK_WORKER_WORK_UPLOAD_JSON,
        .json_type = type,
        .json_body = json_body,
    };
    if (device_id != NULL && device_id[0] != '\0') {
        if (!gateway_config_child_allowed(device_id)) {
            return ESP_ERR_NOT_ALLOWED;
        }
        strlcpy(item.device_id, device_id, sizeof(item.device_id));
        if (type == NETWORK_WORKER_SERVER_JSON_INGEST) {
            char payload_device_id[CHILD_REGISTRY_DEVICE_ID_LEN] = {0};
            device_id_from_server_json(json_body,
                                       payload_device_id,
                                       sizeof(payload_device_id));
            if (payload_device_id[0] != '\0' &&
                strcmp(payload_device_id, device_id) != 0) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    } else if (type == NETWORK_WORKER_SERVER_JSON_INGEST) {
        device_id_from_server_json(json_body, item.device_id, sizeof(item.device_id));
    }
    if (item.device_id[0] != '\0' &&
        (!sensor_aggregator_peer_active(item.device_id) ||
         !resource_manager_is_live(item.device_id))) {
        resource_manager_log_session_diagnostic(item.device_id,
                                                NULL,
                                                "upload_reject",
                                                "session_not_active");
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(item.source, source != NULL ? source : "server_json", sizeof(item.source));
    /* 入队成功后 json_body 生命周期转交给 upload_worker。 */
    return enqueue_upload_work_item(&item);
}

esp_err_t network_worker_submit_bme_cached_json(char *json_body,
                                                uint32_t cache_sequence,
                                                const char *source)
{
    return network_worker_submit_bme_cached_json_for_peer(json_body,
                                                          cache_sequence,
                                                          NULL,
                                                          source);
}

esp_err_t network_worker_submit_bme_cached_json_for_peer(char *json_body,
                                                         uint32_t cache_sequence,
                                                         const char *device_id,
                                                         const char *source)
{
    if (json_body == NULL || cache_sequence == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    network_worker_work_item_t item = {
        .work_type = NETWORK_WORKER_WORK_UPLOAD_JSON,
        .json_type = NETWORK_WORKER_SERVER_JSON_INGEST,
        .json_body = json_body,
        .bme_cache_sequence = cache_sequence,
    };
    if (device_id != NULL && device_id[0] != '\0') {
        if (!gateway_config_child_allowed(device_id)) {
            return ESP_ERR_NOT_ALLOWED;
        }
        strlcpy(item.device_id, device_id, sizeof(item.device_id));
        char payload_device_id[CHILD_REGISTRY_DEVICE_ID_LEN] = {0};
        device_id_from_server_json(json_body,
                                   payload_device_id,
                                   sizeof(payload_device_id));
        if (payload_device_id[0] != '\0' &&
            strcmp(payload_device_id, device_id) != 0) {
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        device_id_from_server_json(json_body, item.device_id, sizeof(item.device_id));
    }
    if (item.device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sensor_aggregator_peer_active(item.device_id) ||
        !resource_manager_is_live(item.device_id)) {
        resource_manager_log_session_diagnostic(item.device_id,
                                                NULL,
                                                "bme_cache_reject",
                                                "session_not_active");
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(item.source, source != NULL ? source : "bme_cache", sizeof(item.source));
    esp_err_t ret = bme_cache_manager_mark_in_flight(cache_sequence, true);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = enqueue_upload_work_item(&item);
    if (ret != ESP_OK) {
        (void)bme_cache_manager_mark_in_flight(cache_sequence, false);
    }
    return ret;
}

static size_t cancel_peer_upload_queue(const char *device_id)
{
    if (s_work_queue == NULL || s_work_queue_mutation_lock == NULL || device_id == NULL) {
        return 0U;
    }
    size_t cancelled = 0U;
    xSemaphoreTake(s_work_queue_mutation_lock, portMAX_DELAY);
    const UBaseType_t queued_count = uxQueueMessagesWaiting(s_work_queue);
    network_worker_work_item_t item = {0};
    for (UBaseType_t i = 0; i < queued_count; ++i) {
        if (xQueueReceive(s_work_queue, &item, 0) != pdTRUE) {
            break;
        }
        if (upload_work_is_peer_sensor(&item) &&
            strcmp(item.device_id, device_id) == 0) {
            release_work_item(&item);
            ++cancelled;
        } else if (xQueueSend(s_work_queue, &item, 0) != pdTRUE) {
            release_work_item(&item);
            ++cancelled;
        }
        memset(&item, 0, sizeof(item));
    }
    xSemaphoreGive(s_work_queue_mutation_lock);
    return cancelled;
}

static size_t cancel_command_pull_queue_if_idle(void)
{
    if (s_command_queue == NULL || s_command_queue_mutation_lock == NULL ||
        command_router_has_active_peers()) {
        return 0U;
    }
    size_t cancelled = 0U;
    xSemaphoreTake(s_command_queue_mutation_lock, portMAX_DELAY);
    const UBaseType_t queued_count = uxQueueMessagesWaiting(s_command_queue);
    network_worker_work_item_t item = {0};
    for (UBaseType_t i = 0; i < queued_count; ++i) {
        if (xQueueReceive(s_command_queue, &item, 0) != pdTRUE) {
            break;
        }
        if (item.work_type == NETWORK_WORKER_WORK_COMMAND_PULL) {
            release_work_item(&item);
            ++cancelled;
        } else if (xQueueSend(s_command_queue, &item, 0) != pdTRUE) {
            release_work_item(&item);
            ++cancelled;
        }
        memset(&item, 0, sizeof(item));
    }
    xSemaphoreGive(s_command_queue_mutation_lock);
    return cancelled;
}

esp_err_t network_worker_release_peer_resources(const char *device_id)
{
    if (!gateway_config_child_allowed(device_id)) {
        return ESP_ERR_NOT_ALLOWED;
    }
    size_t upload_cancelled = cancel_peer_upload_queue(device_id);
    size_t command_cancelled = cancel_command_pull_queue_if_idle();
    ESP_LOGI(TAG,
             "peer network work released device_id=%s sensor_cancelled=%u command_pull_cancelled=%u",
             device_id,
             (unsigned int)upload_cancelled,
             (unsigned int)command_cancelled);
    return ESP_OK;
}

esp_err_t network_worker_restore_peer_resources(const char *device_id)
{
    if (!gateway_config_child_allowed(device_id)) {
        return ESP_ERR_NOT_ALLOWED;
    }
    network_replay_worker_request_bme_replay();
    return ESP_OK;
}

esp_err_t network_worker_enqueue_snapshot_upload(void)
{
    if (s_work_queue_mutation_lock == NULL || s_snapshot_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t timestamp_ms = now_ms();
    xSemaphoreTake(s_work_queue_mutation_lock, portMAX_DELAY);
    if (s_last_snapshot_enqueue_ms != 0 &&
        timestamp_ms - s_last_snapshot_enqueue_ms <
            (int64_t)UPLOAD_SNAPSHOT_INTERVAL_MS) {
        ++s_low_priority_coalesce_count;
        xSemaphoreGive(s_work_queue_mutation_lock);
        snapshot_stats_record_coalesce();
        return ESP_OK;
    }
    s_last_snapshot_enqueue_ms = timestamp_ms;
    if (s_snapshot_upload_pending) {
        ++s_low_priority_coalesce_count;
        xSemaphoreGive(s_work_queue_mutation_lock);
        snapshot_stats_record_coalesce();
        return ESP_OK;
    }
    s_snapshot_upload_pending = true;
    xSemaphoreGive(s_work_queue_mutation_lock);
    xTaskNotifyGive(s_snapshot_task);
    return ESP_OK;
}

esp_err_t network_worker_enqueue_command_pull(void)
{
    if (!command_router_has_active_peers()) {
        return ESP_ERR_INVALID_STATE;
    }
    network_worker_work_item_t item = {
        .work_type = NETWORK_WORKER_WORK_COMMAND_PULL,
    };
    strlcpy(item.source, "scheduler", sizeof(item.source));
    return enqueue_command_work_item(&item);
}

esp_err_t network_worker_enqueue_command_ack(const char *command_id, const char *ack_json)
{
    if (command_id == NULL || command_id[0] == '\0' || ack_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ack_len = strlen(ack_json);
    /* ack_json 可能来自调用方栈/临时 buffer，这里复制后再交给异步 command worker。 */
    char *owned_ack = cJSON_malloc(ack_len + 1U);
    if (owned_ack == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(owned_ack, ack_json, ack_len + 1U);

    network_worker_work_item_t item = {
        .work_type = NETWORK_WORKER_WORK_COMMAND_ACK,
        .json_body = owned_ack,
    };
    strlcpy(item.command_id, command_id, sizeof(item.command_id));
    strlcpy(item.source, "command_ack", sizeof(item.source));

    esp_err_t ret = enqueue_command_work_item_wait(&item, 0);
    if (ret != ESP_OK) {
        cJSON_free(owned_ack);
    }
    return ret;
}

esp_err_t network_worker_enqueue_smart_home_poll(void)
{
    network_worker_work_item_t item = {
        .work_type = NETWORK_WORKER_WORK_SMART_HOME_POLL,
    };
    strlcpy(item.source, "scheduler", sizeof(item.source));
    return enqueue_command_work_item(&item);
}

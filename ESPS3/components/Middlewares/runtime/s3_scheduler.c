/**
 * @file s3_scheduler.c
 * @brief ESPS3 统一运行时调度器和事件队列。
 *
 * 本文件属于 ESPS3 网关。它把 local_http_server、device_stream_gateway、
 * gateway_wifi/network_worker 产生的事件统一排队，再分发给 protocol worker、
 * stream worker 或周期 tick。S3 在这里做节奏控制和 worker 解耦；C5 只提交
 * /local/v1 或 UDP 轻量数据，ESP-server 上云仍由 server_client/network_worker 执行。
 */

#include "s3_scheduler.h"

#include <stdbool.h>
#include <string.h>

#include "app_stack_monitor.h"
#include "cJSON.h"
#include "child_registry.h"
#include "command_router.h"
#include "csi_placeholder_gateway.h"
#include "device_stream_gateway.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gateway_config.h"
#include "gateway_event_reporter.h"
#include "gateway_wifi.h"
#include "network_worker.h"
#include "offline_policy.h"
#include "protocol_adapter.h"
#include "sensor_aggregator.h"

static const char *TAG = "s3_scheduler";

#ifndef S3_SCHEDULER_EVENT_QUEUE_DEPTH
#define S3_SCHEDULER_EVENT_QUEUE_DEPTH 12U
#endif

#ifndef S3_SCHEDULER_TASK_STACK
#define S3_SCHEDULER_TASK_STACK 12288U
#endif

#ifndef S3_SCHEDULER_TASK_PRIORITY
#define S3_SCHEDULER_TASK_PRIORITY 4U
#endif

#ifndef S3_PROTOCOL_WORKER_QUEUE_DEPTH
#define S3_PROTOCOL_WORKER_QUEUE_DEPTH 12U
#endif

#ifndef S3_PROTOCOL_WORKER_TASK_STACK
#define S3_PROTOCOL_WORKER_TASK_STACK 8192U
#endif

#ifndef S3_PROTOCOL_WORKER_TASK_PRIORITY
#define S3_PROTOCOL_WORKER_TASK_PRIORITY 3U
#endif

#ifndef S3_STREAM_WORKER_QUEUE_DEPTH
#define S3_STREAM_WORKER_QUEUE_DEPTH 12U
#endif

#ifndef S3_STREAM_WORKER_TASK_STACK
#define S3_STREAM_WORKER_TASK_STACK 8192U
#endif

#ifndef S3_STREAM_WORKER_TASK_PRIORITY
#define S3_STREAM_WORKER_TASK_PRIORITY 3U
#endif

#ifndef S3_SCHEDULER_BASE_TICK_MS
#define S3_SCHEDULER_BASE_TICK_MS 100U
#endif

#ifndef S3_SCHEDULER_SOFT_WATERMARK
#define S3_SCHEDULER_SOFT_WATERMARK 5U
#endif

#ifndef S3_SCHEDULER_HARD_WATERMARK
#define S3_SCHEDULER_HARD_WATERMARK 9U
#endif

#ifndef S3_SCHEDULER_SMART_HOME_POLL_MS
#define S3_SCHEDULER_SMART_HOME_POLL_MS 2000U
#endif

#ifndef S3_SCHEDULER_DIAGNOSTIC_LOG_MS
#define S3_SCHEDULER_DIAGNOSTIC_LOG_MS 10000U
#endif

#ifndef S3_SCHEDULER_HEARTBEAT_LOG_MS
#define S3_SCHEDULER_HEARTBEAT_LOG_MS 30000U
#endif

typedef struct {
    bool valid;
    unified_msg_t latest;
    char device_id[S3_RUNTIME_BUS_DEVICE_ID_LEN];
} s3_runtime_device_state_t;

typedef struct {
    bool valid;
    unified_msg_t latest;
    char device_id[S3_RUNTIME_BUS_DEVICE_ID_LEN];
    char status[16];
    float filtered_v1;
    float filtered_v2;
    float filtered_v3;
} s3_runtime_sensor_state_t;

typedef struct {
    bool valid;
    unified_msg_t event;
    char command_id[S3_RUNTIME_BUS_COMMAND_ID_LEN];
} s3_runtime_event_state_t;

typedef enum {
    S3_STREAM_WORK_FRAME = 0,
    S3_STREAM_WORK_SEND,
} s3_stream_work_type_t;

typedef struct {
    s3_stream_work_type_t type;
    char peer_ip[S3_RUNTIME_BUS_PEER_IP_LEN];
    char source[24];
    uint16_t peer_port;
    size_t payload_len;
    uint8_t *payload;
} s3_stream_work_item_t;

static s3_scheduler_event_t *s_event_queue[S3_SCHEDULER_EVENT_QUEUE_DEPTH];
static size_t s_event_count;
static SemaphoreHandle_t s_event_lock;
static SemaphoreHandle_t s_event_signal;
static QueueHandle_t s_protocol_queue;
static QueueHandle_t s_stream_queue;
static TaskHandle_t s_scheduler_task;
static TaskHandle_t s_protocol_worker_task;
static TaskHandle_t s_stream_worker_task;

static s3_runtime_device_state_t s_device_registry[GATEWAY_CONFIG_MAX_CHILDREN];
static s3_runtime_sensor_state_t s_sensor_state[GATEWAY_CONFIG_MAX_CHILDREN];
static s3_runtime_event_state_t s_event_buffer[GATEWAY_CONFIG_COMMAND_QUEUE_SIZE];
static size_t s_runtime_event_cursor;

static s3_scheduler_network_state_t s_network_state = S3_SCHEDULER_NET_NOT_READY;
static bool s_voice_busy;
static int64_t s_last_csi_flush_ms;
static int64_t s_last_csi_trigger_ms;
static int64_t s_last_snapshot_upload_ms;
static int64_t s_last_smart_home_poll_ms;
static int64_t s_last_command_pull_ms;
static int64_t s_last_diagnostic_log_ms;
static int64_t s_last_heartbeat_log_ms;
static int64_t s_last_stack_monitor_ms;
static int64_t s_last_heap_monitor_ms;
static int64_t s_last_protocol_stack_monitor_ms;
static int64_t s_last_protocol_heap_monitor_ms;
static int64_t s_last_stream_stack_monitor_ms;
static int64_t s_last_stream_heap_monitor_ms;
static int64_t s_last_dispatch_warning_ms;

/* scheduler 使用本机 uptime 驱动 cadence；Server 时间只作为业务 payload 字段。 */
static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

const char *s3_scheduler_network_state_name(s3_scheduler_network_state_t state)
{
    switch (state) {
    case S3_SCHEDULER_NET_NOT_READY:
        return "NET_NOT_READY";
    case S3_SCHEDULER_STA_CONNECTED:
        return "STA_CONNECTED";
    case S3_SCHEDULER_IP_READY:
        return "IP_READY";
    case S3_SCHEDULER_LINK_STABLE:
        return "LINK_STABLE";
    default:
        return "UNKNOWN";
    }
}

static const char *event_type_name(s3_scheduler_event_type_t type)
{
    switch (type) {
    case S3_SCHEDULER_EVENT_INGRESS:
        return "ingress";
    case S3_SCHEDULER_EVENT_STREAM_FRAME:
        return "stream_frame";
    case S3_SCHEDULER_EVENT_STREAM_SEND:
        return "stream_send";
    case S3_SCHEDULER_EVENT_NETWORK_STATE:
        return "network_state";
    case S3_SCHEDULER_EVENT_VOICE_STATE:
        return "voice_state";
    case S3_SCHEDULER_EVENT_COMMAND_PULL:
        return "command_pull";
    case S3_SCHEDULER_EVENT_NONE:
    default:
        return "none";
    }
}

static const char *kind_name(s3_runtime_msg_kind_t kind)
{
    switch (kind) {
    case S3_RUNTIME_MSG_CSI:
        return "csi";
    case S3_RUNTIME_MSG_SENSOR:
        return "sensor";
    case S3_RUNTIME_MSG_STATUS:
        return "status";
    case S3_RUNTIME_MSG_EVENT:
        return "event";
    case S3_RUNTIME_MSG_UNKNOWN:
    default:
        return "unknown";
    }
}

static void unified_copy_text(char *out, size_t out_size, const char *value)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    out[0] = '\0';
    if (value != NULL) {
        strlcpy(out, value, out_size);
    }
}

static int runtime_slot_for_did(const char *did)
{
    if (did == NULL || did[0] == '\0') {
        return -1;
    }
    if (strcmp(did, "C51") == 0) {
        return 0;
    }
    if (strcmp(did, "C52") == 0) {
        return 1;
    }
    return -1;
}

static void update_runtime_state(const s3_runtime_ingress_t *ingress)
{
    if (ingress == NULL || ingress->unified.type[0] == '\0') {
        return;
    }

    int slot = runtime_slot_for_did(ingress->unified.did);
    if (ingress->kind == S3_RUNTIME_MSG_STATUS && slot >= 0 &&
        slot < (int)GATEWAY_CONFIG_MAX_CHILDREN) {
        /* status latest cache 用完整 device_id 保留身份，扁平值只服务 S3 诊断/快照。 */
        s3_runtime_device_state_t *state = &s_device_registry[slot];
        memset(state, 0, sizeof(*state));
        state->valid = true;
        state->latest = ingress->unified;
        strlcpy(state->device_id, ingress->device_id, sizeof(state->device_id));
        return;
    }

    if (ingress->kind == S3_RUNTIME_MSG_SENSOR && slot >= 0 &&
        slot < (int)GATEWAY_CONFIG_MAX_CHILDREN) {
        s3_runtime_sensor_state_t *state = &s_sensor_state[slot];
        const bool had_previous = state->valid;
        state->valid = true;
        state->latest = ingress->unified;
        strlcpy(state->device_id, ingress->device_id, sizeof(state->device_id));
        /* S3 只做 dashboard 侧轻量平滑；原始 sensor envelope 仍按 protocol_adapter 上云。 */
        state->filtered_v1 = had_previous ? (0.30f * ingress->unified.v1) +
                                                (0.70f * state->filtered_v1) :
                                            ingress->unified.v1;
        state->filtered_v2 = had_previous ? (0.30f * ingress->unified.v2) +
                                                (0.70f * state->filtered_v2) :
                                            ingress->unified.v2;
        state->filtered_v3 = had_previous ? (0.30f * ingress->unified.v3) +
                                                (0.70f * state->filtered_v3) :
                                            ingress->unified.v3;
        strlcpy(state->status,
                state->filtered_v3 >= 75.0f ? "good" :
                state->filtered_v3 >= 55.0f ? "moderate" :
                state->filtered_v3 > 0.0f ? "poor" : "unknown",
                sizeof(state->status));
        return;
    }

    if (ingress->kind == S3_RUNTIME_MSG_EVENT) {
        s3_runtime_event_state_t *event = &s_event_buffer[s_runtime_event_cursor];
        memset(event, 0, sizeof(*event));
        event->valid = true;
        event->event = ingress->unified;
        strlcpy(event->command_id, ingress->command_id, sizeof(event->command_id));
        s_runtime_event_cursor = (s_runtime_event_cursor + 1U) % GATEWAY_CONFIG_COMMAND_QUEUE_SIZE;
    }
}

static const char *json_string(cJSON *root, const char *key, const char *fallback)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(value) && value->valuestring != NULL ? value->valuestring : fallback;
}

static float json_float(cJSON *root, const char *key, float fallback)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(value) ? (float)value->valuedouble : fallback;
}

static float json_array_float(cJSON *root, int index, float fallback)
{
    cJSON *values = cJSON_GetObjectItemCaseSensitive(root, ESP111_PROTOCOL_LOCAL_JSON_VALUES);
    cJSON *value = cJSON_IsArray(values) ? cJSON_GetArrayItem(values, index) : NULL;
    return cJSON_IsNumber(value) ? (float)value->valuedouble : fallback;
}

static const char *short_device_id_for_local_id(uint8_t local_id)
{
    return local_id == ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52 ? "C52" : "C51";
}

static void fill_unified_from_envelope(s3_runtime_ingress_t *ingress,
                                       const protocol_adapter_envelope_t *envelope)
{
    if (ingress == NULL || envelope == NULL) {
        return;
    }

    unified_msg_t *msg = &ingress->unified;
    msg->t = envelope->uptime_ms > 0 ? envelope->uptime_ms : now_ms();
    unified_copy_text(msg->did,
                      sizeof(msg->did),
                      short_device_id_for_local_id(envelope->local_id));
    unified_copy_text(msg->type, sizeof(msg->type), kind_name(ingress->kind));
    unified_copy_text(ingress->device_id, sizeof(ingress->device_id), envelope->device_id);

    if (ingress->kind == S3_RUNTIME_MSG_CSI && envelope->payload != NULL) {
        /* unified CSI 只取 confidence/quality/rssi 做运行时摘要，raw/subcarrier 不进入 S3 cache。 */
        unified_copy_text(msg->lid,
                          sizeof(msg->lid),
                          json_string(envelope->payload, "link_id", "unknown"));
        msg->v1 = json_float(envelope->payload, "confidence", 0.0f);
        msg->v2 = json_float(envelope->payload, "quality", 0.0f);
        msg->v3 = (float)((int)json_float(envelope->payload, "rssi", 0.0f));
        return;
    }

    if (ingress->kind == S3_RUNTIME_MSG_SENSOR && envelope->payload != NULL) {
        unified_copy_text(msg->lid, sizeof(msg->lid), "bme690");
        msg->v1 = json_float(envelope->payload, "temperature_c", 0.0f);
        msg->v2 = json_float(envelope->payload, "humidity_percent", 0.0f);
        msg->v3 = json_float(envelope->payload, "air_quality_score", 0.0f);
        return;
    }

    if (ingress->kind == S3_RUNTIME_MSG_STATUS) {
        unified_copy_text(msg->lid, sizeof(msg->lid), "health");
        msg->v1 = envelope->has_wifi_rssi ? (float)envelope->wifi_rssi : 0.0f;
        msg->v2 = (float)heap_caps_get_free_size(MALLOC_CAP_8BIT);
        msg->v3 = (float)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        return;
    }

    if (ingress->kind == S3_RUNTIME_MSG_EVENT) {
        unified_copy_text(msg->lid, sizeof(msg->lid), "event");
    }
}

static void fill_unified_from_raw_body(s3_runtime_ingress_t *ingress)
{
    if (ingress == NULL || ingress->body[0] == '\0') {
        return;
    }

    cJSON *root = cJSON_ParseWithLength(ingress->body, ingress->body_len);
    if (root == NULL) {
        return;
    }

    cJSON *local_id_item =
        cJSON_GetObjectItemCaseSensitive(root, ESP111_PROTOCOL_LOCAL_JSON_ID);
    uint8_t local_id = cJSON_IsNumber(local_id_item) ? (uint8_t)local_id_item->valueint : 0U;
    unified_msg_t *msg = &ingress->unified;
    msg->t = (int64_t)json_float(root,
                                 ESP111_PROTOCOL_LOCAL_JSON_UPTIME_MS,
                                 (float)now_ms());
    unified_copy_text(msg->did,
                      sizeof(msg->did),
                      local_id != 0U ? short_device_id_for_local_id(local_id) : "");
    unified_copy_text(msg->type, sizeof(msg->type), kind_name(ingress->kind));

    if (ingress->kind == S3_RUNTIME_MSG_CSI) {
        /* 旧轻量 body 兼容路径；正式 v2 envelope 会在后续 parse_ingress_envelope 覆盖摘要。 */
        unified_copy_text(msg->lid,
                          sizeof(msg->lid),
                          json_string(root,
                                      "link_id",
                                      local_id == ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52 ?
                                          "S3_TO_C52" :
                                          "S3_TO_C51"));
        msg->v1 = json_float(root, "confidence", json_array_float(root, 0, 0.0f));
        msg->v2 = json_float(root, "quality", json_array_float(root, 1, 0.0f));
        msg->v3 = json_float(root, "rssi", json_array_float(root, 2, 0.0f));
    } else if (ingress->kind == S3_RUNTIME_MSG_SENSOR) {
        unified_copy_text(msg->lid, sizeof(msg->lid), "bme690");
        msg->v1 = json_array_float(root, 0, 0.0f);
        msg->v2 = json_array_float(root, 1, 0.0f);
        msg->v3 = json_array_float(root, 4, 0.0f);
    } else if (ingress->kind == S3_RUNTIME_MSG_STATUS) {
        unified_copy_text(msg->lid, sizeof(msg->lid), "health");
        msg->v1 = json_float(root, ESP111_PROTOCOL_LOCAL_JSON_WIFI_RSSI, 0.0f);
        msg->v2 = json_array_float(root, 3, (float)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        msg->v3 = json_array_float(root, 4, (float)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else if (ingress->kind == S3_RUNTIME_MSG_EVENT) {
        unified_copy_text(msg->lid, sizeof(msg->lid), "event");
        msg->v1 = json_float(root, ESP111_PROTOCOL_LOCAL_JSON_COMMAND_CODE, 0.0f);
    }

    cJSON_Delete(root);
}

static char *capabilities_to_string(const protocol_adapter_envelope_t *envelope)
{
    if (envelope == NULL || envelope->capabilities == NULL) {
        return NULL;
    }
    return cJSON_PrintUnformatted(envelope->capabilities);
}

static esp_err_t parse_ingress_envelope(const s3_runtime_ingress_t *ingress,
                                        protocol_adapter_envelope_t *envelope)
{
    if (ingress == NULL || envelope == NULL || ingress->body_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = protocol_adapter_parse_local_envelope(ingress->body,
                                                          ingress->body_len,
                                                          envelope);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = protocol_adapter_validate_local_envelope(envelope);
    if (ret != ESP_OK) {
        protocol_adapter_release_envelope(envelope);
    }
    return ret;
}

static esp_err_t handle_status_ingress(const s3_runtime_ingress_t *ingress)
{
    protocol_adapter_envelope_t envelope = {0};
    esp_err_t ret = parse_ingress_envelope(ingress, &envelope);
    if (ret != ESP_OK) {
        return ret;
    }

    if (ingress->peer_ip[0] != '\0') {
        (void)child_registry_update_peer_ip(envelope.device_id, ingress->peer_ip);
    }

    /* register/heartbeat/status 先维护 child_registry，再交给 sensor_aggregator 生成 Server payload。 */
    protocol_adapter_message_kind_t kind = protocol_adapter_message_kind(envelope.message_type);
    if (kind == PROTOCOL_ADAPTER_MESSAGE_REGISTER) {
        char *capabilities = capabilities_to_string(&envelope);
        ret = child_registry_register_or_update(envelope.device_id,
                                                envelope.room_id,
                                                envelope.alias,
                                                capabilities,
                                                envelope.seq);
        cJSON_free(capabilities);
    } else if (kind == PROTOCOL_ADAPTER_MESSAGE_HEARTBEAT ||
               kind == PROTOCOL_ADAPTER_MESSAGE_STATUS) {
        ret = child_registry_touch(envelope.device_id, envelope.seq);
    } else {
        ret = ESP_ERR_NOT_SUPPORTED;
    }

    if (ret == ESP_OK) {
        sensor_aggregator_result_t result = {0};
        ret = sensor_aggregator_handle_envelope(&envelope, &result);
    }

    protocol_adapter_release_envelope(&envelope);
    return ret;
}

static esp_err_t handle_sensor_ingress(const s3_runtime_ingress_t *ingress)
{
    protocol_adapter_envelope_t envelope = {0};
    esp_err_t ret = parse_ingress_envelope(ingress, &envelope);
    if (ret != ESP_OK) {
        return ret;
    }

    if (ingress->peer_ip[0] != '\0') {
        (void)child_registry_update_peer_ip(envelope.device_id, ingress->peer_ip);
    }
    (void)child_registry_touch(envelope.device_id, envelope.seq);

    sensor_aggregator_result_t result = {0};
    ret = sensor_aggregator_handle_envelope(&envelope, &result);

    protocol_adapter_release_envelope(&envelope);
    return ret;
}

static esp_err_t handle_csi_ingress(const s3_runtime_ingress_t *ingress)
{
    protocol_adapter_envelope_t envelope = {0};
    esp_err_t ret = parse_ingress_envelope(ingress, &envelope);
    if (ret != ESP_OK) {
        return ret;
    }

    if (ingress->peer_ip[0] != '\0') {
        (void)child_registry_update_peer_ip(envelope.device_id, ingress->peer_ip);
    }
    (void)child_registry_touch(envelope.device_id, envelope.seq);
    /* CSI 状态机只在 S3 csi_placeholder_gateway/csi_fusion 内运行。 */
    ret = csi_placeholder_gateway_handle_result(&envelope);

    protocol_adapter_release_envelope(&envelope);
    return ret;
}

static esp_err_t handle_event_ingress(const s3_runtime_ingress_t *ingress)
{
    if (ingress == NULL || ingress->command_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    update_runtime_state(ingress);
    return command_router_ack(ingress->command_id, ingress->body);
}

static void process_ingress(s3_runtime_ingress_t *ingress)
{
    if (ingress == NULL) {
        return;
    }

    /* 先填一份 best-effort 摘要用于失败日志；解析成 envelope 后再用 canonical 字段覆盖。 */
    fill_unified_from_raw_body(ingress);

    protocol_adapter_envelope_t envelope = {0};
    if ((ingress->kind == S3_RUNTIME_MSG_STATUS ||
         ingress->kind == S3_RUNTIME_MSG_SENSOR ||
         ingress->kind == S3_RUNTIME_MSG_CSI) &&
        parse_ingress_envelope(ingress, &envelope) == ESP_OK) {
        fill_unified_from_envelope(ingress, &envelope);
        protocol_adapter_release_envelope(&envelope);
    }

    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
    switch (ingress->kind) {
    case S3_RUNTIME_MSG_CSI:
        ret = handle_csi_ingress(ingress);
        break;
    case S3_RUNTIME_MSG_SENSOR:
        ret = handle_sensor_ingress(ingress);
        break;
    case S3_RUNTIME_MSG_STATUS:
        ret = handle_status_ingress(ingress);
        break;
    case S3_RUNTIME_MSG_EVENT:
        ret = handle_event_ingress(ingress);
        break;
    case S3_RUNTIME_MSG_UNKNOWN:
    default:
        ret = ESP_ERR_NOT_SUPPORTED;
        break;
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "runtime bus process failed type=%s did=%s lid=%s ret=%s",
                 ingress->unified.type,
                 ingress->unified.did,
                 ingress->unified.lid,
                 esp_err_to_name(ret));
        return;
    }

    if (ingress->kind != S3_RUNTIME_MSG_EVENT) {
        update_runtime_state(ingress);
    }

    ESP_LOGD(TAG,
             "runtime bus processed t=%lld did=%s type=%s lid=%s v1=%.3f v2=%.3f v3=%.3f",
             (long long)ingress->unified.t,
             ingress->unified.did,
             ingress->unified.type,
             ingress->unified.lid,
             (double)ingress->unified.v1,
             (double)ingress->unified.v2,
             (double)ingress->unified.v3);
}

static esp_err_t protocol_worker_enqueue_ingress(s3_runtime_ingress_t *ingress)
{
    if (ingress == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_protocol_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(s_protocol_queue, &ingress, 0) != pdTRUE) {
        ESP_LOGW(TAG,
                 "protocol worker queue full kind=%s depth=%u",
                 kind_name(ingress->kind),
                 (unsigned int)S3_PROTOCOL_WORKER_QUEUE_DEPTH);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void protocol_worker_task(void *arg)
{
    (void)arg;
    const bool wdt_registered = app_task_wdt_add_current(TAG, "protocol_worker");
    ESP_LOGI(TAG,
             "protocol worker started queue_depth=%u",
             (unsigned int)S3_PROTOCOL_WORKER_QUEUE_DEPTH);
    app_stack_monitor_log(TAG, "protocol_worker", "entry");
    app_heap_monitor_log(TAG);

    while (1) {
        s3_runtime_ingress_t *ingress = NULL;
        if (xQueueReceive(s_protocol_queue, &ingress, pdMS_TO_TICKS(S3_SCHEDULER_BASE_TICK_MS)) ==
            pdTRUE) {
            /* protocol worker 独立处理 JSON parse/Server mapping，避免 scheduler 主循环被长路径阻塞。 */
            process_ingress(ingress);
            heap_caps_free(ingress);
        }

        app_stack_monitor_log_periodic(TAG,
                                       "protocol_worker",
                                       &s_last_protocol_stack_monitor_ms,
                                       APP_STACK_MONITOR_INTERVAL_MS);
        app_heap_monitor_log_periodic(TAG,
                                      &s_last_protocol_heap_monitor_ms,
                                      APP_HEAP_MONITOR_INTERVAL_MS);
        app_task_wdt_reset_current(wdt_registered);
    }
}

static void release_stream_work_item(s3_stream_work_item_t *item)
{
    if (item == NULL) {
        return;
    }
    if (item->payload != NULL) {
        heap_caps_free(item->payload);
        item->payload = NULL;
    }
    item->payload_len = 0U;
}

static esp_err_t stream_worker_enqueue_event(s3_scheduler_event_t *event)
{
    if (event == NULL || s_stream_queue == NULL ||
        (event->type != S3_SCHEDULER_EVENT_STREAM_FRAME &&
         event->type != S3_SCHEDULER_EVENT_STREAM_SEND)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (event->payload == NULL || event->payload_len == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* 这里转移 payload 所有权；入队成功后 event_release 不再释放 payload。 */
    s3_stream_work_item_t item = {
        .type = event->type == S3_SCHEDULER_EVENT_STREAM_FRAME ?
                    S3_STREAM_WORK_FRAME :
                    S3_STREAM_WORK_SEND,
        .peer_port = event->peer_port,
        .payload_len = event->payload_len,
        .payload = event->payload,
    };
    strlcpy(item.peer_ip, event->peer_ip, sizeof(item.peer_ip));
    strlcpy(item.source, event->source, sizeof(item.source));

    if (xQueueSend(s_stream_queue, &item, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    event->payload = NULL;
    event->payload_len = 0U;
    return ESP_OK;
}

static void stream_worker_task(void *arg)
{
    (void)arg;
    const bool wdt_registered = app_task_wdt_add_current(TAG, "stream_worker");
    ESP_LOGI(TAG,
             "stream worker started queue_depth=%u",
             (unsigned int)S3_STREAM_WORKER_QUEUE_DEPTH);
    app_stack_monitor_log(TAG, "stream_worker", "entry");
    app_heap_monitor_log(TAG);

    while (1) {
        s3_stream_work_item_t item = {0};
        if (xQueueReceive(s_stream_queue, &item, pdMS_TO_TICKS(S3_SCHEDULER_BASE_TICK_MS)) ==
            pdTRUE) {
            esp_err_t ret = ESP_ERR_INVALID_ARG;
            /* UDP/HTTP stream parse 和 UDP send 都放到 worker，HTTP handler 只承担入队成本。 */
            if (item.type == S3_STREAM_WORK_FRAME) {
                ret = device_stream_gateway_process_json((const char *)item.payload,
                                                         item.payload_len,
                                                         item.peer_ip);
            } else if (item.type == S3_STREAM_WORK_SEND) {
                ret = device_stream_gateway_send_udp_now(item.peer_ip,
                                                         item.peer_port,
                                                         item.payload,
                                                         item.payload_len,
                                                         item.source);
            }
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_ARG &&
                ret != ESP_ERR_INVALID_STATE && ret != ESP_ERR_NOT_ALLOWED) {
                ESP_LOGW(TAG,
                         "stream worker work failed type=%d source=%s ret=%s",
                         (int)item.type,
                         item.source,
                         esp_err_to_name(ret));
            }
            release_stream_work_item(&item);
        }

        app_stack_monitor_log_periodic(TAG,
                                       "stream_worker",
                                       &s_last_stream_stack_monitor_ms,
                                       APP_STACK_MONITOR_INTERVAL_MS);
        app_heap_monitor_log_periodic(TAG,
                                      &s_last_stream_heap_monitor_ms,
                                      APP_HEAP_MONITOR_INTERVAL_MS);
        app_task_wdt_reset_current(wdt_registered);
    }
}

static s3_scheduler_event_t *event_alloc(s3_scheduler_event_type_t type,
                                         s3_scheduler_priority_t priority)
{
    if (type == S3_SCHEDULER_EVENT_NONE ||
        priority > S3_SCHEDULER_PRIORITY_LOW) {
        return NULL;
    }

    s3_scheduler_event_t *event = heap_caps_calloc(1, sizeof(*event), MALLOC_CAP_8BIT);
    if (event == NULL) {
        return NULL;
    }
    event->type = type;
    event->priority = priority;
    return event;
}

static void event_release(s3_scheduler_event_t *event)
{
    if (event == NULL) {
        return;
    }
    if (event->ingress != NULL) {
        heap_caps_free(event->ingress);
        event->ingress = NULL;
    }
    if (event->payload != NULL) {
        heap_caps_free(event->payload);
        event->payload = NULL;
    }
    heap_caps_free(event);
}

static esp_err_t queue_push_owned(s3_scheduler_event_t *event)
{
    if (event == NULL || event->type == S3_SCHEDULER_EVENT_NONE ||
        event->priority > S3_SCHEDULER_PRIORITY_LOW) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_event_lock == NULL || s_event_signal == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_event_lock, portMAX_DELAY);
    if (s_event_count >= S3_SCHEDULER_EVENT_QUEUE_DEPTH) {
        xSemaphoreGive(s_event_lock);
        ESP_LOGW(TAG,
                 "scheduler queue full type=%s priority=%d depth=%u",
                 event_type_name(event->type),
                 (int)event->priority,
                 (unsigned int)S3_SCHEDULER_EVENT_QUEUE_DEPTH);
        return ESP_ERR_TIMEOUT;
    }

    /* 事件队列是拥有型队列；push 成功后释放责任转移给 scheduler task。 */
    s_event_queue[s_event_count] = event;
    ++s_event_count;
    xSemaphoreGive(s_event_lock);
    xSemaphoreGive(s_event_signal);
    return ESP_OK;
}

static bool queue_dequeue_next(s3_scheduler_event_t **out)
{
    if (out == NULL || s_event_lock == NULL) {
        return false;
    }
    *out = NULL;

    xSemaphoreTake(s_event_lock, portMAX_DELAY);
    if (s_event_count == 0U) {
        xSemaphoreGive(s_event_lock);
        return false;
    }

    /* 队列很短，线性扫描可读性更好；priority 数字越小优先级越高。 */
    size_t best_index = 0U;
    s3_scheduler_priority_t best_priority = s_event_queue[0]->priority;
    for (size_t i = 1U; i < s_event_count; ++i) {
        if (s_event_queue[i]->priority < best_priority) {
            best_priority = s_event_queue[i]->priority;
            best_index = i;
        }
    }

    *out = s_event_queue[best_index];
    for (size_t i = best_index + 1U; i < s_event_count; ++i) {
        s_event_queue[i - 1U] = s_event_queue[i];
    }
    --s_event_count;
    s_event_queue[s_event_count] = NULL;
    xSemaphoreGive(s_event_lock);
    return true;
}

static size_t queue_depth_for_priority(s3_scheduler_priority_t priority)
{
    size_t count = 0U;
    if (s_event_lock == NULL) {
        return 0U;
    }
    xSemaphoreTake(s_event_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_event_count; ++i) {
        if (s_event_queue[i] != NULL && s_event_queue[i]->priority == priority) {
            ++count;
        }
    }
    xSemaphoreGive(s_event_lock);
    return count;
}

static size_t queue_depth(void)
{
    size_t count = 0U;
    if (s_event_lock == NULL) {
        return 0U;
    }
    xSemaphoreTake(s_event_lock, portMAX_DELAY);
    count = s_event_count;
    xSemaphoreGive(s_event_lock);
    return count;
}

static uint32_t load_multiplier(size_t depth)
{
    uint32_t multiplier = 1U;
    if (depth >= S3_SCHEDULER_SOFT_WATERMARK || s_voice_busy) {
        multiplier = 3U;
    }
    if (depth >= S3_SCHEDULER_HARD_WATERMARK) {
        multiplier = 6U;
    }
    return multiplier;
}

static uint32_t adjusted_interval(uint32_t base_ms, uint32_t multiplier)
{
    if (base_ms == 0U) {
        base_ms = S3_SCHEDULER_BASE_TICK_MS;
    }
    return base_ms * multiplier;
}

static bool due(int64_t now, int64_t last, uint32_t interval_ms)
{
    return last == 0 || now - last >= (int64_t)interval_ms;
}

static void enqueue_command_pull_if_needed(void)
{
    esp_err_t ret = network_worker_enqueue_command_pull();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "command pull worker enqueue failed ret=%s", esp_err_to_name(ret));
    }
}

static void log_gateway_heartbeat(void)
{
    ESP_LOGI(TAG,
             "gateway heartbeat net=%s softap=%d sta=%d server=%d voice_busy=%d queue=%u free_heap=%u psram_heap=%u last_error=%s",
             s3_scheduler_network_state_name(s_network_state),
             gateway_wifi_is_softap_ready() ? 1 : 0,
             gateway_wifi_is_sta_connected() ? 1 : 0,
             offline_policy_server_available() ? 1 : 0,
             s_voice_busy ? 1 : 0,
             (unsigned int)queue_depth(),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             offline_policy_last_error_code());
}

static void log_dispatch_failure(const s3_scheduler_event_t *event, esp_err_t ret)
{
    if (event == NULL || ret == ESP_OK) {
        return;
    }

    const int64_t timestamp_ms = now_ms();
    const bool force_warning =
        ret == ESP_ERR_NO_MEM || ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_INVALID_STATE;
    if (force_warning &&
        (s_last_dispatch_warning_ms == 0 ||
         timestamp_ms - s_last_dispatch_warning_ms >= S3_SCHEDULER_DIAGNOSTIC_LOG_MS)) {
        s_last_dispatch_warning_ms = timestamp_ms;
        ESP_LOGW(TAG,
                 "event dispatch failed type=%s priority=%d ret=%s queue=%u",
                 event_type_name(event->type),
                 (int)event->priority,
                 esp_err_to_name(ret),
                 (unsigned int)queue_depth());
        return;
    }

    ESP_LOGD(TAG,
             "event dispatch failed type=%s priority=%d ret=%s queue=%u",
             event_type_name(event->type),
             (int)event->priority,
             esp_err_to_name(ret),
             (unsigned int)queue_depth());
}

static void process_event(s3_scheduler_event_t *event)
{
    if (event == NULL) {
        return;
    }

    esp_err_t ret = ESP_OK;
    switch (event->type) {
    case S3_SCHEDULER_EVENT_INGRESS:
        /* ingress 转交 protocol worker；成功后 event 不再持有 ingress。 */
        ret = protocol_worker_enqueue_ingress(event->ingress);
        if (ret == ESP_OK) {
            event->ingress = NULL;
        }
        break;
    case S3_SCHEDULER_EVENT_STREAM_FRAME:
    case S3_SCHEDULER_EVENT_STREAM_SEND:
        ret = stream_worker_enqueue_event(event);
        break;
    case S3_SCHEDULER_EVENT_NETWORK_STATE:
        s3_scheduler_set_network_state(event->network_state);
        break;
    case S3_SCHEDULER_EVENT_VOICE_STATE:
        s3_scheduler_set_voice_busy(event->voice_busy);
        break;
    case S3_SCHEDULER_EVENT_COMMAND_PULL:
        ret = network_worker_enqueue_command_pull();
        break;
    case S3_SCHEDULER_EVENT_NONE:
    default:
        ret = ESP_ERR_INVALID_ARG;
        break;
    }

    log_dispatch_failure(event, ret);
}

void s3_scheduler_tick(void)
{
    const int64_t timestamp_ms = now_ms();
    const size_t depth = queue_depth();
    const uint32_t multiplier = load_multiplier(depth);
    const uint32_t csi_interval_ms = adjusted_interval(CSI_FUSION_TICK_MS, multiplier);
    const uint32_t upload_interval_ms =
        adjusted_interval(gateway_config_get()->sensor_forward_period_ms, multiplier);
    const uint32_t smart_home_interval_ms =
        adjusted_interval(S3_SCHEDULER_SMART_HOME_POLL_MS, multiplier);
    const bool local_net_ready = gateway_wifi_is_net_ready();
    const bool server_allowed = s3_scheduler_is_server_upload_allowed();

    app_stack_monitor_log_periodic(TAG,
                                   "s3_scheduler_task",
                                   &s_last_stack_monitor_ms,
                                   APP_STACK_MONITOR_INTERVAL_MS);
    app_heap_monitor_log_periodic(TAG, &s_last_heap_monitor_ms, APP_HEAP_MONITOR_INTERVAL_MS);

    /*
     * 本地 CSI trigger 只需要 C5<->S3 网络 ready；上云 snapshot/command/smart-home 必须等
     * link stable，并且 voice_busy 时暂停，避免语音代理长连接被周期上云挤占。
     */
    if (local_net_ready && due(timestamp_ms, s_last_csi_trigger_ms,
                              adjusted_interval(gateway_config_get()->csi_trigger_interval_ms,
                                                multiplier))) {
        s_last_csi_trigger_ms = timestamp_ms;
        if (!s_voice_busy && depth < S3_SCHEDULER_HARD_WATERMARK) {
            (void)csi_placeholder_gateway_send_triggers();
        }
    }

    if (due(timestamp_ms, s_last_csi_flush_ms, csi_interval_ms)) {
        s_last_csi_flush_ms = timestamp_ms;
        (void)csi_placeholder_gateway_flush_fusion();
    }

    if (server_allowed && !s_voice_busy &&
        due(timestamp_ms, s_last_snapshot_upload_ms, upload_interval_ms)) {
        s_last_snapshot_upload_ms = timestamp_ms;
        esp_err_t ret = network_worker_enqueue_snapshot_upload();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "snapshot upload worker enqueue failed ret=%s", esp_err_to_name(ret));
        }
    }

    if (server_allowed && !s_voice_busy &&
        due(timestamp_ms, s_last_command_pull_ms, upload_interval_ms)) {
        s_last_command_pull_ms = timestamp_ms;
        enqueue_command_pull_if_needed();
    }

    if (server_allowed && !s_voice_busy &&
        due(timestamp_ms, s_last_smart_home_poll_ms, smart_home_interval_ms)) {
        s_last_smart_home_poll_ms = timestamp_ms;
        esp_err_t ret = network_worker_enqueue_smart_home_poll();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "smart-home poll worker enqueue failed ret=%s", esp_err_to_name(ret));
        }
    }

    if (due(timestamp_ms, s_last_diagnostic_log_ms,
            adjusted_interval(S3_SCHEDULER_DIAGNOSTIC_LOG_MS, multiplier))) {
        s_last_diagnostic_log_ms = timestamp_ms;
        csi_placeholder_gateway_log_latest_diagnostics();
    }

    if (due(timestamp_ms, s_last_heartbeat_log_ms, S3_SCHEDULER_HEARTBEAT_LOG_MS)) {
        s_last_heartbeat_log_ms = timestamp_ms;
        log_gateway_heartbeat();
    }
}

static void s3_scheduler_task(void *arg)
{
    (void)arg;
    const bool wdt_registered = app_task_wdt_add_current(TAG, "s3_scheduler_task");
    ESP_LOGI(TAG,
             "S3 scheduler started queue_depth=%u base_tick_ms=%u",
             (unsigned int)S3_SCHEDULER_EVENT_QUEUE_DEPTH,
             (unsigned int)S3_SCHEDULER_BASE_TICK_MS);
    app_stack_monitor_log(TAG, "s3_scheduler_task", "entry");
    app_heap_monitor_log(TAG);

    int64_t last_tick_ms = 0;
    while (1) {
        if (xSemaphoreTake(s_event_signal, pdMS_TO_TICKS(S3_SCHEDULER_BASE_TICK_MS)) ==
            pdTRUE) {
            s3_scheduler_event_t *event = NULL;
            if (queue_dequeue_next(&event)) {
                process_event(event);
                event_release(event);
            }
        }

        int64_t timestamp_ms = now_ms();
        if (last_tick_ms == 0 ||
            timestamp_ms - last_tick_ms >= (int64_t)S3_SCHEDULER_BASE_TICK_MS) {
            last_tick_ms = timestamp_ms;
            s3_scheduler_tick();
        }
        app_task_wdt_reset_current(wdt_registered);
    }
}

esp_err_t s3_scheduler_init(void)
{
    if (s_event_lock == NULL) {
        s_event_lock = xSemaphoreCreateMutex();
        if (s_event_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_event_signal == NULL) {
        s_event_signal = xSemaphoreCreateCounting(S3_SCHEDULER_EVENT_QUEUE_DEPTH, 0U);
        if (s_event_signal == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_protocol_queue == NULL) {
        s_protocol_queue = xQueueCreate(S3_PROTOCOL_WORKER_QUEUE_DEPTH,
                                        sizeof(s3_runtime_ingress_t *));
        if (s_protocol_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_stream_queue == NULL) {
        s_stream_queue = xQueueCreate(S3_STREAM_WORKER_QUEUE_DEPTH,
                                      sizeof(s3_stream_work_item_t));
        if (s_stream_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    /* init 可用于错误恢复：清掉旧队列对象，避免上一轮未处理事件重复执行。 */
    for (size_t i = 0; i < s_event_count && i < S3_SCHEDULER_EVENT_QUEUE_DEPTH; ++i) {
        event_release(s_event_queue[i]);
        s_event_queue[i] = NULL;
    }
    s3_runtime_ingress_t *stale_ingress = NULL;
    while (xQueueReceive(s_protocol_queue, &stale_ingress, 0) == pdTRUE) {
        heap_caps_free(stale_ingress);
        stale_ingress = NULL;
    }
    s3_stream_work_item_t stale_stream_item = {0};
    while (xQueueReceive(s_stream_queue, &stale_stream_item, 0) == pdTRUE) {
        release_stream_work_item(&stale_stream_item);
        memset(&stale_stream_item, 0, sizeof(stale_stream_item));
    }
    memset(s_event_queue, 0, sizeof(s_event_queue));
    s_event_count = 0U;
    s_network_state = S3_SCHEDULER_NET_NOT_READY;
    s_voice_busy = false;
    return ESP_OK;
}

esp_err_t s3_scheduler_start(void)
{
    if (s_event_lock == NULL || s_event_signal == NULL ||
        s_protocol_queue == NULL || s_stream_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_protocol_worker_task == NULL) {
        BaseType_t created = xTaskCreate(protocol_worker_task,
                                         "protocol_worker",
                                         S3_PROTOCOL_WORKER_TASK_STACK,
                                         NULL,
                                         S3_PROTOCOL_WORKER_TASK_PRIORITY,
                                         &s_protocol_worker_task);
        if (created != pdPASS) {
            s_protocol_worker_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_stream_worker_task == NULL) {
        BaseType_t created = xTaskCreate(stream_worker_task,
                                         "stream_worker",
                                         S3_STREAM_WORKER_TASK_STACK,
                                         NULL,
                                         S3_STREAM_WORKER_TASK_PRIORITY,
                                         &s_stream_worker_task);
        if (created != pdPASS) {
            s_stream_worker_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_scheduler_task != NULL) {
        return ESP_OK;
    }

    BaseType_t created = xTaskCreate(s3_scheduler_task,
                                     "s3_scheduler",
                                     S3_SCHEDULER_TASK_STACK,
                                     NULL,
                                     S3_SCHEDULER_TASK_PRIORITY,
                                     &s_scheduler_task);
    if (created != pdPASS) {
        s_scheduler_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t s3_scheduler_enqueue_event(const s3_scheduler_event_t *event)
{
    if (event == NULL || event->type == S3_SCHEDULER_EVENT_NONE ||
        event->priority > S3_SCHEDULER_PRIORITY_LOW) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 对外 API 统一拷贝异步数据，调用方可以在返回后释放自己的临时 buffer。 */
    s3_scheduler_event_t *owned = event_alloc(event->type, event->priority);
    if (owned == NULL) {
        return ESP_ERR_NO_MEM;
    }
    owned->network_state = event->network_state;
    owned->voice_busy = event->voice_busy;
    owned->peer_port = event->peer_port;
    owned->payload_len = event->payload_len;
    strlcpy(owned->peer_ip, event->peer_ip, sizeof(owned->peer_ip));
    strlcpy(owned->source, event->source, sizeof(owned->source));

    if (event->type == S3_SCHEDULER_EVENT_INGRESS) {
        if (event->ingress == NULL ||
            event->ingress->kind == S3_RUNTIME_MSG_UNKNOWN ||
            event->ingress->body_len > S3_RUNTIME_BUS_BODY_MAX) {
            event_release(owned);
            return ESP_ERR_INVALID_ARG;
        }
        owned->ingress = heap_caps_calloc(1, sizeof(*owned->ingress), MALLOC_CAP_8BIT);
        if (owned->ingress == NULL) {
            event_release(owned);
            return ESP_ERR_NO_MEM;
        }
        *owned->ingress = *event->ingress;
        owned->ingress->body[owned->ingress->body_len] = '\0';
    }
    if ((event->type == S3_SCHEDULER_EVENT_STREAM_FRAME ||
         event->type == S3_SCHEDULER_EVENT_STREAM_SEND) &&
        (event->payload == NULL || event->payload_len == 0U ||
         event->payload_len > ESP111_PROTOCOL_DEVICE_STREAM_MAX_BYTES)) {
        event_release(owned);
        return ESP_ERR_INVALID_SIZE;
    }
    if (event->type == S3_SCHEDULER_EVENT_STREAM_FRAME ||
        event->type == S3_SCHEDULER_EVENT_STREAM_SEND) {
        owned->payload = heap_caps_malloc(event->payload_len + 1U, MALLOC_CAP_8BIT);
        if (owned->payload == NULL) {
            event_release(owned);
            return ESP_ERR_NO_MEM;
        }
        memcpy(owned->payload, event->payload, event->payload_len);
        owned->payload[event->payload_len] = '\0';
    }

    if (owned->ingress != NULL) {
        if (owned->ingress->unified.t <= 0) {
            owned->ingress->unified.t = now_ms();
        }
        if (owned->ingress->unified.type[0] == '\0') {
            unified_copy_text(owned->ingress->unified.type,
                              sizeof(owned->ingress->unified.type),
                              kind_name(owned->ingress->kind));
        }
    }

    esp_err_t ret = queue_push_owned(owned);
    if (ret != ESP_OK) {
        event_release(owned);
    }
    return ret;
}

esp_err_t s3_scheduler_enqueue_ingress(const s3_runtime_ingress_t *ingress,
                                       s3_scheduler_priority_t priority)
{
    if (ingress == NULL || priority > S3_SCHEDULER_PRIORITY_LOW ||
        ingress->kind == S3_RUNTIME_MSG_UNKNOWN ||
        ingress->body_len > S3_RUNTIME_BUS_BODY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    s3_runtime_ingress_t *owned_ingress =
        heap_caps_calloc(1, sizeof(*owned_ingress), MALLOC_CAP_8BIT);
    if (owned_ingress == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *owned_ingress = *ingress;
    return s3_scheduler_enqueue_ingress_owned(owned_ingress, priority);
}

esp_err_t s3_scheduler_enqueue_ingress_owned(s3_runtime_ingress_t *ingress,
                                             s3_scheduler_priority_t priority)
{
    if (ingress == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (priority > S3_SCHEDULER_PRIORITY_LOW ||
        ingress->kind == S3_RUNTIME_MSG_UNKNOWN ||
        ingress->body_len > S3_RUNTIME_BUS_BODY_MAX) {
        heap_caps_free(ingress);
        return ESP_ERR_INVALID_ARG;
    }

    /* ingress 已由调用方分配；本函数负责补终止符并接管生命周期。 */
    ingress->body[ingress->body_len] = '\0';
    if (ingress->unified.t <= 0) {
        ingress->unified.t = now_ms();
    }
    if (ingress->unified.type[0] == '\0') {
        unified_copy_text(ingress->unified.type,
                          sizeof(ingress->unified.type),
                          kind_name(ingress->kind));
    }

    s3_scheduler_event_t *event =
        event_alloc(S3_SCHEDULER_EVENT_INGRESS, priority);
    if (event == NULL) {
        heap_caps_free(ingress);
        return ESP_ERR_NO_MEM;
    }
    event->ingress = ingress;

    esp_err_t ret = queue_push_owned(event);
    if (ret != ESP_OK) {
        event_release(event);
    }
    return ret;
}

esp_err_t s3_scheduler_enqueue_network_state(s3_scheduler_network_state_t state)
{
    if (state > S3_SCHEDULER_LINK_STABLE) {
        return ESP_ERR_INVALID_ARG;
    }

    s3_scheduler_event_t *event =
        event_alloc(S3_SCHEDULER_EVENT_NETWORK_STATE, S3_SCHEDULER_PRIORITY_HIGH);
    if (event == NULL) {
        return ESP_ERR_NO_MEM;
    }
    event->network_state = state;

    esp_err_t ret = queue_push_owned(event);
    if (ret != ESP_OK) {
        event_release(event);
    }
    return ret;
}

esp_err_t s3_scheduler_enqueue_stream_frame(const char *json,
                                            size_t json_len,
                                            const char *peer_ip,
                                            const char *source)
{
    if (json == NULL || json_len == 0U ||
        json_len > ESP111_PROTOCOL_DEVICE_STREAM_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (peer_ip != NULL && strlen(peer_ip) >= S3_RUNTIME_BUS_PEER_IP_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    s3_scheduler_event_t *event =
        event_alloc(S3_SCHEDULER_EVENT_STREAM_FRAME, S3_SCHEDULER_PRIORITY_NORMAL);
    if (event == NULL) {
        return ESP_ERR_NO_MEM;
    }
    event->payload = heap_caps_malloc(json_len + 1U, MALLOC_CAP_8BIT);
    if (event->payload == NULL) {
        event_release(event);
        return ESP_ERR_NO_MEM;
    }
    event->payload_len = json_len;
    memcpy(event->payload, json, json_len);
    event->payload[json_len] = '\0';
    strlcpy(event->peer_ip, peer_ip != NULL ? peer_ip : "", sizeof(event->peer_ip));
    strlcpy(event->source, source != NULL ? source : "stream", sizeof(event->source));

    esp_err_t ret = queue_push_owned(event);
    if (ret != ESP_OK) {
        event_release(event);
    }
    return ret;
}

esp_err_t s3_scheduler_enqueue_stream_send(const char *peer_ip,
                                           uint16_t peer_port,
                                           const void *payload,
                                           size_t payload_len,
                                           const char *source)
{
    if (peer_ip == NULL || peer_ip[0] == '\0' ||
        strlen(peer_ip) >= S3_RUNTIME_BUS_PEER_IP_LEN ||
        peer_port == 0 || payload == NULL || payload_len == 0U ||
        payload_len > ESP111_PROTOCOL_DEVICE_STREAM_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    s3_scheduler_event_t *event =
        event_alloc(S3_SCHEDULER_EVENT_STREAM_SEND, S3_SCHEDULER_PRIORITY_LOW);
    if (event == NULL) {
        return ESP_ERR_NO_MEM;
    }
    event->payload = heap_caps_malloc(payload_len, MALLOC_CAP_8BIT);
    if (event->payload == NULL) {
        event_release(event);
        return ESP_ERR_NO_MEM;
    }
    event->peer_port = peer_port;
    event->payload_len = payload_len;
    strlcpy(event->peer_ip, peer_ip, sizeof(event->peer_ip));
    strlcpy(event->source, source != NULL ? source : "unknown", sizeof(event->source));
    memcpy(event->payload, payload, payload_len);

    esp_err_t ret = queue_push_owned(event);
    if (ret != ESP_OK) {
        event_release(event);
    }
    return ret;
}

esp_err_t s3_scheduler_enqueue_command_pull(void)
{
    s3_scheduler_event_t *event =
        event_alloc(S3_SCHEDULER_EVENT_COMMAND_PULL, S3_SCHEDULER_PRIORITY_LOW);
    if (event == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = queue_push_owned(event);
    if (ret != ESP_OK) {
        event_release(event);
    }
    return ret;
}

s3_scheduler_load_t s3_scheduler_get_load(void)
{
    const size_t depth = queue_depth();
    const uint32_t multiplier = load_multiplier(depth);
    s3_scheduler_load_t load = {
        .queue_depth = depth,
        .high_depth = queue_depth_for_priority(S3_SCHEDULER_PRIORITY_HIGH),
        .normal_depth = queue_depth_for_priority(S3_SCHEDULER_PRIORITY_NORMAL),
        .low_depth = queue_depth_for_priority(S3_SCHEDULER_PRIORITY_LOW),
        .network_state = s_network_state,
        .voice_busy = s_voice_busy,
        .csi_interval_ms = adjusted_interval(CSI_FUSION_TICK_MS, multiplier),
        .upload_interval_ms =
            adjusted_interval(gateway_config_get()->sensor_forward_period_ms, multiplier),
        .smart_home_interval_ms =
            adjusted_interval(S3_SCHEDULER_SMART_HOME_POLL_MS, multiplier),
    };
    return load;
}

void s3_scheduler_set_voice_busy(bool busy)
{
    s_voice_busy = busy;
}

bool s3_scheduler_is_voice_busy(void)
{
    return s_voice_busy;
}

void s3_scheduler_set_network_state(s3_scheduler_network_state_t state)
{
    if (state > S3_SCHEDULER_LINK_STABLE) {
        state = S3_SCHEDULER_NET_NOT_READY;
    }
    if (s_network_state == state) {
        return;
    }

    ESP_LOGI(TAG,
             "scheduler network transition %s -> %s",
             s3_scheduler_network_state_name(s_network_state),
             s3_scheduler_network_state_name(state));
    s_network_state = state;
}

s3_scheduler_network_state_t s3_scheduler_get_network_state(void)
{
    return s_network_state;
}

bool s3_scheduler_is_net_ready(void)
{
    return s_network_state == S3_SCHEDULER_STA_CONNECTED ||
           s_network_state == S3_SCHEDULER_IP_READY ||
           s_network_state == S3_SCHEDULER_LINK_STABLE;
}

bool s3_scheduler_is_sta_connected(void)
{
    return s_network_state == S3_SCHEDULER_STA_CONNECTED ||
           s_network_state == S3_SCHEDULER_IP_READY ||
           s_network_state == S3_SCHEDULER_LINK_STABLE;
}

bool s3_scheduler_is_server_upload_allowed(void)
{
    return s_network_state == S3_SCHEDULER_LINK_STABLE;
}

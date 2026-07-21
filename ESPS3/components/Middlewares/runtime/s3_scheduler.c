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
#include "resource_manager.h"
#include "sensor_aggregator.h"

static const char *TAG = "s3_scheduler";

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
#define S3_PROTOCOL_WORKER_TASK_STACK 10240U
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

#ifndef S3_CSI_FUSION_WORKER_QUEUE_DEPTH
#define S3_CSI_FUSION_WORKER_QUEUE_DEPTH 16U
#endif

#ifndef S3_CSI_FUSION_WORKER_TASK_STACK
#define S3_CSI_FUSION_WORKER_TASK_STACK 12288U
#endif

#ifndef S3_CSI_FUSION_WORKER_TASK_PRIORITY
#define S3_CSI_FUSION_WORKER_TASK_PRIORITY 3U
#endif

#ifndef S3_CSI_FUSION_WORKER_BUDGET
#define S3_CSI_FUSION_WORKER_BUDGET 12U
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
#define S3_SCHEDULER_SMART_HOME_POLL_MS 10000U
#endif

#ifndef S3_SCHEDULER_COMMAND_PULL_MIN_MS
#define S3_SCHEDULER_COMMAND_PULL_MIN_MS 10000U
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
    S3_CSI_FUSION_WORK_INGRESS = 0,
    S3_CSI_FUSION_WORK_FLUSH,
} s3_csi_fusion_work_type_t;

typedef struct {
    s3_csi_fusion_work_type_t type;
    s3_runtime_ingress_t *ingress;
} s3_csi_fusion_work_item_t;

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

static QueueHandle_t s_protocol_queue;
static QueueHandle_t s_csi_fusion_queue;
static QueueHandle_t s_stream_queue;
static SemaphoreHandle_t s_csi_fusion_queue_lock;
static TaskHandle_t s_scheduler_task;
static TaskHandle_t s_protocol_worker_task;
static TaskHandle_t s_csi_fusion_worker_task;
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
static int64_t s_last_csi_fusion_stack_monitor_ms;
static int64_t s_last_csi_fusion_heap_monitor_ms;
static int64_t s_last_stream_stack_monitor_ms;
static int64_t s_last_stream_heap_monitor_ms;
static int64_t s_last_dispatch_warning_ms;
static bool s_pending_csi_fusion_flush;
static bool s_csi_fusion_flush_queued;
static bool s_pending_command_pull;
static uint32_t s_csi_ingress_drop_count;
static uint32_t s_csi_ingress_coalesce_count;
static uint32_t s_csi_worker_yield_count;

static s3_scheduler_event_t *event_alloc(s3_scheduler_event_type_t type,
                                         s3_scheduler_priority_t priority);
static void event_release(s3_scheduler_event_t *event);
static esp_err_t queue_push_owned(s3_scheduler_event_t *event);
static esp_err_t queue_push_reliable_owned(s3_scheduler_event_t *event);
static esp_err_t queue_push_timed_owned(s3_scheduler_event_t *event,
                                        uint32_t event_bus_lock_timeout_ms,
                                        s3_scheduler_enqueue_diagnostics_t *diagnostics);

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
    case S3_SCHEDULER_EVENT_CSI_FUSION_FLUSH:
        return "csi_fusion_flush";
    case S3_SCHEDULER_EVENT_BACKGROUND_STATS:
        return "background_stats";
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

static bool ingress_is_c51(const s3_runtime_ingress_t *ingress)
{
    if (ingress == NULL) {
        return false;
    }
    return strcmp(ingress->unified.did, "C51") == 0 ||
           strcmp(ingress->device_id, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51) == 0;
}

static bool ingress_is_c52(const s3_runtime_ingress_t *ingress)
{
    if (ingress == NULL) {
        return false;
    }
    return strcmp(ingress->unified.did, "C52") == 0 ||
           strcmp(ingress->device_id, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52) == 0;
}

static bool ingress_is_deprecated_stream_csi(const s3_runtime_ingress_t *ingress)
{
    return ingress != NULL && ingress->is_stream_frame &&
           ingress->kind == S3_RUNTIME_MSG_CSI;
}

static void log_deprecated_stream_csi_reject(const s3_runtime_ingress_t *ingress,
                                             const char *source)
{
    ESP_LOGW(TAG,
             "deprecated stream CSI rejected source=%s did=%s device_id=%s link_id=%s",
             source != NULL ? source : "scheduler",
             ingress != NULL ? ingress->unified.did : "-",
             ingress != NULL ? ingress->device_id : "-",
             ingress != NULL ? ingress->unified.lid : "-");
}

static s3_event_bus_state_key_t state_key_for_ingress(const s3_runtime_ingress_t *ingress)
{
    if (ingress == NULL) {
        return S3_EVENT_BUS_STATE_NONE;
    }

    switch (ingress->kind) {
    case S3_RUNTIME_MSG_CSI:
        if (ingress_is_c51(ingress)) {
            return S3_EVENT_BUS_STATE_CSI_LATEST_C51;
        }
        if (ingress_is_c52(ingress)) {
            return S3_EVENT_BUS_STATE_CSI_LATEST_C52;
        }
        return S3_EVENT_BUS_STATE_NONE;
    case S3_RUNTIME_MSG_SENSOR:
        if (ingress_is_c51(ingress)) {
            return S3_EVENT_BUS_STATE_BME_LATEST_C51;
        }
        if (ingress_is_c52(ingress)) {
            return S3_EVENT_BUS_STATE_BME_LATEST_C52;
        }
        return S3_EVENT_BUS_STATE_NONE;
    case S3_RUNTIME_MSG_STATUS:
        if (ingress_is_c51(ingress)) {
            return S3_EVENT_BUS_STATE_DEVICE_STATUS_C51;
        }
        if (ingress_is_c52(ingress)) {
            return S3_EVENT_BUS_STATE_DEVICE_STATUS_C52;
        }
        return S3_EVENT_BUS_STATE_NONE;
    case S3_RUNTIME_MSG_UNKNOWN:
    case S3_RUNTIME_MSG_EVENT:
    default:
        return S3_EVENT_BUS_STATE_NONE;
    }
}

static s3_event_bus_level_t bus_level_for_event(const s3_scheduler_event_t *event)
{
    if (event == NULL) {
        return S3_EVENT_BUS_LEVEL_BACKGROUND;
    }

    switch (event->type) {
    case S3_SCHEDULER_EVENT_NETWORK_STATE:
        return S3_EVENT_BUS_LEVEL_CRITICAL;
    case S3_SCHEDULER_EVENT_COMMAND_PULL:
        return S3_EVENT_BUS_LEVEL_BACKGROUND;
    case S3_SCHEDULER_EVENT_CSI_FUSION_FLUSH:
        return S3_EVENT_BUS_LEVEL_REALTIME;
    case S3_SCHEDULER_EVENT_STREAM_SEND:
    case S3_SCHEDULER_EVENT_BACKGROUND_STATS:
        return S3_EVENT_BUS_LEVEL_BACKGROUND;
    case S3_SCHEDULER_EVENT_INGRESS:
        if (event->ingress == NULL) {
            return S3_EVENT_BUS_LEVEL_BACKGROUND;
        }
        if (event->ingress->kind == S3_RUNTIME_MSG_EVENT) {
            return event->ingress->is_stream_frame ?
                       S3_EVENT_BUS_LEVEL_REALTIME :
                       S3_EVENT_BUS_LEVEL_CRITICAL;
        }
        if (event->ingress->kind == S3_RUNTIME_MSG_CSI) {
            return S3_EVENT_BUS_LEVEL_STATE;
        }
        if (event->ingress->kind == S3_RUNTIME_MSG_SENSOR ||
            event->ingress->kind == S3_RUNTIME_MSG_STATUS) {
            if (!event->ingress->is_stream_frame &&
                event->priority == S3_SCHEDULER_PRIORITY_HIGH) {
                return S3_EVENT_BUS_LEVEL_CRITICAL;
            }
            return S3_EVENT_BUS_LEVEL_STATE;
        }
        break;
    case S3_SCHEDULER_EVENT_STREAM_FRAME:
        return S3_EVENT_BUS_LEVEL_REALTIME;
    case S3_SCHEDULER_EVENT_VOICE_STATE:
        return S3_EVENT_BUS_LEVEL_STATE;
    case S3_SCHEDULER_EVENT_NONE:
    default:
        break;
    }

    return event->priority == S3_SCHEDULER_PRIORITY_HIGH ?
               S3_EVENT_BUS_LEVEL_CRITICAL :
           event->priority == S3_SCHEDULER_PRIORITY_NORMAL ?
               S3_EVENT_BUS_LEVEL_REALTIME :
               S3_EVENT_BUS_LEVEL_BACKGROUND;
}

static s3_event_bus_state_key_t bus_state_key_for_event(const s3_scheduler_event_t *event)
{
    if (event == NULL) {
        return S3_EVENT_BUS_STATE_NONE;
    }
    if (event->type == S3_SCHEDULER_EVENT_INGRESS && event->ingress != NULL) {
        return state_key_for_ingress(event->ingress);
    }
    if (event->type == S3_SCHEDULER_EVENT_VOICE_STATE) {
        return S3_EVENT_BUS_STATE_DEVICE_STATUS_C51;
    }
    return S3_EVENT_BUS_STATE_NONE;
}

static void stamp_event_bus_policy(s3_scheduler_event_t *event)
{
    if (event == NULL) {
        return;
    }
    event->bus_level = bus_level_for_event(event);
    event->state_key = bus_state_key_for_event(event);
    if (event->bus_level != S3_EVENT_BUS_LEVEL_STATE) {
        event->state_key = S3_EVENT_BUS_STATE_NONE;
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
    switch (local_id) {
    case ESP111_PROTOCOL_LOCAL_DEVICE_ID_C51:
        return "C51";
    case ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52:
        return "C52";
    default:
        return NULL;
    }
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
        /* unified CSI 只取 motion_score/quality/rssi 做运行时摘要，raw/subcarrier 不进入 S3 cache。 */
        msg->t = ingress->rx_time_ms > 0 ? ingress->rx_time_ms : now_ms();
        unified_copy_text(msg->lid,
                          sizeof(msg->lid),
                          json_string(envelope->payload, "link_id", "unknown"));
        msg->v1 = json_float(envelope->payload,
                             "motion_score",
                             json_float(envelope->payload, "confidence", 0.0f));
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
    if (local_id == 0U && cJSON_IsString(local_id_item) && local_id_item->valuestring != NULL) {
        local_id = protocol_adapter_device_id_to_local_id(local_id_item->valuestring);
    }
    unified_msg_t *msg = &ingress->unified;
    msg->t = (int64_t)json_float(root,
                                 ESP111_PROTOCOL_LOCAL_JSON_UPTIME_MS,
                                 (float)now_ms());
    if (ingress->kind == S3_RUNTIME_MSG_CSI) {
        msg->t = ingress->rx_time_ms > 0 ? ingress->rx_time_ms : now_ms();
    }
    unified_copy_text(msg->did, sizeof(msg->did), short_device_id_for_local_id(local_id));
    unified_copy_text(msg->type, sizeof(msg->type), kind_name(ingress->kind));

    if (ingress->kind == S3_RUNTIME_MSG_CSI) {
        /* 旧轻量 body 兼容路径；正式 v2 envelope 会在后续 parse_ingress_envelope 覆盖摘要。 */
        const char *default_link_id =
            local_id == ESP111_PROTOCOL_LOCAL_DEVICE_ID_C51 ? "S3_TO_C51" :
            local_id == ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52 ? "S3_TO_C52" :
                                                              "unknown";
        unified_copy_text(msg->lid,
                          sizeof(msg->lid),
                          json_string(root,
                                      "link_id",
                                      default_link_id));
        msg->v1 = json_float(root,
                             "motion_score",
                             json_float(root, "confidence", json_array_float(root, 0, 0.0f)));
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

static void update_csi_latest_cache(const s3_runtime_ingress_t *ingress)
{
    if (ingress == NULL || ingress->kind != S3_RUNTIME_MSG_CSI) {
        return;
    }

    s3_event_bus_csi_latest_t latest = {
        .valid = true,
        .motion_score = ingress->unified.v1,
        .quality = ingress->unified.v2,
        .timestamp_ms = ingress->unified.t,
        .rx_time_us = ingress->rx_time_us,
    };
    strlcpy(latest.device_id,
            ingress->device_id[0] != '\0' ? ingress->device_id : ingress->unified.did,
            sizeof(latest.device_id));
    strlcpy(latest.link_id, ingress->unified.lid, sizeof(latest.link_id));
    s3_event_bus_update_csi_latest(&latest);
}

static bool drop_stale_csi_ingress(const s3_runtime_ingress_t *ingress)
{
    if (ingress == NULL || ingress->kind != S3_RUNTIME_MSG_CSI) {
        return false;
    }

    resource_manager_session_view_t current = {0};
    const bool found = resource_manager_get_session(ingress->device_id, &current);
    if (!found) {
        resource_manager_log_session_diagnostic(ingress->device_id,
                                                ingress->unified.lid,
                                                "csi_drop",
                                                "session_missing");
        return true;
    }

    if (ingress->resource_generation != 0U &&
        current.generation != ingress->resource_generation) {
        resource_manager_log_session_diagnostic(ingress->device_id,
                                                ingress->unified.lid,
                                                "csi_drop",
                                                "stale_generation");
        ESP_LOGI(TAG,
                 "CSI_STALE_DROP device_id=%s link_id=%s rx_generation=%lu current_generation=%lu rx_state=%s current_state=%s rx_time_us=%lld",
                 ingress->device_id,
                 ingress->unified.lid[0] != '\0' ? ingress->unified.lid : "-",
                 (unsigned long)ingress->resource_generation,
                 (unsigned long)current.generation,
                 resource_manager_session_state_name(ingress->resource_state_at_rx),
                 resource_manager_session_state_name(current.state),
                 (long long)ingress->rx_time_us);
        return true;
    }

    return false;
}

static void record_confirmed_peer_network_identity(const char *device_id,
                                                   const char *peer_ip,
                                                   const char *previous_peer_ip)
{
    if (device_id == NULL || device_id[0] == '\0' ||
        peer_ip == NULL || peer_ip[0] == '\0') {
        return;
    }

    if (previous_peer_ip != NULL && previous_peer_ip[0] != '\0' &&
        strcmp(previous_peer_ip, peer_ip) != 0) {
        device_stream_gateway_reset_timestamp_baseline(device_id, "peer_ip_changed");
    }

    uint8_t peer_mac[6] = {0};
    if (gateway_wifi_get_ap_client_mac(peer_ip, peer_mac)) {
        esp_err_t mac_ret = child_registry_update_peer_mac(device_id, peer_mac);
        if (mac_ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "child peer MAC update failed device_id=%s peer_ip=%s ret=%s",
                     device_id,
                     peer_ip,
                     esp_err_to_name(mac_ret));
        }
    }
    /* AP event may precede identity ingress; bind the worker-owned pending station only now. */
    (void)network_worker_bind_ap_station_identity(device_id, peer_ip);
}

static esp_err_t confirm_peer_network_identity(const char *device_id,
                                               const char *peer_ip,
                                               resource_manager_identity_signal_t signal,
                                               int64_t observed_at_us)
{
    char previous_peer_ip[16] = {0};
    (void)child_registry_get_peer_ip(device_id,
                                     previous_peer_ip,
                                     sizeof(previous_peer_ip));
    esp_err_t ret = resource_manager_confirm_peer_at_us(device_id,
                                                        peer_ip,
                                                        signal,
                                                        observed_at_us);
    if (ret == ESP_OK) {
        record_confirmed_peer_network_identity(device_id, peer_ip, previous_peer_ip);
    }
    return ret;
}

static esp_err_t process_stream_ingress(s3_runtime_ingress_t *ingress)
{
    if (ingress == NULL || !ingress->is_stream_frame) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ingress->device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    sensor_aggregator_result_t result = {0};
    esp_err_t ret = ESP_OK;
    switch (ingress->kind) {
    case S3_RUNTIME_MSG_CSI:
        log_deprecated_stream_csi_reject(ingress, "process_stream_ingress");
        ret = ESP_ERR_NOT_SUPPORTED;
        break;
    case S3_RUNTIME_MSG_SENSOR:
        ret = confirm_peer_network_identity(ingress->device_id,
                                            ingress->peer_ip,
                                            RESOURCE_MANAGER_SIGNAL_SENSOR,
                                            ingress->rx_time_us > 0 ?
                                                ingress->rx_time_us :
                                                ingress->rx_time_ms * 1000);
        if (ret != ESP_OK) {
            break;
        }
        (void)child_registry_touch(ingress->device_id,
                                   (uint32_t)(ingress->unified.t & 0xffffffffU));
        ret = sensor_aggregator_handle_stream_sensor(ingress->device_id,
                                                     ingress->unified.t,
                                                     ingress->unified.lid,
                                                     ingress->unified.v1,
                                                     ingress->unified.v2,
                                                     ingress->unified.v3,
                                                     &result);
        break;
    case S3_RUNTIME_MSG_STATUS:
        ret = confirm_peer_network_identity(ingress->device_id,
                                            ingress->peer_ip,
                                            RESOURCE_MANAGER_SIGNAL_STATUS,
                                            ingress->rx_time_us > 0 ?
                                                ingress->rx_time_us :
                                                ingress->rx_time_ms * 1000);
        if (ret == ESP_OK) {
            (void)child_registry_note_activity(ingress->device_id,
                                               (uint32_t)(ingress->unified.t & 0xffffffffU));
            ret = sensor_aggregator_handle_stream_status(ingress->device_id,
                                                         ingress->unified.t,
                                                         ingress->unified.v1,
                                                         ingress->unified.v2,
                                                         ingress->unified.v3,
                                                         &result);
        }
        break;
    case S3_RUNTIME_MSG_EVENT:
        ESP_LOGD(TAG,
                 "stream event device_id=%s link=%s v1=%.2f v2=%.2f v3=%.2f",
                 ingress->device_id,
                 ingress->unified.lid,
                 (double)ingress->unified.v1,
                 (double)ingress->unified.v2,
                 (double)ingress->unified.v3);
        break;
    case S3_RUNTIME_MSG_UNKNOWN:
    default:
        ret = ESP_ERR_NOT_SUPPORTED;
        break;
    }

    if (ret == ESP_OK) {
        update_runtime_state(ingress);
    }
    return ret;
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

    /* resource_manager rejects stale ingress, then records retained identity before restore. */
    protocol_adapter_message_kind_t kind = protocol_adapter_message_kind(envelope.message_type);
    if (kind == PROTOCOL_ADAPTER_MESSAGE_REGISTER) {
        ret = confirm_peer_network_identity(envelope.device_id,
                                            ingress->peer_ip,
                                            RESOURCE_MANAGER_SIGNAL_REGISTER,
                                            ingress->rx_time_us > 0 ?
                                                ingress->rx_time_us :
                                                ingress->rx_time_ms * 1000);
        if (ret == ESP_OK) {
            char *capabilities = capabilities_to_string(&envelope);
            ret = child_registry_register_or_update(envelope.device_id,
                                                    envelope.room_id,
                                                    envelope.alias,
                                                    capabilities,
                                                    envelope.seq);
            cJSON_free(capabilities);
            if (ret == ESP_OK) {
                device_stream_gateway_reset_timestamp_baseline(envelope.device_id,
                                                               "child_registered_online");
            } else {
                (void)resource_manager_release_peer_at_us(envelope.device_id,
                                                          esp_timer_get_time(),
                                                          "register_update_failed");
            }
        }
    } else if (kind == PROTOCOL_ADAPTER_MESSAGE_HEARTBEAT) {
        ret = confirm_peer_network_identity(envelope.device_id,
                                            ingress->peer_ip,
                                            RESOURCE_MANAGER_SIGNAL_HEARTBEAT,
                                            ingress->rx_time_us > 0 ?
                                                ingress->rx_time_us :
                                                ingress->rx_time_ms * 1000);
        if (ret == ESP_OK) {
            ret = child_registry_touch(envelope.device_id, envelope.seq);
        }
    } else if (kind == PROTOCOL_ADAPTER_MESSAGE_STATUS) {
        ret = confirm_peer_network_identity(envelope.device_id,
                                            ingress->peer_ip,
                                            RESOURCE_MANAGER_SIGNAL_STATUS,
                                            ingress->rx_time_us > 0 ?
                                                ingress->rx_time_us :
                                                ingress->rx_time_ms * 1000);
        if (ret == ESP_OK) {
            ret = child_registry_note_activity(envelope.device_id, envelope.seq);
        }
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
    if (protocol_adapter_message_kind(envelope.message_type) !=
        PROTOCOL_ADAPTER_MESSAGE_SENSOR_BME690) {
        protocol_adapter_release_envelope(&envelope);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ret = confirm_peer_network_identity(envelope.device_id,
                                        ingress->peer_ip,
                                        RESOURCE_MANAGER_SIGNAL_SENSOR,
                                        ingress->rx_time_us > 0 ?
                                            ingress->rx_time_us :
                                            ingress->rx_time_ms * 1000);
    if (ret == ESP_OK) {
        (void)child_registry_touch(envelope.device_id, envelope.seq);
        sensor_aggregator_result_t result = {0};
        ret = sensor_aggregator_handle_envelope(&envelope, &result);
    }

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

    char previous_peer_ip[16] = {0};
    (void)child_registry_get_peer_ip(envelope.device_id,
                                     previous_peer_ip,
                                     sizeof(previous_peer_ip));
    /* CSI gateway confirms identity only after feature-level validation. */
    ret = csi_placeholder_gateway_handle_result_from_peer_at_us(
        &envelope,
        ingress->peer_ip,
        ingress->rx_time_us > 0 ? ingress->rx_time_us : ingress->rx_time_ms * 1000);
    if (ret == ESP_OK) {
        record_confirmed_peer_network_identity(envelope.device_id,
                                               ingress->peer_ip,
                                               previous_peer_ip);
        (void)child_registry_touch(envelope.device_id, envelope.seq);
    }

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

    if (ingress->is_stream_frame) {
        esp_err_t ret = process_stream_ingress(ingress);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "stream runtime bus process failed type=%s did=%s lid=%s ret=%s",
                     ingress->unified.type,
                     ingress->unified.did,
                     ingress->unified.lid,
                     esp_err_to_name(ret));
        }
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

    if (drop_stale_csi_ingress(ingress)) {
        return;
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
        if (ingress->kind == S3_RUNTIME_MSG_CSI && ret == ESP_ERR_INVALID_STATE) {
            resource_manager_log_session_diagnostic(ingress->device_id,
                                                    ingress->unified.lid,
                                                    "csi_drop",
                                                    "invalid_state_suppressed");
            ESP_LOGI(TAG,
                     "CSI_INVALID_STATE_SUPPRESSED device_id=%s link_id=%s generation=%lu rx_time_us=%lld",
                     ingress->device_id,
                     ingress->unified.lid[0] != '\0' ? ingress->unified.lid : "-",
                     (unsigned long)ingress->resource_generation,
                     (long long)ingress->rx_time_us);
            return;
        }
        ESP_LOGW(TAG,
                 "runtime bus process failed type=%s did=%s lid=%s ret=%s",
                 ingress->unified.type,
                 ingress->unified.did,
                 ingress->unified.lid,
                 esp_err_to_name(ret));
        return;
    }

    if (ingress->kind == S3_RUNTIME_MSG_CSI) {
        resource_manager_session_view_t before = {0};
        if (resource_manager_get_session(ingress->device_id, &before) &&
            before.state == RESOURCE_MANAGER_SESSION_ACTIVE) {
            update_csi_latest_cache(ingress);
            update_runtime_state(ingress);

            resource_manager_session_view_t after = {0};
            if (!resource_manager_get_session(ingress->device_id, &after) ||
                after.state != RESOURCE_MANAGER_SESSION_ACTIVE ||
                after.generation != before.generation) {
                s3_event_bus_clear_csi_latest(ingress->device_id);
            }
        }
    } else if (ingress->kind != S3_RUNTIME_MSG_EVENT) {
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

static esp_err_t protocol_worker_enqueue_ingress(s3_runtime_ingress_t *ingress,
                                                 TickType_t ticks_to_wait)
{
    if (ingress == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_protocol_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(s_protocol_queue, &ingress, ticks_to_wait) != pdTRUE) {
        ESP_LOGW(TAG,
                 "protocol worker queue full kind=%s depth=%u",
                 kind_name(ingress->kind),
                 (unsigned int)S3_PROTOCOL_WORKER_QUEUE_DEPTH);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void release_csi_fusion_work_item(s3_csi_fusion_work_item_t *item)
{
    if (item == NULL) {
        return;
    }
    if (item->ingress != NULL) {
        heap_caps_free(item->ingress);
        item->ingress = NULL;
    }
}

static void coalesce_csi_fusion_ingress_queue(const s3_csi_fusion_work_item_t *incoming)
{
    if (incoming == NULL || incoming->type != S3_CSI_FUSION_WORK_INGRESS ||
        incoming->ingress == NULL || s_csi_fusion_queue == NULL) {
        return;
    }

    const s3_event_bus_state_key_t incoming_key = state_key_for_ingress(incoming->ingress);
    if (incoming_key != S3_EVENT_BUS_STATE_CSI_LATEST_C51 &&
        incoming_key != S3_EVENT_BUS_STATE_CSI_LATEST_C52) {
        return;
    }

    s3_csi_fusion_work_item_t kept[S3_CSI_FUSION_WORKER_QUEUE_DEPTH];
    size_t kept_count = 0U;
    s3_csi_fusion_work_item_t queued = {0};
    while (xQueueReceive(s_csi_fusion_queue, &queued, 0) == pdTRUE) {
        if (queued.type == S3_CSI_FUSION_WORK_INGRESS && queued.ingress != NULL &&
            state_key_for_ingress(queued.ingress) == incoming_key) {
            ++s_csi_ingress_coalesce_count;
            release_csi_fusion_work_item(&queued);
            memset(&queued, 0, sizeof(queued));
            continue;
        }
        if (kept_count < S3_CSI_FUSION_WORKER_QUEUE_DEPTH) {
            kept[kept_count++] = queued;
        } else {
            ++s_csi_ingress_drop_count;
            release_csi_fusion_work_item(&queued);
        }
        memset(&queued, 0, sizeof(queued));
    }

    for (size_t i = 0; i < kept_count; ++i) {
        if (xQueueSend(s_csi_fusion_queue, &kept[i], 0) != pdTRUE) {
            ++s_csi_ingress_drop_count;
            release_csi_fusion_work_item(&kept[i]);
        }
    }
}

static esp_err_t csi_fusion_worker_enqueue(s3_csi_fusion_work_item_t *item,
                                           TickType_t ticks_to_wait)
{
    if (item == NULL || s_csi_fusion_queue == NULL || s_csi_fusion_queue_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_csi_fusion_queue_lock, portMAX_DELAY);
    if (item->type == S3_CSI_FUSION_WORK_FLUSH && s_csi_fusion_flush_queued) {
        xSemaphoreGive(s_csi_fusion_queue_lock);
        return ESP_OK;
    }
    coalesce_csi_fusion_ingress_queue(item);
    BaseType_t queued = item->type == S3_CSI_FUSION_WORK_FLUSH ?
                            xQueueSendToFront(s_csi_fusion_queue, item, ticks_to_wait) :
                            xQueueSend(s_csi_fusion_queue, item, ticks_to_wait);
    if (queued != pdTRUE) {
        if (item->type == S3_CSI_FUSION_WORK_INGRESS) {
            ++s_csi_ingress_drop_count;
        }
        ESP_LOGW(TAG,
                 "csi fusion worker queue full type=%d depth=%u",
                 (int)item->type,
                 (unsigned int)S3_CSI_FUSION_WORKER_QUEUE_DEPTH);
        xSemaphoreGive(s_csi_fusion_queue_lock);
        return ESP_ERR_TIMEOUT;
    }
    if (item->type == S3_CSI_FUSION_WORK_FLUSH) {
        s_csi_fusion_flush_queued = true;
    }
    xSemaphoreGive(s_csi_fusion_queue_lock);
    return ESP_OK;
}

static void csi_fusion_worker_task(void *arg)
{
    (void)arg;
    const bool wdt_registered = app_task_wdt_add_current(TAG, "csi_fusion_worker");
    ESP_LOGI(TAG,
             "csi fusion worker started queue_depth=%u",
             (unsigned int)S3_CSI_FUSION_WORKER_QUEUE_DEPTH);
    app_stack_monitor_log(TAG, "csi_fusion_worker", "entry");
    app_heap_monitor_log(TAG);

    while (1) {
        uint32_t processed = 0U;
        do {
            s3_csi_fusion_work_item_t item = {0};
            TickType_t wait_ticks = processed == 0U ? pdMS_TO_TICKS(S3_SCHEDULER_BASE_TICK_MS) : 0;
            if (xQueueReceive(s_csi_fusion_queue, &item, wait_ticks) != pdTRUE) {
                break;
            }
            ++processed;
            esp_err_t ret = ESP_OK;
            if (item.type == S3_CSI_FUSION_WORK_INGRESS) {
                if (item.ingress != NULL) {
                    process_ingress(item.ingress);
                    release_csi_fusion_work_item(&item);
                }
            } else if (item.type == S3_CSI_FUSION_WORK_FLUSH) {
                s_csi_fusion_flush_queued = false;
                ret = csi_placeholder_gateway_flush_fusion();
            } else {
                ret = ESP_ERR_INVALID_ARG;
            }
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "csi fusion worker failed ret=%s", esp_err_to_name(ret));
            }
            app_task_wdt_reset_current(wdt_registered);
        } while (processed < S3_CSI_FUSION_WORKER_BUDGET);

        app_stack_monitor_log_periodic(TAG,
                                       "csi_fusion_worker",
                                       &s_last_csi_fusion_stack_monitor_ms,
                                       APP_STACK_MONITOR_INTERVAL_MS);
        app_heap_monitor_log_periodic(TAG,
                                      &s_last_csi_fusion_heap_monitor_ms,
                                      APP_HEAP_MONITOR_INTERVAL_MS);
        app_task_wdt_reset_current(wdt_registered);
        if (processed > 0U) {
            ++s_csi_worker_yield_count;
            taskYIELD();
        }
    }
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

static esp_err_t stream_worker_enqueue_event(s3_scheduler_event_t *event,
                                             TickType_t ticks_to_wait)
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

    if (xQueueSend(s_stream_queue, &item, ticks_to_wait) != pdTRUE) {
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
            /* UDP/HTTP stream parse 只生成 runtime event；业务处理继续回到 event bus worker。 */
            if (item.type == S3_STREAM_WORK_FRAME) {
                ret = device_stream_gateway_process_json((const char *)item.payload,
                                                         item.payload_len,
                                                         item.peer_ip);
            } else if (item.type == S3_STREAM_WORK_SEND) {
                char device_id[CHILD_REGISTRY_DEVICE_ID_LEN] = {0};
                const bool is_csi_trigger = strcmp(item.source, "csi_trigger") == 0;
                if (is_csi_trigger &&
                    (!child_registry_find_device_by_peer_ip(item.peer_ip,
                                                            device_id,
                                                            sizeof(device_id)) ||
                     !resource_manager_is_live(device_id))) {
                    if (device_id[0] != '\0') {
                        resource_manager_log_session_diagnostic(device_id,
                                                                NULL,
                                                                "csi_trigger_drop",
                                                                "session_not_live");
                    }
                    ESP_LOGI(TAG,
                             "CSI trigger dropped peer_ip=%s device_id=%s reason=session_not_live",
                             item.peer_ip,
                             device_id[0] != '\0' ? device_id : "unmapped");
                    ret = ESP_ERR_INVALID_STATE;
                } else {
                    ret = device_stream_gateway_send_udp_now(item.peer_ip,
                                                             item.peer_port,
                                                             item.payload,
                                                             item.payload_len,
                                                             item.source);
                }
            }
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_ARG &&
                ret != ESP_ERR_INVALID_STATE && ret != ESP_ERR_NOT_ALLOWED &&
                ret != ESP_ERR_NOT_SUPPORTED) {
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
    stamp_event_bus_policy(event);
    return s3_event_bus_push_owned(event);
}

static esp_err_t queue_push_reliable_owned(s3_scheduler_event_t *event)
{
    if (event == NULL || event->type == S3_SCHEDULER_EVENT_NONE ||
        event->priority > S3_SCHEDULER_PRIORITY_LOW) {
        return ESP_ERR_INVALID_ARG;
    }
    stamp_event_bus_policy(event);

    while (1) {
        const s3_event_bus_level_t level = event->bus_level;
        esp_err_t ret = s3_event_bus_push_owned(event);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        if (ret != ESP_ERR_TIMEOUT ||
            (level != S3_EVENT_BUS_LEVEL_CRITICAL &&
             level != S3_EVENT_BUS_LEVEL_REALTIME)) {
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
}

static esp_err_t queue_push_timed_owned(s3_scheduler_event_t *event,
                                        uint32_t event_bus_lock_timeout_ms,
                                        s3_scheduler_enqueue_diagnostics_t *diagnostics)
{
    if (event == NULL || event->type == S3_SCHEDULER_EVENT_NONE ||
        event->priority > S3_SCHEDULER_PRIORITY_LOW) {
        return ESP_ERR_INVALID_ARG;
    }
    stamp_event_bus_policy(event);
    esp_err_t ret = s3_event_bus_push_owned_timed(
        event,
        event_bus_lock_timeout_ms,
        diagnostics != NULL ? &diagnostics->event_bus_lock_wait_ms : NULL,
        diagnostics != NULL ? &diagnostics->event_bus : NULL);
    if (diagnostics != NULL) {
        diagnostics->event_bus_stats_valid = ret == ESP_OK;
    }
    return ret;
}

static size_t queue_depth(void)
{
    return s3_event_bus_get_stats().queue_depth;
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

static uint32_t interval_at_least(uint32_t interval_ms, uint32_t min_ms)
{
    return interval_ms < min_ms ? min_ms : interval_ms;
}

static bool due(int64_t now, int64_t last, uint32_t interval_ms)
{
    return last == 0 || now - last >= (int64_t)interval_ms;
}

static void enqueue_command_pull_if_needed(void)
{
    s3_scheduler_event_t *event =
        event_alloc(S3_SCHEDULER_EVENT_COMMAND_PULL, S3_SCHEDULER_PRIORITY_LOW);
    if (event == NULL) {
        s_pending_command_pull = true;
        return;
    }
    esp_err_t ret = queue_push_owned(event);
    if (ret == ESP_OK) {
        s_pending_command_pull = false;
        return;
    }
    event_release(event);
    s_pending_command_pull = true;
    if (ret != ESP_ERR_TIMEOUT && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "command pull enqueue failed ret=%s", esp_err_to_name(ret));
    }
}

static void enqueue_csi_fusion_flush_if_needed(void)
{
    s3_scheduler_event_t *event =
        event_alloc(S3_SCHEDULER_EVENT_CSI_FUSION_FLUSH, S3_SCHEDULER_PRIORITY_NORMAL);
    if (event == NULL) {
        s_pending_csi_fusion_flush = true;
        return;
    }
    esp_err_t ret = queue_push_owned(event);
    if (ret == ESP_OK) {
        s_pending_csi_fusion_flush = false;
        return;
    }
    event_release(event);
    s_pending_csi_fusion_flush = true;
    if (ret != ESP_ERR_TIMEOUT && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "CSI fusion flush enqueue failed ret=%s", esp_err_to_name(ret));
    }
}

static void enqueue_background_stats_log(const char *reason)
{
    s3_scheduler_event_t *event =
        event_alloc(S3_SCHEDULER_EVENT_BACKGROUND_STATS, S3_SCHEDULER_PRIORITY_LOW);
    if (event == NULL) {
        return;
    }
    strlcpy(event->source, reason != NULL ? reason : "periodic", sizeof(event->source));
    esp_err_t ret = queue_push_owned(event);
    if (ret != ESP_OK) {
        event_release(event);
    }
}

static void log_gateway_heartbeat(void)
{
    s3_event_bus_stats_t stats = s3_event_bus_get_stats();
    const network_worker_snapshot_stats_t snapshot_stats = network_worker_get_snapshot_stats();
    ESP_LOGI(TAG,
             "gateway heartbeat net=%s softap=%d sta=%d server=%d voice_busy=%d queue=%u drop_count=%lu coalesce_count=%lu csi_ingress_drop_count=%lu csi_ingress_coalesce_count=%lu csi_worker_yield_count=%lu snapshot_skip_count=%lu snapshot_upload_count=%lu snapshot_coalesce_count=%lu free_heap=%u psram_heap=%u last_error=%s",
             s3_scheduler_network_state_name(s_network_state),
             gateway_wifi_is_softap_ready() ? 1 : 0,
             gateway_wifi_is_sta_connected() ? 1 : 0,
             network_worker_is_server_ready() ? 1 : 0,
             s_voice_busy ? 1 : 0,
             (unsigned int)stats.queue_depth,
             (unsigned long)stats.drop_count,
             (unsigned long)stats.coalesce_count,
             (unsigned long)(stats.csi_ingress_drop_count + s_csi_ingress_drop_count),
             (unsigned long)(stats.csi_ingress_coalesce_count + s_csi_ingress_coalesce_count),
             (unsigned long)s_csi_worker_yield_count,
             (unsigned long)snapshot_stats.snapshot_skip_count,
             (unsigned long)snapshot_stats.snapshot_upload_count,
             (unsigned long)snapshot_stats.snapshot_coalesce_count,
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

static bool event_requeue_delay_ok(const s3_scheduler_event_t *event)
{
    return event != NULL && event->bus_level != S3_EVENT_BUS_LEVEL_BACKGROUND;
}

static bool requeue_event_for_retry(s3_scheduler_event_t *event, esp_err_t reason)
{
    if (!event_requeue_delay_ok(event) || reason != ESP_ERR_TIMEOUT) {
        return false;
    }
    esp_err_t ret = queue_push_owned(event);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG,
                 "event requeued type=%s level=%s reason=%s",
                 event_type_name(event->type),
                 s3_event_bus_level_name(event->bus_level),
                 esp_err_to_name(reason));
        return true;
    }
    ESP_LOGW(TAG,
             "event requeue failed type=%s level=%s ret=%s",
             event_type_name(event->type),
             s3_event_bus_level_name(event->bus_level),
             esp_err_to_name(ret));
    return false;
}

static bool process_event(s3_scheduler_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    esp_err_t ret = ESP_OK;
    const TickType_t wait_ticks =
        event->bus_level == S3_EVENT_BUS_LEVEL_CRITICAL ? portMAX_DELAY : 0;
    switch (event->type) {
    case S3_SCHEDULER_EVENT_INGRESS:
        /* CSI fusion 独立 worker，避免高频 CSI 阻塞 status/sensor protocol worker。 */
        if (event->ingress != NULL && !event->ingress->is_stream_frame &&
            event->ingress->kind == S3_RUNTIME_MSG_CSI) {
            s3_csi_fusion_work_item_t item = {
                .type = S3_CSI_FUSION_WORK_INGRESS,
                .ingress = event->ingress,
            };
            ret = csi_fusion_worker_enqueue(&item, wait_ticks);
            if (ret == ESP_OK) {
                event->ingress = NULL;
            }
            break;
        }
        /* register/heartbeat/status/sensor/ack 转交 protocol worker。 */
        ret = protocol_worker_enqueue_ingress(event->ingress, wait_ticks);
        if (ret == ESP_OK) {
            event->ingress = NULL;
        }
        break;
    case S3_SCHEDULER_EVENT_STREAM_FRAME:
    case S3_SCHEDULER_EVENT_STREAM_SEND:
        ret = stream_worker_enqueue_event(event, wait_ticks);
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
    case S3_SCHEDULER_EVENT_CSI_FUSION_FLUSH: {
        s3_csi_fusion_work_item_t item = {
            .type = S3_CSI_FUSION_WORK_FLUSH,
        };
        ret = csi_fusion_worker_enqueue(&item, wait_ticks);
        break;
    }
    case S3_SCHEDULER_EVENT_BACKGROUND_STATS:
        s3_event_bus_log_stats(event->source[0] != '\0' ? event->source : "background");
        break;
    case S3_SCHEDULER_EVENT_NONE:
    default:
        ret = ESP_ERR_INVALID_ARG;
        break;
    }

    log_dispatch_failure(event, ret);
    if (ret != ESP_OK && requeue_event_for_retry(event, ret)) {
        return true;
    }
    return false;
}

void s3_scheduler_tick(void)
{
    resource_manager_tick();
    const int64_t timestamp_ms = now_ms();
    const size_t depth = queue_depth();
    const uint32_t multiplier = load_multiplier(depth);
    const uint32_t csi_interval_ms = CSI_FUSION_TICK_MS;
    const uint32_t upload_interval_ms =
        adjusted_interval(gateway_config_get()->sensor_forward_period_ms, multiplier);
    const uint32_t snapshot_upload_interval_ms = UPLOAD_SNAPSHOT_INTERVAL_MS;
    const uint32_t command_pull_interval_ms =
        interval_at_least(upload_interval_ms, S3_SCHEDULER_COMMAND_PULL_MIN_MS);
    const uint32_t smart_home_interval_ms =
        adjusted_interval(S3_SCHEDULER_SMART_HOME_POLL_MS, multiplier);
    const bool local_net_ready = gateway_wifi_is_net_ready();
    const bool server_allowed = s3_scheduler_is_server_upload_allowed();
    const bool has_active_session = resource_manager_has_active_sessions();
    const bool has_live_session = resource_manager_has_live_sessions();

    app_stack_monitor_log_periodic(TAG,
                                   "s3_scheduler_task",
                                   &s_last_stack_monitor_ms,
                                   APP_STACK_MONITOR_INTERVAL_MS);
    app_heap_monitor_log_periodic(TAG, &s_last_heap_monitor_ms, APP_HEAP_MONITOR_INTERVAL_MS);

    /*
     * 本地 CSI trigger 只需要 C5<->S3 网络 ready；上云 snapshot/command/smart-home 必须等
     * link stable，并且 voice_busy 时暂停，避免语音代理长连接被周期上云挤占。
     */
    if (has_live_session && local_net_ready && due(timestamp_ms, s_last_csi_trigger_ms,
                              adjusted_interval(gateway_config_get()->csi_trigger_interval_ms,
                                                multiplier))) {
        s_last_csi_trigger_ms = timestamp_ms;
        if (!s_voice_busy && depth < S3_SCHEDULER_HARD_WATERMARK) {
            (void)csi_placeholder_gateway_send_triggers();
        }
    }

    if (has_active_session &&
        (s_pending_csi_fusion_flush ||
         due(timestamp_ms, s_last_csi_flush_ms, csi_interval_ms))) {
        s_last_csi_flush_ms = timestamp_ms;
        enqueue_csi_fusion_flush_if_needed();
    } else if (!has_active_session) {
        s_pending_csi_fusion_flush = false;
    }

    if (server_allowed && !s_voice_busy &&
        due(timestamp_ms, s_last_snapshot_upload_ms, snapshot_upload_interval_ms)) {
        s_last_snapshot_upload_ms = timestamp_ms;
        esp_err_t ret = network_worker_enqueue_snapshot_upload();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "snapshot upload worker enqueue failed ret=%s", esp_err_to_name(ret));
        }
    }

    if (has_live_session && server_allowed && !s_voice_busy &&
        (s_pending_command_pull ||
         due(timestamp_ms, s_last_command_pull_ms, command_pull_interval_ms))) {
        s_last_command_pull_ms = timestamp_ms;
        enqueue_command_pull_if_needed();
    } else if (!has_live_session) {
        s_pending_command_pull = false;
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
        if (has_active_session) {
            csi_placeholder_gateway_log_latest_diagnostics();
        }
        enqueue_background_stats_log("diagnostic");
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
             "S3 scheduler started as priority event bus base_tick_ms=%u priority_order=CRITICAL>REALTIME>STATE>BACKGROUND",
             (unsigned int)S3_SCHEDULER_BASE_TICK_MS);
    s3_event_bus_log_stats("start");
    app_stack_monitor_log(TAG, "s3_scheduler_task", "entry");
    app_heap_monitor_log(TAG);

    int64_t last_tick_ms = 0;
    while (1) {
        if (s3_event_bus_wait(S3_SCHEDULER_BASE_TICK_MS)) {
            s3_scheduler_event_t *event = NULL;
            if (s3_event_bus_dequeue(&event)) {
                bool requeued = process_event(event);
                if (!requeued) {
                    event_release(event);
                }
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
    esp_err_t bus_ret = s3_event_bus_init(event_release);
    if (bus_ret != ESP_OK) {
        return bus_ret;
    }
    if (s_protocol_queue == NULL) {
        s_protocol_queue = xQueueCreate(S3_PROTOCOL_WORKER_QUEUE_DEPTH,
                                        sizeof(s3_runtime_ingress_t *));
        if (s_protocol_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_csi_fusion_queue == NULL) {
        s_csi_fusion_queue = xQueueCreate(S3_CSI_FUSION_WORKER_QUEUE_DEPTH,
                                          sizeof(s3_csi_fusion_work_item_t));
        if (s_csi_fusion_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_csi_fusion_queue_lock == NULL) {
        s_csi_fusion_queue_lock = xSemaphoreCreateMutex();
        if (s_csi_fusion_queue_lock == NULL) {
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
    s3_event_bus_reset();
    s3_runtime_ingress_t *stale_ingress = NULL;
    while (xQueueReceive(s_protocol_queue, &stale_ingress, 0) == pdTRUE) {
        heap_caps_free(stale_ingress);
        stale_ingress = NULL;
    }
    s3_csi_fusion_work_item_t stale_csi_item = {0};
    while (xQueueReceive(s_csi_fusion_queue, &stale_csi_item, 0) == pdTRUE) {
        if (stale_csi_item.ingress != NULL) {
            heap_caps_free(stale_csi_item.ingress);
        }
        memset(&stale_csi_item, 0, sizeof(stale_csi_item));
    }
    s3_stream_work_item_t stale_stream_item = {0};
    while (xQueueReceive(s_stream_queue, &stale_stream_item, 0) == pdTRUE) {
        release_stream_work_item(&stale_stream_item);
        memset(&stale_stream_item, 0, sizeof(stale_stream_item));
    }
    s_network_state = S3_SCHEDULER_NET_NOT_READY;
    s_voice_busy = false;
    s_pending_csi_fusion_flush = false;
    s_csi_fusion_flush_queued = false;
    s_pending_command_pull = false;
    s_csi_ingress_drop_count = 0U;
    s_csi_ingress_coalesce_count = 0U;
    s_csi_worker_yield_count = 0U;
    return ESP_OK;
}

esp_err_t s3_scheduler_start(void)
{
    if (s_protocol_queue == NULL || s_csi_fusion_queue == NULL || s_stream_queue == NULL) {
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
    if (s_csi_fusion_worker_task == NULL) {
        BaseType_t created = xTaskCreate(csi_fusion_worker_task,
                                         "csi_fusion_worker",
                                         S3_CSI_FUSION_WORKER_TASK_STACK,
                                         NULL,
                                         S3_CSI_FUSION_WORKER_TASK_PRIORITY,
                                         &s_csi_fusion_worker_task);
        if (created != pdPASS) {
            s_csi_fusion_worker_task = NULL;
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
    if (event->type == S3_SCHEDULER_EVENT_INGRESS &&
        ingress_is_deprecated_stream_csi(event->ingress)) {
        log_deprecated_stream_csi_reject(event->ingress, "enqueue_event");
        return ESP_ERR_NOT_SUPPORTED;
    }

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

    esp_err_t ret = queue_push_reliable_owned(owned);
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
    if (ingress_is_deprecated_stream_csi(ingress)) {
        log_deprecated_stream_csi_reject(ingress, "enqueue_ingress");
        return ESP_ERR_NOT_SUPPORTED;
    }
    s3_runtime_ingress_t *owned_ingress =
        heap_caps_calloc(1, sizeof(*owned_ingress), MALLOC_CAP_8BIT);
    if (owned_ingress == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *owned_ingress = *ingress;
    return s3_scheduler_enqueue_ingress_owned(owned_ingress, priority);
}

static esp_err_t enqueue_ingress_owned_internal(
    s3_runtime_ingress_t *ingress,
    s3_scheduler_priority_t priority,
    bool bounded_event_bus_lock,
    uint32_t event_bus_lock_timeout_ms,
    s3_scheduler_enqueue_diagnostics_t *out_diagnostics)
{
    const int64_t enqueue_started_us = esp_timer_get_time();
    if (out_diagnostics != NULL) {
        memset(out_diagnostics, 0, sizeof(*out_diagnostics));
    }
    esp_err_t ret = ESP_OK;
    s3_scheduler_event_t *event = NULL;
    if (ingress == NULL) {
        ret = ESP_ERR_INVALID_ARG;
        goto done;
    }
    if (priority > S3_SCHEDULER_PRIORITY_LOW ||
        ingress->kind == S3_RUNTIME_MSG_UNKNOWN ||
        ingress->body_len > S3_RUNTIME_BUS_BODY_MAX) {
        heap_caps_free(ingress);
        ret = ESP_ERR_INVALID_ARG;
        goto done;
    }
    if (ingress_is_deprecated_stream_csi(ingress)) {
        log_deprecated_stream_csi_reject(ingress, "enqueue_ingress_owned");
        heap_caps_free(ingress);
        ret = ESP_ERR_NOT_SUPPORTED;
        goto done;
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

    event = event_alloc(S3_SCHEDULER_EVENT_INGRESS, priority);
    if (event == NULL) {
        heap_caps_free(ingress);
        ret = ESP_ERR_NO_MEM;
        goto done;
    }
    event->ingress = ingress;

    ret = bounded_event_bus_lock ?
              queue_push_timed_owned(event, event_bus_lock_timeout_ms, out_diagnostics) :
              queue_push_reliable_owned(event);
    if (ret != ESP_OK) {
        event_release(event);
    }
done:
    if (out_diagnostics != NULL) {
        out_diagnostics->enqueue_duration_ms =
            (uint32_t)((esp_timer_get_time() - enqueue_started_us) / 1000);
    }
    return ret;
}

esp_err_t s3_scheduler_enqueue_ingress_owned(s3_runtime_ingress_t *ingress,
                                             s3_scheduler_priority_t priority)
{
    return enqueue_ingress_owned_internal(ingress, priority, false, 0U, NULL);
}

esp_err_t s3_scheduler_enqueue_ingress_owned_timed(
    s3_runtime_ingress_t *ingress,
    s3_scheduler_priority_t priority,
    uint32_t event_bus_lock_timeout_ms,
    s3_scheduler_enqueue_diagnostics_t *out_diagnostics)
{
    return enqueue_ingress_owned_internal(ingress,
                                          priority,
                                          true,
                                          event_bus_lock_timeout_ms,
                                          out_diagnostics);
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

    esp_err_t ret = queue_push_reliable_owned(event);
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

    esp_err_t ret = queue_push_reliable_owned(event);
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

    esp_err_t ret = queue_push_reliable_owned(event);
    if (ret != ESP_OK) {
        event_release(event);
    }
    return ret;
}

esp_err_t s3_scheduler_enqueue_command_pull(void)
{
    if (!resource_manager_has_live_sessions()) {
        return ESP_ERR_INVALID_STATE;
    }
    s3_scheduler_event_t *event =
        event_alloc(S3_SCHEDULER_EVENT_COMMAND_PULL, S3_SCHEDULER_PRIORITY_LOW);
    if (event == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = queue_push_reliable_owned(event);
    if (ret != ESP_OK) {
        event_release(event);
    }
    return ret;
}

void s3_scheduler_reset_stream_queue(const char *reason)
{
    if (s_stream_queue == NULL) {
        return;
    }

    size_t drained = 0U;
    s3_stream_work_item_t stale_stream_item = {0};
    while (xQueueReceive(s_stream_queue, &stale_stream_item, 0) == pdTRUE) {
        release_stream_work_item(&stale_stream_item);
        memset(&stale_stream_item, 0, sizeof(stale_stream_item));
        ++drained;
    }

    if (drained > 0U) {
        ESP_LOGI(TAG,
                 "stream worker queue reset reason=%s drained=%u",
                 reason != NULL ? reason : "unknown",
                 (unsigned int)drained);
    }
}

static bool csi_ingress_at_or_before_cutoff(const s3_runtime_ingress_t *ingress,
                                            int64_t cutoff_us)
{
    return ingress == NULL || ingress->rx_time_us <= 0 || ingress->rx_time_us <= cutoff_us;
}

void s3_scheduler_clear_csi_peer_before(const char *device_id,
                                        int64_t cutoff_us,
                                        const char *reason)
{
    if (device_id == NULL || device_id[0] == '\0' || s_csi_fusion_queue == NULL ||
        s_csi_fusion_queue_lock == NULL) {
        return;
    }

    s3_csi_fusion_work_item_t kept[S3_CSI_FUSION_WORKER_QUEUE_DEPTH] = {0};
    size_t kept_count = 0U;
    size_t cleared = 0U;
    s3_csi_fusion_work_item_t item = {0};

    xSemaphoreTake(s_csi_fusion_queue_lock, portMAX_DELAY);
    while (xQueueReceive(s_csi_fusion_queue, &item, 0) == pdTRUE) {
        const bool matches = item.type == S3_CSI_FUSION_WORK_INGRESS &&
                             item.ingress != NULL &&
                             strcmp(item.ingress->device_id, device_id) == 0 &&
                             csi_ingress_at_or_before_cutoff(item.ingress, cutoff_us);
        if (matches) {
            release_csi_fusion_work_item(&item);
            ++cleared;
        } else if (kept_count < S3_CSI_FUSION_WORKER_QUEUE_DEPTH) {
            kept[kept_count++] = item;
        } else {
            release_csi_fusion_work_item(&item);
        }
        memset(&item, 0, sizeof(item));
    }
    for (size_t i = 0; i < kept_count; ++i) {
        if (xQueueSend(s_csi_fusion_queue, &kept[i], 0) != pdTRUE) {
            release_csi_fusion_work_item(&kept[i]);
        }
    }
    xSemaphoreGive(s_csi_fusion_queue_lock);

    if (cleared > 0U) {
        ESP_LOGI(TAG,
                 "CSI queued ingress cleared device_id=%s count=%u cutoff_us=%lld reason=%s",
                 device_id,
                 (unsigned int)cleared,
                 (long long)cutoff_us,
                 reason != NULL ? reason : "resource_release");
    }
}

void s3_scheduler_clear_csi_peer(const char *device_id, const char *reason)
{
    s3_scheduler_clear_csi_peer_before(device_id, INT64_MAX, reason);
}

s3_scheduler_load_t s3_scheduler_get_load(void)
{
    const s3_event_bus_stats_t stats = s3_event_bus_get_stats();
    const size_t depth = stats.queue_depth;
    const uint32_t multiplier = load_multiplier(depth);
    s3_scheduler_load_t load = {
        .queue_depth = depth,
        .high_depth = stats.critical_depth,
        .normal_depth = stats.realtime_depth + stats.state_depth,
        .low_depth = stats.background_depth,
        .critical_depth = stats.critical_depth,
        .realtime_depth = stats.realtime_depth,
        .state_depth = stats.state_depth,
        .background_depth = stats.background_depth,
        .drop_count = stats.drop_count,
        .coalesce_count = stats.coalesce_count,
        .csi_ingress_drop_count = stats.csi_ingress_drop_count + s_csi_ingress_drop_count,
        .csi_ingress_coalesce_count =
            stats.csi_ingress_coalesce_count + s_csi_ingress_coalesce_count,
        .csi_worker_yield_count = s_csi_worker_yield_count,
        .csi_latest = stats.csi_latest,
        .network_state = s_network_state,
        .voice_busy = s_voice_busy,
        .csi_interval_ms = CSI_FUSION_TICK_MS,
        .upload_interval_ms = UPLOAD_SNAPSHOT_INTERVAL_MS,
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
    return gateway_wifi_is_local_ingest_ready();
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

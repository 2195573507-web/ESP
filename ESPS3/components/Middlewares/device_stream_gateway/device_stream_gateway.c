/**
 * @file device_stream_gateway.c
 * @brief S3 侧 C5 扁平 device stream ingress。
 *
 * 本文件属于 ESPS3 网关，负责接收 C5 的 UDP/HTTP 短字段 stream frame，并交给
 * scheduler stream worker 解析并重新入 S3 event bus。它只接受固定 7 字段对象，
 * 维护每个 device_id 的单调 timestamp，并拒绝固定 schema 以外的 payload。
 */

#include "device_stream_gateway.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "app_stack_monitor.h"
#include "esp111_protocol_common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gateway_config.h"
#include "gateway_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "resource_manager.h"
#include "s3_scheduler.h"

static const char *TAG = "device_stream_gateway";

#ifndef DEVICE_STREAM_UDP_TASK_STACK
#define DEVICE_STREAM_UDP_TASK_STACK 8192U
#endif

#ifndef DEVICE_STREAM_UDP_TASK_PRIORITY
#define DEVICE_STREAM_UDP_TASK_PRIORITY 3U
#endif

#ifndef DEVICE_STREAM_STATS_LOG_INTERVAL_MS
#define DEVICE_STREAM_STATS_LOG_INTERVAL_MS 10000U
#endif

#ifndef DEVICE_STREAM_REJECT_LOG_INTERVAL_MS
#define DEVICE_STREAM_REJECT_LOG_INTERVAL_MS 10000U
#endif

#ifndef DEVICE_STREAM_RECONNECT_GAP_MS
#define DEVICE_STREAM_RECONNECT_GAP_MS 5000U
#endif

#ifndef DEVICE_STREAM_REBOOT_SMALL_TS_MS
#define DEVICE_STREAM_REBOOT_SMALL_TS_MS 60000U
#endif

#define DEVICE_STREAM_CLOCK_COUNT (sizeof(s_clocks) / sizeof(s_clocks[0]))

typedef struct {
    char device_id[48];
    char type[12];
    int64_t last_t_ms;
    int64_t last_valid_wall_ms;
    char peer_ip[16];
} stream_clock_t;

typedef struct {
    int64_t timestamp_ms;
    char device_id[48];
    char type[12];
    char link_id[32];
    double v1;
    double v2;
    double v3;
} device_stream_frame_t;

static TaskHandle_t s_udp_task;
static SemaphoreHandle_t s_clock_lock;
static stream_clock_t s_clocks[GATEWAY_CONFIG_MAX_CHILDREN * 4U];
static uint32_t s_net_not_ready_drop_count;
static uint32_t s_queue_overflow_drop_count;
static uint32_t s_stream_reject_count;
static uint32_t s_udp_enqueue_fail_count;
static uint32_t s_udp_invalid_state_suppressed_count;
static int64_t s_last_stats_log_ms;
static int64_t s_last_reject_log_ms;
static int64_t s_last_udp_enqueue_log_ms;
static int64_t s_last_stack_log_ms;
static int64_t s_last_heap_log_ms;
static char s_last_reject_device_id[48];
static size_t s_last_reject_len;
static esp_err_t s_last_reject_ret;
static volatile bool s_stream_running;

/* stream ingress 使用 S3 本机 uptime 做日志节流和 net gate 等待，不参与业务 timestamp。 */
static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static size_t stream_queue_depth(void)
{
    return s3_scheduler_get_load().queue_depth;
}

static void log_stream_stats(const char *reason, bool force)
{
    int64_t timestamp_ms = now_ms();
    if (!force && s_last_stats_log_ms != 0 &&
        timestamp_ms - s_last_stats_log_ms < DEVICE_STREAM_STATS_LOG_INTERVAL_MS) {
        return;
    }
    s_last_stats_log_ms = timestamp_ms;

    ESP_LOGI(TAG,
             "STREAM_QUEUE_DEPTH=%u NET_NOT_READY_DROP_COUNT=%lu STREAM_QUEUE_OVERFLOW_DROP_COUNT=%lu STREAM_REJECT_COUNT=%lu reason=%s",
             (unsigned int)stream_queue_depth(),
             (unsigned long)s_net_not_ready_drop_count,
             (unsigned long)s_queue_overflow_drop_count,
             (unsigned long)s_stream_reject_count,
             reason != NULL ? reason : "periodic");
}

static void record_stream_reject_for_device(const char *reason,
                                            size_t len,
                                            esp_err_t ret,
                                            const char *device_id)
{
    ++s_stream_reject_count;
    s_last_reject_len = len;
    s_last_reject_ret = ret;
    strlcpy(s_last_reject_device_id,
            device_id != NULL && device_id[0] != '\0' ? device_id : "-",
            sizeof(s_last_reject_device_id));

    const int64_t timestamp_ms = now_ms();
    if (s_last_reject_log_ms == 0 ||
        timestamp_ms - s_last_reject_log_ms >= DEVICE_STREAM_REJECT_LOG_INTERVAL_MS) {
        s_last_reject_log_ms = timestamp_ms;
        ESP_LOGW(TAG,
                 "stream reject summary reason=%s device_id=%s count=%lu last_len=%u ret=%s depth=%u",
                 reason != NULL ? reason : "unknown",
                 device_id != NULL && device_id[0] != '\0' ? device_id : "-",
                 (unsigned long)s_stream_reject_count,
                 (unsigned int)len,
                 esp_err_to_name(ret),
                 (unsigned int)stream_queue_depth());
        return;
    }

    ESP_LOGD(TAG,
             "stream reject reason=%s device_id=%s count=%lu len=%u ret=%s",
             reason != NULL ? reason : "unknown",
             device_id != NULL && device_id[0] != '\0' ? device_id : "-",
             (unsigned long)s_stream_reject_count,
             (unsigned int)len,
             esp_err_to_name(ret));
}

static void record_stream_reject(const char *reason, size_t len, esp_err_t ret)
{
    record_stream_reject_for_device(reason, len, ret, NULL);
}

static void record_net_not_ready_drop(const char *source)
{
    s_net_not_ready_drop_count++;
    ESP_LOGD(TAG,
             "stream drop net_not_ready count=%lu depth=%u source=%s",
             (unsigned long)s_net_not_ready_drop_count,
             (unsigned int)stream_queue_depth(),
             source != NULL ? source : "unknown");
    log_stream_stats(source, false);
}

static void reset_stream_runtime(const char *reason)
{
    device_stream_gateway_reset_timestamp_baseline(NULL, reason);
    s3_scheduler_reset_stream_queue(reason);
}

static void log_udp_enqueue_failed(esp_err_t ret, size_t len, const char *peer_ip)
{
    ++s_udp_enqueue_fail_count;
    if (ret == ESP_ERR_INVALID_STATE) {
        ++s_udp_invalid_state_suppressed_count;
    }

    const int64_t timestamp_ms = now_ms();
    if (s_last_udp_enqueue_log_ms != 0 &&
        timestamp_ms - s_last_udp_enqueue_log_ms < DEVICE_STREAM_REJECT_LOG_INTERVAL_MS) {
        return;
    }
    s_last_udp_enqueue_log_ms = timestamp_ms;

    ESP_LOGW(TAG,
             "stream UDP enqueue failed summary count=%lu invalid_state_count=%lu device_id=%s peer_ip=%s last_len=%u ret=%s depth=%u",
             (unsigned long)s_udp_enqueue_fail_count,
             (unsigned long)s_udp_invalid_state_suppressed_count,
             s_last_reject_device_id[0] != '\0' ? s_last_reject_device_id : "-",
             peer_ip != NULL && peer_ip[0] != '\0' ? peer_ip : "-",
             (unsigned int)(s_last_reject_len != 0U ? s_last_reject_len : len),
             esp_err_to_name(s_last_reject_ret != ESP_OK ? s_last_reject_ret : ret),
             (unsigned int)stream_queue_depth());
}

static void wait_until_net_ready(const char *task_name, bool wdt_registered)
{
    while (!s_stream_running || !gateway_wifi_is_net_ready()) {
        log_stream_stats(task_name, false);
        app_stack_monitor_log_periodic(TAG,
                                       "device_stream_gateway",
                                       &s_last_stack_log_ms,
                                       APP_STACK_MONITOR_INTERVAL_MS);
        app_heap_monitor_log_periodic(TAG,
                                      &s_last_heap_log_ms,
                                      APP_HEAP_MONITOR_INTERVAL_MS);
        app_task_wdt_delay_ms(wdt_registered, 200U);
    }
}

static bool stream_type_is_allowed(const char *type)
{
    return type != NULL &&
           (strcmp(type, ESP111_PROTOCOL_DEVICE_STREAM_TYPE_SENSOR) == 0 ||
            strcmp(type, ESP111_PROTOCOL_DEVICE_STREAM_TYPE_STATUS) == 0 ||
            strcmp(type, ESP111_PROTOCOL_DEVICE_STREAM_TYPE_EVENT) == 0);
}

static bool stream_key_is_allowed(const char *key)
{
    return key != NULL &&
           (strcmp(key, ESP111_PROTOCOL_DEVICE_STREAM_JSON_TIMESTAMP) == 0 ||
            strcmp(key, ESP111_PROTOCOL_DEVICE_STREAM_JSON_DEVICE_ID) == 0 ||
            strcmp(key, ESP111_PROTOCOL_DEVICE_STREAM_JSON_TYPE) == 0 ||
            strcmp(key, ESP111_PROTOCOL_DEVICE_STREAM_JSON_LINK_ID) == 0 ||
            strcmp(key, ESP111_PROTOCOL_DEVICE_STREAM_JSON_VALUE1) == 0 ||
            strcmp(key, ESP111_PROTOCOL_DEVICE_STREAM_JSON_VALUE2) == 0 ||
            strcmp(key, ESP111_PROTOCOL_DEVICE_STREAM_JSON_VALUE3) == 0);
}

static bool json_is_strict_stream_object(cJSON *root)
{
    if (!cJSON_IsObject(root)) {
        return false;
    }
    size_t field_count = 0;
    for (cJSON *item = root->child; item != NULL; item = item->next) {
        ++field_count;
        /* 扁平 stream 只允许 t/did/type/lid/v1/v2/v3，禁止嵌套对象混入。 */
        if (!stream_key_is_allowed(item->string) ||
            cJSON_IsArray(item) || cJSON_IsObject(item)) {
            return false;
        }
    }
    return field_count == 7U;
}

static const char *json_string(cJSON *root, const char *key)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(value) && value->valuestring != NULL ? value->valuestring : NULL;
}

static bool json_number(cJSON *root, const char *key, double *out)
{
    if (out == NULL) {
        return false;
    }
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(value)) {
        return false;
    }
    *out = value->valuedouble;
    return isfinite(*out);
}

static bool parse_frame(const char *json, device_stream_frame_t *out)
{
    if (json == NULL || out == NULL) {
        return false;
    }

    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    if (root == NULL) {
        return false;
    }
    bool ok = false;
    if (json_is_strict_stream_object(root)) {
        memset(out, 0, sizeof(*out));
        double timestamp = 0.0;
        const char *device_id = json_string(root, ESP111_PROTOCOL_DEVICE_STREAM_JSON_DEVICE_ID);
        const char *type = json_string(root, ESP111_PROTOCOL_DEVICE_STREAM_JSON_TYPE);
        const char *link_id = json_string(root, ESP111_PROTOCOL_DEVICE_STREAM_JSON_LINK_ID);
        ok = json_number(root, ESP111_PROTOCOL_DEVICE_STREAM_JSON_TIMESTAMP, &timestamp) &&
             json_number(root, ESP111_PROTOCOL_DEVICE_STREAM_JSON_VALUE1, &out->v1) &&
             json_number(root, ESP111_PROTOCOL_DEVICE_STREAM_JSON_VALUE2, &out->v2) &&
             json_number(root, ESP111_PROTOCOL_DEVICE_STREAM_JSON_VALUE3, &out->v3) &&
             device_id != NULL && stream_type_is_allowed(type) && link_id != NULL &&
             device_id[0] != '\0' && type[0] != '\0' && link_id[0] != '\0';
        if (ok) {
            ok = strlen(device_id) < sizeof(out->device_id) &&
                 strlen(type) < sizeof(out->type) &&
                 strlen(link_id) < sizeof(out->link_id);
        }
        if (ok) {
            /* timestamp 必须是整数毫秒；小数时间戳会被拒绝，避免 S3 单调表出现隐式截断。 */
            out->timestamp_ms = (int64_t)timestamp;
            ok = timestamp == (double)out->timestamp_ms && out->timestamp_ms > 0;
        }
        if (ok) {
            strlcpy(out->device_id, device_id, sizeof(out->device_id));
            strlcpy(out->type, type, sizeof(out->type));
            strlcpy(out->link_id, link_id, sizeof(out->link_id));
        }
    }
    cJSON_Delete(root);
    return ok;
}

static bool peer_ip_changed(const stream_clock_t *slot, const char *peer_ip)
{
    return slot != NULL && slot->peer_ip[0] != '\0' &&
           peer_ip != NULL && peer_ip[0] != '\0' &&
           strcmp(slot->peer_ip, peer_ip) != 0;
}

static const char *timestamp_reset_reason(const stream_clock_t *slot,
                                          const device_stream_frame_t *frame,
                                          const char *peer_ip,
                                          int64_t wall_ms)
{
    if (slot == NULL || frame == NULL || frame->timestamp_ms >= slot->last_t_ms) {
        return NULL;
    }
    if (peer_ip_changed(slot, peer_ip)) {
        return "peer_ip_changed";
    }
    if (frame->timestamp_ms < (int64_t)DEVICE_STREAM_REBOOT_SMALL_TS_MS &&
        slot->last_t_ms > (int64_t)DEVICE_STREAM_REBOOT_SMALL_TS_MS) {
        return "child_reboot_timestamp";
    }
    if (slot->last_valid_wall_ms > 0 &&
        wall_ms - slot->last_valid_wall_ms > (int64_t)DEVICE_STREAM_RECONNECT_GAP_MS) {
        return "stream_gap";
    }
    return NULL;
}

static bool timestamp_is_monotonic(const device_stream_frame_t *frame,
                                   const char *peer_ip,
                                   size_t len)
{
    if (frame == NULL || frame->timestamp_ms <= 0) {
        return false;
    }

    const int64_t wall_ms = now_ms();
    bool ok = false;
    bool table_full = false;
    bool accepted_reset = false;
    int64_t old_child_ts = 0;
    const char *reset_reason = NULL;

    if (s_clock_lock != NULL) {
        xSemaphoreTake(s_clock_lock, portMAX_DELAY);
    }

    stream_clock_t *slot = NULL;
    for (size_t i = 0; i < DEVICE_STREAM_CLOCK_COUNT; ++i) {
        if (s_clocks[i].device_id[0] == '\0') {
            if (slot == NULL) {
                slot = &s_clocks[i];
            }
            continue;
        }
        if (strcmp(s_clocks[i].device_id, frame->device_id) == 0 &&
            strcmp(s_clocks[i].type, frame->type) == 0) {
            slot = &s_clocks[i];
            break;
        }
    }

    if (slot != NULL) {
        if (slot->device_id[0] == '\0') {
            strlcpy(slot->device_id, frame->device_id, sizeof(slot->device_id));
            strlcpy(slot->type, frame->type, sizeof(slot->type));
        }
        old_child_ts = slot->last_t_ms;
        ok = frame->timestamp_ms > slot->last_t_ms;
        if (!ok) {
            reset_reason = timestamp_reset_reason(slot, frame, peer_ip, wall_ms);
            ok = reset_reason != NULL;
            accepted_reset = ok;
        }
        if (ok) {
            slot->last_t_ms = frame->timestamp_ms;
            slot->last_valid_wall_ms = wall_ms;
            if (peer_ip != NULL && peer_ip[0] != '\0') {
                strlcpy(slot->peer_ip, peer_ip, sizeof(slot->peer_ip));
            }
        }
    } else {
        table_full = true;
    }

    if (s_clock_lock != NULL) {
        xSemaphoreGive(s_clock_lock);
    }
    if (table_full) {
        record_stream_reject_for_device("clock_table_full",
                                        len,
                                        ESP_ERR_NO_MEM,
                                        frame->device_id);
        ESP_LOGD(TAG,
                 "stream clock table full device_id=%s type=%s max_entries=%u",
                 frame->device_id,
                 frame->type,
                 (unsigned int)DEVICE_STREAM_CLOCK_COUNT);
    } else if (accepted_reset) {
        ESP_LOGI(TAG,
                 "stream timestamp baseline accepted reset device_id=%s type=%s old_child_ts=%lld new_child_ts=%lld reason=%s",
                 frame->device_id,
                 frame->type,
                 (long long)old_child_ts,
                 (long long)frame->timestamp_ms,
                 reset_reason != NULL ? reset_reason : "unknown");
    } else if (!ok) {
        record_stream_reject_for_device("timestamp_non_monotonic",
                                        len,
                                        ESP_ERR_INVALID_STATE,
                                        frame->device_id);
        ESP_LOGD(TAG,
                 "stream timestamp rejected device_id=%s type=%s timestamp=%lld last=%lld",
                 frame->device_id,
                 frame->type,
                 (long long)frame->timestamp_ms,
                 (long long)old_child_ts);
    }
    return ok;
}

static s3_runtime_msg_kind_t kind_from_frame_type(const char *type)
{
    if (type == NULL) {
        return S3_RUNTIME_MSG_UNKNOWN;
    }
    if (strcmp(type, ESP111_PROTOCOL_DEVICE_STREAM_TYPE_SENSOR) == 0) {
        return S3_RUNTIME_MSG_SENSOR;
    }
    if (strcmp(type, ESP111_PROTOCOL_DEVICE_STREAM_TYPE_STATUS) == 0) {
        return S3_RUNTIME_MSG_STATUS;
    }
    if (strcmp(type, ESP111_PROTOCOL_DEVICE_STREAM_TYPE_EVENT) == 0) {
        return S3_RUNTIME_MSG_EVENT;
    }
    return S3_RUNTIME_MSG_UNKNOWN;
}

static esp_err_t enqueue_frame_event(const device_stream_frame_t *frame,
                                     const char *peer_ip,
                                     int64_t received_at_us)
{
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s3_runtime_ingress_t ingress = {
        .kind = kind_from_frame_type(frame->type),
        .is_stream_frame = true,
        .rx_time_us = received_at_us > 0 ? received_at_us : esp_timer_get_time(),
    };
    ingress.rx_time_ms = ingress.rx_time_us / 1000;
    if (ingress.kind == S3_RUNTIME_MSG_UNKNOWN) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    ingress.unified.t = frame->timestamp_ms;
    strlcpy(ingress.unified.did, frame->device_id, sizeof(ingress.unified.did));
    strlcpy(ingress.unified.type, frame->type, sizeof(ingress.unified.type));
    strlcpy(ingress.unified.lid, frame->link_id, sizeof(ingress.unified.lid));
    ingress.unified.v1 = (float)frame->v1;
    ingress.unified.v2 = (float)frame->v2;
    ingress.unified.v3 = (float)frame->v3;
    strlcpy(ingress.device_id, frame->device_id, sizeof(ingress.device_id));
    resource_manager_session_view_t view = {0};
    if (resource_manager_get_session(ingress.device_id, &view)) {
        ingress.resource_generation = view.generation;
        ingress.resource_state_at_rx = view.state;
        ingress.resource_state_since_ms_at_rx = view.state_since_ms;
    }
    if (peer_ip != NULL) {
        strlcpy(ingress.peer_ip, peer_ip, sizeof(ingress.peer_ip));
    }

    s3_scheduler_priority_t priority =
        ingress.kind == S3_RUNTIME_MSG_STATUS ? S3_SCHEDULER_PRIORITY_HIGH :
                                                S3_SCHEDULER_PRIORITY_NORMAL;
    return s3_scheduler_enqueue_ingress(&ingress, priority);
}

static esp_err_t process_json_at_us(const char *json,
                                    size_t json_len,
                                    const char *peer_ip,
                                    int64_t received_at_us)
{
    if (!s_stream_running || !gateway_wifi_is_net_ready()) {
        reset_stream_runtime("stream_json_not_ready");
        record_net_not_ready_drop("stream_json");
        return ESP_ERR_INVALID_STATE;
    }
    if (json == NULL || json_len == 0U ||
        json_len > ESP111_PROTOCOL_DEVICE_STREAM_MAX_BYTES) {
        record_stream_reject("invalid_size", json_len, ESP_ERR_INVALID_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    char *body = heap_caps_malloc(json_len + 1U, MALLOC_CAP_8BIT);
    if (body == NULL) {
        record_stream_reject("alloc_failed", json_len, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    memcpy(body, json, json_len);
    body[json_len] = '\0';
    if (strlen(body) != json_len) {
        heap_caps_free(body);
        record_stream_reject("embedded_nul", json_len, ESP_ERR_INVALID_ARG);
        return ESP_ERR_INVALID_ARG;
    }

    device_stream_frame_t frame = {0};
    if (!parse_frame(body, &frame)) {
        heap_caps_free(body);
        record_stream_reject("parse", json_len, ESP_ERR_INVALID_ARG);
        return ESP_ERR_INVALID_ARG;
    }
    heap_caps_free(body);
    if (!gateway_config_child_allowed(frame.device_id)) {
        record_stream_reject("not_allowed", json_len, ESP_ERR_NOT_ALLOWED);
        ESP_LOGD(TAG, "stream frame rejected device_id=%s reason=not_allowed", frame.device_id);
        return ESP_ERR_NOT_ALLOWED;
    }
    if (!timestamp_is_monotonic(&frame, peer_ip, json_len)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* stream gateway 到这里为止只完成 parse/monotonic/allowlist；业务处理交给 event bus worker。 */
    return enqueue_frame_event(&frame, peer_ip, received_at_us);
}

esp_err_t device_stream_gateway_process_json(const char *json,
                                             size_t json_len,
                                             const char *peer_ip)
{
    return process_json_at_us(json, json_len, peer_ip, esp_timer_get_time());
}

static esp_err_t enqueue_frame_at_us(const char *json,
                                     size_t json_len,
                                     const char *peer_ip,
                                     const char *source,
                                     int64_t received_at_us)
{
    if (json == NULL || json_len == 0U ||
        json_len > ESP111_PROTOCOL_DEVICE_STREAM_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    (void)source;
    return process_json_at_us(json, json_len, peer_ip, received_at_us);
}

esp_err_t device_stream_gateway_enqueue_frame(const char *json,
                                              size_t json_len,
                                              const char *peer_ip,
                                              const char *source)
{
    /* 入口只做 flat stream parse/allowlist/monotonic check，再提交 typed runtime event。 */
    esp_err_t ret = enqueue_frame_at_us(json,
                                        json_len,
                                        peer_ip,
                                        source,
                                        esp_timer_get_time());
    if (ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_NO_MEM) {
        s_queue_overflow_drop_count++;
    }
    return ret;
}

esp_err_t device_stream_gateway_enqueue_udp(const char *peer_ip,
                                            uint16_t peer_port,
                                            const void *payload,
                                            size_t payload_len,
                                            const char *source)
{
    if (peer_ip == NULL || peer_ip[0] == '\0' || peer_port == 0 ||
        payload == NULL || payload_len == 0U ||
        payload_len > ESP111_PROTOCOL_DEVICE_STREAM_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_stream_running || !gateway_wifi_is_net_ready()) {
        reset_stream_runtime(source != NULL ? source : "stream_udp_not_ready");
        record_net_not_ready_drop(source);
        return ESP_ERR_INVALID_STATE;
    }

    /* UDP send 也走 scheduler 队列，避免入口线程直接阻塞 socket send。 */
    esp_err_t ret = s3_scheduler_enqueue_stream_send(peer_ip,
                                                     peer_port,
                                                     payload,
                                                     payload_len,
                                                     source);
    if (ret != ESP_OK) {
        s_queue_overflow_drop_count++;
        ESP_LOGW(TAG,
                 "stream send enqueue failed source=%s STREAM_QUEUE_DEPTH=%u STREAM_QUEUE_OVERFLOW_DROP_COUNT=%lu ret=%s",
                 source != NULL ? source : "unknown",
                 (unsigned int)stream_queue_depth(),
                 (unsigned long)s_queue_overflow_drop_count,
                 esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t device_stream_gateway_send_udp_now(const char *peer_ip,
                                             uint16_t peer_port,
                                             const void *payload,
                                             size_t payload_len,
                                             const char *source)
{
    if (peer_ip == NULL || peer_ip[0] == '\0' || peer_port == 0 ||
        payload == NULL || payload_len == 0U ||
        payload_len > ESP111_PROTOCOL_DEVICE_STREAM_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_stream_running || !gateway_wifi_is_net_ready()) {
        reset_stream_runtime(source != NULL ? source : "stream_send_not_ready");
        record_net_not_ready_drop(source);
        return ESP_ERR_INVALID_STATE;
    }

    /* 只允许 stream worker 调用本函数；调用方已通过 scheduler 做过节奏控制。 */
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGW(TAG, "stream sender socket open failed source=%s", source != NULL ? source : "unknown");
        return ESP_FAIL;
    }
    struct timeval send_timeout = {
        .tv_sec = 1,
        .tv_usec = 0,
    };
    (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(peer_port);
    if (inet_pton(AF_INET, peer_ip, &dest.sin_addr) != 1) {
        close(sock);
        ESP_LOGW(TAG, "stream sender invalid peer_ip=%s source=%s", peer_ip, source != NULL ? source : "unknown");
        return ESP_ERR_INVALID_ARG;
    }

    int sent = sendto(sock,
                      payload,
                      payload_len,
                      0,
                      (const struct sockaddr *)&dest,
                      sizeof(dest));
    close(sock);
    if (sent < 0) {
        ESP_LOGW(TAG,
                 "stream sender send failed source=%s peer=%s:%u STREAM_QUEUE_DEPTH=%u",
                 source != NULL ? source : "unknown",
                 peer_ip,
                 (unsigned int)peer_port,
                 (unsigned int)stream_queue_depth());
        return ESP_FAIL;
    }
    ESP_LOGD(TAG,
             "stream sender sent source=%s peer=%s:%u bytes=%d STREAM_QUEUE_DEPTH=%u",
             source != NULL ? source : "unknown",
             peer_ip,
             (unsigned int)peer_port,
             sent,
             (unsigned int)stream_queue_depth());
    return ESP_OK;
}

esp_err_t device_stream_gateway_handle_http(httpd_req_t *req)
{
    const int64_t received_at_us = esp_timer_get_time();
    if (req == NULL || req->content_len <= 0 ||
        (size_t)req->content_len > ESP111_PROTOCOL_DEVICE_STREAM_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *body = heap_caps_calloc(1, (size_t)req->content_len + 1U, MALLOC_CAP_8BIT);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int remaining = req->content_len;
    int offset = 0;
    while (remaining > 0) {
        int read = httpd_req_recv(req, body + offset, remaining);
        if (read <= 0) {
            heap_caps_free(body);
            return read == HTTPD_SOCK_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
        offset += read;
        remaining -= read;
    }

    char peer_ip[16] = {0};
    int sock = httpd_req_to_sockfd(req);
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    if (sock >= 0 && getpeername(sock, (struct sockaddr *)&addr, &addr_len) == 0 &&
        addr.ss_family == AF_INET) {
        const struct sockaddr_in *addr_in = (const struct sockaddr_in *)&addr;
        (void)inet_ntop(AF_INET, &addr_in->sin_addr, peer_ip, sizeof(peer_ip));
    }

    /* HTTP fallback 与 UDP 入口走同一条 scheduler stream worker 路径。 */
    esp_err_t ret = enqueue_frame_at_us(body,
                                        (size_t)req->content_len,
                                        peer_ip,
                                        "stream_http",
                                        received_at_us);
    heap_caps_free(body);
    return ret;
}

static void udp_task(void *arg)
{
    (void)arg;
    const bool wdt_registered = app_task_wdt_add_current(TAG, "device_stream_gateway");
    app_stack_monitor_log(TAG, "device_stream_gateway", "entry");
    char *rx_buffer = heap_caps_malloc(ESP111_PROTOCOL_DEVICE_STREAM_MAX_BYTES + 1U, MALLOC_CAP_8BIT);
    while (rx_buffer == NULL) {
        ESP_LOGW(TAG, "stream UDP rx buffer allocation failed; retrying");
        app_task_wdt_delay_ms(wdt_registered, 2000U);
        rx_buffer = heap_caps_malloc(ESP111_PROTOCOL_DEVICE_STREAM_MAX_BYTES + 1U, MALLOC_CAP_8BIT);
    }
    while (1) {
        wait_until_net_ready("device_stream_udp", wdt_registered);
        /* 网络 gate ready 后才 bind；网络掉线时关闭 socket，下一轮重新创建。 */
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGW(TAG, "stream UDP socket open failed");
            app_task_wdt_delay_ms(wdt_registered, 1000U);
            continue;
        }

        struct timeval timeout = {
            .tv_sec = 1,
            .tv_usec = 0,
        };
        (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        struct sockaddr_in listen_addr;
        memset(&listen_addr, 0, sizeof(listen_addr));
        listen_addr.sin_family = AF_INET;
        listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        listen_addr.sin_port = htons(ESP111_PROTOCOL_DEVICE_STREAM_UDP_PORT);
        if (bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
            close(sock);
            ESP_LOGW(TAG, "stream UDP bind failed port=%u", ESP111_PROTOCOL_DEVICE_STREAM_UDP_PORT);
            app_task_wdt_delay_ms(wdt_registered, 1000U);
            continue;
        }

        ESP_LOGI(TAG, "stream UDP listener started port=%u", ESP111_PROTOCOL_DEVICE_STREAM_UDP_PORT);
        while (1) {
            if (!s_stream_running || !gateway_wifi_is_net_ready()) {
                close(sock);
                ESP_LOGW(TAG, "stream UDP listener paused: net not ready");
                reset_stream_runtime("udp_listener_paused");
                break;
            }
            struct sockaddr_in source_addr;
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock,
                               rx_buffer,
                               ESP111_PROTOCOL_DEVICE_STREAM_MAX_BYTES,
                               0,
                               (struct sockaddr *)&source_addr,
                               &socklen);
            if (len <= 0) {
                app_stack_monitor_log_periodic(TAG,
                                               "device_stream_gateway",
                                               &s_last_stack_log_ms,
                                               APP_STACK_MONITOR_INTERVAL_MS);
                app_heap_monitor_log_periodic(TAG,
                                              &s_last_heap_log_ms,
                                              APP_HEAP_MONITOR_INTERVAL_MS);
                app_task_wdt_reset_current(wdt_registered);
                continue;
            }
            if ((size_t)len > ESP111_PROTOCOL_DEVICE_STREAM_MAX_BYTES) {
                ESP_LOGW(TAG,
                         "stream UDP rejected oversize len=%d max=%u",
                         len,
                         ESP111_PROTOCOL_DEVICE_STREAM_MAX_BYTES);
                continue;
            }
            rx_buffer[len] = '\0';
            const int64_t received_at_us = esp_timer_get_time();
            char peer_ip[16] = {0};
            (void)inet_ntop(AF_INET, &source_addr.sin_addr, peer_ip, sizeof(peer_ip));
            /* UDP 收包任务只保留串口可读的失败摘要；详细 reject 由 parser 侧聚合。 */
            esp_err_t ret = enqueue_frame_at_us(rx_buffer,
                                                (size_t)len,
                                                peer_ip,
                                                "stream_udp",
                                                received_at_us);
            if (ret != ESP_OK) {
                log_udp_enqueue_failed(ret, (size_t)len, peer_ip);
            }
            app_stack_monitor_log_periodic(TAG,
                                           "device_stream_gateway",
                                           &s_last_stack_log_ms,
                                           APP_STACK_MONITOR_INTERVAL_MS);
            app_heap_monitor_log_periodic(TAG,
                                          &s_last_heap_log_ms,
                                          APP_HEAP_MONITOR_INTERVAL_MS);
            app_task_wdt_reset_current(wdt_registered);
        }
    }
}

esp_err_t device_stream_gateway_init(void)
{
    if (s_clock_lock == NULL) {
        s_clock_lock = xSemaphoreCreateMutex();
        if (s_clock_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_stream_running = false;
    return ESP_OK;
}

esp_err_t device_stream_gateway_start(void)
{
    if (s_clock_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_udp_task == NULL) {
        BaseType_t created = xTaskCreateWithCaps(udp_task,
                                                 "device_stream_udp",
                                                 DEVICE_STREAM_UDP_TASK_STACK,
                                                 NULL,
                                                 DEVICE_STREAM_UDP_TASK_PRIORITY,
                                                 &s_udp_task,
                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (created != pdPASS) {
            s_udp_task = NULL;
            return ESP_ERR_NO_MEM;
        }
        app_stack_monitor_log_task_created(TAG,
                                           "device_stream_udp",
                                           s_udp_task,
                                           DEVICE_STREAM_UDP_TASK_STACK);
    }
    s_stream_running = true;
    return ESP_OK;
}

void device_stream_gateway_stop(void)
{
    s_stream_running = false;
    reset_stream_runtime("stream_gateway_stop");
}

bool device_stream_gateway_is_running(void)
{
    return s_stream_running;
}

void device_stream_gateway_reset_timestamp_baseline(const char *device_id,
                                                    const char *reason)
{
    const bool reset_all = device_id == NULL || device_id[0] == '\0';
    if (s_clock_lock != NULL) {
        xSemaphoreTake(s_clock_lock, portMAX_DELAY);
    }
    if (reset_all) {
        memset(s_clocks, 0, sizeof(s_clocks));
    } else {
        for (size_t i = 0; i < DEVICE_STREAM_CLOCK_COUNT; ++i) {
            if (strcmp(s_clocks[i].device_id, device_id) == 0) {
                memset(&s_clocks[i], 0, sizeof(s_clocks[i]));
            }
        }
    }
    if (s_clock_lock != NULL) {
        xSemaphoreGive(s_clock_lock);
    }
    ESP_LOGI(TAG,
             "stream timestamp baseline reset reason=%s device_id=%s",
             reason != NULL ? reason : "unknown",
             reset_all ? "all" : device_id);
}

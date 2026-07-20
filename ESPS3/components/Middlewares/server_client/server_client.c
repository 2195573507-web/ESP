/**
 * @file server_client.c
 * @brief S3 网关访问 ESP-server 的完整协议客户端。
 *
 * 本文件属于 ESPS3 网关，负责把 protocol_adapter/sensor_aggregator/voice_proxy/
 * command_router 交来的完整请求发送到 ESP-server。它不暴露给 C5、不解析 C5 轻量
 * JSON、不推断 schema、不做 fallback parsing，也不实现本地 ASR/LLM/TTS，只转发
 * voice PCM 并回传响应数据。
 * 语音独占模式由调用方 gate 普通同步；本层确保每次失败、超时、EAGAIN/CONNECT 失败后
 * 都显式 close+cleanup，避免语音长连接期间 socket 泄漏或复用失败连接。
 */

#include "server_client.h"
#include "server_client_voice_eof.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_stack_monitor.h"
#include "esp111_protocol_common.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gateway_config.h"
#include "gateway_wifi.h"
#include "s3_scheduler.h"

static const char *TAG = "server_client";

#ifndef VOICE_TOTAL_TIMEOUT_MS
#define VOICE_TOTAL_TIMEOUT_MS 90000U
#endif

enum {
    SERVER_CLIENT_HTTP_CONNECT_TIMEOUT_MS = 3000,
    SERVER_CLIENT_HTTP_READ_TIMEOUT_MS = 3000,
    SERVER_CLIENT_HTTP_WRITE_TIMEOUT_MS = 3000,
    SERVER_CLIENT_SNAPSHOT_CONNECT_TIMEOUT_MS = 2000,
    SERVER_CLIENT_SNAPSHOT_REQUEST_TIMEOUT_MS = 3000,
    SERVER_CLIENT_PROBE_TIMEOUT_MS = 1500,
    SERVER_CLIENT_UPLOAD_INCOMPLETE_LOG_MS = 5000,
    SERVER_CLIENT_VOICE_TOTAL_TIMEOUT_MS = VOICE_REQUEST_TIMEOUT_MS,
    SERVER_CLIENT_VOICE_IO_TIMEOUT_MS = VOICE_REQUEST_TIMEOUT_MS,
    SERVER_CLIENT_HTTP_SLOT_WAIT_MS = 500,
    SERVER_CLIENT_HTTP_SLOT_VOICE_WAIT_MS = 5000,
    SERVER_CLIENT_HTTP_SLOT_BUSY_LOG_MS = 2000,
    SERVER_CLIENT_HTTP_DIAGNOSTIC_LOG_MS = 5000,
    SERVER_CLIENT_HTTP_MAX_INFLIGHT = 3,
    SERVER_CLIENT_CANCEL_POLL_MS = 100,
    SERVER_CLIENT_CANCEL_START_POLL_MS = 10,
    SERVER_CLIENT_CANCEL_START_WAIT_MS = SERVER_CLIENT_HTTP_CONNECT_TIMEOUT_MS,
    SERVER_CLIENT_DEVICE_ID_BYTES = 48,
    SERVER_CLIENT_ENDPOINT_BYTES = 160,
};

#define SERVER_CLIENT_PROBE_ENDPOINT "/api/dashboard/v1/overview"

static uint32_t s_request_seq;
static int64_t s_last_upload_incomplete_log_ms;
static StaticSemaphore_t s_http_core_slot_storage;
static StaticSemaphore_t s_http_telemetry_slot_storage;
static StaticSemaphore_t s_http_snapshot_slot_storage;
static SemaphoreHandle_t s_http_core_slot;
static SemaphoreHandle_t s_http_telemetry_slot;
static SemaphoreHandle_t s_http_snapshot_slot;
static portMUX_TYPE s_http_scheduler_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_http_core_inflight;
static uint32_t s_http_telemetry_inflight;
static uint32_t s_http_snapshot_inflight;
static uint32_t s_http_core_wait_count;
static uint32_t s_http_core_waiting;
static uint32_t s_http_telemetry_drop_count;
static uint32_t s_http_slot_busy_count;
static int64_t s_last_http_scheduler_log_ms;
static int64_t s_last_http_request_latency_ms;
static int64_t s_last_http_queue_wait_ms;
static StaticSemaphore_t s_active_request_lock_storage;
static SemaphoreHandle_t s_active_request_lock;
static portMUX_TYPE s_active_request_lock_mux = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_epoch_request_lock_storage;
static SemaphoreHandle_t s_epoch_request_lock;
static portMUX_TYPE s_epoch_request_lock_mux = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    char device_id[SERVER_CLIENT_DEVICE_ID_BYTES];
    uint32_t cancel_generation;
    esp_http_client_handle_t client;
    char endpoint[SERVER_CLIENT_ENDPOINT_BYTES];
} server_client_peer_request_t;

typedef struct {
    const char *device_id;
    uint32_t cancel_generation;
} server_client_request_scope_t;

static server_client_peer_request_t s_peer_requests[GATEWAY_CONFIG_MAX_CHILDREN];

typedef struct {
    esp_http_client_handle_t client;
    uint32_t epoch;
    char endpoint[SERVER_CLIENT_ENDPOINT_BYTES];
} server_client_epoch_request_t;

static server_client_epoch_request_t s_epoch_requests[SERVER_CLIENT_HTTP_MAX_INFLIGHT];

typedef struct {
    char *body;
    size_t body_size;
    size_t body_len;
    bool overflow;
} server_body_ctx_t;

typedef enum {
    SERVER_CLIENT_HTTP_CHANNEL_HIGH = 0,
    SERVER_CLIENT_HTTP_CHANNEL_MEDIUM,
    SERVER_CLIENT_HTTP_CHANNEL_LOW,
} server_client_http_channel_t;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool current_task_wdt_registered(void)
{
    return esp_task_wdt_status(NULL) == ESP_OK;
}

static bool server_link_ready(void)
{
    return gateway_wifi_is_net_ready() && gateway_wifi_is_sta_ip_ready() &&
           gateway_wifi_get_sta_network_epoch() != 0U &&
           s3_scheduler_is_server_upload_allowed();
}

static bool server_probe_link_ready(void)
{
    return gateway_wifi_is_sta_ip_ready() && gateway_wifi_get_sta_network_epoch() != 0U;
}

static const char *http_channel_name(server_client_http_channel_t channel)
{
    switch (channel) {
    case SERVER_CLIENT_HTTP_CHANNEL_HIGH:
        return "HIGH_PRIORITY";
    case SERVER_CLIENT_HTTP_CHANNEL_MEDIUM:
        return "MEDIUM_PRIORITY";
    case SERVER_CLIENT_HTTP_CHANNEL_LOW:
        return "LOW_PRIORITY";
    default:
        return "unknown";
    }
}

static SemaphoreHandle_t http_channel_slot_handle(server_client_http_channel_t channel)
{
    SemaphoreHandle_t *slot;
    StaticSemaphore_t *storage;
    switch (channel) {
    case SERVER_CLIENT_HTTP_CHANNEL_HIGH:
        slot = &s_http_core_slot;
        storage = &s_http_core_slot_storage;
        break;
    case SERVER_CLIENT_HTTP_CHANNEL_MEDIUM:
        slot = &s_http_telemetry_slot;
        storage = &s_http_telemetry_slot_storage;
        break;
    case SERVER_CLIENT_HTTP_CHANNEL_LOW:
        slot = &s_http_snapshot_slot;
        storage = &s_http_snapshot_slot_storage;
        break;
    default:
        return NULL;
    }
    if (*slot == NULL) {
        portENTER_CRITICAL(&s_http_scheduler_mux);
        if (*slot == NULL) {
            *slot = xSemaphoreCreateMutexStatic(storage);
        }
        portEXIT_CRITICAL(&s_http_scheduler_mux);
    }
    return *slot;
}

static uint32_t next_request_sequence(void)
{
    portENTER_CRITICAL(&s_http_scheduler_mux);
    ++s_request_seq;
    if (s_request_seq == 0U) {
        s_request_seq = 1U;
    }
    const uint32_t sequence = s_request_seq;
    portEXIT_CRITICAL(&s_http_scheduler_mux);
    return sequence;
}

static void log_http_scheduler_diagnostics(const char *reason,
                                           const char *endpoint,
                                           server_client_http_channel_t channel,
                                           bool force)
{
    const int64_t timestamp_ms = now_ms();
    uint32_t core_inflight;
    uint32_t telemetry_inflight;
    uint32_t snapshot_inflight;
    uint32_t core_wait_count;
    uint32_t telemetry_drop_count;
    uint32_t slot_busy_count;
    int64_t request_latency_ms;
    int64_t queue_wait_ms;

    portENTER_CRITICAL(&s_http_scheduler_mux);
    const int64_t interval_ms = force ? SERVER_CLIENT_HTTP_SLOT_BUSY_LOG_MS :
                                        SERVER_CLIENT_HTTP_DIAGNOSTIC_LOG_MS;
    if (s_last_http_scheduler_log_ms != 0 &&
        timestamp_ms - s_last_http_scheduler_log_ms < interval_ms) {
        portEXIT_CRITICAL(&s_http_scheduler_mux);
        return;
    }
    s_last_http_scheduler_log_ms = timestamp_ms;
    core_inflight = s_http_core_inflight;
    telemetry_inflight = s_http_telemetry_inflight;
    snapshot_inflight = s_http_snapshot_inflight;
    core_wait_count = s_http_core_wait_count;
    telemetry_drop_count = s_http_telemetry_drop_count;
    slot_busy_count = s_http_slot_busy_count;
    request_latency_ms = s_last_http_request_latency_ms;
    queue_wait_ms = s_last_http_queue_wait_ms;
    portEXIT_CRITICAL(&s_http_scheduler_mux);

    ESP_LOGI(TAG,
             "http scheduler reason=%s channel=%s endpoint=%s http_core_inflight=%lu http_telemetry_inflight=%lu http_snapshot_inflight=%lu core_wait_count=%lu telemetry_drop_count=%lu slot_busy_count=%lu request_latency_ms=%lld queue_wait_time=%lld max_inflight=%u",
             reason != NULL ? reason : "-",
             http_channel_name(channel),
             endpoint != NULL ? endpoint : "-",
             (unsigned long)core_inflight,
             (unsigned long)telemetry_inflight,
             (unsigned long)snapshot_inflight,
             (unsigned long)core_wait_count,
             (unsigned long)telemetry_drop_count,
             (unsigned long)slot_busy_count,
             (long long)request_latency_ms,
             (long long)queue_wait_ms,
             (unsigned int)SERVER_CLIENT_HTTP_MAX_INFLIGHT);
}

static uint32_t mark_http_channel_acquired(server_client_http_channel_t channel)
{
    portENTER_CRITICAL(&s_http_scheduler_mux);
    if (channel == SERVER_CLIENT_HTTP_CHANNEL_HIGH) {
        ++s_http_core_inflight;
    } else if (channel == SERVER_CLIENT_HTTP_CHANNEL_MEDIUM) {
        ++s_http_telemetry_inflight;
    } else {
        ++s_http_snapshot_inflight;
    }
    const uint32_t active_count = s_http_core_inflight + s_http_telemetry_inflight +
                                  s_http_snapshot_inflight;
    portEXIT_CRITICAL(&s_http_scheduler_mux);
    return active_count;
}

static uint32_t mark_http_channel_released(server_client_http_channel_t channel,
                                           int64_t request_latency_ms)
{
    portENTER_CRITICAL(&s_http_scheduler_mux);
    if (channel == SERVER_CLIENT_HTTP_CHANNEL_HIGH) {
        if (s_http_core_inflight > 0U) {
            --s_http_core_inflight;
        }
    } else if (channel == SERVER_CLIENT_HTTP_CHANNEL_MEDIUM &&
               s_http_telemetry_inflight > 0U) {
        --s_http_telemetry_inflight;
    } else if (channel == SERVER_CLIENT_HTTP_CHANNEL_LOW &&
               s_http_snapshot_inflight > 0U) {
        --s_http_snapshot_inflight;
    }
    s_last_http_request_latency_ms = request_latency_ms;
    const uint32_t active_count = s_http_core_inflight + s_http_telemetry_inflight +
                                  s_http_snapshot_inflight;
    portEXIT_CRITICAL(&s_http_scheduler_mux);
    return active_count;
}

static esp_err_t take_http_channel_slot(server_client_http_channel_t channel,
                                        const char *endpoint,
                                        const char *phase,
                                        uint32_t wait_ms)
{
    const int64_t wait_started_ms = now_ms();
    SemaphoreHandle_t slot = http_channel_slot_handle(channel);
    if (slot == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (channel == SERVER_CLIENT_HTTP_CHANNEL_LOW) {
        bool low_must_drop;
        portENTER_CRITICAL(&s_http_scheduler_mux);
        low_must_drop = s_http_core_inflight > 0U || s_http_core_waiting > 0U ||
                        s_http_telemetry_inflight > 0U;
        if (low_must_drop) {
            ++s_http_slot_busy_count;
        }
        portEXIT_CRITICAL(&s_http_scheduler_mux);
        if (low_must_drop) {
            return ESP_ERR_HTTP_EAGAIN;
        }
    }

    if (xSemaphoreTake(slot, 0) == pdTRUE) {
        portENTER_CRITICAL(&s_http_scheduler_mux);
        s_last_http_queue_wait_ms = now_ms() - wait_started_ms;
        portEXIT_CRITICAL(&s_http_scheduler_mux);
        const uint32_t active_count = mark_http_channel_acquired(channel);
        ESP_LOGI(TAG,
                 "HTTP_RESOURCE_ACQUIRE request_type=%s priority=%s active_count=%lu",
                 endpoint != NULL ? endpoint : "-",
                 http_channel_name(channel),
                 (unsigned long)active_count);
        log_http_scheduler_diagnostics("request_start", endpoint, channel, false);
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_http_scheduler_mux);
    ++s_http_slot_busy_count;
    if (channel == SERVER_CLIENT_HTTP_CHANNEL_HIGH) {
        ++s_http_core_wait_count;
        ++s_http_core_waiting;
    } else if (channel == SERVER_CLIENT_HTTP_CHANNEL_MEDIUM) {
        ++s_http_telemetry_drop_count;
    }
    portEXIT_CRITICAL(&s_http_scheduler_mux);

    if (channel == SERVER_CLIENT_HTTP_CHANNEL_LOW || wait_ms == 0U) {
        if (channel == SERVER_CLIENT_HTTP_CHANNEL_HIGH) {
            portENTER_CRITICAL(&s_http_scheduler_mux);
            if (s_http_core_waiting > 0U) {
                --s_http_core_waiting;
            }
            portEXIT_CRITICAL(&s_http_scheduler_mux);
        }
        if (channel != SERVER_CLIENT_HTTP_CHANNEL_LOW) {
            log_http_scheduler_diagnostics("resource_slot_busy", endpoint, channel, true);
        }
        return channel == SERVER_CLIENT_HTTP_CHANNEL_LOW ? ESP_ERR_HTTP_EAGAIN : ESP_ERR_TIMEOUT;
    }

    BaseType_t acquired = xSemaphoreTake(slot, pdMS_TO_TICKS(wait_ms));
    portENTER_CRITICAL(&s_http_scheduler_mux);
    if (s_http_core_waiting > 0U) {
        --s_http_core_waiting;
    }
    portEXIT_CRITICAL(&s_http_scheduler_mux);
    if (acquired == pdTRUE) {
        portENTER_CRITICAL(&s_http_scheduler_mux);
        s_last_http_queue_wait_ms = now_ms() - wait_started_ms;
        portEXIT_CRITICAL(&s_http_scheduler_mux);
        const uint32_t active_count = mark_http_channel_acquired(channel);
        ESP_LOGI(TAG,
                 "HTTP_RESOURCE_ACQUIRE request_type=%s priority=%s active_count=%lu",
                 endpoint != NULL ? endpoint : "-",
                 http_channel_name(channel),
                 (unsigned long)active_count);
        log_http_scheduler_diagnostics("request_start", endpoint, channel, false);
        return ESP_OK;
    }

    log_http_scheduler_diagnostics("core_slot_busy", endpoint, channel, true);
    ESP_LOGW(TAG,
             "server http client busy channel=%s endpoint=%s phase=%s wait_ms=%u reason=socket_slot_occupied",
             http_channel_name(channel),
             endpoint != NULL ? endpoint : "-",
             phase != NULL ? phase : "-",
             (unsigned int)wait_ms);
    return ESP_ERR_TIMEOUT;
}

bool server_client_snapshot_upload_should_skip(void)
{
    bool skip;
    portENTER_CRITICAL(&s_http_scheduler_mux);
    skip = s_http_core_inflight > 0U || s_http_core_waiting > 0U ||
           s_http_telemetry_inflight > 0U;
    portEXIT_CRITICAL(&s_http_scheduler_mux);
    return skip;
}

static void give_http_channel_slot(server_client_http_channel_t channel,
                                  int64_t request_started_ms,
                                  const char *request_type,
                                  esp_err_t result)
{
    SemaphoreHandle_t slot = http_channel_slot_handle(channel);
    if (slot != NULL) {
        const int64_t latency_ms = request_started_ms > 0 ? now_ms() - request_started_ms : 0;
        const uint32_t active_count = mark_http_channel_released(channel, latency_ms);
        xSemaphoreGive(slot);
        ESP_LOGI(TAG,
                 "HTTP_RESOURCE_RELEASE request_type=%s duration_ms=%lld result=%s active_count=%lu",
                 request_type != NULL ? request_type : "-",
                 (long long)latency_ms,
                 esp_err_to_name(result),
                 (unsigned long)active_count);
        log_http_scheduler_diagnostics("request_complete", NULL, channel, false);
    }
}

static SemaphoreHandle_t active_request_lock_handle(void)
{
    if (s_active_request_lock == NULL) {
        portENTER_CRITICAL(&s_active_request_lock_mux);
        if (s_active_request_lock == NULL) {
            s_active_request_lock =
                xSemaphoreCreateMutexStatic(&s_active_request_lock_storage);
        }
        portEXIT_CRITICAL(&s_active_request_lock_mux);
    }
    return s_active_request_lock;
}

static SemaphoreHandle_t epoch_request_lock_handle(void)
{
    if (s_epoch_request_lock == NULL) {
        portENTER_CRITICAL(&s_epoch_request_lock_mux);
        if (s_epoch_request_lock == NULL) {
            s_epoch_request_lock = xSemaphoreCreateMutexStatic(&s_epoch_request_lock_storage);
        }
        portEXIT_CRITICAL(&s_epoch_request_lock_mux);
    }
    return s_epoch_request_lock;
}

static bool network_epoch_current(uint32_t epoch)
{
    return epoch != 0U && gateway_wifi_is_sta_ip_ready() &&
           gateway_wifi_get_sta_network_epoch() == epoch;
}

static esp_err_t register_epoch_request(esp_http_client_handle_t client,
                                        uint32_t epoch,
                                        const char *endpoint)
{
    if (client == NULL || !network_epoch_current(epoch)) {
        return ESP_ERR_INVALID_STATE;
    }

    SemaphoreHandle_t lock = epoch_request_lock_handle();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    server_client_epoch_request_t *empty = NULL;
    for (size_t i = 0; i < SERVER_CLIENT_HTTP_MAX_INFLIGHT; ++i) {
        if (s_epoch_requests[i].client == client) {
            xSemaphoreGive(lock);
            return ESP_OK;
        }
        if (s_epoch_requests[i].client == NULL && empty == NULL) {
            empty = &s_epoch_requests[i];
        }
    }
    if (empty != NULL) {
        empty->client = client;
        empty->epoch = epoch;
        strlcpy(empty->endpoint, endpoint != NULL ? endpoint : "-", sizeof(empty->endpoint));
    }
    xSemaphoreGive(lock);
    return empty != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void unregister_epoch_request(esp_http_client_handle_t client)
{
    if (client == NULL) {
        return;
    }
    SemaphoreHandle_t lock = epoch_request_lock_handle();
    if (lock == NULL) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    for (size_t i = 0; i < SERVER_CLIENT_HTTP_MAX_INFLIGHT; ++i) {
        if (s_epoch_requests[i].client == client) {
            memset(&s_epoch_requests[i], 0, sizeof(s_epoch_requests[i]));
            break;
        }
    }
    xSemaphoreGive(lock);
}

void server_client_on_network_epoch_changed(uint32_t current_epoch, const char *reason)
{
    SemaphoreHandle_t lock = epoch_request_lock_handle();
    if (lock == NULL) {
        ESP_LOGW(TAG, "NETWORK_EPOCH abort skipped reason=%s lock=unavailable",
                 reason != NULL ? reason : "unknown");
        return;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    for (size_t i = 0; i < SERVER_CLIENT_HTTP_MAX_INFLIGHT; ++i) {
        server_client_epoch_request_t *entry = &s_epoch_requests[i];
        if (entry->client == NULL ||
            (current_epoch != 0U && entry->epoch == current_epoch)) {
            continue;
        }

        esp_err_t ret = esp_http_client_cancel_request(entry->client);
        ESP_LOGW(TAG,
                 "NETWORK_EPOCH abort epoch_id=%lu request_epoch=%lu endpoint=%s reason=%s ret=%s",
                 (unsigned long)current_epoch,
                 (unsigned long)entry->epoch,
                 entry->endpoint[0] != '\0' ? entry->endpoint : "-",
                 reason != NULL ? reason : "unknown",
                 esp_err_to_name(ret));
    }
    xSemaphoreGive(lock);
}

static server_client_peer_request_t *find_peer_request_locked(const char *device_id,
                                                              bool create)
{
    server_client_peer_request_t *empty = NULL;
    for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN; ++i) {
        server_client_peer_request_t *entry = &s_peer_requests[i];
        if (entry->device_id[0] == '\0') {
            if (empty == NULL) {
                empty = entry;
            }
            continue;
        }
        if (strcmp(entry->device_id, device_id) == 0) {
            return entry;
        }
    }
    if (!create || empty == NULL) {
        return NULL;
    }
    strlcpy(empty->device_id, device_id, sizeof(empty->device_id));
    empty->cancel_generation = 1U;
    return empty;
}

static esp_err_t snapshot_peer_cancel_generation(const char *device_id,
                                                 uint32_t *out_generation)
{
    if (device_id == NULL || device_id[0] == '\0' ||
        strlen(device_id) >= SERVER_CLIENT_DEVICE_ID_BYTES || out_generation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    SemaphoreHandle_t lock = active_request_lock_handle();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    server_client_peer_request_t *entry = find_peer_request_locked(device_id, true);
    if (entry != NULL) {
        *out_generation = entry->cancel_generation;
    }
    xSemaphoreGive(lock);
    return entry != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t register_active_peer_request(const server_client_request_scope_t *scope,
                                              esp_http_client_handle_t client,
                                              const char *endpoint)
{
    if (scope == NULL) {
        return ESP_OK;
    }
    SemaphoreHandle_t lock = active_request_lock_handle();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    server_client_peer_request_t *entry = find_peer_request_locked(scope->device_id, false);
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (entry != NULL && entry->cancel_generation == scope->cancel_generation &&
        entry->client == NULL) {
        entry->client = client;
        strlcpy(entry->endpoint, endpoint, sizeof(entry->endpoint));
        ret = ESP_OK;
    }
    xSemaphoreGive(lock);
    return ret;
}

static bool unregister_active_peer_request(const server_client_request_scope_t *scope,
                                           esp_http_client_handle_t client)
{
    if (scope == NULL) {
        return false;
    }
    SemaphoreHandle_t lock = active_request_lock_handle();
    if (lock == NULL) {
        return true;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    server_client_peer_request_t *entry = find_peer_request_locked(scope->device_id, false);
    const bool cancelled = entry == NULL ||
                           entry->cancel_generation != scope->cancel_generation;
    if (entry != NULL && entry->client == client) {
        entry->client = NULL;
        entry->endpoint[0] = '\0';
    }
    xSemaphoreGive(lock);
    return cancelled;
}

static bool is_upload_request(esp_http_client_method_t method, const char *json_body)
{
    return json_body != NULL && method == HTTP_METHOD_POST;
}

static bool upload_incomplete_can_be_soft_success(esp_http_client_method_t method,
                                                  const char *json_body,
                                                  esp_err_t ret,
                                                  int status)
{
    return is_upload_request(method, json_body) &&
           ret == ESP_ERR_HTTP_INCOMPLETE_DATA &&
           status >= 200 && status < 300;
}

static void log_upload_incomplete_soft_success(const char *endpoint, int status)
{
    const int64_t timestamp_ms = now_ms();
    if (s_last_upload_incomplete_log_ms != 0 &&
        timestamp_ms - s_last_upload_incomplete_log_ms <
            SERVER_CLIENT_UPLOAD_INCOMPLETE_LOG_MS) {
        return;
    }
    s_last_upload_incomplete_log_ms = timestamp_ms;
    ESP_LOGW(TAG,
             "server upload degraded_success endpoint=%s status=%d ret=%s reason=response_body_incomplete",
             endpoint != NULL ? endpoint : "-",
             status,
             esp_err_to_name(ESP_ERR_HTTP_INCOMPLETE_DATA));
}

static const char *voice_http_error_reason(esp_err_t ret, int status)
{
    if (status >= 500) {
        return "server_unavailable";
    }
    if (status >= 400) {
        return "server_rejected";
    }

    switch (ret) {
    case ESP_OK:
        return "none";
    case ESP_ERR_INVALID_STATE:
        return "server_link_not_ready";
    case ESP_ERR_TIMEOUT:
        return "timeout";
    case ESP_ERR_HTTP_CONNECT:
        return "connect_failed";
    case ESP_ERR_HTTP_EAGAIN:
        return "eagain";
    case ESP_ERR_HTTP_CONNECTION_CLOSED:
        return "connection_closed";
    case ESP_ERR_HTTP_FETCH_HEADER:
        return "fetch_header_failed";
    case ESP_ERR_INVALID_RESPONSE:
        return "bad_status";
    default:
        return "transport_error";
    }
}

static esp_err_t voice_fetch_headers_error_to_ret(int64_t header_ret)
{
    if (header_ret == -ESP_ERR_HTTP_EAGAIN) {
        return ESP_ERR_HTTP_EAGAIN;
    }
    if (header_ret == -ESP_ERR_HTTP_CONNECTION_CLOSED) {
        return ESP_ERR_HTTP_CONNECTION_CLOSED;
    }
    if (header_ret == -ESP_ERR_HTTP_CONNECT) {
        return ESP_ERR_HTTP_CONNECT;
    }
    if (header_ret == -ESP_ERR_TIMEOUT) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_ERR_HTTP_FETCH_HEADER;
}

static esp_err_t build_url(const char *endpoint, char *out, size_t out_size)
{
    if (endpoint == NULL || endpoint[0] == '\0' || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *base = gateway_config_get()->server_base_url;
    size_t base_len = strlen(base);
    while (base_len > 0 && base[base_len - 1] == '/') {
        base_len--;
    }

    int written = snprintf(out,
                           out_size,
                           "%.*s%s%s",
                           (int)base_len,
                           base,
                           endpoint[0] == '/' ? "" : "/",
                           endpoint);
    return written > 0 && written < (int)out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t body_event_handler(esp_http_client_event_t *evt)
{
    if (evt == NULL || evt->event_id != HTTP_EVENT_ON_DATA || evt->data == NULL ||
        evt->data_len <= 0 || evt->user_data == NULL) {
        return ESP_OK;
    }

    server_body_ctx_t *ctx = (server_body_ctx_t *)evt->user_data;
    if (ctx->body == NULL || ctx->body_size == 0) {
        return ESP_OK;
    }

    size_t usable = ctx->body_size > 0 ? ctx->body_size - 1U : 0U;
    size_t remain = usable > ctx->body_len ? usable - ctx->body_len : 0U;
    size_t copy_len = (size_t)evt->data_len <= remain ? (size_t)evt->data_len : remain;
    if (copy_len > 0) {
        memcpy(ctx->body + ctx->body_len, evt->data, copy_len);
        ctx->body_len += copy_len;
        ctx->body[ctx->body_len] = '\0';
    }
    if ((size_t)evt->data_len > copy_len) {
        ctx->overflow = true;
    }
    return ESP_OK;
}

static bool request_cancelled(server_client_cancel_cb_t cancel_cb, void *cancel_ctx);

static esp_err_t snapshot_set_remaining_timeout(esp_http_client_handle_t client,
                                                int64_t deadline_ms)
{
    const int64_t remaining_ms = deadline_ms - now_ms();
    if (remaining_ms <= 0) {
        return ESP_ERR_TIMEOUT;
    }
    const int timeout_ms = remaining_ms < SERVER_CLIENT_SNAPSHOT_REQUEST_TIMEOUT_MS ?
                               (int)remaining_ms :
                               SERVER_CLIENT_SNAPSHOT_REQUEST_TIMEOUT_MS;
    return esp_http_client_set_timeout_ms(client, timeout_ms);
}

static esp_err_t read_snapshot_response(esp_http_client_handle_t client, int64_t deadline_ms)
{
    char response_chunk[256];
    const bool wdt_registered = current_task_wdt_registered();

    while (!esp_http_client_is_complete_data_received(client)) {
        esp_err_t ret = snapshot_set_remaining_timeout(client, deadline_ms);
        if (ret != ESP_OK) {
            return ret;
        }
        app_task_wdt_reset_current(wdt_registered);
        const int read_len = esp_http_client_read(client, response_chunk, sizeof(response_chunk));
        if (read_len > 0) {
            continue;
        }
        if (read_len == 0 || read_len == -ESP_ERR_HTTP_EAGAIN) {
            return ESP_ERR_TIMEOUT;
        }
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t perform_snapshot_http_request(esp_http_client_handle_t client,
                                               const char *json_body)
{
    if (client == NULL || json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t json_len = strlen(json_body);
    if (json_len > INT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    const bool wdt_registered = current_task_wdt_registered();
    const int64_t deadline_ms = now_ms() + SERVER_CLIENT_SNAPSHOT_REQUEST_TIMEOUT_MS;
    esp_err_t ret = esp_http_client_set_timeout_ms(client,
                                                    SERVER_CLIENT_SNAPSHOT_CONNECT_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return ret;
    }

    app_task_wdt_reset_current(wdt_registered);
    ret = esp_http_client_open(client, (int)json_len);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t written = 0;
    while (written < json_len) {
        ret = snapshot_set_remaining_timeout(client, deadline_ms);
        if (ret != ESP_OK) {
            return ret;
        }
        app_task_wdt_reset_current(wdt_registered);
        const int write_len = esp_http_client_write(client,
                                                     json_body + written,
                                                     (int)(json_len - written));
        if (write_len <= 0) {
            return write_len == 0 ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
        written += (size_t)write_len;
    }

    ret = snapshot_set_remaining_timeout(client, deadline_ms);
    if (ret != ESP_OK) {
        return ret;
    }
    app_task_wdt_reset_current(wdt_registered);
    const int64_t header_ret = esp_http_client_fetch_headers(client);
    if (header_ret < 0) {
        return header_ret == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT :
                                                     ESP_ERR_HTTP_FETCH_HEADER;
    }

    if (esp_http_client_get_content_length(client) == 0 &&
        !esp_http_client_is_chunked_response(client)) {
        return ESP_OK;
    }
    return read_snapshot_response(client, deadline_ms);
}

static esp_err_t perform_json_once(esp_http_client_method_t method,
                                   const char *endpoint,
                                   const char *json_body,
                                   char *response_body,
                                   size_t response_body_size,
                                   int *http_status,
                                   bool *slot_busy,
                                   server_client_http_channel_t channel,
                                   const server_client_request_scope_t *scope,
                                   server_client_cancel_cb_t cancel_cb,
                                   void *cancel_ctx)
{
    if (slot_busy != NULL) {
        *slot_busy = false;
    }
    const uint32_t request_epoch = gateway_wifi_get_sta_network_epoch();
    if (!server_link_ready() || !network_epoch_current(request_epoch)) {
        if (http_status != NULL) {
            *http_status = 0;
        }
        return ESP_ERR_INVALID_STATE;
    }

    char url[256];
    esp_err_t ret = build_url(endpoint, url, sizeof(url));
    if (ret != ESP_OK) {
        return ret;
    }

    if (response_body != NULL && response_body_size > 0) {
        response_body[0] = '\0';
    }
    if (http_status != NULL) {
        *http_status = 0;
    }

    server_body_ctx_t body_ctx = {
        .body = response_body,
        .body_size = response_body_size,
    };

    const bool snapshot_request = channel == SERVER_CLIENT_HTTP_CHANNEL_LOW;
    const uint32_t slot_wait_ms = !snapshot_request ?
                                      SERVER_CLIENT_HTTP_SLOT_WAIT_MS :
                                      0U;
    ret = take_http_channel_slot(channel, endpoint, "json", slot_wait_ms);
    if (ret != ESP_OK) {
        if (slot_busy != NULL &&
            (ret == ESP_ERR_TIMEOUT || (snapshot_request && ret == ESP_ERR_HTTP_EAGAIN))) {
            *slot_busy = true;
        }
        return ret;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = snapshot_request ? SERVER_CLIENT_SNAPSHOT_CONNECT_TIMEOUT_MS :
                                        SERVER_CLIENT_HTTP_CONNECT_TIMEOUT_MS,
        .event_handler = body_event_handler,
        .user_data = &body_ctx,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        give_http_channel_slot(channel, 0, endpoint, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    const int64_t request_started_ms = now_ms();
    bool epoch_request_registered = false;
    ret = register_epoch_request(client, request_epoch, endpoint);
    if (ret == ESP_OK) {
        epoch_request_registered = true;
    }

    if (ret == ESP_OK) {
        ret = esp_http_client_set_header(client, "X-Gateway-Id", gateway_config_get()->gateway_id);
    }
    if (ret == ESP_OK && gateway_config_get()->auth_token != NULL &&
        gateway_config_get()->auth_token[0] != '\0') {
        ret = esp_http_client_set_header(client,
                                         "X-Gateway-Token",
                                         gateway_config_get()->auth_token);
    }
    if (ret == ESP_OK) {
        ret = esp_http_client_set_header(client,
                                         "X-Device-Id",
                                         gateway_config_get()->gateway_id);
    }
    if (ret == ESP_OK) {
        ret = esp_http_client_set_header(client,
                                         "X-Schema-Version",
                                         ESP111_PROTOCOL_SCHEMA_VERSION_STRING);
    }
    if (ret == ESP_OK) {
        char seq[16];
        snprintf(seq, sizeof(seq), "%lu", (unsigned long)next_request_sequence());
        ret = esp_http_client_set_header(client, "X-Request-Seq", seq);
    }
    if (ret == ESP_OK) {
        ret = esp_http_client_set_header(client, "X-Time-Synced", "false");
    }
    if (ret == ESP_OK && json_body != NULL) {
        ret = esp_http_client_set_header(client, "Content-Type", "application/json");
    }
    if (ret == ESP_OK && json_body != NULL && !snapshot_request) {
        (void)esp_http_client_set_timeout_ms(client, SERVER_CLIENT_HTTP_WRITE_TIMEOUT_MS);
        ret = esp_http_client_set_post_field(client, json_body, strlen(json_body));
    }
    if (ret == ESP_OK && !snapshot_request && request_cancelled(cancel_cb, cancel_ctx)) {
        ret = ESP_ERR_INVALID_STATE;
    }
    bool peer_request_registered = false;
    if (ret == ESP_OK && !snapshot_request && scope != NULL) {
        ret = register_active_peer_request(scope, client, endpoint);
        peer_request_registered = ret == ESP_OK;
    }
    if (ret == ESP_OK && !network_epoch_current(request_epoch)) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK && snapshot_request) {
        ret = perform_snapshot_http_request(client, json_body);
    } else if (ret == ESP_OK) {
        (void)esp_http_client_set_timeout_ms(client, SERVER_CLIENT_HTTP_READ_TIMEOUT_MS);
        ret = esp_http_client_perform(client);
    }
    if (ret == ESP_OK && !network_epoch_current(request_epoch)) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (peer_request_registered && unregister_active_peer_request(scope, client)) {
        ret = ESP_ERR_INVALID_STATE;
    }

    int status = esp_http_client_get_status_code(client);
    if (http_status != NULL) {
        *http_status = status;
    }
    if (!snapshot_request && upload_incomplete_can_be_soft_success(method, json_body, ret, status)) {
        log_upload_incomplete_soft_success(endpoint, status);
        ret = ESP_OK;
    }
    if (ret == ESP_OK && (status < 200 || status >= 300)) {
        if (!snapshot_request) {
            ESP_LOGW(TAG,
                     "server rejected endpoint=%s status=%d body=%s",
                     endpoint,
                     status,
                     response_body != NULL && response_body[0] != '\0' ? response_body : "-");
        }
        ret = ESP_ERR_INVALID_RESPONSE;
    }
    if (ret == ESP_OK && body_ctx.overflow) {
        ret = ESP_ERR_INVALID_SIZE;
    }

    if (epoch_request_registered) {
        unregister_epoch_request(client);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    give_http_channel_slot(channel, request_started_ms, endpoint, ret);
    return ret;
}

static bool request_cancelled(server_client_cancel_cb_t cancel_cb, void *cancel_ctx)
{
    return cancel_cb != NULL && cancel_cb(cancel_ctx);
}

static esp_err_t wait_retry_backoff(uint32_t delay_ms,
                                    server_client_cancel_cb_t cancel_cb,
                                    void *cancel_ctx)
{
    uint32_t waited_ms = 0U;
    const bool wdt_registered = current_task_wdt_registered();
    while (waited_ms < delay_ms) {
        if (request_cancelled(cancel_cb, cancel_ctx)) {
            return ESP_ERR_INVALID_STATE;
        }
        const uint32_t remaining_ms = delay_ms - waited_ms;
        const uint32_t slice_ms = remaining_ms < SERVER_CLIENT_CANCEL_POLL_MS ?
                                      remaining_ms :
                                      SERVER_CLIENT_CANCEL_POLL_MS;
        app_task_wdt_delay_ms(wdt_registered, slice_ms);
        waited_ms += slice_ms;
    }
    return request_cancelled(cancel_cb, cancel_ctx) ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static esp_err_t perform_json_with_cancel(esp_http_client_method_t method,
                                          const char *endpoint,
                                          const char *json_body,
                                          char *response_body,
                                          size_t response_body_size,
                                          int *http_status,
                                          server_client_http_channel_t channel,
                                          const char *device_id,
                                          server_client_cancel_cb_t cancel_cb,
                                          void *cancel_ctx)
{
    if (endpoint == NULL || endpoint[0] == '\0') {
        if (http_status != NULL) {
            *http_status = 0;
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (http_status != NULL) {
        *http_status = 0;
    }

    server_client_request_scope_t scope = {0};
    const server_client_request_scope_t *scope_ptr = NULL;
    if (device_id != NULL) {
        esp_err_t scope_ret = snapshot_peer_cancel_generation(
            device_id,
            &scope.cancel_generation);
        if (scope_ret != ESP_OK) {
            return scope_ret;
        }
        scope.device_id = device_id;
        scope_ptr = &scope;
    }

    const uint32_t backoff_ms[] = {2000U, 5000U, 10000U, 30000U};
    esp_err_t ret = ESP_FAIL;
    int status = 0;
    const size_t retry_count = sizeof(backoff_ms) / sizeof(backoff_ms[0]);
    for (size_t attempt = 0; attempt <= retry_count; attempt++) {
        if (request_cancelled(cancel_cb, cancel_ctx)) {
            ret = ESP_ERR_INVALID_STATE;
            break;
        }
        app_task_wdt_reset_current(current_task_wdt_registered());
        ret = perform_json_once(method,
                                endpoint,
                                json_body,
                                response_body,
                                response_body_size,
                                &status,
                                NULL,
                                channel,
                                scope_ptr,
                                cancel_cb,
                                cancel_ctx);
        if (http_status != NULL) {
            *http_status = status;
        }
        if (ret == ESP_OK && status >= 200 && status < 300) {
            break;
        }
        if (request_cancelled(cancel_cb, cancel_ctx)) {
            ret = ESP_ERR_INVALID_STATE;
            break;
        }
        if (ret == ESP_ERR_INVALID_STATE) {
            break;
        }

        if (status >= 500 || ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_HTTP_CONNECT ||
            ret == ESP_ERR_HTTP_CONNECTION_CLOSED || ret == ESP_ERR_HTTP_EAGAIN) {
            if (attempt < retry_count) {
                ESP_LOGW(TAG,
                         "server request retry endpoint=%s attempt=%u next_backoff_ms=%u status=%d ret=%s",
                         endpoint,
                         (unsigned int)(attempt + 1U),
                         (unsigned int)backoff_ms[attempt],
                         status,
                         esp_err_to_name(ret));
                ret = wait_retry_backoff(backoff_ms[attempt], cancel_cb, cancel_ctx);
                if (ret != ESP_OK) {
                    break;
                }
                if (!server_link_ready()) {
                    ret = ESP_ERR_INVALID_STATE;
                    break;
                }
                continue;
            }
        }
        break;
    }

    return ret;
}

static esp_err_t perform_json(esp_http_client_method_t method,
                              const char *endpoint,
                              const char *json_body,
                              char *response_body,
                              size_t response_body_size,
                              int *http_status)
{
    return perform_json_with_cancel(method,
                                    endpoint,
                                    json_body,
                                    response_body,
                                    response_body_size,
                                    http_status,
                                    SERVER_CLIENT_HTTP_CHANNEL_HIGH,
                                    NULL,
                                    NULL,
                                    NULL);
}

static esp_err_t perform_telemetry_json(esp_http_client_method_t method,
                                        const char *endpoint,
                                        const char *json_body,
                                        char *response_body,
                                        size_t response_body_size,
                                        int *http_status)
{
    bool slot_busy = false;
    esp_err_t ret = perform_json_once(method,
                                      endpoint,
                                      json_body,
                                      response_body,
                                      response_body_size,
                                      http_status,
                                      &slot_busy,
                                      SERVER_CLIENT_HTTP_CHANNEL_MEDIUM,
                                      NULL,
                                      NULL,
                                      NULL);
    return slot_busy ? ESP_ERR_NOT_FINISHED : ret;
}

static esp_err_t perform_snapshot_json(esp_http_client_method_t method,
                                       const char *endpoint,
                                       const char *json_body,
                                       char *response_body,
                                       size_t response_body_size,
                                       int *http_status)
{
    if (server_client_snapshot_upload_should_skip()) {
        return ESP_ERR_HTTP_EAGAIN;
    }
    bool slot_busy = false;
    esp_err_t ret = perform_json_once(method,
                                      endpoint,
                                      json_body,
                                      response_body,
                                      response_body_size,
                                      http_status,
                                      &slot_busy,
                                      SERVER_CLIENT_HTTP_CHANNEL_LOW,
                                      NULL,
                                      NULL,
                                      NULL);
    return ret;
}

esp_err_t server_client_post_ingest_json(const char *json_body,
                                         char *response_body,
                                         size_t response_body_size,
                                         int *http_status)
{
    return server_client_post_ingest_json_cancellable(json_body,
                                                      response_body,
                                                      response_body_size,
                                                      http_status,
                                                      NULL,
                                                      NULL);
}

esp_err_t server_client_post_ingest_json_cancellable(const char *json_body,
                                                     char *response_body,
                                                     size_t response_body_size,
                                                     int *http_status,
                                                     server_client_cancel_cb_t cancel_cb,
                                                     void *cancel_ctx)
{
    if (json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return perform_json_with_cancel(HTTP_METHOD_POST,
                                    ESP111_PROTOCOL_SERVER_ROUTE_DEVICE_INGEST,
                                    json_body,
                                    response_body,
                                    response_body_size,
                                    http_status,
                                    SERVER_CLIENT_HTTP_CHANNEL_MEDIUM,
                                    NULL,
                                    cancel_cb,
                                    cancel_ctx);
}

esp_err_t server_client_post_ingest_json_cancellable_for_device(
    const char *device_id,
    const char *json_body,
    char *response_body,
    size_t response_body_size,
    int *http_status,
    server_client_cancel_cb_t cancel_cb,
    void *cancel_ctx)
{
    if (device_id == NULL || device_id[0] == '\0' || json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return perform_json_with_cancel(HTTP_METHOD_POST,
                                    ESP111_PROTOCOL_SERVER_ROUTE_DEVICE_INGEST,
                                    json_body,
                                    response_body,
                                    response_body_size,
                                    http_status,
                                    SERVER_CLIENT_HTTP_CHANNEL_MEDIUM,
                                    device_id,
                                    cancel_cb,
                                    cancel_ctx);
}

esp_err_t server_client_cancel_peer_requests(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0' ||
        strlen(device_id) >= SERVER_CLIENT_DEVICE_ID_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    SemaphoreHandle_t lock = active_request_lock_handle();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    server_client_peer_request_t *entry = find_peer_request_locked(device_id, true);
    if (entry == NULL) {
        xSemaphoreGive(lock);
        return ESP_ERR_NO_MEM;
    }

    ++entry->cancel_generation;
    if (entry->cancel_generation == 0U) {
        entry->cancel_generation = 1U;
    }
    esp_http_client_handle_t client = entry->client;
    if (client == NULL) {
        xSemaphoreGive(lock);
        return ESP_OK;
    }

    char endpoint[SERVER_CLIENT_ENDPOINT_BYTES];
    strlcpy(endpoint, entry->endpoint, sizeof(endpoint));
    const bool wdt_registered = current_task_wdt_registered();
    const int64_t deadline_ms = now_ms() + SERVER_CLIENT_CANCEL_START_WAIT_MS;
    esp_err_t ret;
    do {
        app_task_wdt_reset_current(wdt_registered);
        ret = esp_http_client_cancel_request(client);
        if (ret != ESP_ERR_INVALID_STATE || now_ms() >= deadline_ms) {
            break;
        }
        app_task_wdt_delay_ms(wdt_registered, SERVER_CLIENT_CANCEL_START_POLL_MS);
    } while (true);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "peer http request cancelled device_id=%s endpoint=%s",
                 device_id,
                 endpoint[0] != '\0' ? endpoint : "-");
    } else {
        ESP_LOGW(TAG,
                 "peer http request cancel incomplete device_id=%s endpoint=%s ret=%s",
                 device_id,
                 endpoint[0] != '\0' ? endpoint : "-",
                 esp_err_to_name(ret));
    }
    xSemaphoreGive(lock);
    return ret;
}

esp_err_t server_client_post_gateway_state_json(const char *json_body,
                                                char *response_body,
                                                size_t response_body_size,
                                                int *http_status)
{
    if (json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return perform_json(HTTP_METHOD_POST,
                        ESP111_PROTOCOL_SERVER_ROUTE_GATEWAY_STATE,
                        json_body,
                        response_body,
                        response_body_size,
                        http_status);
}

esp_err_t server_client_post_gateway_snapshot_json(const char *json_body,
                                                   char *response_body,
                                                   size_t response_body_size,
                                                   int *http_status)
{
    if (json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return perform_snapshot_json(HTTP_METHOD_POST,
                                 ESP111_PROTOCOL_SERVER_ROUTE_GATEWAY_STATE,
                                 json_body,
                                 response_body,
                                 response_body_size,
                                 http_status);
}

esp_err_t server_client_post_system_log_json(const char *json_body,
                                             char *response_body,
                                             size_t response_body_size,
                                             int *http_status)
{
    if (json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return perform_telemetry_json(HTTP_METHOD_POST,
                                  ESP111_PROTOCOL_SERVER_ROUTE_LOGS_SYSTEM,
                                  json_body,
                                  response_body,
                                  response_body_size,
                                  http_status);
}

esp_err_t server_client_post_alarm_json(const char *json_body,
                                        char *response_body,
                                        size_t response_body_size,
                                        int *http_status)
{
    if (json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return perform_telemetry_json(HTTP_METHOD_POST,
                                  ESP111_PROTOCOL_SERVER_ROUTE_LOGS_ALARMS,
                                  json_body,
                                  response_body,
                                  response_body_size,
                                  http_status);
}

esp_err_t server_client_post_habit_event_json(const char *json_body,
                                              char *response_body,
                                              size_t response_body_size,
                                              int *http_status)
{
    if (json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return perform_telemetry_json(HTTP_METHOD_POST,
                                  "/api/habit-events",
                                  json_body,
                                  response_body,
                                  response_body_size,
                                  http_status);
}

esp_err_t server_client_post_weather_refresh_json(const char *json_body,
                                                  char *response_body,
                                                  size_t response_body_size,
                                                  int *http_status)
{
    if (json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return perform_snapshot_json(HTTP_METHOD_POST,
                                 ESP111_PROTOCOL_SERVER_ROUTE_WEATHER_REFRESH,
                                 json_body,
                                 response_body,
                                 response_body_size,
                                 http_status);
}

esp_err_t server_client_probe_available(int *http_status)
{
    if (http_status != NULL) {
        *http_status = 0;
    }
    const uint32_t request_epoch = gateway_wifi_get_sta_network_epoch();
    if (!server_probe_link_ready() || !network_epoch_current(request_epoch)) {
        return ESP_ERR_INVALID_STATE;
    }

    char url[256];
    esp_err_t ret = build_url(SERVER_CLIENT_PROBE_ENDPOINT, url, sizeof(url));
    if (ret != ESP_OK) {
        return ret;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = SERVER_CLIENT_PROBE_TIMEOUT_MS,
        .keep_alive_enable = false,
    };
    ret = take_http_channel_slot(SERVER_CLIENT_HTTP_CHANNEL_HIGH,
                                 SERVER_CLIENT_PROBE_ENDPOINT,
                                 "probe",
                                 0U);
    if (ret != ESP_OK) {
        return ret == ESP_ERR_TIMEOUT ? ESP_ERR_NOT_FINISHED : ret;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        give_http_channel_slot(SERVER_CLIENT_HTTP_CHANNEL_HIGH,
                               0,
                               SERVER_CLIENT_PROBE_ENDPOINT,
                               ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    const int64_t request_started_ms = now_ms();
    bool epoch_request_registered = false;
    ret = register_epoch_request(client, request_epoch, SERVER_CLIENT_PROBE_ENDPOINT);
    if (ret == ESP_OK) {
        epoch_request_registered = true;
    }

    if (ret == ESP_OK) {
        ret = esp_http_client_set_header(client, "X-Gateway-Id", gateway_config_get()->gateway_id);
    }
    if (ret == ESP_OK && gateway_config_get()->auth_token != NULL &&
        gateway_config_get()->auth_token[0] != '\0') {
        ret = esp_http_client_set_header(client,
                                         "X-Gateway-Token",
                                         gateway_config_get()->auth_token);
    }
    if (ret == ESP_OK) {
        ret = esp_http_client_set_header(client,
                                         "X-Device-Id",
                                         gateway_config_get()->gateway_id);
    }
    if (ret == ESP_OK) {
        ret = esp_http_client_set_header(client,
                                         "X-Schema-Version",
                                         ESP111_PROTOCOL_SCHEMA_VERSION_STRING);
    }
    if (ret == ESP_OK) {
        char seq[16];
        snprintf(seq, sizeof(seq), "%lu", (unsigned long)next_request_sequence());
        ret = esp_http_client_set_header(client, "X-Request-Seq", seq);
    }
    if (ret == ESP_OK) {
        ret = esp_http_client_set_header(client, "X-Time-Synced", "false");
    }
    if (ret == ESP_OK && network_epoch_current(request_epoch)) {
        ret = esp_http_client_perform(client);
    } else if (ret == ESP_OK) {
        ret = ESP_ERR_INVALID_STATE;
    }

    if (ret == ESP_OK && !network_epoch_current(request_epoch)) {
        ret = ESP_ERR_INVALID_STATE;
    }

    int status = esp_http_client_get_status_code(client);
    if (http_status != NULL) {
        *http_status = status;
    }
    if (ret == ESP_OK && (status < 200 || status >= 300)) {
        ret = ESP_ERR_INVALID_RESPONSE;
    }

    if (epoch_request_registered) {
        unregister_epoch_request(client);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    give_http_channel_slot(SERVER_CLIENT_HTTP_CHANNEL_HIGH,
                           request_started_ms,
                           SERVER_CLIENT_PROBE_ENDPOINT,
                           ret);
    return ret;
}

esp_err_t server_client_get_smart_home_pending(char *response_body,
                                               size_t response_body_size,
                                               int *http_status)
{
    char endpoint[160];
    int written = snprintf(endpoint,
                           sizeof(endpoint),
                           ESP111_PROTOCOL_SERVER_ROUTE_SMART_HOME_PENDING
                           "?gateway_id=%s&limit=20",
                           gateway_config_get()->gateway_id);
    if (written <= 0 || written >= (int)sizeof(endpoint)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return perform_json(HTTP_METHOD_GET,
                        endpoint,
                        NULL,
                        response_body,
                        response_body_size,
                        http_status);
}

esp_err_t server_client_ack_smart_home_command(const char *command_id,
                                               const char *ack_json,
                                               char *response_body,
                                               size_t response_body_size,
                                               int *http_status)
{
    if (command_id == NULL || command_id[0] == '\0' || ack_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char endpoint[192];
    int written = snprintf(endpoint,
                           sizeof(endpoint),
                           ESP111_PROTOCOL_SERVER_ROUTE_SMART_HOME_COMMANDS_PREFIX "%s"
                           ESP111_PROTOCOL_SERVER_ROUTE_COMMAND_ACK_SUFFIX,
                           command_id);
    if (written <= 0 || written >= (int)sizeof(endpoint)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return perform_json(HTTP_METHOD_POST,
                        endpoint,
                        ack_json,
                        response_body,
                        response_body_size,
                        http_status);
}

esp_err_t server_client_get_pending_commands(const char *device_id,
                                             char *response_body,
                                             size_t response_body_size,
                                             int *http_status)
{
    return server_client_get_pending_commands_cancellable(device_id,
                                                          response_body,
                                                          response_body_size,
                                                          http_status,
                                                          NULL,
                                                          NULL);
}

esp_err_t server_client_get_pending_commands_cancellable(const char *device_id,
                                                         char *response_body,
                                                         size_t response_body_size,
                                                         int *http_status,
                                                         server_client_cancel_cb_t cancel_cb,
                                                         void *cancel_ctx)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char endpoint[128];
    int written = snprintf(endpoint,
                           sizeof(endpoint),
                           ESP111_PROTOCOL_SERVER_ROUTE_COMMANDS_PENDING "?device_id=%s",
                           device_id);
    if (written <= 0 || written >= (int)sizeof(endpoint)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return perform_json_with_cancel(HTTP_METHOD_GET,
                                    endpoint,
                                    NULL,
                                    response_body,
                                    response_body_size,
                                    http_status,
                                    SERVER_CLIENT_HTTP_CHANNEL_HIGH,
                                    device_id,
                                    cancel_cb,
                                    cancel_ctx);
}

esp_err_t server_client_ack_command(const char *command_id,
                                    const char *ack_json,
                                    char *response_body,
                                    size_t response_body_size,
                                    int *http_status)
{
    if (command_id == NULL || command_id[0] == '\0' || ack_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char endpoint[128];
    int written = snprintf(endpoint,
                           sizeof(endpoint),
                           ESP111_PROTOCOL_SERVER_ROUTE_COMMANDS_PREFIX "%s"
                           ESP111_PROTOCOL_SERVER_ROUTE_COMMAND_ACK_SUFFIX,
                           command_id);
    if (written <= 0 || written >= (int)sizeof(endpoint)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return perform_json(HTTP_METHOD_POST,
                        endpoint,
                        ack_json,
                        response_body,
                        response_body_size,
                        http_status);
}

static server_client_voice_eof_state_t response_eof_state(esp_http_client_handle_t client,
                                                           int64_t content_length,
                                                           size_t total_read,
                                                           bool zero_read)
{
    const bool transport_complete = content_length < 0 && client != NULL &&
                                    esp_http_client_is_complete_data_received(client);
    return server_client_voice_eof_state(content_length,
                                         total_read,
                                         transport_complete,
                                         zero_read);
}

static esp_err_t read_voice_response(esp_http_client_handle_t client,
                                     server_client_data_cb_t on_data,
                                     void *user_ctx,
                                     int64_t request_start_ms,
                                     uint32_t request_epoch,
                                     size_t *out_total_read,
                                     bool *out_upstream_closed)
{
    uint8_t buf[1024];
    size_t total_read = 0;
    int empty_reads = 0;
    int repeated_zero_reads = 0;
    int64_t content_length = esp_http_client_get_content_length(client);
    bool chunked = esp_http_client_is_chunked_response(client);
    server_client_voice_eof_state_t eof_state =
        response_eof_state(client, content_length, total_read, false);
    bool complete = eof_state == SERVER_CLIENT_VOICE_EOF_COMPLETE;
    bool known_length_mismatch = false;
    bool downstream_close_requested = false;
    esp_err_t downstream_close_ret = ESP_OK;
    int64_t first_byte_ms = -1;
    int64_t last_byte_ms = -1;
    esp_err_t ret = ESP_OK;

    if (out_total_read != NULL) {
        *out_total_read = 0;
    }
    if (out_upstream_closed != NULL) {
        *out_upstream_closed = false;
    }
    if (request_start_ms <= 0) {
        request_start_ms = now_ms();
    }

    while (!complete) {
        if (!network_epoch_current(request_epoch)) {
            ret = ESP_ERR_INVALID_STATE;
            break;
        }
        int64_t elapsed_ms = now_ms() - request_start_ms;
        if (elapsed_ms >= SERVER_CLIENT_VOICE_TOTAL_TIMEOUT_MS) {
            ESP_LOGW(TAG,
                     "voice upstream timeout/error stage=read_response ret=%s response_bytes=%u content_length=%lld timeout_ms=%u elapsed_ms=%lld",
                     esp_err_to_name(ESP_ERR_TIMEOUT),
                     (unsigned int)total_read,
                     (long long)content_length,
                     (unsigned int)SERVER_CLIENT_VOICE_TOTAL_TIMEOUT_MS,
                     (long long)elapsed_ms);
            ret = ESP_ERR_TIMEOUT;
            break;
        }
        app_task_wdt_reset_current(current_task_wdt_registered());
        int read_len = esp_http_client_read(client, (char *)buf, sizeof(buf));
        if (read_len > 0) {
            empty_reads = 0;
            total_read += (size_t)read_len;
            if (out_total_read != NULL) {
                *out_total_read = total_read;
            }
            int64_t response_received_ms = now_ms();
            last_byte_ms = response_received_ms - request_start_ms;
            if (first_byte_ms < 0) {
                first_byte_ms = last_byte_ms;
                ESP_LOGI(TAG,
                         "response received timestamp=%lld first_body_bytes=%d elapsed_ms=%lld",
                         (long long)response_received_ms,
                         read_len,
                         (long long)(response_received_ms - request_start_ms));
            }
            eof_state = response_eof_state(client, content_length, total_read, false);
            if (eof_state == SERVER_CLIENT_VOICE_EOF_OVERREAD) {
                known_length_mismatch = true;
                ESP_LOGW(TAG,
                         "voice response content-length overread total=%u content_length=%lld",
                         (unsigned int)total_read,
                         (long long)content_length);
                ret = ESP_ERR_INVALID_SIZE;
                break;
            }
            if (on_data != NULL) {
                ret = on_data(buf, (size_t)read_len, user_ctx);
                if (ret != ESP_OK) {
                    downstream_close_requested = true;
                    downstream_close_ret = esp_http_client_close(client);
                    if (out_upstream_closed != NULL && downstream_close_ret == ESP_OK) {
                        *out_upstream_closed = true;
                    }
                    ESP_LOGW(TAG,
                             "voice upstream closed after downstream send failure ret=%s close_ret=%s response_bytes=%u",
                             esp_err_to_name(ret),
                             esp_err_to_name(downstream_close_ret),
                             (unsigned int)total_read);
                    break;
                }
            }
            complete = eof_state == SERVER_CLIENT_VOICE_EOF_COMPLETE;
            continue;
        }

        eof_state = response_eof_state(client, content_length, total_read, read_len == 0);
        complete = eof_state == SERVER_CLIENT_VOICE_EOF_COMPLETE;
        if (complete) {
            complete = true;
            break;
        }
        if (eof_state == SERVER_CLIENT_VOICE_EOF_INCOMPLETE ||
            eof_state == SERVER_CLIENT_VOICE_EOF_OVERREAD) {
            known_length_mismatch = true;
            ESP_LOGW(TAG,
                     "voice response content-length incomplete total=%u content_length=%lld read_len=%d",
                     (unsigned int)total_read,
                     (long long)content_length,
                     read_len);
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (read_len == -ESP_ERR_HTTP_EAGAIN || read_len == 0) {
            empty_reads++;
            if (read_len == 0) {
                repeated_zero_reads++;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        ESP_LOGW(TAG,
                 "voice response read failed read_len=%d total=%u content_length=%lld empty_reads=%d elapsed_ms=%lld",
                 read_len,
                 (unsigned int)total_read,
                 (long long)content_length,
                 empty_reads,
                 (long long)(now_ms() - request_start_ms));
        ret = read_len == 0 || read_len == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
        break;
    }

    if (out_total_read != NULL) {
        *out_total_read = total_read;
    }
    ESP_LOGI(TAG,
             "voice_response: content_length=%lld chunked=%d total_read=%u complete=%d empty_reads=%d repeated_zero_reads=%d known_length_mismatch=%d downstream_close=%d downstream_close_ret=%s first_byte_ms=%lld last_byte_ms=%lld",
             (long long)content_length,
             chunked ? 1 : 0,
             (unsigned int)total_read,
             complete && ret == ESP_OK ? 1 : 0,
             empty_reads,
             repeated_zero_reads,
             known_length_mismatch ? 1 : 0,
             downstream_close_requested ? 1 : 0,
             esp_err_to_name(downstream_close_ret),
             (long long)first_byte_ms,
             (long long)last_byte_ms);
    return ret;
}

esp_err_t server_client_post_voice_turn(const char *device_id,
                                        const uint8_t *pcm,
                                        size_t pcm_len,
                                        server_client_data_cb_t on_data,
                                        void *user_ctx,
                                        int *http_status,
                                        int64_t *response_content_length,
                                        server_client_voice_meta_cb_t on_meta,
                                        void *meta_ctx)
{
    if (device_id == NULL || device_id[0] == '\0' || pcm == NULL || pcm_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const int64_t request_start_ms = now_ms();
    const uint32_t request_epoch = gateway_wifi_get_sta_network_epoch();
    if (!server_link_ready() || !network_epoch_current(request_epoch)) {
        if (http_status != NULL) {
            *http_status = 0;
        }
        if (response_content_length != NULL) {
            *response_content_length = -1;
        }
        ESP_LOGW(TAG,
                 "http error reason=%s endpoint=%s ret=%s status=%d upload pcm bytes=%u",
                 voice_http_error_reason(ESP_ERR_INVALID_STATE, 0),
                 ESP111_PROTOCOL_SERVER_ROUTE_VOICE_TURN,
                 esp_err_to_name(ESP_ERR_INVALID_STATE),
                 0,
                 (unsigned int)pcm_len);
        return ESP_ERR_INVALID_STATE;
    }
    if (response_content_length != NULL) {
        *response_content_length = -1;
    }
    if (http_status != NULL) {
        *http_status = 0;
    }

    char url[256];
    esp_err_t ret = build_url(ESP111_PROTOCOL_SERVER_ROUTE_VOICE_TURN, url, sizeof(url));
    if (ret != ESP_OK) {
        return ret;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = VOICE_REQUEST_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .keep_alive_enable = false,
    };
    ret = take_http_channel_slot(SERVER_CLIENT_HTTP_CHANNEL_HIGH,
                                 ESP111_PROTOCOL_SERVER_ROUTE_VOICE_TURN,
                                 "voice",
                                 SERVER_CLIENT_HTTP_SLOT_VOICE_WAIT_MS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "http error reason=%s endpoint=%s ret=%s status=%d upload pcm bytes=%u",
                 voice_http_error_reason(ret, 0),
                 ESP111_PROTOCOL_SERVER_ROUTE_VOICE_TURN,
                 esp_err_to_name(ret),
                 0,
                 (unsigned int)pcm_len);
        return ret;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        give_http_channel_slot(SERVER_CLIENT_HTTP_CHANNEL_HIGH,
                               0,
                               ESP111_PROTOCOL_SERVER_ROUTE_VOICE_TURN,
                               ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    const char *stage = "set_headers";
    size_t written = 0;
    size_t response_bytes = 0;
    bool upstream_closed = false;
    bool epoch_request_registered = false;
    ret = register_epoch_request(client, request_epoch, ESP111_PROTOCOL_SERVER_ROUTE_VOICE_TURN);
    if (ret == ESP_OK) {
        epoch_request_registered = true;
    }
    ESP_LOGI(TAG,
             "request start timestamp=%lld endpoint=%s timeout_ms=%u upload pcm bytes=%u device_id=%s url=%s",
             (long long)request_start_ms,
             ESP111_PROTOCOL_SERVER_ROUTE_VOICE_TURN,
             (unsigned int)SERVER_CLIENT_VOICE_IO_TIMEOUT_MS,
             (unsigned int)pcm_len,
             device_id,
             url);

    if (ret == ESP_OK) {
        ret = esp_http_client_set_header(client,
                                         "Content-Type",
                                         ESP111_PROTOCOL_AUDIO_CONTENT_TYPE_L16_16K_MONO);
    }
    if (ret == ESP_OK) {
        ret = esp_http_client_set_header(client,
                                         "X-Audio-Format",
                                         ESP111_PROTOCOL_AUDIO_FORMAT_PCM_S16LE_MONO_16K);
    }
    if (ret == ESP_OK) {
        ret = esp_http_client_set_header(client, "X-Device-Id", device_id);
    }
    if (ret == ESP_OK) {
        ret = esp_http_client_set_header(client, "X-Gateway-Id", gateway_config_get()->gateway_id);
    }
    if (ret == ESP_OK && gateway_config_get()->auth_token != NULL &&
        gateway_config_get()->auth_token[0] != '\0') {
        ret = esp_http_client_set_header(client,
                                         "X-Gateway-Token",
                                         gateway_config_get()->auth_token);
    }
    if (ret == ESP_OK) {
        stage = "open";
        (void)esp_http_client_set_timeout_ms(client, SERVER_CLIENT_VOICE_IO_TIMEOUT_MS);
        ret = esp_http_client_open(client, (int)pcm_len);
    }
    if (ret == ESP_OK) {
        stage = "write_body";
        (void)esp_http_client_set_timeout_ms(client, SERVER_CLIENT_VOICE_IO_TIMEOUT_MS);
        while (written < pcm_len) {
            if (!network_epoch_current(request_epoch)) {
                ret = ESP_ERR_INVALID_STATE;
                break;
            }
            if (now_ms() - request_start_ms >= SERVER_CLIENT_VOICE_TOTAL_TIMEOUT_MS) {
                ret = ESP_ERR_TIMEOUT;
                break;
            }
            app_task_wdt_reset_current(current_task_wdt_registered());
            int write_len = esp_http_client_write(client,
                                                  (const char *)pcm + written,
                                                  (int)(pcm_len - written));
            if (write_len <= 0) {
                ret = write_len == 0 ? ESP_ERR_TIMEOUT : ESP_FAIL;
                break;
            }
            written += (size_t)write_len;
        }
        if (ret == ESP_OK) {
            ESP_LOGI(TAG,
                     "upload pcm bytes=%u written_bytes=%u elapsed_ms=%lld",
                     (unsigned int)pcm_len,
                     (unsigned int)written,
                     (long long)(now_ms() - request_start_ms));
        }
    }

    int status = 0;
    int64_t content_length = -1;
    if (ret == ESP_OK) {
        stage = "fetch_headers";
        if (!network_epoch_current(request_epoch)) {
            ret = ESP_ERR_INVALID_STATE;
        }
    }
    if (ret == ESP_OK) {
        (void)esp_http_client_set_timeout_ms(client, SERVER_CLIENT_VOICE_IO_TIMEOUT_MS);
        int64_t header_ret = esp_http_client_fetch_headers(client);
        if (header_ret < 0) {
            ret = voice_fetch_headers_error_to_ret(header_ret);
        } else {
            content_length = esp_http_client_get_content_length(client);
            bool chunked = esp_http_client_is_chunked_response(client);
            if (response_content_length != NULL) {
                *response_content_length = content_length;
            }
            if (on_meta != NULL) {
                on_meta(content_length, meta_ctx);
            }
            int64_t response_received_ms = now_ms();
            status = esp_http_client_get_status_code(client);
            ESP_LOGI(TAG,
                     "response received timestamp=%lld endpoint=%s status=%d content_length=%lld chunked=%d elapsed_ms=%lld",
                     (long long)response_received_ms,
                     ESP111_PROTOCOL_SERVER_ROUTE_VOICE_TURN,
                     status,
                     (long long)content_length,
                     chunked ? 1 : 0,
                     (long long)(response_received_ms - request_start_ms));
        }
    }
    if (ret == ESP_OK) {
        if (http_status != NULL) {
            *http_status = status;
        }
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG,
                     "voice server rejected status=%d content_length=%lld",
                     status,
                     (long long)content_length);
            ret = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (ret == ESP_OK) {
        stage = "read_response";
        (void)esp_http_client_set_timeout_ms(client, SERVER_CLIENT_VOICE_IO_TIMEOUT_MS);
        ret = read_voice_response(client,
                                  on_data,
                                  user_ctx,
                                  request_start_ms,
                                  request_epoch,
                                  &response_bytes,
                                  &upstream_closed);
    }
    if (http_status != NULL && status == 0) {
        *http_status = esp_http_client_get_status_code(client);
    }
    int final_status = esp_http_client_get_status_code(client);
    if (status == 0) {
        status = final_status;
    }

    int64_t elapsed_ms = now_ms() - request_start_ms;
    ESP_LOGI(TAG,
             "forward response time device_id=%s elapsed_ms=%lld status=%d ret=%s response_bytes=%u content_length=%lld",
             device_id,
             (long long)elapsed_ms,
             status,
             esp_err_to_name(ret),
             (unsigned int)response_bytes,
             (long long)content_length);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "http error reason=%s device_id=%s phase=%s endpoint=%s ret=%s status=%d elapsed_ms=%lld timeout_ms=%u written_bytes=%u upload pcm bytes=%u response_bytes=%u",
                 voice_http_error_reason(ret, status),
                 device_id,
                 stage,
                 ESP111_PROTOCOL_SERVER_ROUTE_VOICE_TURN,
                 esp_err_to_name(ret),
                 status,
                 (long long)elapsed_ms,
                 (unsigned int)SERVER_CLIENT_VOICE_TOTAL_TIMEOUT_MS,
                 (unsigned int)written,
                 (unsigned int)pcm_len,
                 (unsigned int)response_bytes);
    }

    if (epoch_request_registered) {
        unregister_epoch_request(client);
    }
    /* Keep the socket open after the PCM body write until headers/body are read or an error ends the turn. */
    if (!upstream_closed) {
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    give_http_channel_slot(SERVER_CLIENT_HTTP_CHANNEL_HIGH,
                           request_start_ms,
                           ESP111_PROTOCOL_SERVER_ROUTE_VOICE_TURN,
                           ret);
    return ret;
}

/**
 * @file local_http_server.c
 * @brief S3 网关 /local/v1 HTTP 入口。
 *
 * 本文件属于 ESPS3 网关，负责暴露 C5<->S3 本地接口：register、heartbeat、status、
 * sensor、voice 和 commands。POST 输入 handler 做轻量协议校验后
 * 只把 body 通过 s3_scheduler 入队；状态更新和 Server 适配统一在 runtime
 * worker 内完成。
 */

#include "local_http_server.h"

#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "command_router.h"
#include "device_stream_gateway.h"
#include "esp111_protocol_common.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gateway_config.h"
#include "gateway_wifi.h"
#include "offline_policy.h"
#include "protocol_adapter.h"
#include "radar_local_handler.h"
#include "resource_manager.h"
#include "s3_scheduler.h"
#include "voice_proxy.h"
#include "wake_prompt_cache_gateway.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "local_http";

static httpd_handle_t s_server;
static bool s_server_routes_registered;
static local_http_server_state_t s_server_state = LOCAL_HTTP_SERVER_STATE_STOPPED;
static StaticSemaphore_t s_server_lock_storage;
static SemaphoreHandle_t s_server_lock;
static portMUX_TYPE s_server_lock_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_handler_metrics_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_local_http_active_count;
static uint32_t s_telemetry_http_active;
static int64_t s_last_sensor_ingress_success_log_ms;
static int64_t s_last_sensor_ingress_failure_log_ms;

#define LOCAL_HTTP_SENSOR_INGRESS_ADMISSION_TIMEOUT_MS 100U
#define LOCAL_HTTP_SENSOR_DIAGNOSTIC_LOG_MS 5000LL
#define ESP111_PROTOCOL_LOCAL_JSON_LOCAL_ID "local_id"

typedef struct {
    int content_length;
    size_t received_length;
    int64_t recv_duration_ms;
    const char *failure_stage;
} local_http_body_read_metrics_t;

typedef struct {
    uint32_t resource_lock_wait_ms;
    bool admission_deadline_exhausted;
    s3_scheduler_enqueue_diagnostics_t enqueue;
} local_http_sensor_ingress_metrics_t;

static int64_t local_http_telemetry_begin(const char *route, bool emit_debug)
{
    const int64_t started_us = esp_timer_get_time();
    uint32_t active;
    uint32_t telemetry;
    portENTER_CRITICAL(&s_handler_metrics_lock);
    active = ++s_local_http_active_count;
    telemetry = ++s_telemetry_http_active;
    portEXIT_CRITICAL(&s_handler_metrics_lock);
    if (emit_debug) {
        ESP_LOGD(TAG,
                 "local_http_active_count=%lu handler=%s handler_latency=0 telemetry_http_active=%lu queue_wait_time=0",
                 (unsigned long)active,
                 route != NULL ? route : "telemetry",
                 (unsigned long)telemetry);
    }
    return started_us;
}

static void local_http_telemetry_finish(const char *route,
                                        int64_t started_us,
                                        esp_err_t ret,
                                        bool emit_debug)
{
    uint32_t active;
    uint32_t telemetry;
    portENTER_CRITICAL(&s_handler_metrics_lock);
    if (s_local_http_active_count > 0U) {
        --s_local_http_active_count;
    }
    if (s_telemetry_http_active > 0U) {
        --s_telemetry_http_active;
    }
    active = s_local_http_active_count;
    telemetry = s_telemetry_http_active;
    portEXIT_CRITICAL(&s_handler_metrics_lock);
    if (emit_debug) {
        ESP_LOGD(TAG,
                 "local_http_active_count=%lu handler=%s handler_latency=%lld telemetry_http_active=%lu queue_wait_time=0 result=%s",
                 (unsigned long)active,
                 route != NULL ? route : "telemetry",
                 (long long)((esp_timer_get_time() - started_us) / 1000),
                 (unsigned long)telemetry,
                 esp_err_to_name(ret));
    }
}

static bool sensor_ingress_diagnostic_due(bool failure)
{
    const int64_t now = esp_timer_get_time() / 1000;
    bool due = false;
    portENTER_CRITICAL(&s_handler_metrics_lock);
    int64_t *last_log_ms = failure ? &s_last_sensor_ingress_failure_log_ms :
                                     &s_last_sensor_ingress_success_log_ms;
    if (*last_log_ms == 0 ||
        now - *last_log_ms >= LOCAL_HTTP_SENSOR_DIAGNOSTIC_LOG_MS) {
        *last_log_ms = now;
        due = true;
    }
    portEXIT_CRITICAL(&s_handler_metrics_lock);
    return due;
}

static void log_sensor_ingress(const httpd_req_t *req,
                               uint8_t local_id,
                               const local_http_body_read_metrics_t *body_metrics,
                               const local_http_sensor_ingress_metrics_t *ingress_metrics,
                               const char *stage,
                               esp_err_t ret)
{
    const bool failure = ret != ESP_OK;
    if (!sensor_ingress_diagnostic_due(failure)) {
        return;
    }

    const s3_scheduler_enqueue_diagnostics_t *enqueue =
        ingress_metrics != NULL ? &ingress_metrics->enqueue : NULL;
    const s3_event_bus_stats_t *stats = enqueue != NULL ? &enqueue->event_bus : NULL;
    const bool stats_valid = enqueue != NULL && enqueue->event_bus_stats_valid;
    const char *device_id = protocol_adapter_local_device_id_to_device_id(local_id);
    ESP_LOG_LEVEL_LOCAL(failure ? ESP_LOG_WARN : ESP_LOG_INFO,
                        TAG,
                        "SENSOR_INGRESS uri=%s device_id=%s local_id=%u content_length=%d "
                        "received_length=%u recv_duration_ms=%lld resource_lock_wait_ms=%lu "
                        "event_bus_lock_wait_ms=%lu enqueue_duration_ms=%lu "
                        "event_bus_depth=%u event_bus_stats_valid=%d internal_free=%u "
                        "internal_largest=%u dma_free=%u dma_largest=%u stage=%s ret=%s",
                        req != NULL && req->uri != NULL ? req->uri : ESP111_PROTOCOL_ROUTE_SENSOR,
                        device_id != NULL ? device_id : "-",
                        (unsigned int)local_id,
                        body_metrics != NULL ? body_metrics->content_length : 0,
                        (unsigned int)(body_metrics != NULL ? body_metrics->received_length : 0U),
                        (long long)(body_metrics != NULL ? body_metrics->recv_duration_ms : 0),
                        (unsigned long)(ingress_metrics != NULL ?
                                            ingress_metrics->resource_lock_wait_ms : 0U),
                        (unsigned long)(enqueue != NULL ? enqueue->event_bus_lock_wait_ms : 0U),
                        (unsigned long)(enqueue != NULL ? enqueue->enqueue_duration_ms : 0U),
                        (unsigned int)(stats_valid ? stats->queue_depth : 0U),
                        stats_valid ? 1 : 0,
                        (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                        (unsigned int)heap_caps_get_largest_free_block(
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                        (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA),
                        (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                        stage != NULL ? stage : "unknown",
                        esp_err_to_name(ret));
}

static bool admission_timeout_remaining_ms(int64_t deadline_us, uint32_t *out_timeout_ms)
{
    if (deadline_us <= 0 || out_timeout_ms == NULL) {
        return false;
    }

    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return false;
    }

    const uint32_t remaining_ms = (uint32_t)(remaining_us / 1000LL);
    if (remaining_ms == 0U) {
        return false;
    }
    *out_timeout_ms = remaining_ms;
    return true;
}

static esp_err_t snapshot_resource_session(const char *device_id,
                                           s3_runtime_ingress_t *ingress,
                                           uint32_t lock_timeout_ms,
                                           uint32_t *out_lock_wait_ms)
{
    if (device_id == NULL || device_id[0] == '\0' || ingress == NULL) {
        return ESP_OK;
    }
    if (out_lock_wait_ms != NULL) {
        *out_lock_wait_ms = 0U;
    }

    resource_manager_session_view_t view = {0};
    if (lock_timeout_ms == 0U) {
        if (resource_manager_get_session(device_id, &view)) {
            ingress->resource_generation = view.generation;
            ingress->resource_state_at_rx = view.state;
            ingress->resource_state_since_ms_at_rx = view.state_since_ms;
        }
        return ESP_OK;
    }

    bool found = false;
    const int64_t lock_wait_started_us = esp_timer_get_time();
    esp_err_t ret = resource_manager_get_session_timed(device_id,
                                                        &view,
                                                        lock_timeout_ms,
                                                        &found);
    if (out_lock_wait_ms != NULL) {
        *out_lock_wait_ms =
            (uint32_t)((esp_timer_get_time() - lock_wait_started_us) / 1000);
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (found) {
        ingress->resource_generation = view.generation;
        ingress->resource_state_at_rx = view.state;
        ingress->resource_state_since_ms_at_rx = view.state_since_ms;
    }
    return ESP_OK;
}

static SemaphoreHandle_t server_lock_handle(void)
{
    if (s_server_lock == NULL) {
        portENTER_CRITICAL(&s_server_lock_mux);
        if (s_server_lock == NULL) {
            s_server_lock = xSemaphoreCreateMutexStatic(&s_server_lock_storage);
        }
        portEXIT_CRITICAL(&s_server_lock_mux);
    }
    return s_server_lock;
}

const char *local_http_server_state_name(local_http_server_state_t state)
{
    switch (state) {
    case LOCAL_HTTP_SERVER_STATE_STARTING:
        return "STARTING";
    case LOCAL_HTTP_SERVER_STATE_RUNNING:
        return "RUNNING";
    case LOCAL_HTTP_SERVER_STATE_STOPPING:
        return "STOPPING";
    case LOCAL_HTTP_SERVER_STATE_FAILED:
        return "FAILED";
    case LOCAL_HTTP_SERVER_STATE_STOPPED:
    default:
        return "STOPPED";
    }
}

static void log_server_state_locked(local_http_server_state_t state,
                                    const char *reason,
                                    esp_err_t ret)
{
    s_server_state = state;
    const char *state_name = local_http_server_state_name(state);
    const char *safe_reason = reason != NULL && reason[0] != '\0' ? reason : "unspecified";
    const char *ret_name = esp_err_to_name(ret);
    if (state == LOCAL_HTTP_SERVER_STATE_FAILED) {
        ESP_LOGE(TAG,
                 "local_http state: %s reason=%s handle=%p routes=%d ret=%s",
                 state_name,
                 safe_reason,
                 (void *)s_server,
                 s_server_routes_registered ? 1 : 0,
                 ret_name);
        return;
    }
    ESP_LOGI(TAG,
             "local_http state: %s reason=%s handle=%p routes=%d ret=%s",
             state_name,
             safe_reason,
             (void *)s_server,
             s_server_routes_registered ? 1 : 0,
             ret_name);
}

static void clear_server_state_locked(void)
{
    s_server = NULL;
    s_server_routes_registered = false;
}

/* The caller owns s_server_lock. A failed stop retains the handle for the next cleanup attempt. */
static esp_err_t stop_server_locked(const char *reason)
{
    log_server_state_locked(LOCAL_HTTP_SERVER_STATE_STOPPING, reason, ESP_OK);
    if (s_server == NULL) {
        clear_server_state_locked();
        log_server_state_locked(LOCAL_HTTP_SERVER_STATE_STOPPED, reason, ESP_OK);
        return ESP_OK;
    }

    const esp_err_t ret = httpd_stop(s_server);
    if (ret == ESP_OK) {
        clear_server_state_locked();
        log_server_state_locked(LOCAL_HTTP_SERVER_STATE_STOPPED, reason, ret);
        return ESP_OK;
    }

    log_server_state_locked(LOCAL_HTTP_SERVER_STATE_FAILED, reason, ret);
    return ret;
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

static bool local_id_number_is_allowed(const cJSON *item, uint8_t *out_local_id)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) {
        return false;
    }
    if (item->valuedouble != (double)item->valueint) {
        return false;
    }
    if (item->valueint != (int)ESP111_PROTOCOL_LOCAL_DEVICE_ID_C51 &&
        item->valueint != (int)ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52) {
        return false;
    }
    if (out_local_id != NULL) {
        *out_local_id = (uint8_t)item->valueint;
    }
    return true;
}

static cJSON *local_id_item_from_json(cJSON *root)
{
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, ESP111_PROTOCOL_LOCAL_JSON_ID);
    return id != NULL ? id :
                        cJSON_GetObjectItemCaseSensitive(root,
                                                         ESP111_PROTOCOL_LOCAL_JSON_LOCAL_ID);
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_type(req, "application/json");
    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_error(httpd_req_t *req,
                            const char *status,
                            const char *code,
                            const char *message)
{
    char body[192];
    unsigned int local_code = ESP111_PROTOCOL_LOCAL_ERROR_UNKNOWN;
    if (code != NULL) {
        if (strcmp(code, ESP111_PROTOCOL_ERROR_UNSUPPORTED_COMMAND) == 0) {
            local_code = ESP111_PROTOCOL_LOCAL_ERROR_UNSUPPORTED_COMMAND;
        } else if (strcmp(code, ESP111_PROTOCOL_ERROR_INVALID_COMMAND_PAYLOAD) == 0 ||
                   strcmp(code, ESP111_PROTOCOL_ERROR_INVALID_ENVELOPE) == 0 ||
                   strcmp(code, ESP111_PROTOCOL_ERROR_INVALID_ACK) == 0 ||
                   strcmp(code, ESP111_PROTOCOL_ERROR_INVALID_RADAR_STATE) == 0 ||
                   strcmp(code, ESP111_PROTOCOL_ERROR_INVALID_DEVICE_ID) == 0 ||
                   strcmp(code, ESP111_PROTOCOL_ERROR_INVALID_COMMAND_ID) == 0) {
            local_code = ESP111_PROTOCOL_LOCAL_ERROR_INVALID_PAYLOAD;
        } else if (strcmp(code, ESP111_PROTOCOL_ERROR_TIMEOUT) == 0) {
            local_code = ESP111_PROTOCOL_LOCAL_ERROR_TIMEOUT;
        } else if (strcmp(code, ESP111_PROTOCOL_ERROR_COMMAND_FAILED) == 0 ||
                   strcmp(code, ESP111_PROTOCOL_ERROR_ACK_FAILED) == 0 ||
                   strcmp(code, ESP111_PROTOCOL_ERROR_COMMAND_POLL_FAILED) == 0 ||
                   strcmp(code, ESP111_PROTOCOL_ERROR_INTERNAL) == 0) {
            local_code = ESP111_PROTOCOL_LOCAL_ERROR_COMMAND_FAILED;
        }
    }
    (void)message;
    protocol_adapter_build_local_error_response(local_code, body, sizeof(body));
    return send_json(req, status, body);
}

static esp_err_t send_local_ok(httpd_req_t *req, uint8_t local_id, const char *status)
{
    char response[192];
    esp_err_t ret = protocol_adapter_build_local_ok_response(local_id, response, sizeof(response));
    if (ret != ESP_OK) {
        return send_error(req,
                          "500 Internal Server Error",
                          ESP111_PROTOCOL_ERROR_INTERNAL,
                          esp_err_to_name(ret));
    }
    return send_json(req, status != NULL ? status : "200 OK", response);
}

static esp_err_t read_json_body(httpd_req_t *req,
                                char **out_body,
                                size_t *out_len,
                                local_http_body_read_metrics_t *out_metrics)
{
    if (out_metrics != NULL) {
        memset(out_metrics, 0, sizeof(*out_metrics));
        out_metrics->failure_stage = "validation_failure";
    }
    if (req == NULL || out_body == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_body = NULL;
    *out_len = 0;
    if (out_metrics != NULL) {
        out_metrics->content_length = req->content_len;
    }

    const int64_t recv_started_us = esp_timer_get_time();

    if (req->content_len <= 0 ||
        (size_t)req->content_len > gateway_config_get()->local_http_max_json_bytes) {
        if (out_metrics != NULL) {
            out_metrics->recv_duration_ms =
                (esp_timer_get_time() - recv_started_us) / 1000;
            out_metrics->failure_stage = "invalid_content_length";
        }
        return ESP_ERR_INVALID_SIZE;
    }

    char *body = heap_caps_calloc(1, (size_t)req->content_len + 1U, MALLOC_CAP_8BIT);
    if (body == NULL) {
        if (out_metrics != NULL) {
            out_metrics->recv_duration_ms =
                (esp_timer_get_time() - recv_started_us) / 1000;
            out_metrics->failure_stage = "body_alloc_failure";
        }
        return ESP_ERR_NO_MEM;
    }

    int remaining = req->content_len;
    int offset = 0;
    while (remaining > 0) {
        int read = httpd_req_recv(req, body + offset, remaining);
        if (read <= 0) {
            heap_caps_free(body);
            if (out_metrics != NULL) {
                out_metrics->received_length = (size_t)offset;
                out_metrics->recv_duration_ms =
                    (esp_timer_get_time() - recv_started_us) / 1000;
                out_metrics->failure_stage =
                    read == HTTPD_SOCK_ERR_TIMEOUT ? "recv_timeout" :
                    offset > 0 ? "partial_body" : "peer_closed";
            }
            return read == HTTPD_SOCK_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
        offset += read;
        remaining -= read;
    }

    *out_body = body;
    *out_len = (size_t)req->content_len;
    if (out_metrics != NULL) {
        out_metrics->received_length = (size_t)offset;
        out_metrics->recv_duration_ms = (esp_timer_get_time() - recv_started_us) / 1000;
        out_metrics->failure_stage = "accepted";
    }
    return ESP_OK;
}

static void read_peer_ip(httpd_req_t *req, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    out[0] = '\0';

    int sock = httpd_req_to_sockfd(req);
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    if (sock >= 0 && getpeername(sock, (struct sockaddr *)&addr, &addr_len) == 0 &&
        addr.ss_family == AF_INET) {
        const struct sockaddr_in *addr_in = (const struct sockaddr_in *)&addr;
        (void)inet_ntop(AF_INET, &addr_in->sin_addr, out, out_size);
    }
}

static uint8_t local_id_from_json_body(const char *body, size_t body_len);

static esp_err_t enqueue_body_buffer_with_admission(
    const char *body,
    size_t body_len,
    s3_runtime_msg_kind_t kind,
    const char *command_id,
    s3_scheduler_priority_t priority,
    int64_t received_at_us,
    const char *peer_ip,
    int64_t admission_deadline_us,
    local_http_sensor_ingress_metrics_t *out_metrics)
{
    if (out_metrics != NULL) {
        memset(out_metrics, 0, sizeof(*out_metrics));
    }
    if (body == NULL || body_len == 0U || body_len > S3_RUNTIME_BUS_BODY_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    s3_runtime_ingress_t *ingress =
        heap_caps_calloc(1, sizeof(*ingress), MALLOC_CAP_8BIT);
    if (ingress == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int64_t rx_time_us = received_at_us > 0 ? received_at_us : esp_timer_get_time();
    int64_t rx_time_ms = rx_time_us / 1000;
    ingress->kind = kind;
    ingress->rx_time_us = rx_time_us;
    ingress->rx_time_ms = rx_time_ms;
    ingress->body_len = body_len;
    memcpy(ingress->body, body, body_len);
    ingress->body[body_len] = '\0';
    ingress->unified.t = rx_time_ms;
    if (command_id != NULL) {
        strlcpy(ingress->command_id, command_id, sizeof(ingress->command_id));
    }
    uint8_t local_id = local_id_from_json_body(body, body_len);
    if (local_id != 0U) {
        const char *short_id = short_device_id_for_local_id(local_id);
        if (short_id != NULL) {
            strlcpy(ingress->unified.did, short_id, sizeof(ingress->unified.did));
        }
        const char *device_id = protocol_adapter_local_device_id_to_device_id(local_id);
        if (device_id != NULL) {
            strlcpy(ingress->device_id, device_id, sizeof(ingress->device_id));
            uint32_t resource_lock_timeout_ms = 0U;
            if (admission_deadline_us > 0 &&
                !admission_timeout_remaining_ms(admission_deadline_us,
                                                &resource_lock_timeout_ms)) {
                if (out_metrics != NULL) {
                    out_metrics->admission_deadline_exhausted = true;
                }
                heap_caps_free(ingress);
                return ESP_ERR_TIMEOUT;
            }
            esp_err_t session_ret = snapshot_resource_session(
                device_id,
                ingress,
                resource_lock_timeout_ms,
                out_metrics != NULL ? &out_metrics->resource_lock_wait_ms : NULL);
            if (session_ret != ESP_OK) {
                heap_caps_free(ingress);
                return session_ret;
            }
        }
    }
    if (peer_ip != NULL && peer_ip[0] != '\0') {
        strlcpy(ingress->peer_ip, peer_ip, sizeof(ingress->peer_ip));
    }

    if (admission_deadline_us > 0) {
        uint32_t event_bus_lock_timeout_ms = 0U;
        if (!admission_timeout_remaining_ms(admission_deadline_us,
                                            &event_bus_lock_timeout_ms)) {
            if (out_metrics != NULL) {
                out_metrics->admission_deadline_exhausted = true;
            }
            heap_caps_free(ingress);
            return ESP_ERR_TIMEOUT;
        }
        return s3_scheduler_enqueue_ingress_owned_timed(
            ingress,
            priority,
            event_bus_lock_timeout_ms,
            out_metrics != NULL ? &out_metrics->enqueue : NULL);
    }
    return s3_scheduler_enqueue_ingress_owned(ingress, priority);
}

static esp_err_t enqueue_body_buffer(const char *body,
                                     size_t body_len,
                                     s3_runtime_msg_kind_t kind,
                                     const char *command_id,
                                     s3_scheduler_priority_t priority,
                                     int64_t received_at_us,
                                     const char *peer_ip)
{
    return enqueue_body_buffer_with_admission(body,
                                              body_len,
                                              kind,
                                              command_id,
                                              priority,
                                              received_at_us,
                                              peer_ip,
                                              0U,
                                              NULL);
}

static esp_err_t enqueue_sensor_body_buffer(const char *body,
                                            size_t body_len,
                                            s3_scheduler_priority_t priority,
                                            int64_t received_at_us,
                                            const char *peer_ip,
                                            local_http_sensor_ingress_metrics_t *out_metrics)
{
    const int64_t admission_deadline_us =
        esp_timer_get_time() + (int64_t)LOCAL_HTTP_SENSOR_INGRESS_ADMISSION_TIMEOUT_MS * 1000LL;
    return enqueue_body_buffer_with_admission(body,
                                              body_len,
                                              S3_RUNTIME_MSG_SENSOR,
                                              NULL,
                                              priority,
                                              received_at_us,
                                              peer_ip,
                                              admission_deadline_us,
                                              out_metrics);
}

static esp_err_t validate_local_body(const char *body,
                                     size_t body_len,
                                     uint8_t *out_local_id)
{
    if (body == NULL || body_len == 0U || out_local_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_local_id = 0;
    protocol_adapter_envelope_t envelope = {0};
    esp_err_t ret = protocol_adapter_parse_local_envelope(body, body_len, &envelope);
    if (ret == ESP_OK) {
        ret = protocol_adapter_validate_local_envelope(&envelope);
    }
    if (ret == ESP_OK) {
        *out_local_id = protocol_adapter_device_id_to_local_id(envelope.device_id);
    }
    protocol_adapter_release_envelope(&envelope);
    return ret;
}

static esp_err_t enqueue_local_or_error(httpd_req_t *req,
                                        s3_runtime_msg_kind_t kind,
                                        s3_scheduler_priority_t priority,
                                        const char *status,
                                        const char *error_code)
{
    const bool sensor_ingress = kind == S3_RUNTIME_MSG_SENSOR;
    const bool telemetry_debug = !sensor_ingress;
    const int64_t started_us = local_http_telemetry_begin(
        req != NULL ? req->uri : "telemetry", telemetry_debug);
    const int64_t received_at_us = esp_timer_get_time();
    char peer_ip[16] = {0};
    read_peer_ip(req, peer_ip, sizeof(peer_ip));
    char *body = NULL;
    size_t body_len = 0;
    local_http_body_read_metrics_t body_metrics = {0};
    local_http_sensor_ingress_metrics_t ingress_metrics = {0};
    esp_err_t ret = read_json_body(req,
                                   &body,
                                   &body_len,
                                   sensor_ingress ? &body_metrics : NULL);
    if (ret != ESP_OK) {
        heap_caps_free(body);
        if (sensor_ingress) {
            log_sensor_ingress(req,
                               0U,
                               &body_metrics,
                               &ingress_metrics,
                               body_metrics.failure_stage,
                               ret);
        }
        const char *http_status = sensor_ingress && ret == ESP_ERR_TIMEOUT ?
                                      "408 Request Timeout" : "400 Bad Request";
        const char *local_error = ret == ESP_ERR_TIMEOUT ? ESP111_PROTOCOL_ERROR_TIMEOUT : error_code;
        esp_err_t send_ret = send_error(req, http_status, local_error, esp_err_to_name(ret));
        local_http_telemetry_finish(req->uri, started_us, ret, telemetry_debug);
        return send_ret;
    }

    uint8_t local_id = 0;
    ret = validate_local_body(body, body_len, &local_id);
    const char *failure_stage = ret == ESP_OK ? "accepted" : "validation_failure";
    if (ret == ESP_OK) {
        ret = sensor_ingress ?
                  enqueue_sensor_body_buffer(body,
                                             body_len,
                                             priority,
                                             received_at_us,
                                             peer_ip,
                                             &ingress_metrics) :
                  enqueue_body_buffer(body,
                                      body_len,
                                      kind,
                                      NULL,
                                      priority,
                                      received_at_us,
                                      peer_ip);
        if (ret != ESP_OK) {
            if (ret == ESP_ERR_TIMEOUT && ingress_metrics.admission_deadline_exhausted) {
                failure_stage = "ingress_deadline_timeout";
            } else if (ret == ESP_ERR_TIMEOUT && ingress_metrics.enqueue.enqueue_duration_ms > 0U) {
                failure_stage = ingress_metrics.enqueue.event_bus_stats_valid ?
                                    "enqueue_timeout" : "event_bus_lock_timeout";
            } else if (ret == ESP_ERR_TIMEOUT) {
                failure_stage = "resource_session_lock_timeout";
            } else {
                failure_stage = "enqueue_failure";
            }
        }
    }
    heap_caps_free(body);

    if (ret != ESP_OK) {
        const char *http_status =
            (ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_NO_MEM ||
             ret == ESP_ERR_INVALID_STATE) ? "503 Service Unavailable" : "400 Bad Request";
        const char *local_error =
            ret == ESP_ERR_TIMEOUT ? ESP111_PROTOCOL_ERROR_TIMEOUT : error_code;
        if (sensor_ingress) {
            log_sensor_ingress(req,
                               local_id,
                               &body_metrics,
                               &ingress_metrics,
                               failure_stage,
                               ret);
        }
        esp_err_t send_ret = send_error(req, http_status, local_error, esp_err_to_name(ret));
        local_http_telemetry_finish(req->uri, started_us, ret, telemetry_debug);
        return send_ret;
    }
    if (sensor_ingress) {
        log_sensor_ingress(req,
                           local_id,
                           &body_metrics,
                           &ingress_metrics,
                           "accepted",
                           ESP_OK);
    }
    esp_err_t send_ret = send_local_ok(req, local_id, status);
    local_http_telemetry_finish(req->uri, started_us, send_ret, telemetry_debug);
    return send_ret;
}

static uint8_t local_id_from_json_body(const char *body, size_t body_len)
{
    uint8_t local_id = 0;
    cJSON *root = body != NULL ? cJSON_ParseWithLength(body, body_len) : NULL;
    if (root != NULL) {
        cJSON *id = local_id_item_from_json(root);
        if (local_id_number_is_allowed(id, &local_id)) {
            /* accepted */
        } else if (cJSON_IsString(id) && id->valuestring != NULL) {
            local_id = protocol_adapter_device_id_to_local_id(id->valuestring);
        }
        cJSON_Delete(root);
    }
    return local_id;
}

static esp_err_t validate_command_ack_local_id(const char *body,
                                               size_t body_len,
                                               const char *command_id,
                                               uint8_t *out_local_id)
{
    if (body == NULL || body_len == 0U || out_local_id == NULL) {
        ESP_LOGW(TAG, "command ACK rejected command_id=%s reason=empty_body",
                 command_id != NULL ? command_id : "-");
        return ESP_ERR_INVALID_ARG;
    }

    *out_local_id = 0U;
    cJSON *root = cJSON_ParseWithLength(body, body_len);
    if (root == NULL) {
        ESP_LOGW(TAG, "command ACK rejected command_id=%s reason=invalid_json",
                 command_id != NULL ? command_id : "-");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, ESP111_PROTOCOL_LOCAL_JSON_ID);
    cJSON *local_id =
        cJSON_GetObjectItemCaseSensitive(root, ESP111_PROTOCOL_LOCAL_JSON_LOCAL_ID);
    cJSON *identity = id != NULL ? id : local_id;
    uint8_t parsed = 0U;
    esp_err_t ret = ESP_OK;
    if (identity == NULL) {
        ESP_LOGW(TAG, "command ACK rejected command_id=%s reason=missing_id",
                 command_id != NULL ? command_id : "-");
        ret = ESP_ERR_INVALID_ARG;
    } else if (!local_id_number_is_allowed(identity, &parsed)) {
        ESP_LOGW(TAG,
                 "command ACK rejected command_id=%s reason=invalid_id value=%d",
                 command_id != NULL ? command_id : "-",
                 cJSON_IsNumber(identity) ? identity->valueint : -1);
        ret = ESP_ERR_NOT_ALLOWED;
    } else if (id != NULL && local_id != NULL) {
        uint8_t local_id_value = 0U;
        if (!local_id_number_is_allowed(local_id, &local_id_value) ||
            local_id_value != parsed) {
            ESP_LOGW(TAG,
                     "command ACK rejected command_id=%s reason=id_local_id_mismatch",
                     command_id != NULL ? command_id : "-");
            ret = ESP_ERR_NOT_ALLOWED;
        }
    }

    if (ret == ESP_OK) {
        *out_local_id = parsed;
    }
    cJSON_Delete(root);
    return ret;
}

static esp_err_t health_handler(httpd_req_t *req)
{
    char body[512];
    int written = snprintf(body,
                           sizeof(body),
                           "{\"ok\":true,\"gateway_id\":\"%s\",\"role\":\"gateway\",\"softap_ready\":%s,\"sta_connected\":%s,\"server_available\":%s,\"voice_busy\":%s,\"last_error\":\"%s\"}",
                           gateway_config_get()->gateway_id,
                           gateway_wifi_is_softap_ready() ? "true" : "false",
                           gateway_wifi_is_sta_connected() ? "true" : "false",
                           offline_policy_server_available() ? "true" : "false",
                           voice_proxy_is_busy() ? "true" : "false",
                           offline_policy_last_error_code());
    if (written <= 0 || written >= (int)sizeof(body)) {
        return send_error(req,
                          "500 Internal Server Error",
                          ESP111_PROTOCOL_ERROR_INTERNAL,
                          "health body overflow");
    }
    return send_json(req, "200 OK", body);
}

static esp_err_t register_handler(httpd_req_t *req)
{
    return enqueue_local_or_error(req,
                                  S3_RUNTIME_MSG_STATUS,
                                  S3_SCHEDULER_PRIORITY_HIGH,
                                  "200 OK",
                                  ESP111_PROTOCOL_ERROR_INVALID_ENVELOPE);
}

static esp_err_t heartbeat_handler(httpd_req_t *req)
{
    return enqueue_local_or_error(req,
                                  S3_RUNTIME_MSG_STATUS,
                                  S3_SCHEDULER_PRIORITY_HIGH,
                                  "200 OK",
                                  ESP111_PROTOCOL_ERROR_INVALID_HEARTBEAT);
}

static esp_err_t health_update_handler(httpd_req_t *req)
{
    return enqueue_local_or_error(req,
                                  S3_RUNTIME_MSG_STATUS,
                                  S3_SCHEDULER_PRIORITY_NORMAL,
                                  "202 Accepted",
                                  ESP111_PROTOCOL_ERROR_INVALID_HEARTBEAT);
}

static esp_err_t status_or_sensor_handler(httpd_req_t *req)
{
    s3_runtime_msg_kind_t kind =
        strcmp(req->uri, ESP111_PROTOCOL_ROUTE_SENSOR) == 0 ? S3_RUNTIME_MSG_SENSOR :
                                                              S3_RUNTIME_MSG_STATUS;
    return enqueue_local_or_error(req,
                                  kind,
                                  S3_SCHEDULER_PRIORITY_NORMAL,
                                  "202 Accepted",
                                  ESP111_PROTOCOL_ERROR_INVALID_ENVELOPE);
}

static esp_err_t device_stream_handler(httpd_req_t *req)
{
    esp_err_t ret = device_stream_gateway_handle_http(req);
    return ret == ESP_OK ? send_json(req, "202 Accepted", "{\"ok\":1}")
                         : send_error(req,
                                      "400 Bad Request",
                                      ESP111_PROTOCOL_ERROR_INVALID_ENVELOPE,
                                      esp_err_to_name(ret));
}

static esp_err_t commands_pending_handler(httpd_req_t *req)
{
    char query[128] = {0};
    char local_id_text[8] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query,
                              ESP111_PROTOCOL_LOCAL_JSON_ID,
                              local_id_text,
                              sizeof(local_id_text)) != ESP_OK) {
        return send_error(req,
                          "400 Bad Request",
                          ESP111_PROTOCOL_ERROR_INVALID_DEVICE_ID,
                          "id query is required");
    }

    uint8_t local_id = (uint8_t)atoi(local_id_text);
    const char *device_id = protocol_adapter_local_device_id_to_device_id(local_id);
    if (device_id == NULL) {
        return send_error(req,
                          "400 Bad Request",
                          ESP111_PROTOCOL_ERROR_INVALID_DEVICE_ID,
                          "id is not allowed");
    }

    char *body = heap_caps_calloc(1U, 2048U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) {
        return send_error(req,
                          "503 Service Unavailable",
                          ESP111_PROTOCOL_ERROR_COMMAND_POLL_FAILED,
                          "no_memory");
    }
    esp_err_t ret = command_router_build_pending_json(device_id, body, 2048U);
    const esp_err_t response_ret = ret == ESP_OK
        ? send_json(req, "200 OK", body)
        : send_error(req,
                     "400 Bad Request",
                     ESP111_PROTOCOL_ERROR_COMMAND_POLL_FAILED,
                     esp_err_to_name(ret));
    heap_caps_free(body);
    return response_ret;
}

static esp_err_t command_ack_handler(httpd_req_t *req)
{
    const int64_t received_at_us = esp_timer_get_time();
    char peer_ip[16] = {0};
    read_peer_ip(req, peer_ip, sizeof(peer_ip));
    const char *prefix = ESP111_PROTOCOL_ROUTE_COMMANDS_PREFIX;
    const char *suffix = ESP111_PROTOCOL_ROUTE_COMMAND_ACK_SUFFIX;
    const char *start = req->uri + strlen(prefix);
    const char *end = strstr(start, suffix);
    if (end == NULL || end <= start) {
        return send_error(req,
                          "400 Bad Request",
                          ESP111_PROTOCOL_ERROR_INVALID_COMMAND_ID,
                          "command_id is required");
    }

    char command_id[48] = {0};
    size_t len = (size_t)(end - start);
    if (len >= sizeof(command_id)) {
        return send_error(req,
                          "414 URI Too Long",
                          ESP111_PROTOCOL_ERROR_INVALID_COMMAND_ID,
                          "command_id is too long");
    }
    memcpy(command_id, start, len);

    char *body = NULL;
    size_t body_len = 0;
    esp_err_t ret = read_json_body(req, &body, &body_len, NULL);
    if (ret != ESP_OK) {
        heap_caps_free(body);
        return send_error(req,
                          "400 Bad Request",
                          ESP111_PROTOCOL_ERROR_INVALID_ACK,
                          esp_err_to_name(ret));
    }

    uint8_t local_id = 0U;
    ret = validate_command_ack_local_id(body, body_len, command_id, &local_id);
    if (ret == ESP_OK) {
        ret = enqueue_body_buffer(body,
                                  body_len,
                                  S3_RUNTIME_MSG_EVENT,
                                  command_id,
                                  S3_SCHEDULER_PRIORITY_HIGH,
                                  received_at_us,
                                  peer_ip);
    }
    heap_caps_free(body);
    if (ret != ESP_OK) {
        const char *http_status =
            (ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_NO_MEM ||
             ret == ESP_ERR_INVALID_STATE) ? "503 Service Unavailable" : "400 Bad Request";
        const bool invalid_ack =
            ret == ESP_ERR_INVALID_ARG || ret == ESP_ERR_NOT_ALLOWED ||
            ret == ESP_ERR_INVALID_SIZE;
        return send_error(req,
                          http_status,
                          ret == ESP_ERR_TIMEOUT ? ESP111_PROTOCOL_ERROR_TIMEOUT :
                          invalid_ack ? ESP111_PROTOCOL_ERROR_INVALID_ACK :
                                        ESP111_PROTOCOL_ERROR_ACK_FAILED,
                          esp_err_to_name(ret));
    }

    return send_local_ok(req, local_id, "200 OK");
}

local_http_server_state_t local_http_server_get_state(void)
{
    SemaphoreHandle_t lock = server_lock_handle();
    if (lock == NULL) {
        return LOCAL_HTTP_SERVER_STATE_FAILED;
    }
    if (xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) {
        return LOCAL_HTTP_SERVER_STATE_FAILED;
    }

    const local_http_server_state_t state = s_server_state;
    xSemaphoreGive(lock);
    return state;
}

bool local_http_server_is_running(void)
{
    SemaphoreHandle_t lock = server_lock_handle();
    if (lock == NULL || xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    const bool running = s_server != NULL && s_server_routes_registered &&
                         s_server_state == LOCAL_HTTP_SERVER_STATE_RUNNING;
    xSemaphoreGive(lock);
    return running;
}

bool local_http_server_has_handle(void)
{
    SemaphoreHandle_t lock = server_lock_handle();
    if (lock == NULL || xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) {
        return true;
    }

    const bool has_handle = s_server != NULL;
    xSemaphoreGive(lock);
    return has_handle;
}

esp_err_t local_http_server_start(void)
{
    return local_http_server_start_with_reason("direct_start");
}

esp_err_t local_http_server_start_with_reason(const char *reason)
{
    SemaphoreHandle_t lock = server_lock_handle();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_server != NULL && s_server_routes_registered) {
        log_server_state_locked(LOCAL_HTTP_SERVER_STATE_RUNNING, reason, ESP_OK);
        xSemaphoreGive(lock);
        return ESP_OK;
    }

    if (s_server != NULL) {
        /* Never start another listener while a partial instance still owns port 80. */
        esp_err_t stop_ret = stop_server_locked("start_preflight_cleanup");
        if (stop_ret != ESP_OK) {
            xSemaphoreGive(lock);
            return stop_ret;
        }
    }

    if (!gateway_wifi_is_softap_ready()) {
        clear_server_state_locked();
        log_server_state_locked(LOCAL_HTTP_SERVER_STATE_FAILED, "softap_not_ready", ESP_ERR_INVALID_STATE);
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }

    log_server_state_locked(LOCAL_HTTP_SERVER_STATE_STARTING, reason, ESP_OK);
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = gateway_config_get()->local_http_port;
    config.max_open_sockets = 4;
    config.max_uri_handlers = 14;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    config.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

    httpd_handle_t server = NULL;
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        /* ESP-IDF normally returns NULL on this path; clean defensively if that ever changes. */
        if (server != NULL) {
            s_server = server;
            s_server_routes_registered = false;
            (void)stop_server_locked("httpd_start_failure_cleanup");
        } else {
            clear_server_state_locked();
        }
        log_server_state_locked(LOCAL_HTTP_SERVER_STATE_FAILED, reason, ret);
        xSemaphoreGive(lock);
        return ret;
    }

    const httpd_uri_t routes[] = {
        /* /local/v1 是 C5<->S3 边界；/api/... Server 路径不在本地 HTTP server 暴露。 */
        {.uri = ESP111_PROTOCOL_ROUTE_HEALTH, .method = HTTP_GET, .handler = health_handler},
        {.uri = ESP111_PROTOCOL_ROUTE_HEALTH, .method = HTTP_POST, .handler = health_update_handler},
        {.uri = ESP111_PROTOCOL_ROUTE_REGISTER, .method = HTTP_POST, .handler = register_handler},
        {.uri = ESP111_PROTOCOL_ROUTE_HEARTBEAT, .method = HTTP_POST, .handler = heartbeat_handler},
        {.uri = ESP111_PROTOCOL_ROUTE_STATUS, .method = HTTP_POST, .handler = status_or_sensor_handler},
        {.uri = ESP111_PROTOCOL_ROUTE_SENSOR, .method = HTTP_POST, .handler = status_or_sensor_handler},
        {.uri = ESP111_PROTOCOL_ROUTE_RADAR_RESULT, .method = HTTP_POST, .handler = radar_local_handler},
        {.uri = ESP111_PROTOCOL_ROUTE_RADAR_DEBUG, .method = HTTP_GET, .handler = radar_debug_handler},
        {.uri = ESP111_PROTOCOL_ROUTE_RADAR_HOME_SNAPSHOT, .method = HTTP_GET, .handler = radar_home_snapshot_handler},
        {.uri = ESP111_PROTOCOL_ROUTE_DEVICE_STREAM, .method = HTTP_POST, .handler = device_stream_handler},
        {.uri = ESP111_PROTOCOL_ROUTE_VOICE_TURN, .method = HTTP_POST, .handler = voice_proxy_handle_turn},
        {.uri = ESP111_PROTOCOL_ROUTE_WAKE_PROMPT_AUDIO, .method = HTTP_GET, .handler = wake_prompt_cache_gateway_handle_http},
        {.uri = ESP111_PROTOCOL_ROUTE_COMMANDS_PENDING, .method = HTTP_GET, .handler = commands_pending_handler},
        {.uri = ESP111_PROTOCOL_ROUTE_COMMAND_ACK_WILDCARD, .method = HTTP_POST, .handler = command_ack_handler},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ret = httpd_register_uri_handler(server, &routes[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "register route failed uri=%s ret=%s", routes[i].uri, esp_err_to_name(ret));
            s_server = server;
            s_server_routes_registered = false;
            (void)stop_server_locked("route_register_failure_cleanup");
            log_server_state_locked(LOCAL_HTTP_SERVER_STATE_FAILED, "route_register_failure", ret);
            xSemaphoreGive(lock);
            return ret;
        }
    }

    s_server = server;
    s_server_routes_registered = true;
    log_server_state_locked(LOCAL_HTTP_SERVER_STATE_RUNNING, reason, ESP_OK);
    ESP_LOGI(TAG, "local HTTP server started port=%u base=%s handle=%p",
             (unsigned int)gateway_config_get()->local_http_port,
             ESP111_PROTOCOL_LOCAL_BASE,
             (void *)s_server);
    xSemaphoreGive(lock);
    return ESP_OK;
}

esp_err_t local_http_server_stop(void)
{
    return local_http_server_stop_with_reason("direct_stop");
}

esp_err_t local_http_server_stop_with_reason(const char *reason)
{
    SemaphoreHandle_t lock = server_lock_handle();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const esp_err_t ret = stop_server_locked(reason);
    xSemaphoreGive(lock);
    return ret;
}

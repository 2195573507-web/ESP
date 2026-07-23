/**
 * @file server_comm_http.c
 * @brief C5 终端访问 S3 local HTTP 的公共客户端实现。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），负责 GET/POST JSON、raw PCM
 * 和流式响应读取的通用 HTTP 封装。它只面向 ESPS3 的 /local/v1 路径，不实现
 * Server 完整协议、不解析业务命令，也不决定 BME/voice/display 的业务语义。
 */

#include "server_comm_http.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "gateway_link.h"
#include "esp111_protocol_common.h"
#include "server_comm_config.h"
#include "server_comm_errors.h"

static const char *TAG = "local_gateway_comm";

#define SERVER_COMM_CHUNK_END "0\r\n\r\n"
#define SERVER_COMM_HTTP_OPEN_CHUNKED (-1)
#define SERVER_COMM_VOICE_BUSY_LOG_INTERVAL_MS 30000

typedef struct {
    char *buf;
    size_t buf_size;
    size_t len;
    bool overflow;
} server_comm_body_ctx_t;

typedef struct {
    server_comm_body_ctx_t *body_ctx;
    server_comm_http_response_t *response;
} server_comm_event_ctx_t;

struct server_comm_raw_stream {
    esp_http_client_handle_t client;
    TaskHandle_t owner_task;
    volatile bool abort_requested;
    bool headers_fetched;
    bool detailed_errors;
    uint32_t fetch_headers_timeout_ms;
    uint32_t read_timeout_ms;
    uint32_t total_timeout_ms;
    int64_t start_ms;
    int64_t first_response_byte_ms;
    size_t upload_bytes;
    size_t response_bytes;
    server_comm_http_response_t response_headers;
    server_comm_event_ctx_t event_ctx;
};

static void server_comm_reset_response(server_comm_http_response_t *response);
static void server_comm_stream_reset_transport(server_comm_raw_stream_t *stream);
static bool server_comm_request_is_reconnect_for_current_task(void);
static bool server_comm_endpoint_is_wake_prompt(const char *endpoint);
static bool server_comm_endpoint_is_voice_turn(const char *endpoint);
static void server_comm_log_voice_busy_skip(const char *label, const char *endpoint);

static portMUX_TYPE s_server_comm_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_voice_request_task;
static TaskHandle_t s_wake_prompt_request_task;
static bool s_non_voice_paused;
static uint32_t s_non_voice_inflight;
static int64_t s_last_voice_busy_log_ms;

void server_comm_http_set_voice_request_active(bool active)
{
    portENTER_CRITICAL(&s_server_comm_lock);
    s_voice_request_task = active ? xTaskGetCurrentTaskHandle() : NULL;
    portEXIT_CRITICAL(&s_server_comm_lock);
}

void server_comm_http_set_wake_prompt_request_active(bool active)
{
    portENTER_CRITICAL(&s_server_comm_lock);
    s_wake_prompt_request_task = active ? xTaskGetCurrentTaskHandle() : NULL;
    portEXIT_CRITICAL(&s_server_comm_lock);
}

void server_comm_http_set_non_voice_paused(bool paused)
{
    portENTER_CRITICAL(&s_server_comm_lock);
    s_non_voice_paused = paused;
    portEXIT_CRITICAL(&s_server_comm_lock);
}

esp_err_t server_comm_http_wait_for_non_voice_idle(uint32_t timeout_ms)
{
    const int64_t deadline_ms = (esp_timer_get_time() / 1000) + (int64_t)timeout_ms;
    while (true) {
        uint32_t inflight;
        portENTER_CRITICAL(&s_server_comm_lock);
        inflight = s_non_voice_inflight;
        portEXIT_CRITICAL(&s_server_comm_lock);
        if (inflight == 0U) {
            return ESP_OK;
        }
        if ((esp_timer_get_time() / 1000) >= deadline_ms) {
            ESP_LOGW(TAG,
                     "non-voice HTTP quiesce timeout inflight=%lu timeout_ms=%u",
                     (unsigned long)inflight,
                     (unsigned int)timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool server_comm_voice_request_is_active_for_current_task(void)
{
    TaskHandle_t task = NULL;

    portENTER_CRITICAL(&s_server_comm_lock);
    task = s_voice_request_task;
    portEXIT_CRITICAL(&s_server_comm_lock);

    return task != NULL && task == xTaskGetCurrentTaskHandle();
}

static bool server_comm_wake_prompt_request_is_active_for_current_task(void)
{
    TaskHandle_t task = NULL;

    portENTER_CRITICAL(&s_server_comm_lock);
    task = s_wake_prompt_request_task;
    portEXIT_CRITICAL(&s_server_comm_lock);

    return task != NULL && task == xTaskGetCurrentTaskHandle();
}

static bool server_comm_non_voice_paused(void)
{
    bool paused;

    portENTER_CRITICAL(&s_server_comm_lock);
    paused = s_non_voice_paused;
    portEXIT_CRITICAL(&s_server_comm_lock);

    return paused;
}

static void server_comm_end_non_voice_admission(bool admitted)
{
    if (!admitted) {
        return;
    }
    portENTER_CRITICAL(&s_server_comm_lock);
    if (s_non_voice_inflight > 0U) {
        s_non_voice_inflight--;
    }
    portEXIT_CRITICAL(&s_server_comm_lock);
}

static esp_err_t server_comm_begin_non_voice_admission(const char *label,
                                                        const char *endpoint,
                                                        bool *out_admitted)
{
    if (out_admitted == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_admitted = false;

    const bool voice_request = server_comm_voice_request_is_active_for_current_task();
    const bool wake_prompt_request =
        server_comm_wake_prompt_request_is_active_for_current_task() &&
        server_comm_endpoint_is_wake_prompt(endpoint);
    const bool voice_turn_endpoint = server_comm_endpoint_is_voice_turn(endpoint);
    const bool reconnect_request = server_comm_request_is_reconnect_for_current_task();
    if (voice_request || wake_prompt_request || voice_turn_endpoint || reconnect_request) {
        return ESP_OK;
    }

    bool paused = false;
    portENTER_CRITICAL(&s_server_comm_lock);
    paused = s_non_voice_paused;
    if (!paused) {
        s_non_voice_inflight++;
        *out_admitted = true;
    }
    portEXIT_CRITICAL(&s_server_comm_lock);
    if (paused) {
        server_comm_log_voice_busy_skip(label, endpoint);
        return SERVER_COMM_ERR_BLOCKED_BY_VOICE_BUSY;
    }
    return ESP_OK;
}

static bool server_comm_request_is_reconnect_for_current_task(void)
{
    return gateway_link_reconnect_request_is_active_for_current_task();
}

static const char *server_comm_endpoint_path(const char *endpoint)
{
    if (endpoint == NULL || endpoint[0] == '\0') {
        return NULL;
    }
    const char *scheme = strstr(endpoint, "://");
    if (scheme == NULL) {
        return endpoint;
    }
    const char *path = strchr(scheme + 3, '/');
    return path != NULL ? path : "/";
}

static bool server_comm_path_matches(const char *path, const char *allowed)
{
    if (path == NULL || allowed == NULL) {
        return false;
    }
    size_t allowed_len = strlen(allowed);
    return strncmp(path, allowed, allowed_len) == 0 &&
           (path[allowed_len] == '\0' ||
            path[allowed_len] == '?' ||
            path[allowed_len] == '#');
}

static bool server_comm_endpoint_is_wake_prompt(const char *endpoint)
{
    const char *path = server_comm_endpoint_path(endpoint);
    return server_comm_path_matches(path, ESP111_PROTOCOL_ROUTE_WAKE_PROMPT_AUDIO);
}

static bool server_comm_endpoint_is_voice_turn(const char *endpoint)
{
    const char *path = server_comm_endpoint_path(endpoint);
    return server_comm_path_matches(path, ESP111_PROTOCOL_ROUTE_VOICE_TURN);
}

static void server_comm_log_voice_busy_skip(const char *label, const char *endpoint)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    bool should_log = false;

    portENTER_CRITICAL(&s_server_comm_lock);
    if (s_last_voice_busy_log_ms == 0 ||
        now_ms - s_last_voice_busy_log_ms >= SERVER_COMM_VOICE_BUSY_LOG_INTERVAL_MS) {
        s_last_voice_busy_log_ms = now_ms;
        should_log = true;
    }
    portEXIT_CRITICAL(&s_server_comm_lock);

    if (should_log) {
        const char *path = server_comm_endpoint_path(endpoint);
        ESP_LOGI(TAG,
                 "voice busy, skip non-voice task=%s endpoint=%s path=%s reason=blocked_by_voice_busy",
                 label != NULL && label[0] != '\0' ? label : "http",
                 endpoint != NULL && endpoint[0] != '\0' ? endpoint : "<none>",
                 path != NULL && path[0] != '\0' ? path : "<none>");
    }
}

static int64_t server_comm_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static uint32_t server_comm_stream_config_timeout(uint32_t configured, uint32_t fallback)
{
    return configured > 0 ? configured : fallback;
}

static void server_comm_stream_apply_timing(server_comm_raw_stream_t *stream,
                                            const server_comm_raw_stream_config_t *config,
                                            uint32_t connect_timeout_ms)
{
    if (stream == NULL || config == NULL) {
        return;
    }

    stream->fetch_headers_timeout_ms =
        server_comm_stream_config_timeout(config->fetch_headers_timeout_ms, connect_timeout_ms);
    stream->read_timeout_ms =
        server_comm_stream_config_timeout(config->read_timeout_ms,
                                          SERVER_COMM_HTTP_STREAM_READ_TIMEOUT_MS);
    stream->total_timeout_ms = config->total_timeout_ms;
    stream->start_ms = server_comm_now_ms();
    stream->detailed_errors = server_comm_endpoint_is_wake_prompt(config->endpoint);
    server_comm_reset_response(&stream->response_headers);
    stream->event_ctx.body_ctx = NULL;
    stream->event_ctx.response = &stream->response_headers;
}

/* A failed header fetch leaves esp_http_client in a transport-specific error
 * state (notably after a peer reset). Close that handle before the caller's
 * normal stream cleanup so no stale socket/transport state can leak into a
 * later operation. The stream object itself remains owned by its caller. */
static void server_comm_stream_reset_transport(server_comm_raw_stream_t *stream)
{
    if (stream == NULL || stream->client == NULL) {
        return;
    }
    (void)esp_http_client_close(stream->client);
    stream->headers_fetched = false;
    stream->response_bytes = 0U;
    stream->first_response_byte_ms = 0;
    server_comm_reset_response(&stream->response_headers);
}

static esp_err_t server_comm_classify_header_open_error(esp_err_t ret, bool detailed)
{
    return detailed && ret == ESP_FAIL ? SERVER_COMM_ERR_HEADER_BUFFER_TOO_SMALL : ret;
}

static esp_err_t server_comm_classify_fetch_header_error(int64_t header_ret, bool detailed)
{
    if (!detailed) {
        return ESP_FAIL;
    }
    if (header_ret == -ESP_ERR_HTTP_EAGAIN) {
        return SERVER_COMM_ERR_FETCH_HEADER_TIMEOUT;
    }
    if (header_ret == ESP_FAIL) {
        return SERVER_COMM_ERR_HEADER_BUFFER_TOO_SMALL;
    }
    if (header_ret < 0 && header_ret >= (int64_t)INT32_MIN && header_ret <= (int64_t)INT32_MAX) {
        return (esp_err_t)(-header_ret);
    }
    return ESP_ERR_HTTP_FETCH_HEADER;
}

static bool server_comm_stream_total_timeout_expired(const server_comm_raw_stream_t *stream)
{
    if (stream == NULL || stream->total_timeout_ms == 0 || stream->start_ms <= 0) {
        return false;
    }

    return server_comm_now_ms() - stream->start_ms >= (int64_t)stream->total_timeout_ms;
}

const char *server_comm_err_to_name(server_comm_err_t err)
{
    switch (err) {
    case SERVER_COMM_ERR_WIFI_NOT_READY:
        return "SERVER_COMM_ERR_WIFI_NOT_READY";
    case SERVER_COMM_ERR_BAD_STATUS:
        return "SERVER_COMM_ERR_BAD_STATUS";
    case SERVER_COMM_ERR_RESPONSE_OVERFLOW:
        return "SERVER_COMM_ERR_RESPONSE_OVERFLOW";
    case SERVER_COMM_ERR_BUSY:
        return "SERVER_COMM_ERR_BUSY";
    case SERVER_COMM_ERR_BLOCKED_BY_VOICE_BUSY:
        return "SERVER_COMM_ERR_BLOCKED_BY_VOICE_BUSY";
    case SERVER_COMM_ERR_HEADER_BUFFER_TOO_SMALL:
        return "SERVER_COMM_ERR_HEADER_BUFFER_TOO_SMALL";
    case SERVER_COMM_ERR_FETCH_HEADER_TIMEOUT:
        return "SERVER_COMM_ERR_FETCH_HEADER_TIMEOUT";
    case SERVER_COMM_ERR_STREAM_READ_FAILED:
        return "SERVER_COMM_ERR_STREAM_READ_FAILED";
    default:
        return esp_err_to_name(err);
    }
}

bool server_comm_wifi_is_ready(void)
{
    return gateway_link_wifi_is_stable();
}

bool server_comm_http_status_is_success(int status_code)
{
    return status_code >= 200 && status_code < 300;
}

static esp_err_t server_comm_check_ready(const char *label, const char *endpoint)
{
    bool voice_request = server_comm_voice_request_is_active_for_current_task();
    bool reconnect_request = server_comm_request_is_reconnect_for_current_task();
    bool wake_prompt_request =
        server_comm_wake_prompt_request_is_active_for_current_task() &&
        server_comm_endpoint_is_wake_prompt(endpoint);
    bool voice_turn_endpoint = server_comm_endpoint_is_voice_turn(endpoint);

    if (!voice_request && !reconnect_request && !wake_prompt_request &&
        !voice_turn_endpoint && server_comm_non_voice_paused()) {
        server_comm_log_voice_busy_skip(label, endpoint);
        return SERVER_COMM_ERR_BLOCKED_BY_VOICE_BUSY;
    }

    if (!server_comm_wifi_is_ready()) {
        if (!gateway_link_in_reconnect_mode()) {
            ESP_LOGW(TAG,
                     "%s blocked: Wi-Fi is not ready endpoint=%s",
                     label != NULL ? label : "http",
                     endpoint != NULL && endpoint[0] != '\0' ? endpoint : "<none>");
        }
        return SERVER_COMM_ERR_WIFI_NOT_READY;
    }

    if (!reconnect_request) {
        if (voice_request || voice_turn_endpoint) {
            if (!gateway_link_can_start_voice_turn()) {
                return ESP_ERR_INVALID_STATE;
            }
        } else if (wake_prompt_request) {
            if (!gateway_link_is_ready()) {
                ESP_LOGI(TAG,
                         "gateway offline, skip wake prompt stream state=%s",
                         gateway_link_state_name(gateway_link_get_state()));
                return ESP_ERR_INVALID_STATE;
            }
        } else if (!gateway_link_can_run_non_voice_task(label)) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    uint32_t free_heap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
    uint32_t largest_block = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (free_heap < SERVER_COMM_HTTP_MIN_FREE_HEAP ||
        largest_block < SERVER_COMM_HTTP_MIN_LARGEST_BLOCK) {
        ESP_LOGE(TAG,
                 "%s blocked: low heap endpoint=%s free=%u largest=%u min_free=%u min_largest=%u",
                 label != NULL ? label : "http",
                 endpoint != NULL && endpoint[0] != '\0' ? endpoint : "<none>",
                 (unsigned int)free_heap,
                 (unsigned int)largest_block,
                 (unsigned int)SERVER_COMM_HTTP_MIN_FREE_HEAP,
                 (unsigned int)SERVER_COMM_HTTP_MIN_LARGEST_BLOCK);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void server_comm_capture_response_header(server_comm_http_response_t *response,
                                                const char *key,
                                                const char *value)
{
    if (response == NULL || key == NULL || value == NULL) {
        return;
    }

    if (strcasecmp(key, "Content-Type") == 0) {
        strlcpy(response->content_type, value, sizeof(response->content_type));
    } else if (strcasecmp(key, "Content-Length") == 0) {
        char *end = NULL;
        long long length = strtoll(value, &end, 10);
        if (end != value && length >= 0) {
            response->content_length = (int64_t)length;
        }
    } else if (strcasecmp(key, "Transfer-Encoding") == 0) {
        strlcpy(response->transfer_encoding, value, sizeof(response->transfer_encoding));
    } else if (strcasecmp(key, "X-Audio-Sample-Rate") == 0 ||
               strcasecmp(key, "X-Sample-Rate") == 0) {
        strlcpy(response->audio_sample_rate, value, sizeof(response->audio_sample_rate));
    } else if (strcasecmp(key, "X-Audio-Format") == 0) {
        strlcpy(response->audio_format, value, sizeof(response->audio_format));
    } else if (strcasecmp(key, "X-Audio-Channels") == 0 ||
               strcasecmp(key, "X-Channels") == 0) {
        strlcpy(response->audio_channels, value, sizeof(response->audio_channels));
    } else if (strcasecmp(key, "X-Audio-Version") == 0 ||
               strcasecmp(key, "X-Prompt-Version") == 0) {
        strlcpy(response->audio_version, value, sizeof(response->audio_version));
    } else if (strcasecmp(key, "X-Audio-Config-Hash") == 0 ||
               strcasecmp(key, "X-Voice-Config-Hash") == 0) {
        strlcpy(response->voice_config_hash, value, sizeof(response->voice_config_hash));
    }
}

static esp_err_t server_comm_body_event_handler(esp_http_client_event_t *evt)
{
    if (evt == NULL || evt->user_data == NULL) {
        return ESP_OK;
    }

    server_comm_event_ctx_t *event_ctx = (server_comm_event_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        server_comm_capture_response_header(event_ctx->response,
                                            evt->header_key,
                                            evt->header_value);
        return ESP_OK;
    }
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data == NULL || evt->data_len <= 0 ||
        event_ctx->body_ctx == NULL) {
        return ESP_OK;
    }

    server_comm_body_ctx_t *ctx = event_ctx->body_ctx;
    if (ctx->buf == NULL || ctx->buf_size == 0) {
        return ESP_OK;
    }

    size_t remain = ctx->buf_size > ctx->len ? ctx->buf_size - ctx->len : 0;
    if (remain > 0) {
        size_t usable = ctx->buf_size > 0 ? ctx->buf_size - 1 : 0;
        remain = usable > ctx->len ? usable - ctx->len : 0;
    }

    size_t copy_len = (size_t)evt->data_len <= remain ? (size_t)evt->data_len : remain;
    if (copy_len > 0) {
        memcpy(ctx->buf + ctx->len, evt->data, copy_len);
        ctx->len += copy_len;
        ctx->buf[ctx->len] = '\0';
    }

    if ((size_t)evt->data_len > copy_len) {
        ctx->overflow = true;
    }

    return ESP_OK;
}

static void server_comm_reset_response(server_comm_http_response_t *response)
{
    if (response != NULL) {
        memset(response, 0, sizeof(*response));
        response->content_length = -1;
    }
}

static void server_comm_capture_response_info(esp_http_client_handle_t client,
                                              server_comm_body_ctx_t *body_ctx,
                                              const server_comm_http_response_t *captured_headers,
                                              server_comm_http_response_t *response)
{
    if (response == NULL || client == NULL) {
        return;
    }

    if (captured_headers != NULL && captured_headers != response) {
        response->content_length = captured_headers->content_length;
        strlcpy(response->content_type,
                captured_headers->content_type,
                sizeof(response->content_type));
        strlcpy(response->transfer_encoding,
                captured_headers->transfer_encoding,
                sizeof(response->transfer_encoding));
        strlcpy(response->audio_format,
                captured_headers->audio_format,
                sizeof(response->audio_format));
        strlcpy(response->audio_sample_rate,
                captured_headers->audio_sample_rate,
                sizeof(response->audio_sample_rate));
        strlcpy(response->audio_channels,
                captured_headers->audio_channels,
                sizeof(response->audio_channels));
        strlcpy(response->audio_version,
                captured_headers->audio_version,
                sizeof(response->audio_version));
        strlcpy(response->voice_config_hash,
                captured_headers->voice_config_hash,
                sizeof(response->voice_config_hash));
    }

    response->status_code = esp_http_client_get_status_code(client);
    int64_t content_length = esp_http_client_get_content_length(client);
    if (content_length >= 0 || response->content_length < 0) {
        response->content_length = content_length;
    }
    response->chunked = esp_http_client_is_chunked_response(client);
    if (body_ctx != NULL) {
        response->body_len = body_ctx->len;
        response->body_overflow = body_ctx->overflow;
    }

}

static esp_err_t server_comm_set_common_headers(esp_http_client_handle_t client,
                                                const char *content_type,
                                                const server_comm_header_t *headers,
                                                size_t header_count)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = esp_http_client_set_header(client, "X-Device-Id", server_comm_get_device_id());
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_http_client_set_header(client, "X-Gateway-Id", server_comm_get_gateway_id());
    if (ret != ESP_OK) {
        return ret;
    }

    if (content_type != NULL && content_type[0] != '\0') {
        ret = esp_http_client_set_header(client, "Content-Type", content_type);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    for (size_t i = 0; i < header_count; i++) {
        if (headers[i].key == NULL || headers[i].value == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        ret = esp_http_client_set_header(client, headers[i].key, headers[i].value);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

static esp_err_t server_comm_perform(esp_http_client_method_t method,
                                     const char *endpoint,
                                     const char *content_type,
                                     const server_comm_header_t *headers,
                                     size_t header_count,
                                     const uint8_t *body,
                                     size_t body_len,
                                     uint32_t timeout_ms,
                                     char *response_body,
                                     size_t response_body_size,
                                     server_comm_http_response_t *response)
{
    if (endpoint == NULL || endpoint[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (body_len > 0 && body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (response_body_size > 0 && response_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool voice_request = server_comm_voice_request_is_active_for_current_task();
    bool reconnect_request = server_comm_request_is_reconnect_for_current_task();
    bool non_voice_admitted = false;
    esp_err_t ret = server_comm_begin_non_voice_admission("http request",
                                                           endpoint,
                                                           &non_voice_admitted);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = server_comm_check_ready("http request", endpoint);
    if (ret != ESP_OK) {
        server_comm_end_non_voice_admission(non_voice_admitted);
        return ret;
    }

    char *url = (char *)heap_caps_calloc(1, SERVER_COMM_URL_BUFFER_SIZE, MALLOC_CAP_8BIT);
    if (url == NULL) {
        ESP_LOGE(TAG,
                 "http url buffer alloc failed endpoint=%s bytes=%u",
                 endpoint,
                 (unsigned int)SERVER_COMM_URL_BUFFER_SIZE);
        gateway_link_record_http_result(ESP_ERR_NO_MEM, voice_request, reconnect_request);
        server_comm_end_non_voice_admission(non_voice_admitted);
        return ESP_ERR_NO_MEM;
    }

    ret = server_comm_build_url(endpoint, url, SERVER_COMM_URL_BUFFER_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "build URL failed endpoint=%s ret=%s", endpoint, esp_err_to_name(ret));
        heap_caps_free(url);
        gateway_link_record_http_result(ret, voice_request, reconnect_request);
        server_comm_end_non_voice_admission(non_voice_admitted);
        return ret;
    }

    if (response_body != NULL && response_body_size > 0) {
        response_body[0] = '\0';
    }

    server_comm_body_ctx_t body_ctx = {
        .buf = response_body,
        .buf_size = response_body_size,
        .len = 0,
        .overflow = false,
    };
    server_comm_reset_response(response);
    server_comm_event_ctx_t event_ctx = {
        .body_ctx = &body_ctx,
        .response = response,
    };

    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = timeout_ms > 0 ? (int)timeout_ms : (int)server_comm_get_default_timeout_ms(),
        .event_handler = server_comm_body_event_handler,
        .user_data = &event_ctx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "http init failed url=%s", url);
        heap_caps_free(url);
        gateway_link_record_http_result(ESP_ERR_NO_MEM, voice_request, reconnect_request);
        server_comm_end_non_voice_admission(non_voice_admitted);
        return ESP_ERR_NO_MEM;
    }

    ret = server_comm_set_common_headers(client, content_type, headers, header_count);
    if (ret == ESP_OK && body_len > 0) {
        ret = esp_http_client_set_post_field(client, (const char *)body, (int)body_len);
    }
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "%s %s", method == HTTP_METHOD_GET ? "GET" : "POST", url);
        ret = esp_http_client_perform(client);
    }

    server_comm_capture_response_info(client, &body_ctx, response, response);
    int status = response != NULL ? response->status_code : esp_http_client_get_status_code(client);
    ESP_LOGD(TAG,
             "http response status=%d content_length=%lld body_len=%u overflow=%d",
             status,
             response != NULL ? (long long)response->content_length :
                                (long long)esp_http_client_get_content_length(client),
             (unsigned int)body_ctx.len,
             body_ctx.overflow ? 1 : 0);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    heap_caps_free(url);
    server_comm_end_non_voice_admission(non_voice_admitted);

    if (ret != ESP_OK) {
        if (!gateway_link_in_reconnect_mode()) {
            ESP_LOGE(TAG, "http request failed endpoint=%s ret=%s", endpoint, esp_err_to_name(ret));
        }
        gateway_link_record_http_result(ret, voice_request, reconnect_request);
        return ret;
    }
    if (body_ctx.overflow) {
        gateway_link_record_http_result(SERVER_COMM_ERR_RESPONSE_OVERFLOW,
                                        voice_request,
                                        reconnect_request);
        return SERVER_COMM_ERR_RESPONSE_OVERFLOW;
    }
    if (!server_comm_http_status_is_success(status)) {
        ESP_LOGW(TAG,
                 "http bad status endpoint=%s status=%d content_length=%lld body_len=%u",
                 endpoint,
                 status,
                 response != NULL ? (long long)response->content_length :
                                    (long long)body_ctx.len,
                 (unsigned int)body_ctx.len);
        gateway_link_record_http_result(SERVER_COMM_ERR_BAD_STATUS,
                                        voice_request,
                                        reconnect_request);
        return SERVER_COMM_ERR_BAD_STATUS;
    }

    gateway_link_record_http_result(ESP_OK, voice_request, reconnect_request);
    return ESP_OK;
}

esp_err_t server_comm_http_get_json(const char *endpoint,
                                    uint32_t timeout_ms,
                                    char *response_body,
                                    size_t response_body_size,
                                    server_comm_http_response_t *response)
{
    return server_comm_perform(HTTP_METHOD_GET,
                               endpoint,
                               NULL,
                               NULL,
                               0,
                               NULL,
                               0,
                               timeout_ms,
                               response_body,
                               response_body_size,
                               response);
}

esp_err_t server_comm_http_get_json_with_headers(const char *endpoint,
                                                 const server_comm_header_t *headers,
                                                 size_t header_count,
                                                 uint32_t timeout_ms,
                                                 char *response_body,
                                                 size_t response_body_size,
                                                 server_comm_http_response_t *response)
{
    return server_comm_perform(HTTP_METHOD_GET,
                               endpoint,
                               NULL,
                               headers,
                               header_count,
                               NULL,
                               0,
                               timeout_ms,
                               response_body,
                               response_body_size,
                               response);
}

esp_err_t server_comm_http_post_json(const char *endpoint,
                                     const char *json_body,
                                     uint32_t timeout_ms,
                                     char *response_body,
                                     size_t response_body_size,
                                     server_comm_http_response_t *response)
{
    if (json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return server_comm_perform(HTTP_METHOD_POST,
                               endpoint,
                               "application/json",
                               NULL,
                               0,
                               (const uint8_t *)json_body,
                               strlen(json_body),
                               timeout_ms,
                               response_body,
                               response_body_size,
                               response);
}

esp_err_t server_comm_http_post_json_with_headers(const char *endpoint,
                                                  const char *json_body,
                                                  const server_comm_header_t *headers,
                                                  size_t header_count,
                                                  uint32_t timeout_ms,
                                                  char *response_body,
                                                  size_t response_body_size,
                                                  server_comm_http_response_t *response)
{
    if (json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return server_comm_perform(HTTP_METHOD_POST,
                               endpoint,
                               "application/json",
                               headers,
                               header_count,
                               (const uint8_t *)json_body,
                               strlen(json_body),
                               timeout_ms,
                               response_body,
                               response_body_size,
                               response);
}

esp_err_t server_comm_http_post_raw(const char *endpoint,
                                    const char *content_type,
                                    const uint8_t *body,
                                    size_t body_len,
                                    uint32_t timeout_ms,
                                    char *response_body,
                                    size_t response_body_size,
                                    server_comm_http_response_t *response)
{
    return server_comm_perform(HTTP_METHOD_POST,
                               endpoint,
                               content_type,
                               NULL,
                               0,
                               body,
                               body_len,
                               timeout_ms,
                               response_body,
                               response_body_size,
                               response);
}

esp_err_t server_comm_http_post_raw_stream_begin(const server_comm_raw_stream_config_t *config,
                                                server_comm_raw_stream_t **out_stream)
{
    if (config == NULL || out_stream == NULL ||
        config->endpoint == NULL || config->endpoint[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    *out_stream = NULL;
    bool voice_request = server_comm_voice_request_is_active_for_current_task();
    bool reconnect_request = server_comm_request_is_reconnect_for_current_task();
    esp_err_t ret = server_comm_check_ready("raw stream", config->endpoint);
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t connect_timeout_ms =
        config->timeout_ms > 0 ? config->timeout_ms : server_comm_get_default_timeout_ms();
    char *url = (char *)heap_caps_calloc(1, SERVER_COMM_URL_BUFFER_SIZE, MALLOC_CAP_8BIT);
    if (url == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = server_comm_build_url(config->endpoint, url, SERVER_COMM_URL_BUFFER_SIZE);
    if (ret != ESP_OK) {
        heap_caps_free(url);
        return ret;
    }

    server_comm_raw_stream_t *stream = (server_comm_raw_stream_t *)calloc(1, sizeof(*stream));
    if (stream == NULL) {
        heap_caps_free(url);
        return ESP_ERR_NO_MEM;
    }
    server_comm_stream_apply_timing(stream, config, connect_timeout_ms);

    esp_http_client_config_t http_config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = (int)connect_timeout_ms,
        .buffer_size = config->buffer_size > 0 ? config->buffer_size :
                                                  (int)SERVER_COMM_HTTP_READ_CHUNK_BYTES,
        .buffer_size_tx = config->tx_buffer_size > 0 ? config->tx_buffer_size : 512,
        .event_handler = server_comm_body_event_handler,
        .user_data = &stream->event_ctx,
    };

    stream->client = esp_http_client_init(&http_config);
    if (stream->client == NULL) {
        heap_caps_free(url);
        free(stream);
        return ESP_ERR_NO_MEM;
    }

    ret = server_comm_set_common_headers(stream->client,
                                         config->content_type,
                                         config->headers,
                                         config->header_count);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "POST stream begin %s", url);
        ret = esp_http_client_open(stream->client, SERVER_COMM_HTTP_OPEN_CHUNKED);
    }
    heap_caps_free(url);

    if (ret != ESP_OK) {
        ret = server_comm_classify_header_open_error(ret,
                                                     server_comm_endpoint_is_wake_prompt(config->endpoint));
        if (!gateway_link_in_reconnect_mode()) {
            ESP_LOGE(TAG, "POST stream open failed endpoint=%s ret=%s rx_buffer=%d tx_buffer=%d",
                     config->endpoint,
                     server_comm_err_to_name(ret),
                     config->buffer_size > 0 ? config->buffer_size :
                                               (int)SERVER_COMM_HTTP_READ_CHUNK_BYTES,
                     config->tx_buffer_size > 0 ? config->tx_buffer_size : 512);
        }
        server_comm_http_post_raw_stream_close(stream);
        gateway_link_record_http_result(ret, voice_request, reconnect_request);
        return ret;
    }

    gateway_link_record_http_result(ESP_OK, voice_request, reconnect_request);
    stream->owner_task = xTaskGetCurrentTaskHandle();
    *out_stream = stream;
    return ESP_OK;
}

static esp_err_t server_comm_stream_write_all(server_comm_raw_stream_t *stream,
                                              const char *data,
                                              size_t len)
{
    if (stream == NULL || stream->client == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t written = 0;
    while (written < len) {
        int ret = esp_http_client_write(stream->client,
                                        data + written,
                                        (int)(len - written));
        if (ret < 0) {
            ESP_LOGE(TAG,
                     "stream write failed ret=%d written=%u total=%u",
                     ret,
                     (unsigned int)written,
                     (unsigned int)len);
            return ESP_FAIL;
        }
        if (ret == 0) {
            ESP_LOGE(TAG,
                     "stream write stalled written=%u total=%u",
                     (unsigned int)written,
                     (unsigned int)len);
            return ESP_ERR_TIMEOUT;
        }
        written += (size_t)ret;
    }

    return ESP_OK;
}

esp_err_t server_comm_http_post_raw_fixed_stream_begin(const server_comm_raw_stream_config_t *config,
                                                       const uint8_t *body,
                                                       size_t body_len,
                                                       server_comm_raw_stream_t **out_stream)
{
    if (config == NULL || out_stream == NULL ||
        config->endpoint == NULL || config->endpoint[0] == '\0' ||
        body == NULL || body_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_stream = NULL;
    bool voice_request = server_comm_voice_request_is_active_for_current_task();
    bool reconnect_request = server_comm_request_is_reconnect_for_current_task();
    esp_err_t ret = server_comm_check_ready("raw fixed stream", config->endpoint);
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t connect_timeout_ms =
        config->timeout_ms > 0 ? config->timeout_ms : server_comm_get_default_timeout_ms();
    char *url = (char *)heap_caps_calloc(1, SERVER_COMM_URL_BUFFER_SIZE, MALLOC_CAP_8BIT);
    if (url == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = server_comm_build_url(config->endpoint, url, SERVER_COMM_URL_BUFFER_SIZE);
    if (ret != ESP_OK) {
        heap_caps_free(url);
        return ret;
    }

    server_comm_raw_stream_t *stream = (server_comm_raw_stream_t *)calloc(1, sizeof(*stream));
    if (stream == NULL) {
        heap_caps_free(url);
        return ESP_ERR_NO_MEM;
    }
    server_comm_stream_apply_timing(stream, config, connect_timeout_ms);

    esp_http_client_config_t http_config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = (int)connect_timeout_ms,
        .buffer_size = config->buffer_size > 0 ? config->buffer_size :
                                                  (int)SERVER_COMM_HTTP_READ_CHUNK_BYTES,
        .buffer_size_tx = config->tx_buffer_size > 0 ? config->tx_buffer_size : 512,
        .event_handler = server_comm_body_event_handler,
        .user_data = &stream->event_ctx,
    };

    stream->client = esp_http_client_init(&http_config);
    if (stream->client == NULL) {
        heap_caps_free(url);
        free(stream);
        return ESP_ERR_NO_MEM;
    }

    ret = server_comm_set_common_headers(stream->client,
                                         config->content_type,
                                         config->headers,
                                         config->header_count);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "POST fixed stream begin %s bytes=%u", url, (unsigned int)body_len);
        ret = esp_http_client_open(stream->client, (int)body_len);
    }
    heap_caps_free(url);

    if (ret == ESP_OK) {
        ret = server_comm_stream_write_all(stream, (const char *)body, body_len);
    }
    if (ret != ESP_OK) {
        ret = server_comm_classify_header_open_error(ret,
                                                     server_comm_endpoint_is_wake_prompt(config->endpoint));
        if (!gateway_link_in_reconnect_mode()) {
            ESP_LOGE(TAG, "POST fixed stream failed endpoint=%s ret=%s rx_buffer=%d tx_buffer=%d",
                     config->endpoint,
                     server_comm_err_to_name(ret),
                     config->buffer_size > 0 ? config->buffer_size :
                                               (int)SERVER_COMM_HTTP_READ_CHUNK_BYTES,
                     config->tx_buffer_size > 0 ? config->tx_buffer_size : 512);
        }
        server_comm_http_post_raw_stream_close(stream);
        gateway_link_record_http_result(ret, voice_request, reconnect_request);
        return ret;
    }

    gateway_link_record_http_result(ESP_OK, voice_request, reconnect_request);
    stream->upload_bytes = body_len;
    stream->owner_task = xTaskGetCurrentTaskHandle();
    *out_stream = stream;
    return ESP_OK;
}

esp_err_t server_comm_http_get_raw_stream_begin(const server_comm_raw_stream_config_t *config,
                                                server_comm_raw_stream_t **out_stream)
{
    if (config == NULL || out_stream == NULL ||
        config->endpoint == NULL || config->endpoint[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    *out_stream = NULL;
    bool voice_request = server_comm_voice_request_is_active_for_current_task();
    bool reconnect_request = server_comm_request_is_reconnect_for_current_task();
    esp_err_t ret = server_comm_check_ready("raw get stream", config->endpoint);
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t connect_timeout_ms =
        config->timeout_ms > 0 ? config->timeout_ms : server_comm_get_default_timeout_ms();
    char *url = (char *)heap_caps_calloc(1, SERVER_COMM_URL_BUFFER_SIZE, MALLOC_CAP_8BIT);
    if (url == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = server_comm_build_url(config->endpoint, url, SERVER_COMM_URL_BUFFER_SIZE);
    if (ret != ESP_OK) {
        heap_caps_free(url);
        return ret;
    }

    server_comm_raw_stream_t *stream = (server_comm_raw_stream_t *)calloc(1, sizeof(*stream));
    if (stream == NULL) {
        heap_caps_free(url);
        return ESP_ERR_NO_MEM;
    }
    server_comm_stream_apply_timing(stream, config, connect_timeout_ms);

    esp_http_client_config_t http_config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = (int)connect_timeout_ms,
        .buffer_size = config->buffer_size > 0 ? config->buffer_size :
                                                  (int)SERVER_COMM_HTTP_READ_CHUNK_BYTES,
        .buffer_size_tx = config->tx_buffer_size > 0 ? config->tx_buffer_size : 256,
        .event_handler = server_comm_body_event_handler,
        .user_data = &stream->event_ctx,
    };

    stream->client = esp_http_client_init(&http_config);
    if (stream->client == NULL) {
        heap_caps_free(url);
        free(stream);
        return ESP_ERR_NO_MEM;
    }

    ret = server_comm_set_common_headers(stream->client,
                                         config->content_type,
                                         config->headers,
                                         config->header_count);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "GET raw stream begin %s", url);
        ret = esp_http_client_open(stream->client, 0);
    }
    heap_caps_free(url);

    if (ret != ESP_OK) {
        ret = server_comm_classify_header_open_error(ret,
                                                     server_comm_endpoint_is_wake_prompt(config->endpoint));
        if (!gateway_link_in_reconnect_mode()) {
            ESP_LOGE(TAG, "GET raw stream open failed endpoint=%s ret=%s rx_buffer=%d tx_buffer=%d",
                     config->endpoint,
                     server_comm_err_to_name(ret),
                     config->buffer_size > 0 ? config->buffer_size :
                                               (int)SERVER_COMM_HTTP_READ_CHUNK_BYTES,
                     config->tx_buffer_size > 0 ? config->tx_buffer_size : 256);
        }
        server_comm_http_post_raw_stream_close(stream);
        gateway_link_record_http_result(ret, voice_request, reconnect_request);
        return ret;
    }

    gateway_link_record_http_result(ESP_OK, voice_request, reconnect_request);
    stream->owner_task = xTaskGetCurrentTaskHandle();
    *out_stream = stream;
    return ESP_OK;
}

esp_err_t server_comm_http_post_raw_stream_write(server_comm_raw_stream_t *stream,
                                                const uint8_t *data,
                                                size_t len)
{
    if (stream == NULL || stream->client == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char header[16];
    int header_len = snprintf(header, sizeof(header), "%X\r\n", (unsigned int)len);
    if (header_len <= 0 || header_len >= (int)sizeof(header)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = server_comm_stream_write_all(stream, header, (size_t)header_len);
    if (ret == ESP_OK) {
        ret = server_comm_stream_write_all(stream, (const char *)data, len);
    }
    if (ret == ESP_OK) {
        ret = server_comm_stream_write_all(stream, "\r\n", 2);
    }
    if (ret == ESP_OK) {
        stream->upload_bytes += len;
    }
    gateway_link_record_http_result(ret,
                                    server_comm_voice_request_is_active_for_current_task(),
                                    server_comm_request_is_reconnect_for_current_task());

    return ret;
}

esp_err_t server_comm_http_post_raw_stream_finish_upload(server_comm_raw_stream_t *stream)
{
    if (stream == NULL || stream->client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = server_comm_stream_write_all(stream,
                                                 SERVER_COMM_CHUNK_END,
                                                 strlen(SERVER_COMM_CHUNK_END));
    gateway_link_record_http_result(ret,
                                    server_comm_voice_request_is_active_for_current_task(),
                                    server_comm_request_is_reconnect_for_current_task());
    return ret;
}

esp_err_t server_comm_http_fetch_headers(server_comm_raw_stream_t *stream,
                                         server_comm_http_response_t *response)
{
    if (stream == NULL || stream->client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    server_comm_reset_response(response);

    if (stream->fetch_headers_timeout_ms > 0) {
        (void)esp_http_client_set_timeout_ms(stream->client,
                                             (int)stream->fetch_headers_timeout_ms);
    }
    int64_t header_ret = esp_http_client_fetch_headers(stream->client);
    if (header_ret < 0) {
        server_comm_stream_reset_transport(stream);
        esp_err_t fetch_ret =
            server_comm_classify_fetch_header_error(header_ret, stream->detailed_errors);
        if (!gateway_link_in_reconnect_mode()) {
            ESP_LOGE(TAG,
                     "fetch headers failed: %lld ret=%s timeout_ms=%u elapsed_ms=%lld",
                     (long long)header_ret,
                     server_comm_err_to_name(fetch_ret),
                     (unsigned int)stream->fetch_headers_timeout_ms,
                     (long long)(server_comm_now_ms() - stream->start_ms));
        }
        gateway_link_record_http_result(fetch_ret,
                                        server_comm_voice_request_is_active_for_current_task(),
                                        server_comm_request_is_reconnect_for_current_task());
        return fetch_ret;
    }

    stream->headers_fetched = true;
    server_comm_capture_response_info(stream->client,
                                      NULL,
                                      &stream->response_headers,
                                      response);
    gateway_link_record_http_result(ESP_OK,
                                    server_comm_voice_request_is_active_for_current_task(),
                                    server_comm_request_is_reconnect_for_current_task());
    return ESP_OK;
}

static bool server_comm_stream_response_complete(server_comm_raw_stream_t *stream,
                                                 int64_t content_length,
                                                 bool chunked,
                                                 size_t total_read)
{
    if (stream == NULL || stream->client == NULL) {
        return true;
    }
    if (content_length >= 0) {
        return total_read >= (size_t)content_length;
    }
    if (chunked) {
        return esp_http_client_is_complete_data_received(stream->client);
    }
    return false;
}

static bool server_comm_unknown_length_response_closed(int read_len,
                                                       int64_t content_length,
                                                       bool chunked,
                                                       size_t total_read)
{
    return content_length < 0 &&
           !chunked &&
           total_read > 0 &&
           (read_len == 0 || read_len == -ESP_ERR_HTTP_CONNECTION_CLOSED);
}

esp_err_t server_comm_http_read_response(server_comm_raw_stream_t *stream,
                                         server_comm_on_data_cb_t on_data,
                                         void *user_ctx,
                                         server_comm_http_response_t *response)
{
    if (stream == NULL || stream->client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    if (!stream->headers_fetched) {
        ret = server_comm_http_fetch_headers(stream, response);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    int64_t content_length = esp_http_client_get_content_length(stream->client);
    bool chunked = esp_http_client_is_chunked_response(stream->client);
    if (response != NULL) {
        response->content_length = content_length;
        response->chunked = chunked;
    }
    int status = response != NULL ? response->status_code :
                                    esp_http_client_get_status_code(stream->client);
    if (!server_comm_http_status_is_success(status)) {
        ESP_LOGW(TAG,
                 "http stream bad status status=%d content_length=%lld",
                 status,
                 response != NULL ? (long long)response->content_length :
                                    (long long)esp_http_client_get_content_length(stream->client));
        return SERVER_COMM_ERR_BAD_STATUS;
    }

    if (status == 204) {
        ESP_LOGI(TAG,
                 "response read done content_length=0 chunked=%d total_read=0 complete=1 empty_read_count=0 first_byte_ms=0 last_byte_ms=0",
                 chunked ? 1 : 0);
        return ESP_OK;
    }

    uint8_t *read_buf =
        (uint8_t *)heap_caps_malloc(SERVER_COMM_HTTP_READ_CHUNK_BYTES, MALLOC_CAP_8BIT);
    if (read_buf == NULL) {
        ESP_LOGE(TAG,
                 "response read buffer alloc failed bytes=%u",
                 (unsigned int)SERVER_COMM_HTTP_READ_CHUNK_BYTES);
        return ESP_ERR_NO_MEM;
    }

    size_t total_read = 0;
    int empty_reads = 0;
    int64_t first_byte_ms = 0;
    int64_t last_byte_ms = 0;
    bool complete = server_comm_stream_response_complete(stream,
                                                         content_length,
                                                         chunked,
                                                         total_read);
    if (stream->read_timeout_ms > 0) {
        (void)esp_http_client_set_timeout_ms(stream->client,
                                             (int)stream->read_timeout_ms);
    }

    while (!complete) {
        if (stream->abort_requested || !gateway_link_is_ready()) {
            heap_caps_free(read_buf);
            gateway_link_record_http_result(ESP_ERR_INVALID_STATE,
                                            server_comm_voice_request_is_active_for_current_task(),
                                            server_comm_request_is_reconnect_for_current_task());
            return ESP_ERR_INVALID_STATE;
        }
        if (server_comm_stream_total_timeout_expired(stream)) {
            ESP_LOGE(TAG,
                     "response read total timeout content_length=%lld chunked=%d total_read=%u complete=%d empty_read_count=%d first_byte_ms=%lld last_byte_ms=%lld timeout_ms=%u",
                     (long long)content_length,
                     chunked ? 1 : 0,
                     (unsigned int)total_read,
                     complete ? 1 : 0,
                     empty_reads,
                     (long long)first_byte_ms,
                     (long long)last_byte_ms,
                     (unsigned int)stream->total_timeout_ms);
            heap_caps_free(read_buf);
            gateway_link_record_http_result(ESP_ERR_TIMEOUT,
                                            server_comm_voice_request_is_active_for_current_task(),
                                            server_comm_request_is_reconnect_for_current_task());
            return ESP_ERR_TIMEOUT;
        }

        int read_len = esp_http_client_read(stream->client,
                                            (char *)read_buf,
                                            SERVER_COMM_HTTP_READ_CHUNK_BYTES);
        if (read_len > 0) {
            empty_reads = 0;
            total_read += (size_t)read_len;
            stream->response_bytes += (size_t)read_len;
            int64_t now_ms = server_comm_now_ms();
            if (stream->first_response_byte_ms == 0) {
                stream->first_response_byte_ms = now_ms;
            }
            if (first_byte_ms == 0) {
                first_byte_ms = stream->first_response_byte_ms;
            }
            last_byte_ms = now_ms;
            if (on_data != NULL) {
                ret = on_data(read_buf, (size_t)read_len, user_ctx);
                if (ret != ESP_OK) {
                    heap_caps_free(read_buf);
                    return ret;
                }
            }
            complete = server_comm_stream_response_complete(stream,
                                                            content_length,
                                                            chunked,
                                                            total_read);
            continue;
        }

        if (server_comm_unknown_length_response_closed(read_len,
                                                       content_length,
                                                       chunked,
                                                       total_read)) {
            complete = true;
            break;
        }

        complete = server_comm_stream_response_complete(stream,
                                                        content_length,
                                                        chunked,
                                                        total_read);
        if (complete) {
            break;
        }

        if (read_len < 0 && read_len != -ESP_ERR_HTTP_EAGAIN) {
            if (!gateway_link_in_reconnect_mode()) {
                ESP_LOGE(TAG,
                         "response read failed read_len=%d content_length=%lld chunked=%d total_read=%u complete=%d empty_read_count=%d first_byte_ms=%lld last_byte_ms=%lld",
                         read_len,
                         (long long)content_length,
                         chunked ? 1 : 0,
                         (unsigned int)total_read,
                         complete ? 1 : 0,
                         empty_reads,
                         (long long)first_byte_ms,
                         (long long)last_byte_ms);
            }
            heap_caps_free(read_buf);
            esp_err_t read_ret = stream->detailed_errors ?
                                 SERVER_COMM_ERR_STREAM_READ_FAILED :
                                 ESP_FAIL;
            gateway_link_record_http_result(read_ret,
                                            server_comm_voice_request_is_active_for_current_task(),
                                            server_comm_request_is_reconnect_for_current_task());
            return read_ret;
        }

        if (read_len == -ESP_ERR_HTTP_EAGAIN) {
            gateway_link_record_http_result(ESP_ERR_HTTP_EAGAIN,
                                            server_comm_voice_request_is_active_for_current_task(),
                                            server_comm_request_is_reconnect_for_current_task());
        }

        empty_reads++;
        if (empty_reads >= SERVER_COMM_HTTP_MAX_EMPTY_READS) {
            ESP_LOGE(TAG,
                     "response read timeout content_length=%lld chunked=%d total_read=%u complete=%d empty_read_count=%d first_byte_ms=%lld last_byte_ms=%lld",
                     (long long)content_length,
                     chunked ? 1 : 0,
                     (unsigned int)total_read,
                     complete ? 1 : 0,
                     empty_reads,
                     (long long)first_byte_ms,
                     (long long)last_byte_ms);
            heap_caps_free(read_buf);
            gateway_link_record_http_result(ESP_ERR_TIMEOUT,
                                            server_comm_voice_request_is_active_for_current_task(),
                                            server_comm_request_is_reconnect_for_current_task());
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(SERVER_COMM_HTTP_EMPTY_READ_DELAY_MS));
    }

    if (response != NULL) {
        response->body_len = total_read;
    }
    ESP_LOGI(TAG,
             "response read done content_length=%lld chunked=%d total_read=%u complete=%d empty_read_count=%d first_byte_ms=%lld last_byte_ms=%lld",
             (long long)content_length,
             chunked ? 1 : 0,
             (unsigned int)total_read,
             complete ? 1 : 0,
             empty_reads,
             (long long)first_byte_ms,
             (long long)last_byte_ms);
    heap_caps_free(read_buf);
    gateway_link_record_http_result(ESP_OK,
                                    server_comm_voice_request_is_active_for_current_task(),
                                    server_comm_request_is_reconnect_for_current_task());
    return ESP_OK;
}

void server_comm_http_post_raw_stream_close(server_comm_raw_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }
    if (stream->client != NULL) {
        esp_http_client_close(stream->client);
        esp_http_client_cleanup(stream->client);
        stream->client = NULL;
    }
    free(stream);
}

void server_comm_http_post_raw_stream_request_abort(server_comm_raw_stream_t *stream)
{
    if (stream != NULL) {
        stream->abort_requested = true;
        if (stream->client != NULL) {
            esp_err_t ret = esp_http_client_cancel_request(stream->client);
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG,
                         "stream cancel request failed ret=%s",
                         esp_err_to_name(ret));
            }
        }
    }
}

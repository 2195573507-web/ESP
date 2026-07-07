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

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_stack_monitor.h"
#include "esp111_protocol_common.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
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
    SERVER_CLIENT_VOICE_TOTAL_TIMEOUT_MS = VOICE_TOTAL_TIMEOUT_MS,
    SERVER_CLIENT_VOICE_IO_TIMEOUT_MS = ESP111_PROTOCOL_SERVER_VOICE_TIMEOUT_MS,
};

static uint32_t s_request_seq;

typedef struct {
    char *body;
    size_t body_size;
    size_t body_len;
    bool overflow;
} server_body_ctx_t;

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
    return gateway_wifi_is_net_ready() && gateway_wifi_is_sta_connected() &&
           s3_scheduler_is_server_upload_allowed();
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

static esp_err_t perform_json_once(esp_http_client_method_t method,
                                   const char *endpoint,
                                   const char *json_body,
                                   char *response_body,
                                   size_t response_body_size,
                                   int *http_status)
{
    if (!server_link_ready()) {
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
    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = SERVER_CLIENT_HTTP_CONNECT_TIMEOUT_MS,
        .event_handler = body_event_handler,
        .user_data = &body_ctx,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = esp_http_client_set_header(client, "X-Gateway-Id", gateway_config_get()->gateway_id);
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
        snprintf(seq, sizeof(seq), "%lu", (unsigned long)++s_request_seq);
        ret = esp_http_client_set_header(client, "X-Request-Seq", seq);
    }
    if (ret == ESP_OK) {
        ret = esp_http_client_set_header(client, "X-Time-Synced", "false");
    }
    if (ret == ESP_OK && json_body != NULL) {
        ret = esp_http_client_set_header(client, "Content-Type", "application/json");
    }
    if (ret == ESP_OK && json_body != NULL) {
        (void)esp_http_client_set_timeout_ms(client, SERVER_CLIENT_HTTP_WRITE_TIMEOUT_MS);
        ret = esp_http_client_set_post_field(client, json_body, strlen(json_body));
    }
    if (ret == ESP_OK) {
        (void)esp_http_client_set_timeout_ms(client, SERVER_CLIENT_HTTP_READ_TIMEOUT_MS);
        ret = esp_http_client_perform(client);
    }

    int status = esp_http_client_get_status_code(client);
    if (http_status != NULL) {
        *http_status = status;
    }
    if (ret == ESP_OK && (status < 200 || status >= 300)) {
        ESP_LOGW(TAG,
                 "server rejected endpoint=%s status=%d body=%s",
                 endpoint,
                 status,
                 response_body != NULL && response_body[0] != '\0' ? response_body : "-");
        ret = ESP_ERR_INVALID_RESPONSE;
    }
    if (ret == ESP_OK && body_ctx.overflow) {
        ret = ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ret;
}

static esp_err_t perform_json(esp_http_client_method_t method,
                              const char *endpoint,
                              const char *json_body,
                              char *response_body,
                              size_t response_body_size,
                              int *http_status)
{
    if (endpoint == NULL || endpoint[0] == '\0') {
        if (http_status != NULL) {
            *http_status = 0;
        }
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t backoff_ms[] = {2000U, 5000U, 10000U, 30000U};
    esp_err_t ret = ESP_FAIL;
    int status = 0;
    const size_t retry_count = sizeof(backoff_ms) / sizeof(backoff_ms[0]);
    for (size_t attempt = 0; attempt <= retry_count; attempt++) {
        app_task_wdt_reset_current(current_task_wdt_registered());
        ret = perform_json_once(method,
                                endpoint,
                                json_body,
                                response_body,
                                response_body_size,
                                &status);
        if (http_status != NULL) {
            *http_status = status;
        }

        if (ret == ESP_OK && status >= 200 && status < 300) {
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
                app_task_wdt_delay_ms(current_task_wdt_registered(), backoff_ms[attempt]);
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

esp_err_t server_client_post_ingest_json(const char *json_body,
                                         char *response_body,
                                         size_t response_body_size,
                                         int *http_status)
{
    if (json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return perform_json(HTTP_METHOD_POST,
                        ESP111_PROTOCOL_SERVER_ROUTE_DEVICE_INGEST,
                        json_body,
                        response_body,
                        response_body_size,
                        http_status);
}

esp_err_t server_client_post_csi_event_json(const char *json_body,
                                            char *response_body,
                                            size_t response_body_size,
                                            int *http_status)
{
    if (json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return perform_json(HTTP_METHOD_POST,
                        ESP111_PROTOCOL_SERVER_ROUTE_CSI_EVENT,
                        json_body,
                        response_body,
                        response_body_size,
                        http_status);
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

esp_err_t server_client_post_system_log_json(const char *json_body,
                                             char *response_body,
                                             size_t response_body_size,
                                             int *http_status)
{
    if (json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return perform_json(HTTP_METHOD_POST,
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
    return perform_json(HTTP_METHOD_POST,
                        ESP111_PROTOCOL_SERVER_ROUTE_LOGS_ALARMS,
                        json_body,
                        response_body,
                        response_body_size,
                        http_status);
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

    return perform_json(HTTP_METHOD_GET,
                        endpoint,
                        NULL,
                        response_body,
                        response_body_size,
                        http_status);
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

static bool response_complete(esp_http_client_handle_t client,
                              int64_t content_length,
                              size_t total_read)
{
    if (client == NULL) {
        return true;
    }
    if (content_length >= 0) {
        return total_read >= (size_t)content_length;
    }
    return esp_http_client_is_complete_data_received(client);
}

static esp_err_t read_voice_response(esp_http_client_handle_t client,
                                     server_client_data_cb_t on_data,
                                     void *user_ctx,
                                     int64_t request_start_ms,
                                     size_t *out_total_read)
{
    uint8_t buf[1024];
    size_t total_read = 0;
    int empty_reads = 0;
    int64_t content_length = esp_http_client_get_content_length(client);
    bool chunked = esp_http_client_is_chunked_response(client);
    bool complete = response_complete(client, content_length, total_read);
    int64_t first_byte_ms = -1;
    int64_t last_byte_ms = -1;
    esp_err_t ret = ESP_OK;

    if (out_total_read != NULL) {
        *out_total_read = 0;
    }
    if (request_start_ms <= 0) {
        request_start_ms = now_ms();
    }

    while (!complete) {
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
            if (on_data != NULL) {
                ret = on_data(buf, (size_t)read_len, user_ctx);
                if (ret != ESP_OK) {
                    break;
                }
            }
            complete = response_complete(client, content_length, total_read);
            continue;
        }

        complete = response_complete(client, content_length, total_read);
        if (read_len == 0 && (complete || content_length < 0)) {
            complete = true;
            break;
        }
        if (read_len == -ESP_ERR_HTTP_EAGAIN || read_len == 0) {
            empty_reads++;
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
             "voice_response: content_length=%lld chunked=%d total_read=%u complete=%d empty_reads=%d first_byte_ms=%lld last_byte_ms=%lld",
             (long long)content_length,
             chunked ? 1 : 0,
             (unsigned int)total_read,
             complete && ret == ESP_OK ? 1 : 0,
             empty_reads,
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
    if (!server_link_ready()) {
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
        .timeout_ms = ESP111_PROTOCOL_SERVER_VOICE_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const char *stage = "set_headers";
    size_t written = 0;
    size_t response_bytes = 0;
    ESP_LOGI(TAG,
             "request start timestamp=%lld endpoint=%s timeout_ms=%u upload pcm bytes=%u device_id=%s url=%s",
             (long long)request_start_ms,
             ESP111_PROTOCOL_SERVER_ROUTE_VOICE_TURN,
             (unsigned int)SERVER_CLIENT_VOICE_IO_TIMEOUT_MS,
             (unsigned int)pcm_len,
             device_id,
             url);

    ret = esp_http_client_set_header(client,
                                     "Content-Type",
                                     ESP111_PROTOCOL_AUDIO_CONTENT_TYPE_L16_16K_MONO);
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
        ret = read_voice_response(client, on_data, user_ctx, request_start_ms, &response_bytes);
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

    /* Keep the socket open after the PCM body write until headers/body are read or an error ends the turn. */
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ret;
}

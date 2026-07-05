/**
 * @file server_client.c
 * @brief S3 网关访问 ESP-server 的完整协议客户端。
 *
 * 本文件属于 ESPS3 网关，负责把 protocol_adapter/sensor_aggregator/voice_proxy/
 * command_router 交来的完整请求发送到 ESP-server 的 /api/... 路径。它不暴露给 C5、
 * 不解析 C5 轻量 JSON，也不实现本地 ASR/LLM/TTS，只转发 voice PCM 并回传响应数据。
 * 语音独占模式由调用方 gate 普通同步；本层确保每次失败、超时、EAGAIN/CONNECT 失败后
 * 都显式 close+cleanup，避免语音长连接期间 socket 泄漏或复用失败连接。
 */

#include "server_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp111_protocol_common.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "gateway_config.h"
#include "gateway_wifi.h"

static const char *TAG = "server_client";

static uint32_t s_request_seq;

typedef struct {
    char *body;
    size_t body_size;
    size_t body_len;
    bool overflow;
} server_body_ctx_t;

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

static esp_err_t perform_json(esp_http_client_method_t method,
                              const char *endpoint,
                              const char *json_body,
                              char *response_body,
                              size_t response_body_size,
                              int *http_status)
{
    if (!gateway_wifi_is_sta_connected()) {
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
        .timeout_ms = ESP111_PROTOCOL_SERVER_DEFAULT_TIMEOUT_MS,
        .event_handler = body_event_handler,
        .user_data = &body_ctx,
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
        ret = esp_http_client_set_post_field(client, json_body, strlen(json_body));
    }
    if (ret == ESP_OK) {
        ret = esp_http_client_perform(client);
    }

    int status = esp_http_client_get_status_code(client);
    if (http_status != NULL) {
        *http_status = status;
    }
    if (ret == ESP_OK && (status < 200 || status >= 300)) {
        ret = ESP_ERR_INVALID_RESPONSE;
    }
    if (ret == ESP_OK && body_ctx.overflow) {
        ret = ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
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

static bool response_complete(esp_http_client_handle_t client, size_t total_read)
{
    if (client == NULL) {
        return true;
    }
    if (esp_http_client_is_complete_data_received(client)) {
        return true;
    }

    int64_t content_length = esp_http_client_get_content_length(client);
    return content_length >= 0 && total_read >= (size_t)content_length;
}

static esp_err_t read_voice_response(esp_http_client_handle_t client,
                                     server_client_data_cb_t on_data,
                                     void *user_ctx)
{
    uint8_t buf[1024];
    size_t total_read = 0;
    int empty_reads = 0;

    while (!response_complete(client, total_read)) {
        int read_len = esp_http_client_read(client, (char *)buf, sizeof(buf));
        if (read_len > 0) {
            empty_reads = 0;
            total_read += (size_t)read_len;
            if (on_data != NULL) {
                esp_err_t ret = on_data(buf, (size_t)read_len, user_ctx);
                if (ret != ESP_OK) {
                    return ret;
                }
            }
            continue;
        }

        if (read_len == 0 && response_complete(client, total_read)) {
            break;
        }
        if (read_len == -ESP_ERR_HTTP_EAGAIN || read_len == 0) {
            empty_reads++;
            if (empty_reads < 2) {
                continue;
            }
        }

        ESP_LOGW(TAG,
                 "voice response read failed read_len=%d total=%u",
                 read_len,
                 (unsigned int)total_read);
        return read_len == 0 || read_len == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }

    return ESP_OK;
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
    if (!gateway_wifi_is_sta_connected()) {
        if (http_status != NULL) {
            *http_status = 0;
        }
        if (response_content_length != NULL) {
            *response_content_length = -1;
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (response_content_length != NULL) {
        *response_content_length = -1;
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
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

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
        ret = esp_http_client_open(client, (int)pcm_len);
    }
    if (ret == ESP_OK) {
        size_t written = 0;
        while (written < pcm_len) {
            int write_len = esp_http_client_write(client,
                                                  (const char *)pcm + written,
                                                  (int)(pcm_len - written));
            if (write_len <= 0) {
                ret = write_len == 0 ? ESP_ERR_TIMEOUT : ESP_FAIL;
                break;
            }
            written += (size_t)write_len;
        }
    }

    int status = 0;
    int64_t content_length = -1;
    if (ret == ESP_OK) {
        content_length = esp_http_client_fetch_headers(client);
        if (response_content_length != NULL) {
            *response_content_length = content_length;
        }
        if (on_meta != NULL) {
            on_meta(content_length, meta_ctx);
        }
        if (content_length < 0) {
            ret = ESP_ERR_HTTP_FETCH_HEADER;
        }
    }
    if (ret == ESP_OK) {
        status = esp_http_client_get_status_code(client);
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
        ret = read_voice_response(client, on_data, user_ctx);
    }
    if (http_status != NULL && status == 0) {
        *http_status = esp_http_client_get_status_code(client);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ret;
}

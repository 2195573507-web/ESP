#include "server_comm_http.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "server_comm_config.h"
#include "server_comm_errors.h"

static const char *TAG = "server_comm";

#define SERVER_COMM_CHUNK_END "0\r\n\r\n"
#define SERVER_COMM_HTTP_OPEN_CHUNKED (-1)

typedef struct {
    char *buf;
    size_t buf_size;
    size_t len;
    bool overflow;
} server_comm_body_ctx_t;

struct server_comm_raw_stream {
    esp_http_client_handle_t client;
    bool headers_fetched;
    size_t upload_bytes;
};

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
    default:
        return esp_err_to_name(err);
    }
}

bool server_comm_wifi_is_ready(void)
{
    wifi_ap_record_t ap = {0};
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

bool server_comm_http_status_is_success(int status_code)
{
    return status_code >= 200 && status_code < 300;
}

static esp_err_t server_comm_check_ready(const char *label)
{
    if (!server_comm_wifi_is_ready()) {
        ESP_LOGW(TAG, "%s blocked: Wi-Fi is not ready", label != NULL ? label : "http");
        return SERVER_COMM_ERR_WIFI_NOT_READY;
    }

    uint32_t free_heap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
    uint32_t largest_block = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (free_heap < SERVER_COMM_HTTP_MIN_FREE_HEAP ||
        largest_block < SERVER_COMM_HTTP_MIN_LARGEST_BLOCK) {
        ESP_LOGE(TAG,
                 "%s blocked: low heap free=%u largest=%u min_free=%u min_largest=%u",
                 label != NULL ? label : "http",
                 (unsigned int)free_heap,
                 (unsigned int)largest_block,
                 (unsigned int)SERVER_COMM_HTTP_MIN_FREE_HEAP,
                 (unsigned int)SERVER_COMM_HTTP_MIN_LARGEST_BLOCK);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t server_comm_body_event_handler(esp_http_client_event_t *evt)
{
    if (evt == NULL || evt->user_data == NULL) {
        return ESP_OK;
    }

    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }

    server_comm_body_ctx_t *ctx = (server_comm_body_ctx_t *)evt->user_data;
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
                                              server_comm_http_response_t *response)
{
    if (response == NULL || client == NULL) {
        return;
    }

    response->status_code = esp_http_client_get_status_code(client);
    response->content_length = esp_http_client_get_content_length(client);
    response->chunked = esp_http_client_is_chunked_response(client);
    if (body_ctx != NULL) {
        response->body_len = body_ctx->len;
        response->body_overflow = body_ctx->overflow;
    }

    char *content_type = NULL;
    if (esp_http_client_get_header(client, "Content-Type", &content_type) == ESP_OK &&
        content_type != NULL) {
        strlcpy(response->content_type, content_type, sizeof(response->content_type));
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

    esp_err_t ret = server_comm_check_ready("http request");
    if (ret != ESP_OK) {
        return ret;
    }

    char *url = (char *)heap_caps_calloc(1, SERVER_COMM_URL_BUFFER_SIZE, MALLOC_CAP_8BIT);
    if (url == NULL) {
        ESP_LOGE(TAG,
                 "http url buffer alloc failed endpoint=%s bytes=%u",
                 endpoint,
                 (unsigned int)SERVER_COMM_URL_BUFFER_SIZE);
        return ESP_ERR_NO_MEM;
    }

    ret = server_comm_build_url(endpoint, url, SERVER_COMM_URL_BUFFER_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "build URL failed endpoint=%s ret=%s", endpoint, esp_err_to_name(ret));
        heap_caps_free(url);
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

    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = timeout_ms > 0 ? (int)timeout_ms : (int)server_comm_get_default_timeout_ms(),
        .event_handler = server_comm_body_event_handler,
        .user_data = &body_ctx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "http init failed url=%s", url);
        heap_caps_free(url);
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

    server_comm_capture_response_info(client, &body_ctx, response);
    int status = response != NULL ? response->status_code : esp_http_client_get_status_code(client);
    ESP_LOGD(TAG,
             "http response status=%d content_length=%lld body_len=%u overflow=%d",
             status,
             response != NULL ? (long long)response->content_length :
                                (long long)esp_http_client_get_content_length(client),
             (unsigned int)body_ctx.len,
             body_ctx.overflow ? 1 : 0);

    esp_http_client_cleanup(client);
    heap_caps_free(url);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "http request failed endpoint=%s ret=%s", endpoint, esp_err_to_name(ret));
        return ret;
    }
    if (body_ctx.overflow) {
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
        return SERVER_COMM_ERR_BAD_STATUS;
    }

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
    esp_err_t ret = server_comm_check_ready("raw stream");
    if (ret != ESP_OK) {
        return ret;
    }

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

    esp_http_client_config_t http_config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = config->timeout_ms > 0 ? (int)config->timeout_ms :
                                                (int)server_comm_get_default_timeout_ms(),
        .buffer_size = config->buffer_size > 0 ? config->buffer_size :
                                                  (int)SERVER_COMM_HTTP_READ_CHUNK_BYTES,
        .buffer_size_tx = config->tx_buffer_size > 0 ? config->tx_buffer_size : 512,
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
        ESP_LOGE(TAG, "POST stream open failed endpoint=%s ret=%s",
                 config->endpoint,
                 esp_err_to_name(ret));
        server_comm_http_post_raw_stream_close(stream);
        return ret;
    }

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

    return ret;
}

esp_err_t server_comm_http_post_raw_stream_finish_upload(server_comm_raw_stream_t *stream)
{
    if (stream == NULL || stream->client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return server_comm_stream_write_all(stream, SERVER_COMM_CHUNK_END, strlen(SERVER_COMM_CHUNK_END));
}

esp_err_t server_comm_http_fetch_headers(server_comm_raw_stream_t *stream,
                                         server_comm_http_response_t *response)
{
    if (stream == NULL || stream->client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    server_comm_reset_response(response);

    int64_t header_ret = esp_http_client_fetch_headers(stream->client);
    if (header_ret < 0) {
        ESP_LOGE(TAG, "fetch headers failed: %lld", (long long)header_ret);
        return ESP_FAIL;
    }

    stream->headers_fetched = true;
    server_comm_capture_response_info(stream->client, NULL, response);
    return ESP_OK;
}

static bool server_comm_stream_response_complete(server_comm_raw_stream_t *stream,
                                                 size_t total_read)
{
    if (stream == NULL || stream->client == NULL) {
        return true;
    }
    if (esp_http_client_is_complete_data_received(stream->client)) {
        return true;
    }

    int64_t content_length = esp_http_client_get_content_length(stream->client);
    return content_length >= 0 && total_read >= (size_t)content_length;
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

    while (!server_comm_stream_response_complete(stream, total_read)) {
        int read_len = esp_http_client_read(stream->client,
                                            (char *)read_buf,
                                            SERVER_COMM_HTTP_READ_CHUNK_BYTES);
        if (read_len > 0) {
            empty_reads = 0;
            total_read += (size_t)read_len;
            if (on_data != NULL) {
                ret = on_data(read_buf, (size_t)read_len, user_ctx);
                if (ret != ESP_OK) {
                    heap_caps_free(read_buf);
                    return ret;
                }
            }
            continue;
        }

        if (read_len < 0 && read_len != -ESP_ERR_HTTP_EAGAIN) {
            ESP_LOGE(TAG, "response read failed read_len=%d", read_len);
            heap_caps_free(read_buf);
            return ESP_FAIL;
        }

        if (server_comm_stream_response_complete(stream, total_read)) {
            break;
        }

        empty_reads++;
        if (empty_reads >= SERVER_COMM_HTTP_MAX_EMPTY_READS) {
            ESP_LOGE(TAG,
                     "response read timeout total=%u content_length=%lld complete=%d",
                     (unsigned int)total_read,
                     (long long)esp_http_client_get_content_length(stream->client),
                     esp_http_client_is_complete_data_received(stream->client) ? 1 : 0);
            heap_caps_free(read_buf);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(SERVER_COMM_HTTP_EMPTY_READ_DELAY_MS));
    }

    if (response != NULL) {
        response->body_len = total_read;
    }
    heap_caps_free(read_buf);
    return ESP_OK;
}

void server_comm_http_post_raw_stream_close(server_comm_raw_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }
    if (stream->client != NULL) {
        esp_http_client_cleanup(stream->client);
        stream->client = NULL;
    }
    free(stream);
}

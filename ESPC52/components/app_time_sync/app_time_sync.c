#include "app_time_sync.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "server_comm_http.h"

static const char *TAG = "APP_TIME_SYNC";

static bool s_time_synced;
static int64_t s_time_offset_ms;
static int64_t s_last_rtt_ms;
static int64_t s_last_sync_uptime_ms;

int64_t app_time_sync_get_uptime_ms(void)
{
    return esp_timer_get_time() / 1000;
}

bool app_time_sync_is_synced(void)
{
    return s_time_synced;
}

int64_t app_time_sync_get_unix_ms(void)
{
    if (!s_time_synced) {
        return 0;
    }

    return app_time_sync_get_uptime_ms() + s_time_offset_ms;
}

int64_t app_time_sync_get_offset_ms(void)
{
    return s_time_offset_ms;
}

int64_t app_time_sync_get_last_rtt_ms(void)
{
    return s_last_rtt_ms;
}

int64_t app_time_sync_get_last_sync_uptime_ms(void)
{
    return s_last_sync_uptime_ms;
}

static const char *skip_json_spaces(const char *value)
{
    while (value != NULL && (*value == ' ' || *value == '\t' ||
                             *value == '\r' || *value == '\n')) {
        value++;
    }

    return value;
}

static const char *find_json_value(const char *json, const char *key)
{
    const char *key_pos = strstr(json, key);
    if (key_pos == NULL) {
        return NULL;
    }

    const char *value_start = strchr(key_pos + strlen(key), ':');
    if (value_start == NULL) {
        return NULL;
    }

    return skip_json_spaces(value_start + 1);
}

static esp_err_t build_time_now_endpoint(const char *server_time_url, char *endpoint, size_t endpoint_size)
{
    if (server_time_url == NULL || server_time_url[0] == '\0' ||
        endpoint == NULL || endpoint_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *path = NULL;
    const char *scheme_end = strstr(server_time_url, "://");
    if (scheme_end != NULL) {
        const char *host_start = scheme_end + 3;
        path = strchr(host_start, '/');
        if (path == NULL || path[0] == '\0') {
            path = APP_TIME_SYNC_TIME_NOW_PATH;
        }
    } else {
        path = server_time_url;
    }

    size_t local_base_len = strlen(ESP111_PROTOCOL_LOCAL_BASE);
    if (strncmp(path, ESP111_PROTOCOL_LOCAL_BASE, local_base_len) != 0 ||
        (path[local_base_len] != '\0' &&
         path[local_base_len] != '/' &&
         path[local_base_len] != '?' &&
         path[local_base_len] != '#')) {
        return ESP_ERR_NOT_ALLOWED;
    }

    size_t path_len = strlen(path);
    if (path_len + 1 > endpoint_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(endpoint, path, path_len + 1);

    return ESP_OK;
}

/**
 * @brief 只提取 ok 和 server_time_ms 字段，避免新增复杂 JSON 依赖。
 */
static esp_err_t parse_time_response(const char *json, int64_t *server_time_ms)
{
    if (json == NULL || json[0] == '\0' || server_time_ms == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *ok_value = find_json_value(json, APP_TIME_SYNC_JSON_OK_KEY);
    if (ok_value != NULL && strncmp(ok_value, "false", strlen("false")) == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *time_value = find_json_value(json, APP_TIME_SYNC_JSON_SERVER_TIME_MS_KEY);
    if (time_value == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    errno = 0;
    char *value_end = NULL;
    long long parsed_value = strtoll(time_value, &value_end, 10);
    if (errno != 0 || value_end == time_value || parsed_value <= 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *server_time_ms = (int64_t)parsed_value;
    return ESP_OK;
}

esp_err_t app_time_sync_once(const char *server_time_url)
{
    if (server_time_url == NULL || server_time_url[0] == '\0') {
        ESP_LOGE(TAG, "invalid server time url");
        return ESP_ERR_INVALID_ARG;
    }

    char *endpoint = (char *)heap_caps_calloc(1, APP_TIME_SYNC_URL_BUFFER_SIZE, MALLOC_CAP_8BIT);
    if (endpoint == NULL) {
        ESP_LOGE(TAG,
                 "time sync endpoint buffer alloc failed bytes=%u",
                 (unsigned int)APP_TIME_SYNC_URL_BUFFER_SIZE);
        return ESP_ERR_NO_MEM;
    }

    char *body = (char *)heap_caps_calloc(1, APP_TIME_SYNC_RESPONSE_BUFFER_SIZE, MALLOC_CAP_8BIT);
    if (body == NULL) {
        ESP_LOGE(TAG,
                 "time sync response buffer alloc failed bytes=%u",
                 (unsigned int)APP_TIME_SYNC_RESPONSE_BUFFER_SIZE);
        heap_caps_free(endpoint);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = build_time_now_endpoint(server_time_url, endpoint, APP_TIME_SYNC_URL_BUFFER_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "invalid local time endpoint: %s", server_time_url);
        goto cleanup;
    }

    ESP_LOGI(TAG, "GET time endpoint=%s", endpoint);

    int64_t t0_ms = app_time_sync_get_uptime_ms();
    server_comm_http_response_t response = {0};
    ret = server_comm_http_get_json(endpoint,
                                    APP_TIME_SYNC_HTTP_TIMEOUT_MS,
                                    body,
                                    APP_TIME_SYNC_RESPONSE_BUFFER_SIZE,
                                    &response);
    int64_t t1_ms = app_time_sync_get_uptime_ms();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "time sync http failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ESP_LOGI(TAG,
             "time sync http status=%d content_length=%lld body_len=%u overflow=%d",
             response.status_code,
             (long long)response.content_length,
             (unsigned)response.body_len,
             response.body_overflow);
    ESP_LOGI(TAG, "time sync body: %.*s", APP_TIME_SYNC_BODY_LOG_PREVIEW_LEN, body);

    if (response.body_len == 0) {
        ESP_LOGE(TAG, "time sync empty body");
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    int64_t server_time_ms = 0;
    ret = parse_time_response(body, &server_time_ms);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "time sync parse failed");
        goto cleanup;
    }

    int64_t rtt_ms = t1_ms - t0_ms;
    int64_t uptime_ms = app_time_sync_get_uptime_ms();

    s_time_offset_ms = server_time_ms - uptime_ms;
    s_last_rtt_ms = rtt_ms;
    s_last_sync_uptime_ms = uptime_ms;
    s_time_synced = true;

    ESP_LOGI(TAG,
             "time sync ok: server_time_ms=%lld uptime_ms=%lld offset_ms=%lld",
             (long long)server_time_ms,
             (long long)uptime_ms,
             (long long)s_time_offset_ms);

cleanup:
    heap_caps_free(body);
    heap_caps_free(endpoint);
    return ret;
}

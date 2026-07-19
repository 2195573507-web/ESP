/**
 * @file wake_prompt_cache_gateway.c
 * @brief S3 wake prompt 缓存与本地流式下发实现。
 *
 * 本文件属于 ESPS3 网关。它从 ESP-server 读取 wake prompt 配置和 PCM，将结果缓存到
 * storage SPIFFS，并给 C5 暴露 /local/v1/audio/wake-prompt。模块失败时只返回本地
 * HTTP 错误，让 C5 播放 short beep；不得影响 child_registry、offline_policy 或
 * voice_proxy 主链路。
 */

#include "wake_prompt_cache_gateway.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp111_protocol_common.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gateway_config.h"
#include "gateway_wifi.h"
#include "s3_scheduler.h"

static const char *TAG = "wake_prompt_s3";

#define WAKE_PROMPT_PARTITION_LABEL "storage"
#define WAKE_PROMPT_BASE_PATH "/spiffs"
#define WAKE_PROMPT_PCM_PATH "/spiffs/s3_wake_prompt.pcm"
#define WAKE_PROMPT_TMP_PATH "/spiffs/s3_wake_prompt.tmp"
#define WAKE_PROMPT_META_PATH "/spiffs/s3_wake_prompt.json"
#define WAKE_PROMPT_CONFIG_ENDPOINT "/api/voice/prompt/config"
#define WAKE_PROMPT_PCM_ENDPOINT "/api/voice/prompt?prompt_key=wake_ack_zh"
#define WAKE_PROMPT_CONTENT_TYPE ESP111_PROTOCOL_AUDIO_CONTENT_TYPE_L16_16K_MONO
#define WAKE_PROMPT_AUDIO_FORMAT ESP111_PROTOCOL_AUDIO_FORMAT_PCM_S16LE_MONO_16K
#define WAKE_PROMPT_SAMPLE_RATE 16000
#define WAKE_PROMPT_CHANNELS 1
#define WAKE_PROMPT_FORMAT "s16le"
#define WAKE_PROMPT_MAX_PCM_BYTES (96U * 1024U)
#define WAKE_PROMPT_MIN_PCM_BYTES 64U
#define WAKE_PROMPT_HTTP_TIMEOUT_MS 8000
#define WAKE_PROMPT_DOWNLOAD_TOTAL_TIMEOUT_MS 20000
#define WAKE_PROMPT_READ_BYTES 1024U
#define WAKE_PROMPT_HEADER_BYTES 128U
#define WAKE_PROMPT_URL_BYTES 320U
#define WAKE_PROMPT_CONFIG_BODY_BYTES 2048U

typedef struct {
    char wake_prompt_text[192];
    char provider[32];
    char voice_id[96];
    char speaker_id[96];
    double speed;
    double pitch;
    double volume;
    int sample_rate;
    char format[16];
    int channels;
    char prompt_version[96];
    char voice_config_hash[80];
    int64_t updated_at_ms;
    size_t pcm_bytes;
    int64_t fetched_at_ms;
} wake_prompt_metadata_t;

typedef struct {
    char content_type[WAKE_PROMPT_HEADER_BYTES];
    char audio_format[WAKE_PROMPT_HEADER_BYTES];
    char sample_rate[WAKE_PROMPT_HEADER_BYTES];
    char channels[WAKE_PROMPT_HEADER_BYTES];
    char prompt_version[WAKE_PROMPT_HEADER_BYTES];
    char voice_config_hash[WAKE_PROMPT_HEADER_BYTES];
} wake_prompt_http_ctx_t;

typedef struct {
    char *body;
    size_t body_size;
    size_t body_len;
    bool overflow;
} wake_prompt_body_ctx_t;

static SemaphoreHandle_t s_cache_lock;
static bool s_spiffs_mounted;

static bool wake_prompt_streq_ci(const char *lhs, const char *rhs)
{
    if (lhs == NULL || rhs == NULL) {
        return false;
    }
    while (*lhs != '\0' && *rhs != '\0') {
        char a = (*lhs >= 'A' && *lhs <= 'Z') ? (char)(*lhs - 'A' + 'a') : *lhs;
        char b = (*rhs >= 'A' && *rhs <= 'Z') ? (char)(*rhs - 'A' + 'a') : *rhs;
        if (a != b) {
            return false;
        }
        lhs++;
        rhs++;
    }
    return *lhs == '\0' && *rhs == '\0';
}

static esp_err_t wake_prompt_build_url(const char *endpoint, char *out, size_t out_size)
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

static esp_err_t wake_prompt_body_event(esp_http_client_event_t *evt)
{
    if (evt == NULL || evt->event_id != HTTP_EVENT_ON_DATA || evt->data == NULL ||
        evt->data_len <= 0 || evt->user_data == NULL) {
        return ESP_OK;
    }

    wake_prompt_body_ctx_t *ctx = (wake_prompt_body_ctx_t *)evt->user_data;
    if (ctx->body == NULL || ctx->body_size == 0) {
        return ESP_OK;
    }

    size_t usable = ctx->body_size - 1U;
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

static esp_err_t wake_prompt_ensure_lock(void)
{
    if (s_cache_lock != NULL) {
        return ESP_OK;
    }
    s_cache_lock = xSemaphoreCreateMutex();
    return s_cache_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t wake_prompt_mount_locked(void)
{
    if (s_spiffs_mounted) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = WAKE_PROMPT_BASE_PATH,
        .partition_label = WAKE_PROMPT_PARTITION_LABEL,
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mount storage spiffs failed ret=%s", esp_err_to_name(ret));
        return ret;
    }

    s_spiffs_mounted = true;
    return ESP_OK;
}

static esp_err_t wake_prompt_with_mount(void)
{
    esp_err_t ret = wake_prompt_ensure_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    ret = wake_prompt_mount_locked();
    xSemaphoreGive(s_cache_lock);
    return ret;
}

static const char *json_string(cJSON *root, const char *key, const char *fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring != NULL ? item->valuestring : fallback;
}

static double json_number(cJSON *root, const char *key, double fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static int json_integer(cJSON *root, const char *key, int fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static esp_err_t wake_prompt_parse_config(const char *body, wake_prompt_metadata_t *out)
{
    if (body == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *config = cJSON_GetObjectItemCaseSensitive(root, "config");
    if (!cJSON_IsObject(config)) {
        config = root;
    }

    memset(out, 0, sizeof(*out));
    strlcpy(out->wake_prompt_text, json_string(config, "wake_prompt_text", ""), sizeof(out->wake_prompt_text));
    strlcpy(out->provider, json_string(config, "provider", ""), sizeof(out->provider));
    strlcpy(out->voice_id, json_string(config, "voice_id", ""), sizeof(out->voice_id));
    strlcpy(out->speaker_id, json_string(config, "speaker_id", ""), sizeof(out->speaker_id));
    out->speed = json_number(config, "speed", 1.0);
    out->pitch = json_number(config, "pitch", 1.0);
    out->volume = json_number(config, "volume", 1.0);
    out->sample_rate = json_integer(config, "sample_rate", 0);
    out->channels = json_integer(config, "channels", 0);
    strlcpy(out->format, json_string(config, "format", ""), sizeof(out->format));
    strlcpy(out->prompt_version, json_string(config, "prompt_version", ""), sizeof(out->prompt_version));
    strlcpy(out->voice_config_hash, json_string(config, "voice_config_hash", ""), sizeof(out->voice_config_hash));
    out->updated_at_ms = (int64_t)json_number(config, "updated_at_ms", 0);

    cJSON_Delete(root);

    if (out->wake_prompt_text[0] == '\0' ||
        out->provider[0] == '\0' ||
        out->voice_config_hash[0] == '\0' ||
        out->prompt_version[0] == '\0' ||
        out->sample_rate != WAKE_PROMPT_SAMPLE_RATE ||
        out->channels != WAKE_PROMPT_CHANNELS ||
        strcmp(out->format, WAKE_PROMPT_FORMAT) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static esp_err_t wake_prompt_read_config(wake_prompt_metadata_t *out_config)
{
    if (out_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!gateway_wifi_is_net_ready() || !gateway_wifi_is_sta_connected() ||
        !s3_scheduler_is_server_upload_allowed()) {
        return ESP_ERR_INVALID_STATE;
    }

    char url[WAKE_PROMPT_URL_BYTES];
    esp_err_t ret = wake_prompt_build_url(WAKE_PROMPT_CONFIG_ENDPOINT, url, sizeof(url));
    if (ret != ESP_OK) {
        return ret;
    }

    char *body = heap_caps_calloc(1, WAKE_PROMPT_CONFIG_BODY_BYTES, MALLOC_CAP_8BIT);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    wake_prompt_body_ctx_t body_ctx = {
        .body = body,
        .body_size = WAKE_PROMPT_CONFIG_BODY_BYTES,
    };
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = WAKE_PROMPT_HTTP_TIMEOUT_MS,
        .buffer_size = 512,
        .event_handler = wake_prompt_body_event,
        .user_data = &body_ctx,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        heap_caps_free(body);
        return ESP_ERR_NO_MEM;
    }

    ret = esp_http_client_set_header(client, "X-Gateway-Id", gateway_config_get()->gateway_id);
    if (ret == ESP_OK) {
        ret = esp_http_client_perform(client);
    }
    int status = esp_http_client_get_status_code(client);
    if (ret == ESP_OK && (status < 200 || status >= 300)) {
        ret = ESP_ERR_INVALID_RESPONSE;
    }
    if (ret == ESP_OK && (body_ctx.body_len == 0 || body_ctx.overflow)) {
        ret = ESP_ERR_INVALID_RESPONSE;
    }
    if (ret == ESP_OK) {
        ret = wake_prompt_parse_config(body, out_config);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    heap_caps_free(body);
    return ret;
}

static esp_err_t wake_prompt_read_local_meta(wake_prompt_metadata_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(WAKE_PROMPT_META_PATH, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    char *body = heap_caps_calloc(1, WAKE_PROMPT_CONFIG_BODY_BYTES, MALLOC_CAP_8BIT);
    if (body == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    size_t len = fread(body, 1, WAKE_PROMPT_CONFIG_BODY_BYTES - 1U, file);
    bool read_error = ferror(file);
    fclose(file);
    if (read_error || len == 0) {
        heap_caps_free(body);
        return ESP_ERR_INVALID_RESPONSE;
    }
    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    heap_caps_free(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(out, 0, sizeof(*out));
    strlcpy(out->wake_prompt_text, json_string(root, "wake_prompt_text", ""), sizeof(out->wake_prompt_text));
    strlcpy(out->provider, json_string(root, "provider", ""), sizeof(out->provider));
    strlcpy(out->voice_id, json_string(root, "voice_id", ""), sizeof(out->voice_id));
    strlcpy(out->speaker_id, json_string(root, "speaker_id", ""), sizeof(out->speaker_id));
    out->speed = json_number(root, "speed", 1.0);
    out->pitch = json_number(root, "pitch", 1.0);
    out->volume = json_number(root, "volume", 1.0);
    out->sample_rate = json_integer(root, "sample_rate", 0);
    strlcpy(out->format, json_string(root, "format", ""), sizeof(out->format));
    out->channels = json_integer(root, "channels", 0);
    out->pcm_bytes = (size_t)json_number(root, "pcm_bytes", 0);
    strlcpy(out->voice_config_hash, json_string(root, "voice_config_hash", ""), sizeof(out->voice_config_hash));
    strlcpy(out->prompt_version, json_string(root, "prompt_version", ""), sizeof(out->prompt_version));
    out->fetched_at_ms = (int64_t)json_number(root, "fetched_at_ms", 0);
    out->updated_at_ms = (int64_t)json_number(root, "updated_at_ms", 0);
    cJSON_Delete(root);

    return ESP_OK;
}

static bool wake_prompt_meta_matches(const wake_prompt_metadata_t *local,
                                     const wake_prompt_metadata_t *remote,
                                     size_t pcm_bytes)
{
    if (local == NULL || remote == NULL) {
        return false;
    }
    return local->sample_rate == WAKE_PROMPT_SAMPLE_RATE &&
           local->channels == WAKE_PROMPT_CHANNELS &&
           strcmp(local->format, WAKE_PROMPT_FORMAT) == 0 &&
           local->pcm_bytes == pcm_bytes &&
           strcmp(local->voice_config_hash, remote->voice_config_hash) == 0 &&
           strcmp(local->prompt_version, remote->prompt_version) == 0 &&
           strcmp(local->wake_prompt_text, remote->wake_prompt_text) == 0;
}

static esp_err_t wake_prompt_write_meta(const wake_prompt_metadata_t *remote, size_t pcm_bytes)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "wake_prompt_text", remote->wake_prompt_text);
    cJSON_AddStringToObject(root, "provider", remote->provider);
    cJSON_AddStringToObject(root, "voice_id", remote->voice_id);
    cJSON_AddStringToObject(root, "speaker_id", remote->speaker_id);
    cJSON_AddNumberToObject(root, "speed", remote->speed);
    cJSON_AddNumberToObject(root, "pitch", remote->pitch);
    cJSON_AddNumberToObject(root, "volume", remote->volume);
    cJSON_AddNumberToObject(root, "sample_rate", remote->sample_rate);
    cJSON_AddStringToObject(root, "format", remote->format);
    cJSON_AddNumberToObject(root, "channels", remote->channels);
    cJSON_AddNumberToObject(root, "pcm_bytes", (double)pcm_bytes);
    cJSON_AddStringToObject(root, "voice_config_hash", remote->voice_config_hash);
    cJSON_AddStringToObject(root, "prompt_version", remote->prompt_version);
    cJSON_AddNumberToObject(root, "updated_at_ms", (double)remote->updated_at_ms);
    cJSON_AddNumberToObject(root, "fetched_at_ms", (double)(xTaskGetTickCount() * portTICK_PERIOD_MS));

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    FILE *file = fopen(WAKE_PROMPT_META_PATH, "wb");
    if (file == NULL) {
        cJSON_free(json);
        return ESP_FAIL;
    }
    size_t len = strlen(json);
    bool ok = fwrite(json, 1, len, file) == len &&
              fwrite("\n", 1, 1, file) == 1 &&
              fflush(file) == 0;
    fclose(file);
    cJSON_free(json);
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t wake_prompt_http_event(esp_http_client_event_t *evt)
{
    if (evt == NULL || evt->event_id != HTTP_EVENT_ON_HEADER || evt->user_data == NULL) {
        return ESP_OK;
    }
    wake_prompt_http_ctx_t *ctx = (wake_prompt_http_ctx_t *)evt->user_data;
    const char *key = evt->header_key != NULL ? evt->header_key : "";
    const char *value = evt->header_value != NULL ? evt->header_value : "";

    if (wake_prompt_streq_ci(key, "Content-Type")) {
        strlcpy(ctx->content_type, value, sizeof(ctx->content_type));
    } else if (wake_prompt_streq_ci(key, "X-Audio-Format")) {
        strlcpy(ctx->audio_format, value, sizeof(ctx->audio_format));
    } else if (wake_prompt_streq_ci(key, "X-Audio-Sample-Rate") ||
               wake_prompt_streq_ci(key, "X-Sample-Rate")) {
        strlcpy(ctx->sample_rate, value, sizeof(ctx->sample_rate));
    } else if (wake_prompt_streq_ci(key, "X-Audio-Channels") ||
               wake_prompt_streq_ci(key, "X-Channels")) {
        strlcpy(ctx->channels, value, sizeof(ctx->channels));
    } else if (wake_prompt_streq_ci(key, "X-Audio-Version") ||
               wake_prompt_streq_ci(key, "X-Prompt-Version")) {
        strlcpy(ctx->prompt_version, value, sizeof(ctx->prompt_version));
    } else if (wake_prompt_streq_ci(key, "X-Voice-Config-Hash")) {
        strlcpy(ctx->voice_config_hash, value, sizeof(ctx->voice_config_hash));
    }
    return ESP_OK;
}

static esp_err_t wake_prompt_download_pcm(const wake_prompt_metadata_t *remote)
{
    if (!gateway_wifi_is_net_ready() || !gateway_wifi_is_sta_connected() ||
        !s3_scheduler_is_server_upload_allowed()) {
        return ESP_ERR_INVALID_STATE;
    }

    char url[WAKE_PROMPT_URL_BYTES];
    esp_err_t ret = wake_prompt_build_url(WAKE_PROMPT_PCM_ENDPOINT, url, sizeof(url));
    if (ret != ESP_OK) {
        return ret;
    }

    wake_prompt_http_ctx_t http_ctx = {0};
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = WAKE_PROMPT_HTTP_TIMEOUT_MS,
        .buffer_size = WAKE_PROMPT_READ_BYTES,
        .event_handler = wake_prompt_http_event,
        .user_data = &http_ctx,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    FILE *tmp = NULL;
    size_t total = 0;
    bool opened = false;
    ret = esp_http_client_set_header(client, "X-Gateway-Id", gateway_config_get()->gateway_id);
    if (ret == ESP_OK) {
        ret = esp_http_client_open(client, 0);
    }
    if (ret == ESP_OK) {
        opened = true;
        int64_t content_length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (content_length <= 0 || content_length > (int64_t)WAKE_PROMPT_MAX_PCM_BYTES ||
            status < 200 || status >= 300) {
            ret = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (ret == ESP_OK) {
        if (strcmp(http_ctx.audio_format, WAKE_PROMPT_AUDIO_FORMAT) != 0 ||
            strcmp(http_ctx.sample_rate, "16000") != 0 ||
            strcmp(http_ctx.channels, "1") != 0 ||
            strcmp(http_ctx.voice_config_hash, remote->voice_config_hash) != 0 ||
            strcmp(http_ctx.prompt_version, remote->prompt_version) != 0) {
            ret = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (ret == ESP_OK) {
        unlink(WAKE_PROMPT_TMP_PATH);
        tmp = fopen(WAKE_PROMPT_TMP_PATH, "wb");
        if (tmp == NULL) {
            ret = ESP_FAIL;
        }
    }
    if (ret == ESP_OK) {
        uint8_t *buf = heap_caps_malloc(WAKE_PROMPT_READ_BYTES, MALLOC_CAP_8BIT);
        if (buf == NULL) {
            ret = ESP_ERR_NO_MEM;
        } else {
            const int64_t read_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            while (!esp_http_client_is_complete_data_received(client)) {
                const int64_t elapsed_ms =
                    (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - read_start_ms;
                if (elapsed_ms > WAKE_PROMPT_DOWNLOAD_TOTAL_TIMEOUT_MS) {
                    ret = ESP_ERR_TIMEOUT;
                    break;
                }
                int read_len = esp_http_client_read(client, (char *)buf, WAKE_PROMPT_READ_BYTES);
                if (read_len > 0) {
                    total += (size_t)read_len;
                    if (total > WAKE_PROMPT_MAX_PCM_BYTES ||
                        fwrite(buf, 1, (size_t)read_len, tmp) != (size_t)read_len) {
                        ret = ESP_ERR_INVALID_SIZE;
                        break;
                    }
                    continue;
                }
                ret = read_len == 0 || read_len == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
                break;
            }
            heap_caps_free(buf);
        }
    }
    if (tmp != NULL) {
        if (fflush(tmp) != 0 && ret == ESP_OK) {
            ret = ESP_FAIL;
        }
        fclose(tmp);
    }
    if (opened) {
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);

    if (ret != ESP_OK ||
        total < WAKE_PROMPT_MIN_PCM_BYTES ||
        (total % sizeof(int16_t)) != 0) {
        unlink(WAKE_PROMPT_TMP_PATH);
        return ret == ESP_OK ? ESP_ERR_INVALID_SIZE : ret;
    }

    unlink(WAKE_PROMPT_PCM_PATH);
    unlink(WAKE_PROMPT_META_PATH);
    if (rename(WAKE_PROMPT_TMP_PATH, WAKE_PROMPT_PCM_PATH) != 0) {
        unlink(WAKE_PROMPT_TMP_PATH);
        return ESP_FAIL;
    }

    ret = wake_prompt_write_meta(remote, total);
    if (ret != ESP_OK) {
        unlink(WAKE_PROMPT_PCM_PATH);
    }
    ESP_LOGI(TAG,
             "wake prompt cached bytes=%u hash=%s version=%s",
             (unsigned int)total,
             remote->voice_config_hash,
             remote->prompt_version);
    return ret;
}

static esp_err_t wake_prompt_ensure_cached_locked(wake_prompt_metadata_t *out_meta)
{
    wake_prompt_metadata_t remote = {0};
    esp_err_t ret = wake_prompt_read_config(&remote);
    if (ret != ESP_OK) {
        return ret;
    }

    struct stat pcm_stat = {0};
    wake_prompt_metadata_t local = {0};
    if (stat(WAKE_PROMPT_PCM_PATH, &pcm_stat) == 0 &&
        pcm_stat.st_size > 0 &&
        (size_t)pcm_stat.st_size <= WAKE_PROMPT_MAX_PCM_BYTES &&
        wake_prompt_read_local_meta(&local) == ESP_OK &&
        wake_prompt_meta_matches(&local, &remote, (size_t)pcm_stat.st_size)) {
        if (out_meta != NULL) {
            *out_meta = local;
        }
        return ESP_OK;
    }

    ret = wake_prompt_download_pcm(&remote);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = wake_prompt_read_local_meta(&local);
    if (ret == ESP_OK && out_meta != NULL) {
        *out_meta = local;
    }
    return ret;
}

esp_err_t wake_prompt_cache_gateway_init(void)
{
    esp_err_t ret = wake_prompt_ensure_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    ESP_LOGI(TAG, "wake prompt cache gateway initialized route=%s", ESP111_PROTOCOL_ROUTE_WAKE_PROMPT_AUDIO);
    return ESP_OK;
}

esp_err_t wake_prompt_cache_gateway_handle_http(httpd_req_t *req)
{
    if (req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = wake_prompt_with_mount();
    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"wake_prompt_storage_unavailable\"}");
    }

    wake_prompt_metadata_t meta = {0};
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    ret = wake_prompt_ensure_cached_locked(&meta);
    xSemaphoreGive(s_cache_lock);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wake prompt cache unavailable ret=%s", esp_err_to_name(ret));
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"wake_prompt_unavailable\"}");
    }

    struct stat file_stat = {0};
    if (stat(WAKE_PROMPT_PCM_PATH, &file_stat) != 0 || file_stat.st_size <= 0 ||
        (size_t)file_stat.st_size > WAKE_PROMPT_MAX_PCM_BYTES) {
        ESP_LOGW(TAG, "wake prompt cache invalid before stream");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"wake_prompt_cache_missing\"}");
    }

    FILE *file = fopen(WAKE_PROMPT_PCM_PATH, "rb");
    if (file == NULL) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"wake_prompt_cache_missing\"}");
    }

    uint8_t *buf = heap_caps_malloc(WAKE_PROMPT_READ_BYTES, MALLOC_CAP_8BIT);
    if (buf == NULL) {
        fclose(file);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"wake_prompt_no_memory\"}");
    }

    size_t first_chunk_size = fread(buf, 1, WAKE_PROMPT_READ_BYTES, file);
    if (first_chunk_size == 0 || ferror(file)) {
        ESP_LOGW(TAG,
                 "wake prompt cache unreadable before stream first_chunk_size=%u read_error=%d",
                 (unsigned int)first_chunk_size,
                 ferror(file) ? 1 : 0);
        heap_caps_free(buf);
        fclose(file);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"wake_prompt_cache_missing\"}");
    }

    httpd_resp_set_type(req, WAKE_PROMPT_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "X-Audio-Sample-Rate", "16000");
    httpd_resp_set_hdr(req, "X-Audio-Format", WAKE_PROMPT_AUDIO_FORMAT);
    httpd_resp_set_hdr(req, "X-Audio-Channels", "1");
    httpd_resp_set_hdr(req, "X-Audio-Version", meta.prompt_version);
    httpd_resp_set_hdr(req, "X-Voice-Config-Hash", meta.voice_config_hash);

    size_t sent = 0;
    size_t chunks = 0;
    ESP_LOGI(TAG,
             "WAKE_PROMPT_STREAM_START file_size=%u first_chunk_size=%u content_type=%s",
             (unsigned int)file_stat.st_size,
             (unsigned int)first_chunk_size,
             WAKE_PROMPT_CONTENT_TYPE);

    ret = httpd_resp_send_chunk(req, (const char *)buf, first_chunk_size);
    if (ret == ESP_OK) {
        sent += first_chunk_size;
        chunks++;
    }
    while (ret == ESP_OK) {
        size_t got = fread(buf, 1, WAKE_PROMPT_READ_BYTES, file);
        if (got > 0) {
            ret = httpd_resp_send_chunk(req, (const char *)buf, got);
            if (ret != ESP_OK) {
                break;
            }
            sent += got;
            chunks++;
        }
        if (got < WAKE_PROMPT_READ_BYTES) {
            if (ferror(file)) {
                ret = ESP_FAIL;
            }
            break;
        }
    }
    heap_caps_free(buf);
    fclose(file);

    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, NULL, 0);
    }
    ESP_LOGI(TAG,
             "WAKE_PROMPT_STREAM_END sent_bytes=%u chunks=%u",
             (unsigned int)sent,
             (unsigned int)chunks);
    return ret;
}

#include "wake_prompt_cache.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app_debug_config.h"
#include "app_runtime.h"
#include "app_stack_monitor.h"
#include "device_protocol_metadata.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "server_comm_config.h"
#include "server_comm_http.h"
#include "speaker_player.h"

static const char *TAG = "wake_prompt_cache";

#define WAKE_PROMPT_CACHE_PARTITION_LABEL "storage"
#define WAKE_PROMPT_CACHE_BASE_PATH "/spiffs"
#define WAKE_PROMPT_CACHE_PCM_PATH "/spiffs/wake_prompt.pcm"
#define WAKE_PROMPT_CACHE_TMP_PATH "/spiffs/wake_prompt.tmp"
#define WAKE_PROMPT_CACHE_META_PATH "/spiffs/wake_prompt.meta"
#define WAKE_PROMPT_CACHE_TEXT "我在，你说"
#define WAKE_PROMPT_CACHE_FORMAT "pcm_s16le_mono_16k"
#define WAKE_PROMPT_CACHE_CONTENT_TYPE_TOKEN "audio/L16"
#define WAKE_PROMPT_CACHE_VERSION "text=wo_zai_ni_shuo;voice=server_prompt_v1;format=pcm_s16le_mono_16k"
#define WAKE_PROMPT_CACHE_SAMPLE_RATE_HZ 16000U
#define WAKE_PROMPT_CACHE_MAX_BYTES (96U * 1024U)
#define WAKE_PROMPT_CACHE_READ_BYTES AUDIO_PLAYER_PCM_CHUNK_BYTES
#define WAKE_PROMPT_CACHE_MAX_EMPTY_READS 20
#define WAKE_PROMPT_CACHE_EMPTY_READ_DELAY_MS 20U
#define WAKE_PROMPT_CACHE_HEADER_VALUE_BYTES 128U
#define WAKE_PROMPT_CACHE_ENDPOINT_BYTES 512U
#define WAKE_PROMPT_CACHE_QUERY_VALUE_BYTES 384U
#define WAKE_PROMPT_CACHE_META_BUFFER_BYTES 512U

typedef struct {
    char content_type[WAKE_PROMPT_CACHE_HEADER_VALUE_BYTES];
    char audio_format[WAKE_PROMPT_CACHE_HEADER_VALUE_BYTES];
    unsigned int header_count;
} wake_prompt_cache_http_ctx_t;

typedef struct {
    uint32_t peak;
    uint64_t square_sum;
    size_t samples;
} wake_prompt_cache_pcm_stats_t;

typedef struct {
    char endpoint[WAKE_PROMPT_CACHE_ENDPOINT_BYTES];
    char url[SERVER_COMM_URL_BUFFER_SIZE];
    char encoded_device_id[WAKE_PROMPT_CACHE_QUERY_VALUE_BYTES];
    uint8_t read_buf[WAKE_PROMPT_CACHE_READ_BYTES];
    wake_prompt_cache_http_ctx_t http_ctx;
    wake_prompt_cache_pcm_stats_t pcm_stats;
    esp_http_client_config_t http_config;
} wake_prompt_cache_download_buffers_t;

static SemaphoreHandle_t s_cache_lock;
static TaskHandle_t s_download_task;
static bool s_spiffs_mounted;

static void wake_prompt_cache_log_stack_high_water(const char *stage)
{
    app_stack_monitor_log(TAG, "wake_prompt_cache", stage);
}

static esp_err_t wake_prompt_cache_ensure_lock(void)
{
    if (s_cache_lock != NULL) {
        return ESP_OK;
    }

    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (s_cache_lock == NULL) {
        s_cache_lock = lock;
    } else {
        vSemaphoreDelete(lock);
    }
    return ESP_OK;
}

static esp_err_t wake_prompt_cache_mount_locked(void)
{
    if (s_spiffs_mounted) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = WAKE_PROMPT_CACHE_BASE_PATH,
        .partition_label = WAKE_PROMPT_CACHE_PARTITION_LABEL,
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "wake prompt cache fallback builtin reason=spiffs_mount_failed ret=%s",
                 esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0;
    size_t used = 0;
    ret = esp_spiffs_info(WAKE_PROMPT_CACHE_PARTITION_LABEL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wake prompt cache spiffs info failed ret=%s", esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG,
                 "wake prompt cache spiffs mounted partition=%s total=%u used=%u",
                 WAKE_PROMPT_CACHE_PARTITION_LABEL,
                 (unsigned int)total,
                 (unsigned int)used);
    }
    s_spiffs_mounted = true;
    return ESP_OK;
}

static esp_err_t wake_prompt_cache_with_mount(void)
{
    esp_err_t ret = wake_prompt_cache_ensure_lock();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    ret = wake_prompt_cache_mount_locked();
    xSemaphoreGive(s_cache_lock);
    return ret;
}

static char wake_prompt_cache_ascii_lower(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

static bool wake_prompt_cache_streq_ci(const char *lhs, const char *rhs)
{
    if (lhs == NULL || rhs == NULL) {
        return false;
    }

    while (*lhs != '\0' && *rhs != '\0') {
        if (wake_prompt_cache_ascii_lower(*lhs) != wake_prompt_cache_ascii_lower(*rhs)) {
            return false;
        }
        lhs++;
        rhs++;
    }
    return *lhs == '\0' && *rhs == '\0';
}

static bool wake_prompt_cache_contains_ci(const char *haystack, const char *needle)
{
    if (haystack == NULL || needle == NULL) {
        return false;
    }
    if (needle[0] == '\0') {
        return true;
    }

    for (const char *cursor = haystack; *cursor != '\0'; cursor++) {
        const char *h = cursor;
        const char *n = needle;
        while (*h != '\0' && *n != '\0' &&
               wake_prompt_cache_ascii_lower(*h) == wake_prompt_cache_ascii_lower(*n)) {
            h++;
            n++;
        }
        if (*n == '\0') {
            return true;
        }
    }
    return false;
}

static bool wake_prompt_cache_content_type_ok(const char *content_type)
{
    return content_type != NULL &&
           wake_prompt_cache_contains_ci(content_type, WAKE_PROMPT_CACHE_CONTENT_TYPE_TOKEN) &&
           wake_prompt_cache_contains_ci(content_type, "rate=16000") &&
           wake_prompt_cache_contains_ci(content_type, "channels=1");
}

static bool wake_prompt_cache_url_is_unreserved(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

static bool wake_prompt_cache_append_char(char *out, size_t out_size, size_t *out_len, char ch)
{
    if (out == NULL || out_size == 0 || out_len == NULL) {
        return false;
    }

    if (*out_len + 1U >= out_size) {
        out[out_size - 1U] = '\0';
        return false;
    }

    out[*out_len] = ch;
    *out_len += 1U;
    out[*out_len] = '\0';
    return true;
}

static bool wake_prompt_cache_url_encode_component(const char *input, char *out, size_t out_size)
{
    static const char hex[] = "0123456789ABCDEF";

    if (out == NULL || out_size == 0) {
        return false;
    }

    out[0] = '\0';
    if (input == NULL) {
        return true;
    }

    size_t out_len = 0;
    for (const unsigned char *cursor = (const unsigned char *)input; *cursor != '\0'; cursor++) {
        if (wake_prompt_cache_url_is_unreserved(*cursor)) {
            if (!wake_prompt_cache_append_char(out, out_size, &out_len, (char)*cursor)) {
                return false;
            }
            continue;
        }

        if (out_len + 3U >= out_size) {
            out[out_size - 1U] = '\0';
            return false;
        }
        out[out_len++] = '%';
        out[out_len++] = hex[*cursor >> 4];
        out[out_len++] = hex[*cursor & 0x0FU];
        out[out_len] = '\0';
    }

    return true;
}

static uint32_t wake_prompt_cache_isqrt_u64(uint64_t value)
{
    uint64_t bit = (uint64_t)1 << 62;
    uint64_t result = 0;

    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)result;
}

static void wake_prompt_cache_update_pcm_stats(wake_prompt_cache_pcm_stats_t *stats,
                                               const uint8_t *data,
                                               size_t bytes)
{
    if (stats == NULL || data == NULL) {
        return;
    }

    for (size_t i = 0; i + 1 < bytes; i += sizeof(int16_t)) {
        int16_t sample = (int16_t)((uint16_t)data[i] | ((uint16_t)data[i + 1] << 8));
        int32_t sample32 = (int32_t)sample;
        uint32_t abs_sample = sample32 < 0 ? (uint32_t)(-sample32) : (uint32_t)sample32;
        if (abs_sample > stats->peak) {
            stats->peak = abs_sample;
        }
        stats->square_sum += (uint64_t)(sample32 * sample32);
        stats->samples++;
    }
}

static uint32_t wake_prompt_cache_pcm_rms(const wake_prompt_cache_pcm_stats_t *stats)
{
    if (stats == NULL || stats->samples == 0) {
        return 0;
    }
    return wake_prompt_cache_isqrt_u64(stats->square_sum / (uint64_t)stats->samples);
}

static esp_err_t wake_prompt_cache_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt == NULL || evt->event_id != HTTP_EVENT_ON_HEADER) {
        return ESP_OK;
    }

    wake_prompt_cache_http_ctx_t *ctx = (wake_prompt_cache_http_ctx_t *)evt->user_data;
    const char *key = evt->header_key != NULL ? evt->header_key : "";
    const char *value = evt->header_value != NULL ? evt->header_value : "";
#if ENABLE_VERBOSE_AUDIO_LOG
    unsigned int header_index = 0;
    if (ctx != NULL) {
        header_index = ctx->header_count++;
    }

    ESP_LOGI(TAG,
             "wake prompt cache response header[%u] %s: %s",
             header_index,
             key[0] != '\0' ? key : "<empty>",
             value[0] != '\0' ? value : "<empty>");
#else
    if (ctx != NULL) {
        ctx->header_count++;
    }
#endif

    if (ctx == NULL || key[0] == '\0') {
        return ESP_OK;
    }

    if (wake_prompt_cache_streq_ci(key, "Content-Type")) {
        strlcpy(ctx->content_type, value, sizeof(ctx->content_type));
    } else if (wake_prompt_cache_streq_ci(key, "X-Audio-Format")) {
        strlcpy(ctx->audio_format, value, sizeof(ctx->audio_format));
    }
    return ESP_OK;
}

static esp_err_t wake_prompt_cache_parse_meta_bytes(const char *meta, size_t *out_bytes)
{
    if (meta == NULL || out_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *bytes_line = strstr(meta, "bytes=");
    if (bytes_line == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    bytes_line += strlen("bytes=");
    char *end = NULL;
    unsigned long value = strtoul(bytes_line, &end, 10);
    if (end == bytes_line || value == 0 || value > WAKE_PROMPT_CACHE_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_bytes = (size_t)value;
    return ESP_OK;
}

static esp_err_t wake_prompt_cache_validate_locked(size_t *out_bytes)
{
    struct stat pcm_stat = {0};
    if (stat(WAKE_PROMPT_CACHE_PCM_PATH, &pcm_stat) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (pcm_stat.st_size <= 0 ||
        ((size_t)pcm_stat.st_size % sizeof(int16_t)) != 0 ||
        (size_t)pcm_stat.st_size > WAKE_PROMPT_CACHE_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *meta_file = fopen(WAKE_PROMPT_CACHE_META_PATH, "rb");
    if (meta_file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    char *meta = (char *)heap_caps_calloc(1, WAKE_PROMPT_CACHE_META_BUFFER_BYTES, MALLOC_CAP_8BIT);
    if (meta == NULL) {
        fclose(meta_file);
        ESP_LOGW(TAG,
                 "wake prompt cache meta buffer alloc failed bytes=%u",
                 (unsigned int)WAKE_PROMPT_CACHE_META_BUFFER_BYTES);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    size_t read_len = fread(meta, 1, WAKE_PROMPT_CACHE_META_BUFFER_BYTES - 1U, meta_file);
    bool read_error = ferror(meta_file);
    fclose(meta_file);
    if (read_error || read_len == 0) {
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    meta[read_len] = '\0';

    if (strstr(meta, "version=" WAKE_PROMPT_CACHE_VERSION) == NULL ||
        strstr(meta, "sample_rate=16000") == NULL ||
        strstr(meta, "format=" WAKE_PROMPT_CACHE_FORMAT) == NULL) {
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    size_t meta_bytes = 0;
    ret = wake_prompt_cache_parse_meta_bytes(meta, &meta_bytes);
    if (ret != ESP_OK) {
        goto cleanup;
    }
    if (meta_bytes != (size_t)pcm_stat.st_size) {
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    if (out_bytes != NULL) {
        *out_bytes = meta_bytes;
    }

cleanup:
    heap_caps_free(meta);
    return ret;
}

static esp_err_t wake_prompt_cache_write_meta(size_t bytes)
{
    FILE *meta_file = fopen(WAKE_PROMPT_CACHE_META_PATH, "wb");
    if (meta_file == NULL) {
        return ESP_FAIL;
    }

    int written = fprintf(meta_file,
                          "text=%s\n"
                          "sample_rate=%u\n"
                          "format=%s\n"
                          "bytes=%u\n"
                          "version=%s\n",
                          WAKE_PROMPT_CACHE_TEXT,
                          (unsigned int)WAKE_PROMPT_CACHE_SAMPLE_RATE_HZ,
                          WAKE_PROMPT_CACHE_FORMAT,
                          (unsigned int)bytes,
                          WAKE_PROMPT_CACHE_VERSION);
    bool ok = written > 0 && fflush(meta_file) == 0;
    fclose(meta_file);
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t wake_prompt_cache_build_endpoint(char *endpoint,
                                                  size_t endpoint_size,
                                                  char *encoded_device_id,
                                                  size_t encoded_device_id_size)
{
    if (endpoint == NULL || endpoint_size == 0 ||
        encoded_device_id == NULL || encoded_device_id_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!wake_prompt_cache_url_encode_component(server_comm_get_device_id(),
                                                encoded_device_id,
                                                encoded_device_id_size)) {
        endpoint[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    int written = snprintf(endpoint,
                           endpoint_size,
                           "/api/voice/prompt-cache?prompt_key=wake_ack_zh&device_id=%s",
                           encoded_device_id);
    if (written < 0 || written >= (int)endpoint_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t wake_prompt_cache_finish_download(size_t bytes)
{
    if (bytes == 0 || (bytes % sizeof(int16_t)) != 0) {
        unlink(WAKE_PROMPT_CACHE_TMP_PATH);
        return ESP_ERR_INVALID_SIZE;
    }

    unlink(WAKE_PROMPT_CACHE_PCM_PATH);
    unlink(WAKE_PROMPT_CACHE_META_PATH);
    if (rename(WAKE_PROMPT_CACHE_TMP_PATH, WAKE_PROMPT_CACHE_PCM_PATH) != 0) {
        unlink(WAKE_PROMPT_CACHE_TMP_PATH);
        return ESP_FAIL;
    }

    esp_err_t ret = wake_prompt_cache_write_meta(bytes);
    if (ret != ESP_OK) {
        unlink(WAKE_PROMPT_CACHE_PCM_PATH);
        unlink(WAKE_PROMPT_CACHE_META_PATH);
        return ret;
    }
    return ESP_OK;
}

static esp_err_t wake_prompt_cache_download_once(void)
{
    wake_prompt_cache_log_stack_high_water("download_enter");

    esp_err_t ret = wake_prompt_cache_with_mount();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    size_t cached_bytes = 0;
    ret = wake_prompt_cache_validate_locked(&cached_bytes);
    xSemaphoreGive(s_cache_lock);
    wake_prompt_cache_log_stack_high_water("after_cache_validate");
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "wake prompt cache hit bytes=%u",
                 (unsigned int)cached_bytes);
        return ESP_OK;
    }
    ESP_LOGD(TAG,
             "wake prompt cache existing file invalid reason=%s, refresh allowed",
             esp_err_to_name(ret));

    if (!server_comm_wifi_is_ready()) {
        return ESP_ERR_WIFI_NOT_CONNECT;
    }
    if (app_runtime_non_voice_is_paused()) {
        return ESP_ERR_INVALID_STATE;
    }

    wake_prompt_cache_download_buffers_t *buffers =
        (wake_prompt_cache_download_buffers_t *)heap_caps_calloc(1,
                                                                 sizeof(*buffers),
                                                                 MALLOC_CAP_8BIT);
    if (buffers == NULL) {
        ESP_LOGW(TAG,
                 "wake prompt cache download buffer alloc failed bytes=%u",
                 (unsigned int)sizeof(*buffers));
        return ESP_ERR_NO_MEM;
    }
    wake_prompt_cache_log_stack_high_water("after_buffer_alloc");

    esp_http_client_handle_t client = NULL;
    FILE *tmp_file = NULL;
    size_t total_bytes = 0;
    bool opened = false;
    int final_status = 0;
    int64_t final_content_length = -1;
    bool final_chunked = false;

    ret = wake_prompt_cache_build_endpoint(buffers->endpoint,
                                           sizeof(buffers->endpoint),
                                           buffers->encoded_device_id,
                                           sizeof(buffers->encoded_device_id));
    if (ret != ESP_OK) {
        goto cleanup;
    }

    ret = server_comm_build_url(buffers->endpoint, buffers->url, sizeof(buffers->url));
    if (ret != ESP_OK) {
        goto cleanup;
    }

    ESP_LOGD(TAG,
             "wake prompt cache download start endpoint=%s url=%s",
             buffers->endpoint,
             buffers->url);
    buffers->http_config = (esp_http_client_config_t) {
        .url = buffers->url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = (int)WAKE_PROMPT_CACHE_DOWNLOAD_TIMEOUT_MS,
        .buffer_size = (int)WAKE_PROMPT_CACHE_READ_BYTES,
        .event_handler = wake_prompt_cache_http_event_handler,
        .user_data = &buffers->http_ctx,
    };
    client = esp_http_client_init(&buffers->http_config);
    if (client == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    wake_prompt_cache_log_stack_high_water("after_http_init");
    do {
        device_protocol_metadata_t metadata = {0};
        device_protocol_prepare_metadata(&metadata, "voice.prompt");
        for (size_t i = 0; i < metadata.header_count; i++) {
            ret = esp_http_client_set_header(client,
                                             metadata.headers[i].key,
                                             metadata.headers[i].value);
            if (ret != ESP_OK) {
                break;
            }
        }
        if (ret != ESP_OK) {
            break;
        }

        ret = esp_http_client_open(client, 0);
        if (ret != ESP_OK) {
            break;
        }
        opened = true;

        int64_t content_length = esp_http_client_fetch_headers(client);
        final_content_length = content_length;
        if (content_length < 0) {
            ESP_LOGW(TAG,
                     "wake prompt cache download fetch headers failed content_length=%lld",
                     (long long)content_length);
            ret = ESP_FAIL;
            break;
        }
        wake_prompt_cache_log_stack_high_water("after_fetch_headers");

        int status = esp_http_client_get_status_code(client);
        bool chunked = esp_http_client_is_chunked_response(client);
        final_status = status;
        final_chunked = chunked;
        ESP_LOGD(TAG,
                 "wake prompt cache response status=%d content_length=%lld chunked=%d headers=%u content_type=%s audio_format=%s",
                 status,
                 (long long)content_length,
                 chunked ? 1 : 0,
                 buffers->http_ctx.header_count,
                 buffers->http_ctx.content_type[0] != '\0' ? buffers->http_ctx.content_type : "<none>",
                 buffers->http_ctx.audio_format[0] != '\0' ? buffers->http_ctx.audio_format : "<none>");

        if (!server_comm_http_status_is_success(status)) {
            ESP_LOGW(TAG,
                     "wake prompt cache download bad status=%d content_length=%lld",
                     status,
                     (long long)content_length);
            ret = ESP_FAIL;
            break;
        }

        if (buffers->http_ctx.content_type[0] == '\0') {
            ESP_LOGD(TAG,
                     "wake prompt cache download content_type missing, continue with body validation");
        } else if (!wake_prompt_cache_content_type_ok(buffers->http_ctx.content_type)) {
            ESP_LOGD(TAG,
                     "wake prompt cache download unexpected content_type=%s, continue with body validation",
                     buffers->http_ctx.content_type);
        }

        if (buffers->http_ctx.audio_format[0] == '\0') {
            ESP_LOGD(TAG,
                     "wake prompt cache download X-Audio-Format missing, continue with body validation");
        } else if (strcmp(buffers->http_ctx.audio_format, WAKE_PROMPT_CACHE_FORMAT) != 0) {
            ESP_LOGD(TAG,
                     "wake prompt cache download unexpected audio_format=%s expected=%s, continue with body validation",
                     buffers->http_ctx.audio_format,
                     WAKE_PROMPT_CACHE_FORMAT);
        }

        if ((content_length <= 0 && !chunked) || content_length > WAKE_PROMPT_CACHE_MAX_BYTES) {
            ESP_LOGW(TAG,
                     "wake prompt cache download invalid content_length=%lld chunked=%d max=%u",
                     (long long)content_length,
                     chunked ? 1 : 0,
                     (unsigned int)WAKE_PROMPT_CACHE_MAX_BYTES);
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }

        xSemaphoreTake(s_cache_lock, portMAX_DELAY);
        unlink(WAKE_PROMPT_CACHE_PCM_PATH);
        unlink(WAKE_PROMPT_CACHE_META_PATH);
        unlink(WAKE_PROMPT_CACHE_TMP_PATH);
        tmp_file = fopen(WAKE_PROMPT_CACHE_TMP_PATH, "wb");
        xSemaphoreGive(s_cache_lock);
        if (tmp_file == NULL) {
            ret = ESP_FAIL;
            break;
        }
        wake_prompt_cache_log_stack_high_water("after_file_open");

        int empty_reads = 0;
        while (!esp_http_client_is_complete_data_received(client)) {
            if (app_runtime_non_voice_is_paused()) {
                ESP_LOGW(TAG, "wake prompt cache download abort reason=voice_exclusive");
                ret = ESP_ERR_INVALID_STATE;
                break;
            }

            int read_len = esp_http_client_read(client,
                                                (char *)buffers->read_buf,
                                                sizeof(buffers->read_buf));
            if (read_len > 0) {
                empty_reads = 0;
                total_bytes += (size_t)read_len;
                if (total_bytes > WAKE_PROMPT_CACHE_MAX_BYTES) {
                    ret = ESP_ERR_INVALID_SIZE;
                    break;
                }
                if (fwrite(buffers->read_buf, 1, (size_t)read_len, tmp_file) != (size_t)read_len) {
                    ret = ESP_FAIL;
                    break;
                }
                wake_prompt_cache_update_pcm_stats(&buffers->pcm_stats,
                                                   buffers->read_buf,
                                                   (size_t)read_len);
                continue;
            }

            if (read_len < 0 && read_len != -ESP_ERR_HTTP_EAGAIN) {
                ret = ESP_FAIL;
                break;
            }

            empty_reads++;
            if (empty_reads >= WAKE_PROMPT_CACHE_MAX_EMPTY_READS) {
                ret = ESP_ERR_TIMEOUT;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(WAKE_PROMPT_CACHE_EMPTY_READ_DELAY_MS));
        }
        wake_prompt_cache_log_stack_high_water("after_body_read");

        if (ret != ESP_OK) {
            break;
        }
        if (content_length > 0 && total_bytes != (size_t)content_length) {
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (buffers->pcm_stats.samples == 0 || buffers->pcm_stats.peak == 0) {
            ret = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (fflush(tmp_file) != 0) {
            ret = ESP_FAIL;
            break;
        }
    } while (0);

    if (tmp_file != NULL) {
        fclose(tmp_file);
        tmp_file = NULL;
    }
    if (opened) {
        esp_http_client_close(client);
        opened = false;
    }
    if (client != NULL) {
        esp_http_client_cleanup(client);
        client = NULL;
    }

    if (ret != ESP_OK) {
        unlink(WAKE_PROMPT_CACHE_TMP_PATH);
        ESP_LOGW(TAG,
                 "wake prompt cache download failed ret=%s status=%d content_length=%lld chunked=%d bytes=%u samples=%u peak=%u rms=%u content_type=%s audio_format=%s",
                 esp_err_to_name(ret),
                 final_status,
                 (long long)final_content_length,
                 final_chunked ? 1 : 0,
                 (unsigned int)total_bytes,
                 (unsigned int)buffers->pcm_stats.samples,
                 (unsigned int)buffers->pcm_stats.peak,
                 (unsigned int)wake_prompt_cache_pcm_rms(&buffers->pcm_stats),
                 buffers->http_ctx.content_type[0] != '\0' ? buffers->http_ctx.content_type : "<none>",
                 buffers->http_ctx.audio_format[0] != '\0' ? buffers->http_ctx.audio_format : "<none>");
        goto cleanup;
    }

    wake_prompt_cache_log_stack_high_water("before_finish_download");
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    ret = wake_prompt_cache_finish_download(total_bytes);
    xSemaphoreGive(s_cache_lock);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "wake prompt cache save failed ret=%s bytes=%u",
                 esp_err_to_name(ret),
                 (unsigned int)total_bytes);
        goto cleanup;
    }

    ESP_LOGI(TAG,
             "wake prompt cache download success path=%s bytes=%u samples=%u peak=%u rms=%u content_type=%s audio_format=%s",
             WAKE_PROMPT_CACHE_PCM_PATH,
             (unsigned int)total_bytes,
             (unsigned int)buffers->pcm_stats.samples,
             (unsigned int)buffers->pcm_stats.peak,
             (unsigned int)wake_prompt_cache_pcm_rms(&buffers->pcm_stats),
             buffers->http_ctx.content_type[0] != '\0' ? buffers->http_ctx.content_type : "<none>",
             buffers->http_ctx.audio_format[0] != '\0' ? buffers->http_ctx.audio_format : "<none>");

cleanup:
    if (tmp_file != NULL) {
        fclose(tmp_file);
    }
    if (opened && client != NULL) {
        esp_http_client_close(client);
    }
    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    heap_caps_free(buffers);
    wake_prompt_cache_log_stack_high_water("download_exit");
    return ret;
}

static void wake_prompt_cache_download_task(void *arg)
{
    (void)arg;
    wake_prompt_cache_log_stack_high_water("task_entry");
    esp_err_t ret = wake_prompt_cache_download_once();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wake prompt cache background download done ret=%s", esp_err_to_name(ret));
    }

    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    s_download_task = NULL;
    xSemaphoreGive(s_cache_lock);
    wake_prompt_cache_log_stack_high_water("task_exit");
    vTaskDelete(NULL);
}

esp_err_t wake_prompt_cache_start_async(void)
{
    esp_err_t ret = wake_prompt_cache_ensure_lock();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    if (s_download_task != NULL) {
        xSemaphoreGive(s_cache_lock);
        return ESP_OK;
    }

    BaseType_t created = xTaskCreate(wake_prompt_cache_download_task,
                                     "wake_prompt_cache",
                                     WAKE_PROMPT_CACHE_TASK_STACK,
                                     NULL,
                                     WAKE_PROMPT_CACHE_TASK_PRIORITY,
                                     &s_download_task);
    xSemaphoreGive(s_cache_lock);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t wake_prompt_cache_play(void)
{
    esp_err_t ret = wake_prompt_cache_with_mount();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    size_t cached_bytes = 0;
    ret = wake_prompt_cache_validate_locked(&cached_bytes);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_cache_lock);
        return ret;
    }

    FILE *pcm_file = fopen(WAKE_PROMPT_CACHE_PCM_PATH, "rb");
    if (pcm_file == NULL) {
        xSemaphoreGive(s_cache_lock);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG,
             "wake prompt cache hit playback start bytes=%u samples=%u",
             (unsigned int)cached_bytes,
             (unsigned int)(cached_bytes / sizeof(int16_t)));

    ret = audio_player_stream_open();
    if (ret != ESP_OK) {
        fclose(pcm_file);
        xSemaphoreGive(s_cache_lock);
        return ret;
    }

    uint8_t *read_buf = (uint8_t *)heap_caps_malloc(WAKE_PROMPT_CACHE_READ_BYTES, MALLOC_CAP_8BIT);
    if (read_buf == NULL) {
        (void)audio_player_stream_finish();
        fclose(pcm_file);
        xSemaphoreGive(s_cache_lock);
        ESP_LOGW(TAG,
                 "wake prompt cache playback buffer alloc failed bytes=%u",
                 (unsigned int)WAKE_PROMPT_CACHE_READ_BYTES);
        return ESP_ERR_NO_MEM;
    }

    size_t played_bytes = 0;
    while (played_bytes < cached_bytes) {
        size_t remain = cached_bytes - played_bytes;
        size_t want = remain < WAKE_PROMPT_CACHE_READ_BYTES ?
                      remain : WAKE_PROMPT_CACHE_READ_BYTES;
        size_t got = fread(read_buf, 1, want, pcm_file);
        if (got == 0) {
            ret = ferror(pcm_file) ? ESP_FAIL : ESP_ERR_INVALID_SIZE;
            break;
        }
        if ((got % sizeof(int16_t)) != 0) {
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }

        ret = audio_player_write_pcm_chunk((const int16_t *)read_buf,
                                           (uint32_t)(got / sizeof(int16_t)),
                                           (int)WAKE_PROMPT_CACHE_SAMPLE_RATE_HZ);
        if (ret != ESP_OK) {
            break;
        }
        played_bytes += got;
    }
    heap_caps_free(read_buf);
    fclose(pcm_file);

    esp_err_t finish_ret = audio_player_stream_finish();
    if (ret == ESP_OK && finish_ret != ESP_OK) {
        ret = finish_ret;
    }
    xSemaphoreGive(s_cache_lock);
    ESP_LOGI(TAG, "wake prompt cache playback end ret=%s", esp_err_to_name(ret));
    return ret;
}

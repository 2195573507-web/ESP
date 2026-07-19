/**
 * @file wake_prompt_cache.c
 * @brief C5 唤醒提示音流式客户端。
 *
 * 本文件属于 ESP32-C5 终端。唤醒词命中后，local_wake_word 调用
 * wake_prompt_cache_play()，本模块只向 ESPS3 本地接口请求当前 wake prompt PCM。
 * PCM 先写入 C5 storage SPIFFS 临时文件，关闭 HTTP client 后再交给 speaker_player，
 * 避免 HTTP 收发缓冲与 IIS writer task 栈同时占用内部 heap。临时文件不作为缓存，
 * 每次播放后立即删除；请求失败时返回错误给 local_wake_word，由它播放短 beep 兜底。
 */

#include "wake_prompt_cache.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_stack_monitor.h"
#include "device_protocol_metadata.h"
#include "esp111_protocol_common.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "local_wake_word.h"
#include "server_comm_config.h"
#include "server_comm_errors.h"
#include "server_comm_http.h"
#include "speaker_player.h"

static const char *TAG = "wake_prompt_stream";

#define WAKE_PROMPT_STREAM_ENDPOINT ESP111_PROTOCOL_ROUTE_WAKE_PROMPT_AUDIO
#define WAKE_PROMPT_STREAM_SAMPLE_RATE_HZ 16000U
#define WAKE_PROMPT_STREAM_MAX_BYTES (96U * 1024U)
#define WAKE_PROMPT_STREAM_MIN_BYTES 64U
#define WAKE_PROMPT_SPOOL_PARTITION_LABEL "storage"
#define WAKE_PROMPT_SPOOL_BASE_PATH "/wake_prompt"
#define WAKE_PROMPT_SPOOL_PATH WAKE_PROMPT_SPOOL_BASE_PATH "/wake_prompt.pcm"
#define WAKE_PROMPT_SPOOL_BUFFER_BYTES 2048U

typedef struct {
    FILE *file;
    size_t response_bytes;
    uint32_t chunks;
} wake_prompt_spool_ctx_t;

static bool s_wake_prompt_spiffs_mounted;

static uint32_t wake_prompt_abs_i16(int16_t sample)
{
    int32_t value = (int32_t)sample;
    return value < 0 ? (uint32_t)(-value) : (uint32_t)value;
}

static void wake_prompt_log_internal_heap(const char *stage)
{
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI(TAG,
             "WAKE_PROMPT_HEAP stage=%s free=%u min_free=%u largest=%u",
             stage != NULL ? stage : "<none>",
             (unsigned int)heap_caps_get_free_size(caps),
             (unsigned int)heap_caps_get_minimum_free_size(caps),
             (unsigned int)heap_caps_get_largest_free_block(caps));
}

static void wake_prompt_log_spool_heap(const char *marker)
{
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI(TAG,
             "%s internal_free=%u internal_largest=%u",
             marker,
             (unsigned int)heap_caps_get_free_size(caps),
             (unsigned int)heap_caps_get_largest_free_block(caps));
}

static esp_err_t wake_prompt_spool_mount(void)
{
    if (s_wake_prompt_spiffs_mounted) {
        return ESP_OK;
    }

    const esp_vfs_spiffs_conf_t config = {
        .base_path = WAKE_PROMPT_SPOOL_BASE_PATH,
        .partition_label = WAKE_PROMPT_SPOOL_PARTITION_LABEL,
        .max_files = 1,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "wake prompt spool mount failed partition=%s ret=%s",
                 WAKE_PROMPT_SPOOL_PARTITION_LABEL,
                 esp_err_to_name(ret));
        return ret;
    }

    s_wake_prompt_spiffs_mounted = true;
    wake_prompt_log_internal_heap("spiffs_mounted");
    return ESP_OK;
}

static bool wake_prompt_content_type_ok(const char *content_type)
{
    return content_type != NULL &&
           strstr(content_type, "audio/L16") != NULL &&
           strstr(content_type, "rate=16000") != NULL &&
           strstr(content_type, "channels=1") != NULL;
}

static const char *wake_prompt_failure_reason(esp_err_t ret, const char *fallback)
{
    switch (ret) {
    case SERVER_COMM_ERR_HEADER_BUFFER_TOO_SMALL:
        return "header_buffer_too_small";
    case SERVER_COMM_ERR_FETCH_HEADER_TIMEOUT:
        return "fetch_header_timeout";
    case SERVER_COMM_ERR_BAD_STATUS:
        return "http_status_non_2xx";
    case SERVER_COMM_ERR_STREAM_READ_FAILED:
    case ESP_ERR_TIMEOUT:
        return "stream_read_failed";
    case SERVER_COMM_ERR_BLOCKED_BY_VOICE_BUSY:
        return "blocked_by_voice_busy";
    default:
        return fallback != NULL ? fallback : "request_failed";
    }
}

static esp_err_t wake_prompt_spool_chunk(const uint8_t *data, size_t len, void *user_ctx)
{
    wake_prompt_spool_ctx_t *ctx = (wake_prompt_spool_ctx_t *)user_ctx;
    if (ctx == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->response_bytes + len > WAKE_PROMPT_STREAM_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (ctx->file == NULL || fwrite(data, 1, len, ctx->file) != len) {
        return ESP_FAIL;
    }

    ctx->response_bytes += len;
    ctx->chunks++;
    return ESP_OK;
}

static esp_err_t wake_prompt_play_spool_file(FILE *file, size_t expected_bytes, uint32_t *out_peak)
{
    if (file == NULL || out_peak == NULL || expected_bytes == 0 ||
        (expected_bytes % sizeof(int16_t)) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int16_t pcm_buf[WAKE_PROMPT_SPOOL_BUFFER_BYTES / sizeof(int16_t)];
    size_t played_bytes = 0;
    uint32_t peak = 0;
    esp_err_t ret = ESP_OK;

    wake_prompt_log_internal_heap("before_speaker_stream_open");
    ret = audio_player_stream_open();
    if (ret != ESP_OK) {
        return ret;
    }

    while (played_bytes < expected_bytes) {
        size_t bytes_to_read = expected_bytes - played_bytes;
        if (bytes_to_read > sizeof(pcm_buf)) {
            bytes_to_read = sizeof(pcm_buf);
        }
        size_t bytes_read = fread(pcm_buf, 1, bytes_to_read, file);
        if (bytes_read != bytes_to_read) {
            ret = ESP_FAIL;
            break;
        }

        size_t sample_count = bytes_read / sizeof(int16_t);
        for (size_t i = 0; i < sample_count; i++) {
            uint32_t abs_sample = wake_prompt_abs_i16(pcm_buf[i]);
            if (abs_sample > peak) {
                peak = abs_sample;
            }
        }
        ret = audio_player_write_pcm_chunk(pcm_buf,
                                           (uint32_t)sample_count,
                                           (int)WAKE_PROMPT_STREAM_SAMPLE_RATE_HZ);
        if (ret != ESP_OK) {
            break;
        }
        played_bytes += bytes_read;
    }

    esp_err_t finish_ret = ret == ESP_OK ? audio_player_stream_finish() : audio_player_stream_abort();
    if (ret == ESP_OK && finish_ret != ESP_OK) {
        ret = finish_ret;
    }
    if (ret == ESP_OK && played_bytes != expected_bytes) {
        ret = ESP_FAIL;
    }
    *out_peak = peak;
    return ret;
}

static esp_err_t wake_prompt_validate_response(const server_comm_http_response_t *response)
{
    if (response == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!server_comm_http_status_is_success(response->status_code)) {
        return ESP_FAIL;
    }
    if (response->content_length > (int64_t)WAKE_PROMPT_STREAM_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (response->content_type[0] != '\0' &&
        !wake_prompt_content_type_ok(response->content_type)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (strcmp(response->audio_format, ESP111_PROTOCOL_AUDIO_FORMAT_PCM_S16LE_MONO_16K) != 0 ||
        strcmp(response->audio_sample_rate, "16000") != 0 ||
        strcmp(response->audio_channels, "1") != 0 ||
        response->audio_version[0] == '\0' ||
        response->voice_config_hash[0] == '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t wake_prompt_cache_start_async(void)
{
    /*
     * 调用时机：旧启动编排可能在 WiFi ready 后调用本函数。
     * 不预下载或持久缓存提示音；仅挂载播放时使用的临时 spool 分区。
     */
    return wake_prompt_spool_mount();
}

esp_err_t wake_prompt_cache_play(void)
{
    /*
     * 调用时机：WakeNet 命中后、录音窗口打开前调用。
     * 成功路径：GET S3 /local/v1/audio/wake-prompt，写临时文件并关闭 HTTP 后播放。
     * 失败路径：返回错误给 local_wake_word，由本地 short beep 兜底，录音窗口继续打开。
     */
    if (!server_comm_wifi_is_ready()) {
        return SERVER_COMM_ERR_WIFI_NOT_READY;
    }
    if (!local_wake_word_is_ack_active()) {
        ESP_LOGW(TAG, "wake prompt stream rejected outside local_wake ack");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = wake_prompt_spool_mount();
    if (ret != ESP_OK) {
        return ret;
    }

    device_protocol_metadata_t metadata = {0};
    device_protocol_prepare_metadata(&metadata, "voice.prompt");
    server_comm_header_t headers[DEVICE_PROTOCOL_MAX_HEADERS];
    size_t header_count = 0;
    for (size_t i = 0; i < metadata.header_count && header_count < DEVICE_PROTOCOL_MAX_HEADERS; i++) {
        headers[header_count++] = metadata.headers[i];
    }

    server_comm_raw_stream_config_t config = {
        .endpoint = WAKE_PROMPT_STREAM_ENDPOINT,
        .content_type = NULL,
        .headers = headers,
        .header_count = header_count,
        .timeout_ms = WAKE_PROMPT_CACHE_CONNECT_TIMEOUT_MS,
        .fetch_headers_timeout_ms = WAKE_PROMPT_CACHE_FETCH_HEADERS_TIMEOUT_MS,
        .read_timeout_ms = WAKE_PROMPT_CACHE_READ_TIMEOUT_MS,
        .total_timeout_ms = WAKE_PROMPT_CACHE_TOTAL_TIMEOUT_MS,
        .buffer_size = (int)WAKE_PROMPT_CACHE_HTTP_HEADER_BUFFER_BYTES,
        .tx_buffer_size = (int)WAKE_PROMPT_CACHE_HTTP_TX_BUFFER_BYTES,
    };

    server_comm_raw_stream_t *stream = NULL;
    server_comm_http_response_t response = {0};
    wake_prompt_spool_ctx_t spool = {0};
    FILE *spool_reader = NULL;
    uint32_t peak = 0;
    bool spool_path_created = false;
    server_comm_http_set_wake_prompt_request_active(true);
    ret = server_comm_http_get_raw_stream_begin(&config, &stream);
    if (ret != ESP_OK) {
        server_comm_http_set_wake_prompt_request_active(false);
        ESP_LOGW(TAG,
                 "wake prompt S3 request failed reason=%s endpoint=%s ret=%s rx_buffer=%u tx_buffer=%u",
                 wake_prompt_failure_reason(ret, "http_open_failed"),
                 WAKE_PROMPT_STREAM_ENDPOINT,
                 server_comm_err_to_name(ret),
                 (unsigned int)WAKE_PROMPT_CACHE_HTTP_HEADER_BUFFER_BYTES,
                 (unsigned int)WAKE_PROMPT_CACHE_HTTP_TX_BUFFER_BYTES);
        return ret;
    }

    app_stack_monitor_log(TAG, "wake_prompt_rx", "after_open");
    wake_prompt_log_internal_heap("http_open");
    int64_t headers_begin_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG,
             "wake prompt fetch headers begin endpoint=%s timeout_ms=%u rx_buffer=%u",
             WAKE_PROMPT_STREAM_ENDPOINT,
             (unsigned int)WAKE_PROMPT_CACHE_FETCH_HEADERS_TIMEOUT_MS,
             (unsigned int)WAKE_PROMPT_CACHE_HTTP_HEADER_BUFFER_BYTES);
    ret = server_comm_http_fetch_headers(stream, &response);
    int64_t headers_elapsed_ms = esp_timer_get_time() / 1000 - headers_begin_ms;
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "wake prompt S3 request failed reason=%s endpoint=%s ret=%s timeout_ms=%u elapsed_ms=%lld",
                 wake_prompt_failure_reason(ret, "http_fetch_headers_failed"),
                 WAKE_PROMPT_STREAM_ENDPOINT,
                 server_comm_err_to_name(ret),
                 (unsigned int)WAKE_PROMPT_CACHE_FETCH_HEADERS_TIMEOUT_MS,
                 (long long)headers_elapsed_ms);
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "wake prompt fetch headers end endpoint=%s elapsed_ms=%lld status=%d content_length=%lld",
                 WAKE_PROMPT_STREAM_ENDPOINT,
                 (long long)headers_elapsed_ms,
                 response.status_code,
                 (long long)response.content_length);
        if (!server_comm_http_status_is_success(response.status_code)) {
            ret = SERVER_COMM_ERR_BAD_STATUS;
            ESP_LOGW(TAG,
                     "wake prompt S3 request failed reason=http_status_non_2xx endpoint=%s ret=%s status=%d content_length=%lld content_type=%s",
                     WAKE_PROMPT_STREAM_ENDPOINT,
                     server_comm_err_to_name(ret),
                     response.status_code,
                     (long long)response.content_length,
                     response.content_type[0] != '\0' ? response.content_type : "<none>");
        } else {
            ret = wake_prompt_validate_response(&response);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "wake prompt S3 request failed reason=invalid_response endpoint=%s ret=%s status=%d content_length=%lld content_type=%s",
                         WAKE_PROMPT_STREAM_ENDPOINT,
                         server_comm_err_to_name(ret),
                         response.status_code,
                         (long long)response.content_length,
                         response.content_type[0] != '\0' ? response.content_type : "<none>");
            }
        }
    }
    if (ret == ESP_OK) {
        wake_prompt_log_spool_heap("WAKE_PROMPT_SPOOL_START");
        ESP_LOGI(TAG,
                 "wake prompt spool download start status=%d content_length=%lld content_type=%s buffer_bytes=%u",
                 response.status_code,
                 (long long)response.content_length,
                 response.content_type[0] != '\0' ? response.content_type : "<none>",
                 (unsigned int)WAKE_PROMPT_SPOOL_BUFFER_BYTES);
        (void)remove(WAKE_PROMPT_SPOOL_PATH);
        spool.file = fopen(WAKE_PROMPT_SPOOL_PATH, "wb");
        if (spool.file == NULL) {
            ret = ESP_FAIL;
            ESP_LOGW(TAG, "wake prompt spool open failed path=%s", WAKE_PROMPT_SPOOL_PATH);
        } else {
            spool_path_created = true;
            ret = server_comm_http_read_response(stream, wake_prompt_spool_chunk, &spool, &response);
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "wake prompt S3 request failed reason=%s endpoint=%s ret=%s status=%d bytes=%u",
                     wake_prompt_failure_reason(ret, "stream_read_failed"),
                     WAKE_PROMPT_STREAM_ENDPOINT,
                     server_comm_err_to_name(ret),
                     response.status_code,
                     (unsigned int)spool.response_bytes);
        }
    }

    if (spool.file != NULL) {
        if (fclose(spool.file) != 0 && ret == ESP_OK) {
            ret = ESP_FAIL;
        }
        spool.file = NULL;
    }
    server_comm_http_post_raw_stream_close(stream);
    server_comm_http_set_wake_prompt_request_active(false);
    wake_prompt_log_internal_heap("http_closed_before_speaker");

    if (ret == ESP_OK && (spool.response_bytes % sizeof(int16_t)) != 0) {
        ret = ESP_ERR_INVALID_SIZE;
    }
    if (ret == ESP_OK &&
        spool.response_bytes < WAKE_PROMPT_STREAM_MIN_BYTES) {
        ret = ESP_ERR_INVALID_RESPONSE;
        ESP_LOGW(TAG,
                 "wake prompt S3 request failed reason=empty_payload endpoint=%s bytes=%u",
                 WAKE_PROMPT_STREAM_ENDPOINT,
                 (unsigned int)spool.response_bytes);
    }
    if (ret == ESP_OK) {
        wake_prompt_log_spool_heap("WAKE_PROMPT_SPOOL_READY");
        spool_reader = fopen(WAKE_PROMPT_SPOOL_PATH, "rb");
        if (spool_reader == NULL) {
            ret = ESP_FAIL;
        } else {
            ret = wake_prompt_play_spool_file(spool_reader, spool.response_bytes, &peak);
        }
    }
    if (spool_reader != NULL) {
        fclose(spool_reader);
    }
    if (spool_path_created) {
        (void)remove(WAKE_PROMPT_SPOOL_PATH);
        wake_prompt_log_spool_heap("WAKE_PROMPT_SPOOL_DELETE");
    }
    if (ret == ESP_OK && peak == 0) {
        ret = ESP_ERR_INVALID_RESPONSE;
        ESP_LOGW(TAG,
                 "wake prompt S3 request failed reason=empty_payload endpoint=%s bytes=%u peak=%u",
                 WAKE_PROMPT_STREAM_ENDPOINT,
                 (unsigned int)spool.response_bytes,
                 (unsigned int)peak);
    }

    ESP_LOGI(TAG,
             "wake prompt stream end ret=%s bytes=%u chunks=%u peak=%u",
             server_comm_err_to_name(ret),
             (unsigned int)spool.response_bytes,
             (unsigned int)spool.chunks,
             (unsigned int)peak);
    return ret;
}

/**
 * @file wake_prompt_cache.c
 * @brief C5 唤醒提示音流式客户端。
 *
 * 本文件属于 ESP32-C5 终端。唤醒词命中后，local_wake_word 调用
 * wake_prompt_cache_play()，本模块只向 ESPS3 本地接口请求当前 wake prompt PCM，
 * 并边接收边写入 speaker_player；C5 不保存完整中文提示音、不解析提示词文本，
 * 请求失败时把错误返回给 local_wake_word，由它播放本地极短 beep 兜底。
 */

#include "wake_prompt_cache.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "app_stack_monitor.h"
#include "device_protocol_metadata.h"
#include "esp111_protocol_common.h"
#include "esp_err.h"
#include "esp_log.h"
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

typedef struct {
    bool stream_open;
    bool has_leftover;
    uint8_t leftover;
    uint8_t combined_buf[SERVER_COMM_HTTP_READ_CHUNK_BYTES + 1U];
    int16_t pcm_buf[(SERVER_COMM_HTTP_READ_CHUNK_BYTES + 1U) / sizeof(int16_t)];
    size_t response_bytes;
    uint32_t chunks;
    uint32_t peak;
} wake_prompt_stream_ctx_t;

static uint32_t wake_prompt_abs_i16(int16_t sample)
{
    int32_t value = (int32_t)sample;
    return value < 0 ? (uint32_t)(-value) : (uint32_t)value;
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

static void wake_prompt_decode_s16le(const uint8_t *bytes,
                                     int16_t *samples,
                                     size_t sample_count)
{
    if (bytes == NULL || samples == NULL) {
        return;
    }

    for (size_t i = 0; i < sample_count; i++) {
        uint16_t raw = (uint16_t)bytes[i * 2U] |
                       ((uint16_t)bytes[i * 2U + 1U] << 8U);
        samples[i] = (int16_t)raw;
    }
}

static void wake_prompt_update_peak(wake_prompt_stream_ctx_t *ctx,
                                    const int16_t *samples,
                                    size_t sample_count)
{
    if (ctx == NULL || samples == NULL) {
        return;
    }

    for (size_t i = 0; i < sample_count; i++) {
        uint32_t abs_sample = wake_prompt_abs_i16(samples[i]);
        if (abs_sample > ctx->peak) {
            ctx->peak = abs_sample;
        }
    }
}

static esp_err_t wake_prompt_play_chunk(const uint8_t *data, size_t len, void *user_ctx)
{
    wake_prompt_stream_ctx_t *ctx = (wake_prompt_stream_ctx_t *)user_ctx;
    if (ctx == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > SERVER_COMM_HTTP_READ_CHUNK_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (ctx->response_bytes + len > WAKE_PROMPT_STREAM_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t bytes = len;
    const uint8_t *pcm_bytes = data;
    if (ctx->has_leftover) {
        ctx->combined_buf[0] = ctx->leftover;
        memcpy(ctx->combined_buf + 1U, data, len);
        pcm_bytes = ctx->combined_buf;
        bytes++;
        ctx->has_leftover = false;
    }

    if ((bytes % sizeof(int16_t)) != 0) {
        ctx->leftover = pcm_bytes[bytes - 1U];
        ctx->has_leftover = true;
        bytes--;
    }
    if (bytes == 0) {
        return ESP_OK;
    }

    if (!ctx->stream_open) {
        esp_err_t ret = audio_player_stream_open();
        if (ret != ESP_OK) {
            return ret;
        }
        ctx->stream_open = true;
    }

    size_t sample_count = bytes / sizeof(int16_t);
    wake_prompt_decode_s16le(pcm_bytes, ctx->pcm_buf, sample_count);
    wake_prompt_update_peak(ctx, ctx->pcm_buf, sample_count);

    esp_err_t ret = audio_player_write_pcm_chunk(ctx->pcm_buf,
                                                 (uint32_t)sample_count,
                                                 (int)WAKE_PROMPT_STREAM_SAMPLE_RATE_HZ);
    if (ret != ESP_OK) {
        return ret;
    }

    ctx->response_bytes += bytes;
    ctx->chunks++;
    return ESP_OK;
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
     * 当前 C5 不再预下载或写入 SPIFFS；S3 负责缓存，本函数保留为兼容 no-op。
     */
    ESP_LOGI(TAG, "wake prompt local cache disabled; S3 cache will be requested on wake");
    return ESP_OK;
}

esp_err_t wake_prompt_cache_play(void)
{
    /*
     * 调用时机：WakeNet 命中后、录音窗口打开前调用。
     * 成功路径：GET S3 /local/v1/audio/wake-prompt，校验音频头后边收边播。
     * 失败路径：返回错误给 local_wake_word，由本地 short beep 兜底，录音窗口继续打开。
     */
    if (!server_comm_wifi_is_ready()) {
        return SERVER_COMM_ERR_WIFI_NOT_READY;
    }
    if (!local_wake_word_is_ack_active()) {
        ESP_LOGW(TAG, "wake prompt stream rejected outside local_wake ack");
        return ESP_ERR_INVALID_STATE;
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
    wake_prompt_stream_ctx_t playback = {0};
    server_comm_http_set_wake_prompt_request_active(true);
    esp_err_t ret = server_comm_http_get_raw_stream_begin(&config, &stream);
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
        ESP_LOGI(TAG,
                 "wake prompt stream start status=%d content_length=%lld content_type=%s",
                 response.status_code,
                 (long long)response.content_length,
                 response.content_type[0] != '\0' ? response.content_type : "<none>");
        ret = server_comm_http_read_response(stream, wake_prompt_play_chunk, &playback, &response);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "wake prompt S3 request failed reason=%s endpoint=%s ret=%s status=%d bytes=%u",
                     wake_prompt_failure_reason(ret, "stream_read_failed"),
                     WAKE_PROMPT_STREAM_ENDPOINT,
                     server_comm_err_to_name(ret),
                     response.status_code,
                     (unsigned int)playback.response_bytes);
        }
    }

    if (playback.stream_open) {
        esp_err_t finish_ret = ret == ESP_OK ?
                               audio_player_stream_finish() :
                               audio_player_stream_abort();
        if (ret == ESP_OK && finish_ret != ESP_OK) {
            ret = finish_ret;
        }
    }
    server_comm_http_post_raw_stream_close(stream);
    server_comm_http_set_wake_prompt_request_active(false);

    if (ret == ESP_OK && playback.has_leftover) {
        ret = ESP_ERR_INVALID_SIZE;
    }
    if (ret == ESP_OK &&
        (playback.response_bytes < WAKE_PROMPT_STREAM_MIN_BYTES || playback.peak == 0)) {
        ret = ESP_ERR_INVALID_RESPONSE;
        ESP_LOGW(TAG,
                 "wake prompt S3 request failed reason=empty_payload endpoint=%s bytes=%u peak=%u",
                 WAKE_PROMPT_STREAM_ENDPOINT,
                 (unsigned int)playback.response_bytes,
                 (unsigned int)playback.peak);
    }

    ESP_LOGI(TAG,
             "wake prompt stream end ret=%s bytes=%u chunks=%u peak=%u",
             server_comm_err_to_name(ret),
             (unsigned int)playback.response_bytes,
             (unsigned int)playback.chunks,
             (unsigned int)playback.peak);
    return ret;
}

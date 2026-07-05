/**
 * @file server_voice_client.c
 * @brief C5 终端到 S3 voice_proxy 的 PCM 客户端。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），负责缓存/上传 Mic PCM、读取
 * S3 回传 PCM 并交给 speaker_player 播放。它不执行 ASR/LLM/TTS，不解析文本语义，
 * 也不直接调用公网 Server；S3 voice_proxy 是 C5<->S3 轻量语音边界。
 */

#include "server_voice_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_debug_config.h"
#include "app_stack_monitor.h"
#include "device_protocol_metadata.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "server_comm_config.h"
#include "server_comm_http.h"
#include "server_voice_protocol.h"
#include "speaker_player.h"

static const char *TAG = "server_voice_client";

#define SERVER_VOICE_PCM_PREVIEW_SAMPLES 16U
#define SERVER_VOICE_UPLOAD_INITIAL_BYTES (16U * 1024U)
#define SERVER_VOICE_UPLOAD_MAX_BYTES (320U * 1024U)

typedef enum {
    SERVER_VOICE_STATE_IDLE = 0,
    SERVER_VOICE_STATE_PREPARING,
    SERVER_VOICE_STATE_STREAMING,
    SERVER_VOICE_STATE_FINISHING,
} server_voice_state_t;

typedef struct {
    bool initialized;
    server_voice_state_t state;
    server_comm_raw_stream_t *stream;
    TaskHandle_t response_task;
    server_voice_done_cb_t done_cb;
    void *done_ctx;
    server_voice_playback_start_cb_t playback_start_cb;
    void *playback_start_ctx;
    server_voice_error_cb_t error_cb;
    void *error_ctx;
    uint8_t *upload_buf;
    size_t upload_capacity;
    size_t upload_bytes;
    size_t response_bytes;
    int64_t upload_finished_ms;
    int64_t fetch_headers_begin_ms;
    int64_t fetch_headers_end_ms;
    int64_t first_response_byte_ms;
    volatile bool abort_requested;
} server_voice_context_t;

typedef struct {
    bool stream_open;
    bool has_leftover;
    uint8_t leftover;
    uint8_t combined_buf[SERVER_VOICE_READ_CHUNK_BYTES + 1U];
    int16_t pcm_decode_buf[(SERVER_VOICE_READ_CHUNK_BYTES + 1U) / sizeof(int16_t)];
    size_t response_bytes;
    uint64_t sample_count;
    uint64_t zero_sample_count;
    uint64_t sum_squares;
    int16_t first_samples[SERVER_VOICE_PCM_PREVIEW_SAMPLES];
    uint32_t first_sample_count;
    int32_t peak_abs;
} server_voice_playback_ctx_t;

static server_voice_context_t s_voice;

static const char *server_voice_state_name(server_voice_state_t state)
{
    switch (state) {
    case SERVER_VOICE_STATE_IDLE:
        return "IDLE";
    case SERVER_VOICE_STATE_PREPARING:
        return "PREPARING";
    case SERVER_VOICE_STATE_STREAMING:
        return "STREAMING";
    case SERVER_VOICE_STATE_FINISHING:
        return "FINISHING";
    default:
        return "UNKNOWN";
    }
}

static void server_voice_log_heap(const char *label)
{
#if ENABLE_VERBOSE_AUDIO_LOG
    ESP_LOGI(TAG,
             "%s state=%s free_heap=%u min_free_heap=%u largest_free_block=%u upload_bytes=%u response_bytes=%u",
             label != NULL ? label : "server voice",
             server_voice_state_name(s_voice.state),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned int)s_voice.upload_bytes,
             (unsigned int)s_voice.response_bytes);
#else
    (void)label;
#endif
}

static void server_voice_set_state(server_voice_state_t state)
{
    if (s_voice.state != state) {
        ESP_LOGI(TAG,
                 "state %s -> %s",
                 server_voice_state_name(s_voice.state),
                 server_voice_state_name(state));
    }
    s_voice.state = state;
    server_voice_log_heap("server voice state");
}

static int64_t server_voice_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void server_voice_cleanup_client(void)
{
    if (s_voice.stream != NULL) {
        server_comm_http_post_raw_stream_close(s_voice.stream);
        s_voice.stream = NULL;
    }
    if (s_voice.upload_buf != NULL) {
        heap_caps_free(s_voice.upload_buf);
        s_voice.upload_buf = NULL;
    }
    s_voice.upload_capacity = 0;
    s_voice.upload_bytes = 0;
}

static bool server_voice_abort_requested(void)
{
    return s_voice.abort_requested;
}

static esp_err_t server_voice_ensure_upload_capacity(size_t required)
{
    if (required > SERVER_VOICE_UPLOAD_MAX_BYTES) {
        ESP_LOGE(TAG,
                 "local voice upload too large required=%u max=%u",
                 (unsigned int)required,
                 (unsigned int)SERVER_VOICE_UPLOAD_MAX_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }
    if (required <= s_voice.upload_capacity) {
        return ESP_OK;
    }

    size_t new_capacity = s_voice.upload_capacity > 0 ?
                          s_voice.upload_capacity :
                          SERVER_VOICE_UPLOAD_INITIAL_BYTES;
    while (new_capacity < required && new_capacity < SERVER_VOICE_UPLOAD_MAX_BYTES) {
        new_capacity *= 2U;
    }
    if (new_capacity > SERVER_VOICE_UPLOAD_MAX_BYTES) {
        new_capacity = SERVER_VOICE_UPLOAD_MAX_BYTES;
    }

    uint8_t *new_buf =
        (uint8_t *)heap_caps_realloc(s_voice.upload_buf, new_capacity, MALLOC_CAP_8BIT);
    if (new_buf == NULL) {
        ESP_LOGE(TAG,
                 "local voice upload buffer alloc failed required=%u capacity=%u",
                 (unsigned int)required,
                 (unsigned int)new_capacity);
        return ESP_ERR_NO_MEM;
    }

    s_voice.upload_buf = new_buf;
    s_voice.upload_capacity = new_capacity;
    return ESP_OK;
}

static void server_voice_emit_done(void)
{
    if (s_voice.done_cb != NULL) {
        (void)s_voice.done_cb(s_voice.done_ctx);
    }
}

static void server_voice_emit_playback_start(void)
{
    if (s_voice.playback_start_cb != NULL) {
        (void)s_voice.playback_start_cb(s_voice.playback_start_ctx);
    }
}

static void server_voice_emit_error(int code, const char *message)
{
    if (s_voice.error_cb != NULL) {
        (void)s_voice.error_cb(code, message, s_voice.error_ctx);
    }
}

static int32_t server_voice_abs_i16(int16_t sample)
{
    int32_t value = sample;
    return value < 0 ? -value : value;
}

#if ENABLE_VERBOSE_AUDIO_LOG
static uint32_t server_voice_isqrt_u64(uint64_t value)
{
    uint64_t low = 0;
    uint64_t high = 65536;
    while (low + 1U < high) {
        uint64_t mid = (low + high) / 2U;
        if (mid * mid <= value) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return (uint32_t)low;
}
#endif

static void server_voice_pcm_stats_update(server_voice_playback_ctx_t *ctx,
                                          const int16_t *samples,
                                          size_t sample_count)
{
    if (ctx == NULL || samples == NULL || sample_count == 0) {
        return;
    }

    for (size_t i = 0; i < sample_count; i++) {
        int16_t sample = samples[i];
        if (ctx->first_sample_count < SERVER_VOICE_PCM_PREVIEW_SAMPLES) {
            ctx->first_samples[ctx->first_sample_count++] = sample;
        }
        int32_t abs_sample = server_voice_abs_i16(sample);
        if (abs_sample > ctx->peak_abs) {
            ctx->peak_abs = abs_sample;
        }
        if (sample == 0) {
            ctx->zero_sample_count++;
        }
        ctx->sum_squares += (uint64_t)abs_sample * (uint64_t)abs_sample;
    }
    ctx->sample_count += sample_count;
}

static void server_voice_pcm_stats_log(const server_voice_playback_ctx_t *ctx)
{
#if ENABLE_VERBOSE_AUDIO_LOG
    if (ctx == NULL) {
        return;
    }

    char first_samples[160] = {0};
    size_t offset = 0;
    for (uint32_t i = 0; i < ctx->first_sample_count; i++) {
        int written = snprintf(first_samples + offset,
                               sizeof(first_samples) - offset,
                               "%s%d",
                               i == 0 ? "" : ",",
                               (int)ctx->first_samples[i]);
        if (written < 0) {
            break;
        }
        if ((size_t)written >= sizeof(first_samples) - offset) {
            offset = sizeof(first_samples) - 1U;
            break;
        }
        offset += (size_t)written;
    }

    uint64_t mean_square = ctx->sample_count == 0 ? 0 : ctx->sum_squares / ctx->sample_count;
    uint32_t rms = server_voice_isqrt_u64(mean_square);
    ESP_LOGI(TAG,
             "server PCM stats total_bytes=%u samples=%llu first16=[%s] peak_abs=%ld rms=%lu zero_samples=%llu",
             (unsigned int)ctx->response_bytes,
             (unsigned long long)ctx->sample_count,
             first_samples,
             (long)ctx->peak_abs,
             (unsigned long)rms,
             (unsigned long long)ctx->zero_sample_count);
    if (ctx->sample_count > 0 && ctx->peak_abs < 256) {
        ESP_LOGW(TAG,
                 "server PCM peak_abs is very small; response may be silence or wrong byte order/format");
    }
#else
    (void)ctx;
#endif
}

static void server_voice_decode_s16le(const uint8_t *bytes,
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

static esp_err_t server_voice_play_response_chunk(const uint8_t *data,
                                                  size_t len,
                                                  void *user_ctx)
{
    server_voice_playback_ctx_t *ctx = (server_voice_playback_ctx_t *)user_ctx;
    if (ctx == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (server_voice_abort_requested()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (len > SERVER_VOICE_READ_CHUNK_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t bytes = len;
    const uint8_t *pcm_bytes = data;
    if (ctx->has_leftover) {
        ctx->combined_buf[0] = ctx->leftover;
        memcpy(ctx->combined_buf + 1, data, len);
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
        if (server_voice_abort_requested()) {
            return ESP_ERR_INVALID_STATE;
        }
        server_voice_emit_playback_start();
        esp_err_t ret = audio_player_stream_open();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "server PCM playback open failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ctx->stream_open = true;
    }

    size_t sample_count = bytes / sizeof(int16_t);
    server_voice_decode_s16le(pcm_bytes, ctx->pcm_decode_buf, sample_count);
    server_voice_pcm_stats_update(ctx, ctx->pcm_decode_buf, sample_count);

    esp_err_t ret = audio_player_write_pcm_chunk(ctx->pcm_decode_buf,
                                                 (uint32_t)sample_count,
                                                 SERVER_VOICE_SAMPLE_RATE_HZ);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "server PCM playback write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ctx->response_bytes += bytes;
    s_voice.response_bytes += bytes;
    if (s_voice.first_response_byte_ms == 0) {
        s_voice.first_response_byte_ms = server_voice_now_ms();
        ESP_LOGI(TAG,
                 "first response byte timestamp=%lld elapsed_since_upload_ms=%lld",
                 (long long)s_voice.first_response_byte_ms,
                 (long long)(s_voice.first_response_byte_ms - s_voice.upload_finished_ms));
    }
    return ESP_OK;
}

static void server_voice_response_task(void *arg)
{
    (void)arg;
    app_stack_monitor_log(TAG, "server_voice_rx", "entry");
    server_voice_log_heap("server voice response task start");
    esp_err_t ret = ESP_OK;
    server_comm_http_set_voice_request_active(true);

    server_comm_http_response_t response = {0};
    s_voice.fetch_headers_begin_ms = server_voice_now_ms();
    ESP_LOGI(TAG,
             "fetch headers begin timestamp=%lld timeout_ms=%u",
             (long long)s_voice.fetch_headers_begin_ms,
             (unsigned int)SERVER_VOICE_HTTP_FETCH_HEADERS_TIMEOUT_MS);
    ret = server_voice_abort_requested() ? ESP_ERR_INVALID_STATE :
                                           server_comm_http_fetch_headers(s_voice.stream, &response);
    s_voice.fetch_headers_end_ms = server_voice_now_ms();
    ESP_LOGI(TAG,
             "fetch headers end timestamp=%lld elapsed_ms=%lld ret=%s http status=%d",
             (long long)s_voice.fetch_headers_end_ms,
             (long long)(s_voice.fetch_headers_end_ms - s_voice.fetch_headers_begin_ms),
             esp_err_to_name(ret),
             response.status_code);
    app_stack_monitor_log(TAG, "server_voice_rx", "after_fetch_headers");

    int status = response.status_code;
    if (ret == ESP_OK) {
        ESP_LOGD(TAG,
                 "server voice response headers: status=%d content_length=%lld chunked=%d content_type=%s",
                 status,
                 (long long)response.content_length,
                 response.chunked ? 1 : 0,
                 response.content_type);
        if (status == 204) {
            ret = ESP_OK;
        } else if (!server_comm_http_status_is_success(status)) {
            ret = ESP_FAIL;
        } else {
            server_voice_playback_ctx_t *playback_ctx =
                (server_voice_playback_ctx_t *)heap_caps_calloc(1,
                                                                sizeof(*playback_ctx),
                                                                MALLOC_CAP_8BIT);
            if (playback_ctx == NULL) {
                ret = ESP_ERR_NO_MEM;
            }
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "alloc server voice playback context failed: %s", esp_err_to_name(ret));
            } else {
                ret = server_voice_abort_requested() ? ESP_ERR_INVALID_STATE :
                                                       server_comm_http_read_response(s_voice.stream,
                                                                                     server_voice_play_response_chunk,
                                                                                     playback_ctx,
                                                                                     &response);
                app_stack_monitor_log(TAG, "server_voice_rx", "after_read_response");
                if (ret == ESP_OK && playback_ctx->has_leftover) {
                    ESP_LOGW(TAG, "drop trailing odd PCM byte from server response");
                }
                server_voice_pcm_stats_log(playback_ctx);
                if (playback_ctx->stream_open) {
                    esp_err_t finish_ret = server_voice_abort_requested() ?
                                               audio_player_stream_abort() :
                                               audio_player_stream_finish();
                    if (ret == ESP_OK && finish_ret != ESP_OK) {
                        ret = finish_ret;
                    }
                }
                heap_caps_free(playback_ctx);
            }
        }
    }

    size_t upload_bytes = s_voice.upload_bytes;
    server_voice_cleanup_client();
    s_voice.response_task = NULL;
    server_voice_set_state(SERVER_VOICE_STATE_IDLE);
    server_voice_log_heap("server voice response task done");
    bool aborted = server_voice_abort_requested();
    s_voice.abort_requested = false;

    if (ret == ESP_OK && !aborted) {
        ESP_LOGI(TAG,
                 "server voice turn done status=%d upload_bytes=%u response_bytes=%u",
                 status,
                 (unsigned int)upload_bytes,
                 (unsigned int)s_voice.response_bytes);
        server_voice_emit_done();
    } else if (aborted) {
        ESP_LOGI(TAG,
                 "server voice turn aborted status=%d ret=%s upload_bytes=%u response_bytes=%u",
                 status,
                 esp_err_to_name(ret),
                 (unsigned int)upload_bytes,
                 (unsigned int)s_voice.response_bytes);
    } else {
        ESP_LOGE(TAG,
                 "server voice turn failed status=%d ret=%s aborted=%d upload_bytes=%u response_bytes=%u",
                 status,
                 esp_err_to_name(ret),
                 aborted ? 1 : 0,
                 (unsigned int)upload_bytes,
                 (unsigned int)s_voice.response_bytes);
        server_voice_emit_error((int)ret, "server voice turn failed");
    }

    app_stack_monitor_log(TAG, "server_voice_rx", ret == ESP_OK ? "exit_ok" : "exit_error");
    server_comm_http_set_voice_request_active(false);
    vTaskDelete(NULL);
}

esp_err_t server_voice_client_init(const server_voice_client_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_voice.initialized) {
        return ESP_OK;
    }

    memset(&s_voice, 0, sizeof(s_voice));
    s_voice.done_cb = config->done_cb;
    s_voice.done_ctx = config->done_ctx;
    s_voice.playback_start_cb = config->playback_start_cb;
    s_voice.playback_start_ctx = config->playback_start_ctx;
    s_voice.error_cb = config->error_cb;
    s_voice.error_ctx = config->error_ctx;
    s_voice.initialized = true;
    s_voice.state = SERVER_VOICE_STATE_IDLE;
    server_voice_log_heap("server voice client initialized");
    return ESP_OK;
}

esp_err_t server_voice_client_prepare_async(void)
{
    if (!s_voice.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_voice.state != SERVER_VOICE_STATE_IDLE) {
        return ESP_OK;
    }

    server_voice_set_state(SERVER_VOICE_STATE_PREPARING);
    ESP_LOGI(TAG,
             "local voice prepare done endpoint=%s gateway_url=%s device_id=%s",
             SERVER_VOICE_TURN_ENDPOINT,
             server_comm_get_base_url(),
             server_comm_get_device_id());
    server_voice_set_state(SERVER_VOICE_STATE_IDLE);
    return ESP_OK;
}

esp_err_t server_voice_client_start_turn(void)
{
    if (!s_voice.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_voice.state != SERVER_VOICE_STATE_IDLE || s_voice.stream != NULL ||
        s_voice.upload_buf != NULL) {
        ESP_LOGW(TAG, "server voice start rejected: state=%s", server_voice_state_name(s_voice.state));
        return ESP_ERR_INVALID_STATE;
    }

    s_voice.upload_bytes = 0;
    s_voice.response_bytes = 0;
    s_voice.upload_finished_ms = 0;
    s_voice.fetch_headers_begin_ms = 0;
    s_voice.fetch_headers_end_ms = 0;
    s_voice.first_response_byte_ms = 0;
    s_voice.abort_requested = false;
    esp_err_t ret = server_voice_ensure_upload_capacity(SERVER_VOICE_UPLOAD_INITIAL_BYTES);
    if (ret != ESP_OK) {
        server_voice_cleanup_client();
        return ret;
    }

    server_voice_set_state(SERVER_VOICE_STATE_STREAMING);
    ESP_LOGI(TAG,
             "local gateway voice turn begin gateway_id=%s device_id=%s",
             server_comm_get_gateway_id(),
             server_comm_get_device_id());
    return ESP_OK;
}

esp_err_t server_voice_client_append_pcm(const int16_t *pcm, size_t samples)
{
    if (pcm == NULL || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (server_voice_abort_requested()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_voice.state != SERVER_VOICE_STATE_STREAMING || s_voice.stream == NULL) {
        if (s_voice.state != SERVER_VOICE_STATE_STREAMING || s_voice.upload_buf == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    size_t bytes = samples * sizeof(int16_t);
    esp_err_t ret = server_voice_ensure_upload_capacity(s_voice.upload_bytes + bytes);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "local voice PCM buffer append failed: %s", esp_err_to_name(ret));
        server_voice_cleanup_client();
        server_voice_set_state(SERVER_VOICE_STATE_IDLE);
        return ret;
    }
    memcpy(s_voice.upload_buf + s_voice.upload_bytes, pcm, bytes);
    s_voice.upload_bytes += bytes;
    return ESP_OK;
}

esp_err_t server_voice_client_finish_turn(void)
{
    if (server_voice_abort_requested()) {
        server_voice_cleanup_client();
        server_voice_set_state(SERVER_VOICE_STATE_IDLE);
        s_voice.abort_requested = false;
        return ESP_ERR_INVALID_STATE;
    }
    if (s_voice.state != SERVER_VOICE_STATE_STREAMING || s_voice.upload_buf == NULL) {
        return ESP_OK;
    }
    if (s_voice.upload_bytes == 0) {
        server_voice_cleanup_client();
        server_voice_set_state(SERVER_VOICE_STATE_IDLE);
        return ESP_ERR_INVALID_SIZE;
    }

    device_protocol_metadata_t metadata = {0};
    device_protocol_prepare_metadata(&metadata, "voice.turn");
    server_comm_header_t headers[DEVICE_PROTOCOL_MAX_HEADERS + 1U];
    size_t header_count = 0;
    for (size_t i = 0; i < metadata.header_count && header_count < DEVICE_PROTOCOL_MAX_HEADERS; i++) {
        headers[header_count++] = metadata.headers[i];
    }
    headers[header_count++] = (server_comm_header_t) {
        .key = "X-Audio-Format",
        .value = SERVER_VOICE_AUDIO_FORMAT,
    };
    const server_comm_raw_stream_config_t stream_config = {
        .endpoint = SERVER_VOICE_TURN_ENDPOINT,
        .content_type = SERVER_VOICE_REQUEST_CONTENT_TYPE,
        .headers = headers,
        .header_count = header_count,
        .timeout_ms = SERVER_VOICE_HTTP_CONNECT_TIMEOUT_MS,
        .fetch_headers_timeout_ms = SERVER_VOICE_HTTP_FETCH_HEADERS_TIMEOUT_MS,
        .read_timeout_ms = SERVER_VOICE_HTTP_READ_TIMEOUT_MS,
        .total_timeout_ms = SERVER_VOICE_HTTP_TURN_TIMEOUT_MS,
        .buffer_size = SERVER_VOICE_READ_CHUNK_BYTES,
        .tx_buffer_size = 512,
    };

    ESP_LOGI(TAG,
             "voice http timeout config connect_ms=%u fetch_headers_ms=%u read_ms=%u turn_ms=%u",
             (unsigned int)SERVER_VOICE_HTTP_CONNECT_TIMEOUT_MS,
             (unsigned int)SERVER_VOICE_HTTP_FETCH_HEADERS_TIMEOUT_MS,
             (unsigned int)SERVER_VOICE_HTTP_READ_TIMEOUT_MS,
             (unsigned int)SERVER_VOICE_HTTP_TURN_TIMEOUT_MS);
    server_voice_log_heap("local voice fixed POST before");
    /*
     * 语音独占模式下只有 server_voice_client 允许继续走本地 HTTP。这里临时允许
     * local_gateway_comm 建立 /local/v1/voice/turn，避免普通 heartbeat/poll 的 gate
     * 误拦截本轮语音长连接。
     */
    server_comm_http_set_voice_request_active(true);
    esp_err_t ret = server_comm_http_post_raw_fixed_stream_begin(&stream_config,
                                                                 s_voice.upload_buf,
                                                                 s_voice.upload_bytes,
                                                                 &s_voice.stream);
    server_comm_http_set_voice_request_active(false);
    server_voice_log_heap("local voice fixed POST after");
    if (ret != ESP_OK) {
        server_voice_cleanup_client();
        server_voice_set_state(SERVER_VOICE_STATE_IDLE);
        return ret;
    }
    s_voice.upload_finished_ms = server_voice_now_ms();
    ESP_LOGI(TAG,
             "upload finished timestamp=%lld bytes=%u",
             (long long)s_voice.upload_finished_ms,
             (unsigned int)s_voice.upload_bytes);
    heap_caps_free(s_voice.upload_buf);
    s_voice.upload_buf = NULL;
    s_voice.upload_capacity = 0;

    server_voice_set_state(SERVER_VOICE_STATE_FINISHING);
    BaseType_t created = xTaskCreate(server_voice_response_task,
                                     "server_voice_rx",
                                     SERVER_VOICE_RESPONSE_TASK_STACK,
                                     NULL,
                                     SERVER_VOICE_RESPONSE_TASK_PRIORITY,
                                     &s_voice.response_task);
    if (created != pdPASS) {
        server_voice_cleanup_client();
        server_voice_set_state(SERVER_VOICE_STATE_IDLE);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t server_voice_client_cancel_turn(void)
{
    s_voice.abort_requested = true;
    if (s_voice.response_task != NULL) {
        (void)audio_player_stream_abort();
        return ESP_OK;
    }
    server_voice_cleanup_client();
    server_voice_set_state(SERVER_VOICE_STATE_IDLE);
    s_voice.abort_requested = false;
    (void)audio_player_stream_abort();
    return ESP_OK;
}

esp_err_t server_voice_client_request_abort(const char *reason)
{
    if (!s_voice.initialized) {
        return ESP_OK;
    }

    s_voice.abort_requested = true;
    ESP_LOGI(TAG, "server voice abort requested reason=%s", reason != NULL ? reason : "<none>");
    if (s_voice.stream != NULL) {
        server_comm_http_post_raw_stream_request_abort(s_voice.stream);
    }
    (void)audio_player_stream_abort();
    if (s_voice.response_task == NULL) {
        server_voice_cleanup_client();
        server_voice_set_state(SERVER_VOICE_STATE_IDLE);
        s_voice.abort_requested = false;
    }
    return ESP_OK;
}

bool server_voice_client_is_idle(void)
{
    return !s_voice.initialized || s_voice.state == SERVER_VOICE_STATE_IDLE;
}

bool server_voice_client_is_active(void)
{
    return s_voice.initialized && s_voice.state != SERVER_VOICE_STATE_IDLE;
}

#include "audio_wake_gateway.h"

#include <stdbool.h>
#include <string.h>

#include "esp111_protocol_common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "model_path.h"

static const char *TAG = "audio_wake_gateway";

#define AUDIO_WAKE_TASK_STACK 6144U
#define AUDIO_WAKE_TASK_PRIORITY 6U
#define AUDIO_WAKE_PCM_BUFFER_SAMPLES 4096U
#define AUDIO_WAKE_RX_TIMEOUT_MS 500U
#define AUDIO_WAKE_STREAM_TIMEOUT_MS 5000U
#define AUDIO_WAKE_MAX_RECOVERABLE_GAP_FRAMES 5U
#define AUDIO_WAKE_STREAM_MISMATCH_LOG_INTERVAL_MS 1000U
#define AUDIO_WAKE_SOURCE_COUNT (ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52 + 1U)
#define AUDIO_WAKE_RETIRED_STREAMS_PER_SOURCE 4U

typedef enum {
    AUDIO_WAKE_PCM_PHASE_UNKNOWN = 0,
    AUDIO_WAKE_PCM_PHASE_PRE_ROLL,
    AUDIO_WAKE_PCM_PHASE_LIVE,
} audio_wake_pcm_phase_t;

static srmodel_list_t *s_models;
static const esp_wn_iface_t *s_wakenet;
static model_iface_data_t *s_model;
static char *s_model_name;
static bool s_model_ready;
static SemaphoreHandle_t s_model_lock;
static int16_t *s_pcm_buffer;
static size_t s_buffered_samples;
static int s_chunk_samples;
static uint8_t s_active_source;
static uint32_t s_active_stream;
static uint32_t s_session_generation;
static uint32_t s_expected_sequence;
static uint32_t s_sequence_gap;
static uint32_t s_rx_pcm_frames;
static uint32_t s_rx_pcm_bytes;
static uint32_t s_feed_frames;
static uint32_t s_feed_samples;
static uint32_t s_inference_count;
static uint32_t s_model_create_count;
static uint32_t s_model_destroy_count;
static uint32_t s_gap_count;
static uint32_t s_max_gap;
static uint32_t s_silence_recovery_frames;
static uint32_t s_silence_recovery_samples;
static uint32_t s_stream_mismatch_drops;
static uint32_t s_stream_mismatch_suppressed;
static int s_last_result;
static bool s_detected;
static bool s_first_pcm_received;
static bool s_first_feed_logged;
static uint16_t s_last_transport_frame_samples;
static audio_wake_pcm_phase_t s_pcm_phase;
static int64_t s_last_rx_ms;
static int64_t s_last_stats_ms;
static int64_t s_last_stream_mismatch_log_ms;
static uint32_t s_retired_streams[AUDIO_WAKE_SOURCE_COUNT][AUDIO_WAKE_RETIRED_STREAMS_PER_SOURCE];
static uint8_t s_retired_stream_next[AUDIO_WAKE_SOURCE_COUNT];
static audio_wake_gateway_event_handler_t s_event_handler;
static void *s_event_context;

static void audio_wake_gateway_cleanup(void)
{
    ESP_LOGI(TAG, "WAKENET_MODEL_DESTROY_BEGIN model_ready=%u", s_model_ready ? 1U : 0U);
    model_iface_data_t *model = s_model;
    s_model = NULL;
    s_model_ready = false;
    if (model != NULL && s_wakenet != NULL && s_wakenet->destroy != NULL) {
        s_wakenet->destroy(model);
        ++s_model_destroy_count;
    }
    if (s_models != NULL) {
        esp_srmodel_deinit(s_models);
    }
    s_models = NULL;
    s_wakenet = NULL;
    s_model_name = NULL;
    if (s_pcm_buffer != NULL) {
        heap_caps_free(s_pcm_buffer);
        s_pcm_buffer = NULL;
    }
    if (s_model_lock != NULL) {
        vSemaphoreDelete(s_model_lock);
        s_model_lock = NULL;
    }
    s_chunk_samples = 0;
    ESP_LOGI(TAG, "WAKENET_MODEL_DESTROY_END result=ok");
}

static bool wakenet_ready(void)
{
    return s_model_ready && s_wakenet != NULL && s_model != NULL &&
           s_wakenet->detect != NULL && s_chunk_samples > 0 &&
           s_chunk_samples <= (int)AUDIO_WAKE_PCM_BUFFER_SAMPLES &&
           s_pcm_buffer != NULL;
}

static esp_err_t create_wakenet_instance(void)
{
    if (s_wakenet == NULL || s_model_name == NULL || s_wakenet->create == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "WAKENET_MODEL_CREATE_BEGIN model_name=%s", s_model_name);
    s_model = s_wakenet->create(s_model_name, DET_MODE_95);
    if (s_model == NULL) {
        s_model_ready = false;
        ESP_LOGE(TAG, "WAKENET_MODEL_CREATE_FAILED reason=create esp_err=%s",
                 esp_err_to_name(ESP_ERR_NO_MEM));
        return ESP_ERR_NO_MEM;
    }
    s_chunk_samples = s_wakenet->get_samp_chunksize(s_model);
    if (s_chunk_samples <= 0 ||
        s_chunk_samples > (int)AUDIO_WAKE_PCM_BUFFER_SAMPLES ||
        s_wakenet->get_samp_rate(s_model) != 16000 ||
        s_wakenet->get_channel_num(s_model) != 1) {
        ESP_LOGE(TAG, "WAKENET_MODEL_CREATE_FAILED reason=format esp_err=%s",
                 esp_err_to_name(ESP_ERR_INVALID_SIZE));
        s_wakenet->destroy(s_model);
        ++s_model_destroy_count;
        s_model = NULL;
        s_model_ready = false;
        return ESP_ERR_INVALID_SIZE;
    }
    s_model_ready = true;
    ++s_model_create_count;
    ESP_LOGI(TAG,
             "WAKENET_MODEL_CREATE_SUCCESS model_name=%s feed_samples=%d model_ready=1",
             s_model_name,
             s_chunk_samples);
    return ESP_OK;
}

static esp_err_t recreate_wakenet_instance(const char *reason)
{
    s_model_ready = false;
    model_iface_data_t *old_model = s_model;
    s_model = NULL;
    if (old_model != NULL && s_wakenet != NULL && s_wakenet->destroy != NULL) {
        ESP_LOGI(TAG, "WAKENET_MODEL_DESTROY_BEGIN model_ready=1 reason=%s",
                 reason != NULL ? reason : "recreate");
        s_wakenet->destroy(old_model);
        ++s_model_destroy_count;
        ESP_LOGI(TAG, "WAKENET_MODEL_DESTROY_END result=ok reason=%s",
                 reason != NULL ? reason : "recreate");
    }
    return create_wakenet_instance();
}

static const char *source_name(uint8_t source_id)
{
    switch (source_id) {
    case ESP111_PROTOCOL_LOCAL_DEVICE_ID_C51: return "C51";
    case ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52: return "C52";
    default: return "UNKNOWN";
    }
}

static const char *pcm_phase_name(audio_wake_pcm_phase_t phase)
{
    switch (phase) {
    case AUDIO_WAKE_PCM_PHASE_PRE_ROLL:
        return "pre_roll";
    case AUDIO_WAKE_PCM_PHASE_LIVE:
        return "live";
    default:
        return "unknown";
    }
}

static bool source_is_tracked(uint8_t source_id)
{
    return source_id > 0U && source_id < AUDIO_WAKE_SOURCE_COUNT;
}

static void retire_stream(uint8_t source_id, uint32_t stream_id)
{
    if (stream_id != 0U && source_is_tracked(source_id)) {
        const uint8_t slot = s_retired_stream_next[source_id];
        s_retired_streams[source_id][slot] = stream_id;
        s_retired_stream_next[source_id] =
            (uint8_t)((slot + 1U) % AUDIO_WAKE_RETIRED_STREAMS_PER_SOURCE);
    }
}

static bool stream_is_retired(uint8_t source_id, uint32_t stream_id)
{
    if (stream_id == 0U || !source_is_tracked(source_id)) {
        return false;
    }
    for (size_t index = 0U; index < AUDIO_WAKE_RETIRED_STREAMS_PER_SOURCE; ++index) {
        if (s_retired_streams[source_id][index] == stream_id) {
            return true;
        }
    }
    return false;
}

static void log_stream_mismatch_drop(const esp111_protocol_audio_frame_t *frame,
                                     const char *state)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    ++s_stream_mismatch_drops;
    if (s_last_stream_mismatch_log_ms != 0 &&
        now_ms - s_last_stream_mismatch_log_ms < AUDIO_WAKE_STREAM_MISMATCH_LOG_INTERVAL_MS) {
        ++s_stream_mismatch_suppressed;
        return;
    }

    ESP_LOGI(TAG,
             "S3_PCM_RX_DROP_STATS reason=stream_mismatch state=%s source_device=%s stream_id=%lu active_source=%s active_stream=%lu drops=%lu suppressed=%lu",
             state != NULL ? state : "unknown",
             source_name(frame->source_id),
             (unsigned long)frame->stream_id,
             source_name(s_active_source),
             (unsigned long)s_active_stream,
             (unsigned long)s_stream_mismatch_drops,
             (unsigned long)s_stream_mismatch_suppressed);
    s_last_stream_mismatch_log_ms = now_ms;
    s_stream_mismatch_suppressed = 0U;
}

static void wake_stats_log(bool force)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (!force && s_last_stats_ms != 0 && now_ms - s_last_stats_ms < 1000) {
        return;
    }
    s_last_stats_ms = now_ms;
    ESP_LOGI(TAG,
             "WAKENET_FEED_STATS feed_count=%lu feed_samples=%lu wakenet_feed_samples=%d buffered_samples=%u inference_count=%lu last_result=%d stream_id=%lu",
             (unsigned long)s_feed_frames,
             (unsigned long)s_feed_samples,
             s_chunk_samples,
             (unsigned int)s_buffered_samples,
             (unsigned long)s_inference_count,
             s_last_result,
             (unsigned long)s_active_stream);
    ESP_LOGI(TAG,
             "PCM_STREAM_STATS role=rx source_device=%s stream_id=%lu tx_frames=0 rx_frames=%lu lost_frames=%lu gap_count=%lu max_gap=%lu silence_recovery_frames=%lu silence_recovery_samples=%lu bytes=%lu sample_rate=16000 transport_frame_samples=%u wakenet_feed_samples=%d queue_depth=0 oldest_frame_age=%lld",
             source_name(s_active_source),
             (unsigned long)s_active_stream,
             (unsigned long)s_rx_pcm_frames,
             (unsigned long)s_sequence_gap,
             (unsigned long)s_gap_count,
             (unsigned long)s_max_gap,
             (unsigned long)s_silence_recovery_frames,
             (unsigned long)s_silence_recovery_samples,
             (unsigned long)s_rx_pcm_bytes,
             (unsigned int)s_last_transport_frame_samples,
             s_chunk_samples,
             (long long)(s_last_rx_ms != 0 ? now_ms - s_last_rx_ms : 0));
    ESP_LOGI(TAG,
             "WAKENET_RUNTIME_STATS model_create_count=%lu model_destroy_count=%lu gap_count=%lu stream_mismatch_drops=%lu feed_count=%lu inference_count=%lu",
             (unsigned long)s_model_create_count,
             (unsigned long)s_model_destroy_count,
             (unsigned long)s_gap_count,
             (unsigned long)s_stream_mismatch_drops,
             (unsigned long)s_feed_frames,
             (unsigned long)s_inference_count);
}

static bool feed_available_chunks(void)
{
    if (!wakenet_ready()) {
        ESP_LOGW(TAG, "WAKENET_FEED_SKIPPED reason=model_not_ready");
        return false;
    }
    while (s_buffered_samples >= (size_t)s_chunk_samples) {
        if (!s_first_feed_logged) {
            s_first_feed_logged = true;
            ESP_LOGI(TAG,
                     "WAKENET_FIRST_FEED stream_id=%lu wakenet_feed_samples=%d pcm_format=pcm16le sample_rate=16000 phase=%s",
                     (unsigned long)s_active_stream,
                     s_chunk_samples,
                     pcm_phase_name(s_pcm_phase));
        }
        const int result = s_wakenet->detect(s_model, s_pcm_buffer);
        s_last_result = result;
        s_feed_frames++;
        s_feed_samples += (uint32_t)s_chunk_samples;
        s_inference_count++;
        if (result == WAKENET_DETECTED && !s_detected) {
            s_detected = true;
            const int64_t detected_at_ms = esp_timer_get_time() / 1000;
            ESP_LOGI(TAG,
                     "WAKE_DETECTED source_device=%s stream_id=%lu result=%d feed_frames=%lu inference_count=%lu phase=%s reason=wakenet_detected timestamp=%lld",
                     source_name(s_active_source),
                     (unsigned long)s_active_stream,
                     result,
                     (unsigned long)s_feed_frames,
                     (unsigned long)s_inference_count,
                     pcm_phase_name(s_pcm_phase),
                     (long long)detected_at_ms);
            ESP_LOGI(TAG,
                     "WAKENET_DETECTED source_device=%s stream_id=%lu confidence=1 timestamp=%lld",
                     source_name(s_active_source),
                     (unsigned long)s_active_stream,
                     (long long)detected_at_ms);
            ESP_LOGI(TAG,
                     "WAKE_EVENT source_device=%s stream_id=%lu wake_word_id=nihaoxiaozhi confidence=1 timestamp=%lld voice_state=WAKE_DETECTED",
                     source_name(s_active_source),
                     (unsigned long)s_active_stream,
                     (long long)detected_at_ms);
            ESP_LOGI(TAG,
                     "WAKE_CONFIRMED source_device=%s stream_id=%lu wake_word_id=nihaoxiaozhi",
                     source_name(s_active_source),
                     (unsigned long)s_active_stream);
            if (s_event_handler == NULL) {
                ESP_LOGW(TAG,
                         "WAKE_TRIGGER_FORWARD result=failed reason=event_handler_not_ready source_device=%s stream_id=%lu",
                         source_name(s_active_source),
                         (unsigned long)s_active_stream);
                ESP_LOGW(TAG,
                         "WAKE_EVENT_DISPATCHED result=not_ready source_device=%s stream_id=%lu",
                         source_name(s_active_source),
                         (unsigned long)s_active_stream);
            } else {
                const audio_wake_event_t event = {
                    .source_id = s_active_source,
                    .wake_stream_id = s_active_stream,
                    .detected_at_ms = detected_at_ms,
                };
                const esp_err_t dispatch_ret = s_event_handler(&event, s_event_context);
                ESP_LOG_LEVEL_LOCAL(dispatch_ret == ESP_OK ? ESP_LOG_INFO : ESP_LOG_WARN,
                                    TAG,
                                    "WAKE_TRIGGER_FORWARD result=%s reason=%s source_device=%s stream_id=%lu",
                                    dispatch_ret == ESP_OK ? "ok" : "failed",
                                    dispatch_ret == ESP_OK ? "event_handler_accepted" : esp_err_to_name(dispatch_ret),
                                    source_name(s_active_source),
                                    (unsigned long)s_active_stream);
                ESP_LOG_LEVEL_LOCAL(dispatch_ret == ESP_OK ? ESP_LOG_INFO : ESP_LOG_WARN,
                                    TAG,
                                    "WAKE_EVENT_DISPATCHED result=%s source_device=%s stream_id=%lu",
                                    esp_err_to_name(dispatch_ret),
                                    source_name(s_active_source),
                                    (unsigned long)s_active_stream);
            }
        }
        s_buffered_samples -= (size_t)s_chunk_samples;
        if (s_buffered_samples > 0U) {
            memmove(s_pcm_buffer,
                    s_pcm_buffer + s_chunk_samples,
                    s_buffered_samples * sizeof(*s_pcm_buffer));
        }
    }
    wake_stats_log(false);
    return true;
}

static bool insert_silence_frames(uint32_t frame_count, uint16_t frame_samples)
{
    size_t remaining_samples = (size_t)frame_count * frame_samples;
    if (frame_count == 0U || frame_samples == 0U ||
        remaining_samples / frame_samples != frame_count) {
        return false;
    }

    while (remaining_samples > 0U) {
        if (s_buffered_samples > AUDIO_WAKE_PCM_BUFFER_SAMPLES) {
            ESP_LOGE(TAG,
                     "S3_PCM_RX_GAP_RECOVERY_FAILED reason=buffer_state_invalid buffered_samples=%u capacity=%u",
                     (unsigned int)s_buffered_samples,
                     (unsigned int)AUDIO_WAKE_PCM_BUFFER_SAMPLES);
            s_buffered_samples = 0U;
        }
        size_t available = AUDIO_WAKE_PCM_BUFFER_SAMPLES - s_buffered_samples;
        if (available == 0U) {
            if (!feed_available_chunks()) {
                return false;
            }
            available = AUDIO_WAKE_PCM_BUFFER_SAMPLES - s_buffered_samples;
            if (available == 0U) {
                return false;
            }
        }
        const size_t copy_count = remaining_samples < available ? remaining_samples : available;
        memset(s_pcm_buffer + s_buffered_samples, 0, copy_count * sizeof(*s_pcm_buffer));
        s_buffered_samples += copy_count;
        remaining_samples -= copy_count;
        if (!feed_available_chunks()) {
            return false;
        }
    }
    return true;
}

static bool reset_session(uint8_t source_id, uint32_t stream_id, const char *reason)
{
    const size_t buffered_before = s_buffered_samples;
    if (!wakenet_ready()) {
        ESP_LOGW(TAG,
                 "WAKENET_SESSION_RESET_SKIPPED stream_id=%lu reason=model_not_ready",
                 (unsigned long)stream_id);
        return false;
    }
    ++s_session_generation;
    if (s_session_generation == 0U) {
        s_session_generation = 1U;
    }
    ESP_LOGI(TAG,
             "WAKENET_SESSION_RESET_BEGIN stream_id=%lu session_generation=%lu model_ready=%u buffered_samples=%u reason=%s",
             (unsigned long)stream_id,
             (unsigned long)s_session_generation,
             s_model_ready ? 1U : 0U,
             (unsigned int)buffered_before,
             reason != NULL ? reason : "new_stream");
    /* Keep the boot-created model alive across stream sessions. The shipped
     * ESP-SR clean() path is unsafe, so reset only application state. */
    s_active_source = source_id;
    s_active_stream = stream_id;
    s_expected_sequence = 0U;
    s_sequence_gap = 0U;
    s_gap_count = 0U;
    s_max_gap = 0U;
    s_silence_recovery_frames = 0U;
    s_silence_recovery_samples = 0U;
    s_rx_pcm_frames = 0U;
    s_rx_pcm_bytes = 0U;
    s_feed_frames = 0U;
    s_feed_samples = 0U;
    s_inference_count = 0U;
    s_last_result = 0;
    s_buffered_samples = 0U;
    s_detected = false;
    s_first_pcm_received = false;
    s_first_feed_logged = false;
    s_last_transport_frame_samples = 0U;
    s_pcm_phase = AUDIO_WAKE_PCM_PHASE_UNKNOWN;
    s_last_stats_ms = 0;
    s_last_rx_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG,
             "WAKENET_SESSION_RESET_END stream_id=%lu session_generation=%lu result=ok",
             (unsigned long)stream_id,
             (unsigned long)s_session_generation);
    return true;
}

static void close_session(const char *reason, bool drain_buffer)
{
    if (s_active_stream == 0U) {
        return;
    }
    if (drain_buffer) {
        (void)feed_available_chunks();
    }
    wake_stats_log(true);
    if (!s_detected) {
        ESP_LOGI(TAG,
                 "WAKE_NOT_DETECTED source_device=%s stream_id=%lu reason=%s feed_frames=%lu inference_count=%lu sequence_gap=%lu phase=%s",
                 source_name(s_active_source),
                 (unsigned long)s_active_stream,
                 reason != NULL ? reason : "session_closed",
                 (unsigned long)s_feed_frames,
                 (unsigned long)s_inference_count,
                 (unsigned long)s_sequence_gap,
                 pcm_phase_name(s_pcm_phase));
    }
    if (s_buffered_samples > 0U) {
        ESP_LOGI(TAG,
                 "WAKENET_SESSION_TAIL_DROPPED stream_id=%lu buffered_samples=%u reason=insufficient_for_frame",
                 (unsigned long)s_active_stream,
                 (unsigned int)s_buffered_samples);
    }
    ESP_LOGI(TAG,
             "S3_PCM_STREAM_CLOSE source_device=%s stream_id=%lu generation=not_on_wire reason=%s detected=%u",
             source_name(s_active_source),
             (unsigned long)s_active_stream,
             reason != NULL ? reason : "unknown",
             s_detected ? 1U : 0U);
    ESP_LOGI(TAG,
             "WAKE_STREAM_CLOSE source_device=%s stream_id=%lu reason=%s detected=%u",
             source_name(s_active_source),
             (unsigned long)s_active_stream,
             reason != NULL ? reason : "unknown",
             s_detected ? 1U : 0U);
    ESP_LOGI(TAG,
             "WAKENET_SESSION_END source_device=%s stream_id=%lu reason=%s",
             source_name(s_active_source),
             (unsigned long)s_active_stream,
             reason != NULL ? reason : "unknown");
    retire_stream(s_active_source, s_active_stream);
    s_active_source = 0U;
    s_active_stream = 0U;
    s_buffered_samples = 0U;
}

static int32_t sequence_delta(uint32_t sequence, uint32_t expected_sequence)
{
    return (int32_t)(sequence - expected_sequence);
}

static void process_packet(const uint8_t *packet, size_t packet_length)
{
    esp111_protocol_audio_frame_t frame = {0};
    const uint8_t *payload = NULL;
    if (!esp111_protocol_audio_decode(packet, packet_length, &frame, &payload) ||
        frame.source_id == 0U || frame.stream_id == 0U ||
        frame.sample_rate != 16000U || frame.bits_per_sample != 16U ||
        frame.channels != 1U) {
        ESP_LOGW(TAG, "S3_PCM_RX_DROP reason=invalid_frame bytes=%u",
                 (unsigned int)packet_length);
        return;
    }
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_active_stream != 0U && now_ms - s_last_rx_ms > AUDIO_WAKE_STREAM_TIMEOUT_MS) {
        close_session("timeout", true);
    }
    if (stream_is_retired(frame.source_id, frame.stream_id)) {
        log_stream_mismatch_drop(&frame, "retired_stream");
        return;
    }
    if (frame.type == ESP111_PROTOCOL_AUDIO_STREAM_VOICE_START) {
        if (s_active_stream != 0U && s_active_source == frame.source_id &&
            s_active_stream == frame.stream_id) {
            /* UDP can duplicate or reorder the opening datagram.  The active
             * session already owns the expected sequence, so resetting it
             * would turn the next normal PCM packet into a false gap. */
            s_last_rx_ms = now_ms;
            ESP_LOGD(TAG,
                     "S3_PCM_STREAM_OPEN duplicate_ignored source_device=%s stream_id=%lu sequence=%lu expected=%lu",
                     source_name(frame.source_id),
                     (unsigned long)frame.stream_id,
                     (unsigned long)frame.sequence,
                     (unsigned long)s_expected_sequence);
            return;
        }
        if (s_active_stream != 0U) {
            ESP_LOGW(TAG,
                     "S3_PCM_STREAM_CLOSE stale_replaced old_source=%s old_stream=%lu new_source=%s new_stream=%lu",
                     source_name(s_active_source),
                     (unsigned long)s_active_stream,
                     source_name(frame.source_id),
                     (unsigned long)frame.stream_id);
            close_session("superseded_by_voice_start", true);
        }
        if (!reset_session(frame.source_id, frame.stream_id, "new_stream")) {
            ESP_LOGW(TAG,
                     "S3_PCM_RX_DROP reason=model_not_ready source_device=%s stream_id=%lu",
                     source_name(frame.source_id),
                     (unsigned long)frame.stream_id);
            return;
        }
        s_expected_sequence = frame.sequence + 1U;
        ESP_LOGI(TAG,
                 "S3_VOICE_RX_READY source_device=%s stream_id=%lu generation=not_on_wire",
                 source_name(frame.source_id),
                 (unsigned long)frame.stream_id);
        ESP_LOGI(TAG,
                 "S3_PCM_STREAM_OPEN source_device=%s stream_id=%lu generation=not_on_wire",
                 source_name(frame.source_id),
                 (unsigned long)frame.stream_id);
        ESP_LOGI(TAG,
                 "WAKENET_SESSION_START source_device=%s stream_id=%lu",
                 source_name(frame.source_id),
                 (unsigned long)frame.stream_id);
        ESP_LOGI(TAG,
                 "WAKE_SESSION_START source_device=%s stream_id=%lu reason=voice_stream_open",
                 source_name(frame.source_id),
                 (unsigned long)frame.stream_id);
        return;
    }
    if (s_active_stream == 0U || frame.source_id != s_active_source ||
        frame.stream_id != s_active_stream) {
        log_stream_mismatch_drop(&frame, "inactive_or_other_stream");
        return;
    }
    s_last_rx_ms = now_ms;
    const audio_wake_pcm_phase_t frame_phase =
        frame.type == ESP111_PROTOCOL_AUDIO_STREAM_PCM ?
            ((frame.flags & ESP111_PROTOCOL_AUDIO_STREAM_FLAG_PREROLL) != 0U ?
                AUDIO_WAKE_PCM_PHASE_PRE_ROLL : AUDIO_WAKE_PCM_PHASE_LIVE) :
            AUDIO_WAKE_PCM_PHASE_UNKNOWN;
    if (frame_phase != AUDIO_WAKE_PCM_PHASE_UNKNOWN) {
        s_pcm_phase = frame_phase;
    }
    const int32_t delta = sequence_delta(frame.sequence, s_expected_sequence);
    if (delta < 0) {
        ESP_LOGW(TAG, "S3_PCM_RX_DROP reason=duplicate sequence=%lu", (unsigned long)frame.sequence);
        return;
    }
    if (frame.type == ESP111_PROTOCOL_AUDIO_STREAM_VOICE_ABORT) {
        s_expected_sequence = frame.sequence + 1U;
        close_session("abort", true);
        return;
    }
    if (frame.type == ESP111_PROTOCOL_AUDIO_STREAM_VOICE_END) {
        s_expected_sequence = frame.sequence + 1U;
        close_session("voice_end", true);
        return;
    }
    if (frame.type != ESP111_PROTOCOL_AUDIO_STREAM_PCM ||
        frame.payload_length == 0U || frame.payload_length % sizeof(int16_t) != 0U ||
        frame.frame_samples != frame.payload_length / sizeof(int16_t)) {
        ESP_LOGW(TAG, "S3_PCM_RX_DROP reason=invalid_pcm_frame stream_id=%lu",
                 (unsigned long)frame.stream_id);
        return;
    }
    if (delta > 0) {
        const uint32_t missing = (uint32_t)delta;
        ++s_gap_count;
        s_sequence_gap += missing;
        if (missing > s_max_gap) {
            s_max_gap = missing;
        }
        ESP_LOGW(TAG,
                 "S3_PCM_RX_GAP source_device=%s stream_id=%lu expected=%lu received=%lu missing=%lu phase=%s",
                 source_name(frame.source_id),
                 (unsigned long)frame.stream_id,
                 (unsigned long)s_expected_sequence,
                 (unsigned long)frame.sequence,
                 (unsigned long)missing,
                 pcm_phase_name(frame_phase));
        if (missing > AUDIO_WAKE_MAX_RECOVERABLE_GAP_FRAMES) {
            close_session("sequence_gap_exceeds_recovery", false);
            return;
        }
        if (!insert_silence_frames(missing, frame.frame_samples)) {
            ESP_LOGW(TAG,
                     "S3_PCM_RX_GAP_RECOVERY_FAILED source_device=%s stream_id=%lu missing=%lu",
                     source_name(frame.source_id),
                     (unsigned long)frame.stream_id,
                     (unsigned long)missing);
            close_session("sequence_gap_recovery_failed", false);
            return;
        }
        s_silence_recovery_frames += missing;
        s_silence_recovery_samples += missing * frame.frame_samples;
        ESP_LOGI(TAG,
                 "S3_PCM_RX_GAP_RECOVERED source_device=%s stream_id=%lu missing=%lu silence_samples=%lu",
                 source_name(frame.source_id),
                 (unsigned long)frame.stream_id,
                 (unsigned long)missing,
                 (unsigned long)(missing * frame.frame_samples));
    }
    s_expected_sequence = frame.sequence + 1U;
    const size_t sample_count = frame.payload_length / sizeof(int16_t);
    s_last_transport_frame_samples = frame.frame_samples;
    if (!s_first_pcm_received) {
        s_first_pcm_received = true;
        ESP_LOGI(TAG,
                 "S3_PCM_FIRST_FRAME stream_id=%lu sequence=%lu transport_frame_samples=%u",
                 (unsigned long)frame.stream_id,
                 (unsigned long)frame.sequence,
                 (unsigned int)frame.frame_samples);
    }
    ++s_rx_pcm_frames;
    s_rx_pcm_bytes += frame.payload_length;
    size_t offset = 0U;
    while (offset < sample_count) {
        if (s_buffered_samples > AUDIO_WAKE_PCM_BUFFER_SAMPLES) {
            ESP_LOGE(TAG,
                     "S3_PCM_RX_DROP reason=buffer_state_invalid buffered_samples=%u capacity=%u",
                     (unsigned int)s_buffered_samples,
                     (unsigned int)AUDIO_WAKE_PCM_BUFFER_SAMPLES);
            s_buffered_samples = 0U;
        }
        size_t available = AUDIO_WAKE_PCM_BUFFER_SAMPLES - s_buffered_samples;
        if (available == 0U) {
            (void)feed_available_chunks();
            available = AUDIO_WAKE_PCM_BUFFER_SAMPLES - s_buffered_samples;
        }
        size_t copy_count = sample_count - offset;
        if (copy_count > available) {
            copy_count = available;
        }
        memcpy(s_pcm_buffer + s_buffered_samples,
               payload + offset * sizeof(int16_t),
               copy_count * sizeof(int16_t));
        s_buffered_samples += copy_count;
        offset += copy_count;
        (void)feed_available_chunks();
    }
}

static int open_audio_socket(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        return -1;
    }
    struct timeval timeout = {.tv_sec = 0, .tv_usec = AUDIO_WAKE_RX_TIMEOUT_MS * 1000};
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(ESP111_PROTOCOL_AUDIO_STREAM_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(sock);
        return -1;
    }
    return sock;
}

static void audio_wake_receiver_task(void *arg)
{
    (void)arg;
    uint8_t packet[ESP111_PROTOCOL_AUDIO_STREAM_HEADER_BYTES + ESP111_PROTOCOL_AUDIO_STREAM_MAX_PAYLOAD];
    int sock = -1;
    while (true) {
        if (sock < 0) {
            sock = open_audio_socket();
            if (sock < 0) {
                ESP_LOGW(TAG, "S3_VOICE_RX_READY failed reason=bind_retry");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            ESP_LOGI(TAG, "S3_VOICE_RX_READY port=%u transport=udp",
                     (unsigned int)ESP111_PROTOCOL_AUDIO_STREAM_PORT);
        }
        int received = recvfrom(sock, packet, sizeof(packet), 0, NULL, NULL);
        if (received > 0) {
            if (s_model_lock != NULL) {
                xSemaphoreTake(s_model_lock, portMAX_DELAY);
            }
            process_packet(packet, (size_t)received);
            if (s_model_lock != NULL) {
                xSemaphoreGive(s_model_lock);
            }
        } else if (s_active_stream != 0U &&
                   esp_timer_get_time() / 1000 - s_last_rx_ms > AUDIO_WAKE_STREAM_TIMEOUT_MS) {
            if (s_model_lock != NULL) {
                xSemaphoreTake(s_model_lock, portMAX_DELAY);
            }
            close_session("timeout", true);
            if (s_model_lock != NULL) {
                xSemaphoreGive(s_model_lock);
            }
        }
    }
}

esp_err_t audio_wake_gateway_init(void)
{
    if (s_models != NULL) {
        return s_model_ready ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "WAKENET_MODEL_CREATE_BEGIN model_name=nihaoxiaozhi");
    ESP_LOGI(TAG, "WAKENET_INIT_BEGIN");
    s_models = esp_srmodel_init("model");
    if (s_models == NULL) {
        ESP_LOGE(TAG, "WAKENET_INIT_FAILED stage=model_list ret=%s", esp_err_to_name(ESP_ERR_NOT_FOUND));
        return ESP_ERR_NOT_FOUND;
    }
    char *name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, "nihaoxiaozhi");
    s_model_name = name;
    s_wakenet = name != NULL ? esp_wn_handle_from_name(name) : NULL;
    if (s_wakenet == NULL || s_wakenet->create == NULL || s_wakenet->destroy == NULL ||
        s_wakenet->clean == NULL ||
        s_wakenet->detect == NULL || s_wakenet->get_samp_chunksize == NULL ||
        s_wakenet->get_samp_rate == NULL || s_wakenet->get_channel_num == NULL) {
        ESP_LOGE(TAG, "WAKENET_INIT_FAILED stage=interface model=%s", name != NULL ? name : "<none>");
        audio_wake_gateway_cleanup();
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "WAKENET_MODEL name=%s", name);
    const esp_err_t create_ret = create_wakenet_instance();
    if (create_ret != ESP_OK) {
        ESP_LOGE(TAG, "WAKENET_INIT_FAILED stage=create ret=%s", esp_err_to_name(create_ret));
        audio_wake_gateway_cleanup();
        return create_ret;
    }
    ESP_LOGI(TAG, "WAKENET_SAMPLE_RATE value=%d", s_wakenet->get_samp_rate(s_model));
    ESP_LOGI(TAG, "WAKENET_FRAME_SIZE samples=%d", s_chunk_samples);
    s_pcm_buffer = heap_caps_calloc(AUDIO_WAKE_PCM_BUFFER_SAMPLES,
                                    sizeof(*s_pcm_buffer),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_model_lock = xSemaphoreCreateMutex();
    if (s_pcm_buffer == NULL || s_model_lock == NULL) {
        ESP_LOGE(TAG, "WAKENET_INIT_FAILED stage=workspace ret=%s", esp_err_to_name(ESP_ERR_NO_MEM));
        audio_wake_gateway_cleanup();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "WAKENET_INIT_READY");
    ESP_LOGI(TAG, "WAKENET_FEED_READY transport=udp port=%u", (unsigned int)ESP111_PROTOCOL_AUDIO_STREAM_PORT);
    if (xTaskCreateWithCaps(audio_wake_receiver_task,
                            "audio_wake_rx",
                            AUDIO_WAKE_TASK_STACK,
                            NULL,
                            AUDIO_WAKE_TASK_PRIORITY,
                            NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "WAKENET_INIT_FAILED stage=receiver_task ret=%s", esp_err_to_name(ESP_ERR_NO_MEM));
        audio_wake_gateway_cleanup();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t audio_wake_gateway_set_event_handler(audio_wake_gateway_event_handler_t handler,
                                               void *context)
{
    if (s_models == NULL || s_model_lock == NULL || handler == NULL) {
        ESP_LOGW(TAG,
                 "WAKE_CALLBACK_REGISTER_FAILED reason=%s",
                 handler == NULL ? "handler_null" : "wakenet_not_initialized");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_model_lock, portMAX_DELAY);
    s_event_handler = handler;
    s_event_context = context;
    xSemaphoreGive(s_model_lock);
    ESP_LOGI(TAG, "WAKE_EVENT_HANDLER_READY");
    ESP_LOGI(TAG, "WAKE_CALLBACK_REGISTERED result=ok");
    return ESP_OK;
}

esp_err_t audio_wake_gateway_detect_pcm(const int16_t *samples,
                                        size_t sample_count,
                                        const char *device_id,
                                        bool *out_detected)
{
    if (samples == NULL || sample_count == 0U || out_detected == NULL ||
        !wakenet_ready() || s_model_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_detected = false;
    if (s_chunk_samples > (int)AUDIO_WAKE_PCM_BUFFER_SAMPLES) {
        return ESP_ERR_INVALID_SIZE;
    }
    xSemaphoreTake(s_model_lock, portMAX_DELAY);
    if (!wakenet_ready() || s_active_stream != 0U) {
        xSemaphoreGive(s_model_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (recreate_wakenet_instance("http_session") != ESP_OK) {
        xSemaphoreGive(s_model_lock);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "WAKENET_SESSION_START source_device=%s stream_id=http",
             device_id != NULL ? device_id : "<unknown>");
    ESP_LOGI(TAG, "WAKE_SESSION_START source_device=%s stream_id=http reason=http_pcm_fallback",
             device_id != NULL ? device_id : "<unknown>");
    for (size_t offset = 0; offset + (size_t)s_chunk_samples <= sample_count;
         offset += (size_t)s_chunk_samples) {
        memcpy(s_pcm_buffer,
               samples + offset,
               (size_t)s_chunk_samples * sizeof(*s_pcm_buffer));
        const int result = s_wakenet->detect(s_model, s_pcm_buffer);
        if (result == WAKENET_DETECTED) {
            *out_detected = true;
            ESP_LOGI(TAG,
                     "WAKE_DETECTED source_device=%s stream_id=http reason=wakenet_detected",
                     device_id != NULL ? device_id : "<unknown>");
            ESP_LOGI(TAG, "WAKENET_DETECTED source_device=%s stream_id=http confidence=1",
                     device_id != NULL ? device_id : "<unknown>");
            break;
        }
    }
    if (!*out_detected) {
        ESP_LOGI(TAG,
                 "WAKE_NOT_DETECTED source_device=%s stream_id=http reason=pcm_exhausted",
                 device_id != NULL ? device_id : "<unknown>");
    }
    ESP_LOGI(TAG, "WAKENET_SESSION_END source_device=%s stream_id=http detected=%u",
             device_id != NULL ? device_id : "<unknown>", *out_detected ? 1U : 0U);
    s_buffered_samples = 0U;
    xSemaphoreGive(s_model_lock);
    return ESP_OK;
}

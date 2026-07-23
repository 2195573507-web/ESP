#include "c5_audio_transport.h"

#include <stdbool.h>
#include <string.h>

#include "esp111_protocol_common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "server_comm_config.h"

static const char *TAG = "c5_audio_transport";
#define C5_AUDIO_QUEUE_DEPTH 64U
#define C5_AUDIO_TASK_STACK 4096U
#define C5_AUDIO_TASK_PRIORITY 5U
#define C5_AUDIO_SLOT_BYTES (ESP111_PROTOCOL_AUDIO_STREAM_HEADER_BYTES + ESP111_PROTOCOL_AUDIO_STREAM_MAX_PAYLOAD)

typedef struct {
    uint16_t length;
    uint8_t wire_type;
    uint32_t stream_id;
    uint8_t bytes[C5_AUDIO_SLOT_BYTES];
} c5_audio_slot_t;
static QueueHandle_t s_free_queue;
static QueueHandle_t s_send_queue;
static c5_audio_slot_t *s_slots;
static TaskHandle_t s_task;
static uint8_t s_source_id;
static uint8_t s_phase_flags;
static uint32_t s_stream_id;
static uint32_t s_stream_generation;
static uint32_t s_stream_id_seed;
static uint32_t s_sequence;
static uint32_t s_sample_counter;
static volatile bool s_active;
static volatile bool s_closing;
static uint32_t s_stream_pcm_frames;
static uint32_t s_stream_pcm_bytes;
static uint32_t s_stream_dropped_frames;
static bool s_live_phase_logged;
static bool s_tail_phase_logged;

/* `server_voice_client_init()` can be retried after a partial voice startup.
 * Treat a transport as ready only after all of its owned objects exist; any
 * earlier allocation failure must not poison the next attempt. */
static void c5_audio_transport_reset_failed_init(void)
{
    if (s_send_queue != NULL) {
        vQueueDelete(s_send_queue);
        s_send_queue = NULL;
    }
    if (s_free_queue != NULL) {
        vQueueDelete(s_free_queue);
        s_free_queue = NULL;
    }
    if (s_slots != NULL) {
        heap_caps_free(s_slots);
        s_slots = NULL;
    }
    s_task = NULL;
    s_source_id = 0U;
    s_stream_id_seed = 0U;
    s_active = false;
    s_closing = false;
}

static void c5_audio_transport_log_summary(const char *event)
{
    ESP_LOGI(TAG,
             "PCM_STREAM_%s source=%u stream=%lu pcm_frames=%lu pcm_bytes=%lu dropped=%lu queued=%u free=%u",
             event,
             (unsigned int)s_source_id,
             (unsigned long)s_stream_id,
             (unsigned long)s_stream_pcm_frames,
             (unsigned long)s_stream_pcm_bytes,
             (unsigned long)s_stream_dropped_frames,
             s_send_queue != NULL ? (unsigned int)uxQueueMessagesWaiting(s_send_queue) : 0U,
             s_free_queue != NULL ? (unsigned int)uxQueueMessagesWaiting(s_free_queue) : 0U);
}

static esp_err_t queue_frame(const esp111_protocol_audio_frame_t *frame, const void *payload)
{
    if (s_send_queue == NULL || frame == NULL) return ESP_ERR_INVALID_STATE;
    c5_audio_slot_t *slot = NULL;
    if (xQueueReceive(s_free_queue, &slot, 0) != pdTRUE) {
        s_stream_dropped_frames++;
        if (s_stream_dropped_frames == 1U || (s_stream_dropped_frames % 16U) == 0U) {
            ESP_LOGW(TAG,
                     "PCM_STREAM_DROP reason=no_free_slot source=%u stream=%lu type=%u dropped=%lu queued=%u",
                     (unsigned int)s_source_id,
                     (unsigned long)s_stream_id,
                     (unsigned int)frame->type,
                     (unsigned long)s_stream_dropped_frames,
                     (unsigned int)uxQueueMessagesWaiting(s_send_queue));
        }
        return ESP_ERR_TIMEOUT;
    }
    slot->wire_type = frame->type;
    slot->stream_id = frame->stream_id;
    slot->length = (uint16_t)esp111_protocol_audio_encode(frame, payload, slot->bytes, sizeof(slot->bytes));
    if (slot->length == 0U || xQueueSend(s_send_queue, &slot, 0) != pdTRUE) {
        (void)xQueueSend(s_free_queue, &slot, 0);
        s_stream_dropped_frames++;
        if (s_stream_dropped_frames == 1U || (s_stream_dropped_frames % 16U) == 0U) {
            ESP_LOGW(TAG,
                     "PCM_STREAM_DROP reason=queue_overflow source=%u stream=%lu seq=%lu dropped=%lu queued=%u",
                     (unsigned int)s_source_id,
                     (unsigned long)s_stream_id,
                     (unsigned long)s_sequence,
                     (unsigned long)s_stream_dropped_frames,
                     (unsigned int)uxQueueMessagesWaiting(s_send_queue));
        }
        return ESP_ERR_TIMEOUT;
    }
    const UBaseType_t queued = uxQueueMessagesWaiting(s_send_queue);
    if (queued >= (C5_AUDIO_QUEUE_DEPTH * 3U) / 4U && (queued % 8U) == 0U) {
        ESP_LOGW(TAG,
                 "PCM_STREAM_QUEUE_PRESSURE source=%u stream=%lu queued=%u free=%u",
                 (unsigned int)s_source_id,
                 (unsigned long)s_stream_id,
                 (unsigned int)queued,
                 (unsigned int)uxQueueMessagesWaiting(s_free_queue));
    }
    return ESP_OK;
}

static void c5_audio_transport_discard_pending(void)
{
    c5_audio_slot_t *pending = NULL;
    while (xQueueReceive(s_send_queue, &pending, 0) == pdTRUE) {
        (void)xQueueSend(s_free_queue, &pending, 0);
        s_stream_dropped_frames++;
    }
}

static void c5_audio_sender_task(void *arg)
{
    (void)arg;
    int sock = -1;
    struct sockaddr_in dest = {0};
    while (true) {
        c5_audio_slot_t *slot = NULL;
        if (xQueueReceive(s_send_queue, &slot, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            if (sock >= 0) {
                dest.sin_family = AF_INET; dest.sin_port = htons(ESP111_PROTOCOL_AUDIO_STREAM_PORT);
                (void)inet_pton(AF_INET, server_comm_get_host(), &dest.sin_addr);
            }
        }
        if (sock < 0 || sendto(sock, slot->bytes, slot->length, 0,
                                (const struct sockaddr *)&dest, sizeof(dest)) != slot->length) {
            s_stream_dropped_frames++;
            ESP_LOGW(TAG, "PCM_STREAM_DROP reason=send_failed source=%u stream=%lu dropped=%lu queued=%u",
                     (unsigned int)s_source_id,
                     (unsigned long)s_stream_id,
                     (unsigned long)s_stream_dropped_frames,
                     (unsigned int)uxQueueMessagesWaiting(s_send_queue));
            s_active = false;
            s_closing = true;
            c5_audio_transport_discard_pending();
            s_closing = false;
        } else if ((slot->wire_type == ESP111_PROTOCOL_AUDIO_STREAM_VOICE_END ||
                    slot->wire_type == ESP111_PROTOCOL_AUDIO_STREAM_VOICE_ABORT) &&
                   s_closing && slot->stream_id == s_stream_id) {
            s_closing = false;
            ESP_LOGI(TAG,
                     "PCM_STREAM_CLOSE_SENT source=%u stream=%lu type=%u",
                     (unsigned int)s_source_id,
                     (unsigned long)slot->stream_id,
                     (unsigned int)slot->wire_type);
        }
        (void)xQueueSend(s_free_queue, &slot, 0);
    }
}

esp_err_t c5_audio_transport_init(uint8_t source_id)
{
    if (source_id == 0U) return ESP_ERR_INVALID_ARG;
    if (s_task != NULL) {
        if (s_slots != NULL && s_free_queue != NULL && s_send_queue != NULL) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "PCM_TRANSPORT_INIT_FAILED reason=running_partial_state");
        return ESP_ERR_INVALID_STATE;
    }

    /* A previous failed init owns no running task, so it is safe to reclaim. */
    if (s_task == NULL && (s_slots != NULL || s_free_queue != NULL || s_send_queue != NULL)) {
        ESP_LOGW(TAG, "PCM_TRANSPORT_INIT_ROLLBACK reason=partial_previous_init");
        c5_audio_transport_reset_failed_init();
    }

    s_slots = heap_caps_calloc(C5_AUDIO_QUEUE_DEPTH, sizeof(*s_slots), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_free_queue = xQueueCreate(C5_AUDIO_QUEUE_DEPTH, sizeof(c5_audio_slot_t *));
    s_send_queue = xQueueCreate(C5_AUDIO_QUEUE_DEPTH, sizeof(c5_audio_slot_t *));
    if (s_slots == NULL || s_free_queue == NULL || s_send_queue == NULL) {
        c5_audio_transport_reset_failed_init();
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < C5_AUDIO_QUEUE_DEPTH; ++i) { c5_audio_slot_t *slot = &s_slots[i]; (void)xQueueSend(s_free_queue, &slot, 0); }
    s_source_id = source_id;
    s_stream_id_seed = esp_random();
    if (xTaskCreateWithCaps(c5_audio_sender_task, "c5_audio_tx", C5_AUDIO_TASK_STACK, NULL,
                            C5_AUDIO_TASK_PRIORITY, &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        c5_audio_transport_reset_failed_init();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t c5_audio_transport_start(void)
{
    if (s_send_queue == NULL || s_active || s_closing) return ESP_ERR_INVALID_STATE;
    ++s_stream_generation;
    if (s_stream_generation == 0U) {
        ++s_stream_generation;
    }
    s_stream_id = s_stream_id_seed ^ s_stream_generation ^
                  (uint32_t)(esp_timer_get_time() / 1000LL) ^
                  ((uint32_t)s_source_id << 24U);
    if (s_stream_id == 0U) s_stream_id = 1U;
    s_sequence = 0U; s_sample_counter = 0U; s_phase_flags = ESP111_PROTOCOL_AUDIO_STREAM_FLAG_PREROLL;
    s_stream_pcm_frames = 0U;
    s_stream_pcm_bytes = 0U;
    s_stream_dropped_frames = 0U;
    s_live_phase_logged = false;
    s_tail_phase_logged = false;
    esp111_protocol_audio_frame_t frame = {.type = ESP111_PROTOCOL_AUDIO_STREAM_VOICE_START,
        .source_id = s_source_id, .stream_id = s_stream_id, .sequence = s_sequence++,
        .sample_rate = 16000U, .bits_per_sample = 16U, .channels = 1U};
    esp_err_t ret = queue_frame(&frame, NULL);
    if (ret == ESP_OK) {
        s_active = true;
        c5_audio_transport_log_summary("START");
    }
    return ret;
}

esp_err_t c5_audio_transport_begin_pre_roll(uint32_t frame_count)
{
    if (!s_active || s_closing ||
        s_phase_flags != ESP111_PROTOCOL_AUDIO_STREAM_FLAG_PREROLL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG,
             "PCM_PRE_ROLL_BEGIN stream_id=%lu frame_count=%lu first_sequence=%lu",
             (unsigned long)s_stream_id,
             (unsigned long)frame_count,
             (unsigned long)s_sequence);
    return ESP_OK;
}

esp_err_t c5_audio_transport_append_pcm(const int16_t *pcm, size_t samples)
{
    if (!s_active || s_closing || pcm == NULL || samples == 0U || samples > ESP111_PROTOCOL_AUDIO_STREAM_MAX_PAYLOAD / 2U) return ESP_ERR_INVALID_STATE;
    esp111_protocol_audio_frame_t frame = {.type = ESP111_PROTOCOL_AUDIO_STREAM_PCM,
        .source_id = s_source_id, .flags = s_phase_flags, .stream_id = s_stream_id,
        .sequence = s_sequence++, .sample_counter = s_sample_counter, .sample_rate = 16000U,
        .bits_per_sample = 16U, .channels = 1U, .frame_samples = (uint16_t)samples,
        .payload_length = (uint16_t)(samples * sizeof(int16_t))};
    esp_err_t ret = queue_frame(&frame, pcm);
    if (ret == ESP_OK) {
        s_sample_counter += (uint32_t)samples;
        s_stream_pcm_frames++;
        s_stream_pcm_bytes += (uint32_t)(samples * sizeof(int16_t));
    }
    return ret;
}

esp_err_t c5_audio_transport_mark_live(void)
{
    if (!s_active || s_closing) return ESP_ERR_INVALID_STATE;
    s_phase_flags = 0U;
    if (!s_live_phase_logged) {
        s_live_phase_logged = true;
        c5_audio_transport_log_summary("LIVE");
    }
    return ESP_OK;
}

esp_err_t c5_audio_transport_mark_tail(void)
{
    if (!s_active || s_closing) return ESP_ERR_INVALID_STATE;
    s_phase_flags = ESP111_PROTOCOL_AUDIO_STREAM_FLAG_TAIL;
    if (s_active && !s_tail_phase_logged) {
        s_tail_phase_logged = true;
        c5_audio_transport_log_summary("TAIL");
    }
    return ESP_OK;
}

esp_err_t c5_audio_transport_finish(void)
{
    if (!s_active || s_closing) return ESP_ERR_INVALID_STATE;
    esp111_protocol_audio_frame_t frame = {.type = ESP111_PROTOCOL_AUDIO_STREAM_VOICE_END,
        .source_id = s_source_id, .flags = ESP111_PROTOCOL_AUDIO_STREAM_FLAG_END,
        .stream_id = s_stream_id, .sequence = s_sequence++, .sample_counter = s_sample_counter,
        .sample_rate = 16000U, .bits_per_sample = 16U, .channels = 1U};
    s_active = false;
    s_closing = true;
    esp_err_t ret = queue_frame(&frame, NULL);
    if (ret != ESP_OK) {
        s_active = true;
        s_closing = false;
        (void)c5_audio_transport_abort("voice_end_enqueue_failed");
    }
    c5_audio_transport_log_summary(ret == ESP_OK ? "STOP" : "STOP_DROP");
    return ret;
}

esp_err_t c5_audio_transport_abort(const char *reason)
{
    if (!s_active && s_closing) {
        ESP_LOGI(TAG, "PCM_STREAM_ABORT_IGNORED reason=close_pending stream=%lu",
                 (unsigned long)s_stream_id);
        return ESP_OK;
    }
    if (!s_active) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "audio stream abort source=%u stream=%lu reason=%s", s_source_id,
             (unsigned long)s_stream_id, reason != NULL ? reason : "unknown");
    s_active = false;
    s_closing = true;
    if (s_send_queue != NULL) {
        c5_audio_transport_discard_pending();
    }
    if (s_stream_id != 0U) {
        esp111_protocol_audio_frame_t frame = {.type = ESP111_PROTOCOL_AUDIO_STREAM_VOICE_ABORT,
            .source_id = s_source_id, .stream_id = s_stream_id, .sequence = s_sequence++,
            .sample_counter = s_sample_counter, .sample_rate = 16000U,
            .bits_per_sample = 16U, .channels = 1U};
        if (queue_frame(&frame, NULL) != ESP_OK) {
            s_closing = false;
        }
    }
    c5_audio_transport_log_summary("ABORT");
    return ESP_OK;
}

bool c5_audio_transport_is_idle(void) { return !s_active && !s_closing; }

uint32_t c5_audio_transport_get_stream_id(void) { return s_stream_id; }

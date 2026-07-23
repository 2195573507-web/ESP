#include "c5_audio_transport.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "esp111_protocol_common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
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
#define C5_AUDIO_SLOT_BYTES ESP111_PROTOCOL_AUDIO_STREAM_MAX_PAYLOAD
#define C5_AUDIO_TX_STATS_INTERVAL 100U
#define C5_AUDIO_SAMPLE_RATE_HZ 16000U

typedef enum {
    C5_AUDIO_SLOT_WIRE = 0,
    C5_AUDIO_SLOT_PRE_ROLL_BEGIN,
    C5_AUDIO_SLOT_LIVE_BEGIN,
} c5_audio_slot_kind_t;

typedef struct {
    uint16_t payload_length;
    uint16_t frame_samples;
    uint32_t marker_frame_count;
    uint8_t kind;
    uint8_t wire_type;
    uint8_t flags;
    uint32_t stream_id;
    uint8_t payload[C5_AUDIO_SLOT_BYTES];
} c5_audio_slot_t;

typedef struct {
    uint32_t stream_id;
    uint32_t tx_frames;
    uint32_t queue_pushes;
    uint32_t queue_pops;
    uint32_t send_error_count;
    uint32_t lost_frames;
    uint32_t max_queue_depth;
    uint32_t last_sequence;
} c5_audio_tx_stats_t;

static QueueHandle_t s_free_queue;
static QueueHandle_t s_send_queue;
static c5_audio_slot_t *s_slots;
static TaskHandle_t s_task;
static uint8_t s_source_id;
static uint8_t s_phase_flags;
static uint32_t s_stream_id;
static uint32_t s_stream_generation;
static uint32_t s_stream_id_seed;
/* This allocator is owned exclusively by c5_audio_sender_task.  A producer
 * never reserves a sequence while it is still possible for another queued
 * frame to precede it on the wire. */
static uint32_t s_sequence;
static uint32_t s_sample_counter;
static volatile bool s_active;
static volatile bool s_closing;
static uint32_t s_stream_pcm_frames;
static uint32_t s_stream_pcm_bytes;
static uint32_t s_stream_dropped_frames;
static uint32_t s_wire_pre_roll_frames;
static uint32_t s_wire_live_frames;
static uint32_t s_last_pcm_sequence;
static bool s_sent_pcm;
static c5_audio_tx_stats_t s_tx_stats;

static void c5_audio_tx_stats_reset(uint32_t stream_id)
{
    __atomic_store_n(&s_tx_stats.stream_id, stream_id, __ATOMIC_RELAXED);
    __atomic_store_n(&s_tx_stats.tx_frames, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&s_tx_stats.queue_pushes, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&s_tx_stats.queue_pops, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&s_tx_stats.send_error_count, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&s_tx_stats.lost_frames, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&s_tx_stats.max_queue_depth, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&s_tx_stats.last_sequence, 0U, __ATOMIC_RELAXED);
}

static void c5_audio_tx_stats_record_drop(void)
{
    __atomic_fetch_add(&s_tx_stats.lost_frames, 1U, __ATOMIC_RELAXED);
}

static void c5_audio_tx_stats_record_queue_depth(UBaseType_t queued)
{
    uint32_t observed = __atomic_load_n(&s_tx_stats.max_queue_depth, __ATOMIC_RELAXED);
    while (queued > observed &&
           !__atomic_compare_exchange_n(&s_tx_stats.max_queue_depth, &observed,
                                        (uint32_t)queued, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

static void c5_audio_tx_stats_log(void)
{
    ESP_LOGI(TAG,
             "PCM_STREAM_STATS role=tx stream_id=%lu tx_frames=%lu rx_frames=0 lost_frames=%lu gap_count=0 max_gap=0 queue_pushes=%lu queue_pops=%lu send_errors=%lu max_queue_depth=%lu last_sequence=%lu",
             (unsigned long)__atomic_load_n(&s_tx_stats.stream_id, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&s_tx_stats.tx_frames, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&s_tx_stats.lost_frames, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&s_tx_stats.queue_pushes, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&s_tx_stats.queue_pops, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&s_tx_stats.send_error_count, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&s_tx_stats.max_queue_depth, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&s_tx_stats.last_sequence, __ATOMIC_RELAXED));
}

static void c5_audio_transport_pace_pcm(uint16_t samples)
{
    /* UDP has no flow control. Pace each FIFO PCM head at its sample duration
     * so a pre-roll burst cannot overrun the S3 socket while WakeNet runs. */
    const uint32_t duration_ms = ((uint32_t)samples * 1000U +
                                  C5_AUDIO_SAMPLE_RATE_HZ - 1U) /
                                 C5_AUDIO_SAMPLE_RATE_HZ;
    const TickType_t delay_ticks = pdMS_TO_TICKS(duration_ms);
    vTaskDelay(delay_ticks > 0U ? delay_ticks : 1U);
}

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
             "PCM_STREAM_%s source=%u stream=%lu pcm_frames=%lu pcm_bytes=%lu pre_roll_sent=%lu live_sent=%lu dropped=%lu queued=%u free=%u",
             event,
             (unsigned int)s_source_id,
             (unsigned long)s_stream_id,
             (unsigned long)s_stream_pcm_frames,
             (unsigned long)s_stream_pcm_bytes,
             (unsigned long)s_wire_pre_roll_frames,
             (unsigned long)s_wire_live_frames,
             (unsigned long)s_stream_dropped_frames,
             s_send_queue != NULL ? (unsigned int)uxQueueMessagesWaiting(s_send_queue) : 0U,
             s_free_queue != NULL ? (unsigned int)uxQueueMessagesWaiting(s_free_queue) : 0U);
}

static esp_err_t queue_slot(c5_audio_slot_t *slot)
{
    if (s_send_queue == NULL || slot == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_send_queue, &slot, 0) != pdTRUE) {
        (void)xQueueSend(s_free_queue, &slot, 0);
        s_stream_dropped_frames++;
        c5_audio_tx_stats_record_drop();
        if (s_stream_dropped_frames == 1U || (s_stream_dropped_frames % 16U) == 0U) {
            ESP_LOGW(TAG,
                     "PCM_STREAM_DROP reason=queue_overflow source=%u stream=%lu type=%u dropped=%lu queued=%u",
                     (unsigned int)s_source_id,
                     (unsigned long)s_stream_id,
                     (unsigned int)slot->wire_type,
                     (unsigned long)s_stream_dropped_frames,
                     (unsigned int)uxQueueMessagesWaiting(s_send_queue));
        }
        return ESP_ERR_TIMEOUT;
    }
    const UBaseType_t queued = uxQueueMessagesWaiting(s_send_queue);
    __atomic_fetch_add(&s_tx_stats.queue_pushes, 1U, __ATOMIC_RELAXED);
    c5_audio_tx_stats_record_queue_depth(queued);
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

static esp_err_t queue_marker(c5_audio_slot_kind_t kind, uint32_t frame_count)
{
    c5_audio_slot_t *slot = NULL;
    if (s_free_queue == NULL || xQueueReceive(s_free_queue, &slot, 0) != pdTRUE) {
        s_stream_dropped_frames++;
        c5_audio_tx_stats_record_drop();
        return ESP_ERR_TIMEOUT;
    }
    memset(slot, 0, sizeof(*slot));
    slot->kind = (uint8_t)kind;
    slot->stream_id = s_stream_id;
    slot->marker_frame_count = frame_count;
    return queue_slot(slot);
}

static esp_err_t queue_wire(uint8_t type,
                            uint8_t flags,
                            const int16_t *pcm,
                            size_t samples)
{
    if (samples > ESP111_PROTOCOL_AUDIO_STREAM_MAX_PAYLOAD / sizeof(int16_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    c5_audio_slot_t *slot = NULL;
    if (s_free_queue == NULL || xQueueReceive(s_free_queue, &slot, 0) != pdTRUE) {
        s_stream_dropped_frames++;
        c5_audio_tx_stats_record_drop();
        return ESP_ERR_TIMEOUT;
    }
    memset(slot, 0, sizeof(*slot));
    slot->kind = C5_AUDIO_SLOT_WIRE;
    slot->wire_type = type;
    slot->flags = flags;
    slot->stream_id = s_stream_id;
    slot->frame_samples = (uint16_t)samples;
    slot->payload_length = (uint16_t)(samples * sizeof(*pcm));
    if (slot->payload_length > 0U) {
        if (pcm == NULL) {
            (void)xQueueSend(s_free_queue, &slot, 0);
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(slot->payload, pcm, slot->payload_length);
    }
    return queue_slot(slot);
}

static void c5_audio_transport_complete_close(const c5_audio_slot_t *slot)
{
    if (slot == NULL ||
        (slot->wire_type != ESP111_PROTOCOL_AUDIO_STREAM_VOICE_END &&
         slot->wire_type != ESP111_PROTOCOL_AUDIO_STREAM_VOICE_ABORT) ||
        !s_closing || slot->stream_id != s_stream_id) {
        return;
    }

    s_closing = false;
    ESP_LOGI(TAG,
             "PCM_STREAM_CLOSE_SENT source=%u stream=%lu type=%u",
             (unsigned int)s_source_id,
             (unsigned long)slot->stream_id,
             (unsigned int)slot->wire_type);
}

static void c5_audio_transport_discard_pending(void)
{
    c5_audio_slot_t *pending = NULL;
    while (xQueueReceive(s_send_queue, &pending, 0) == pdTRUE) {
        (void)xQueueSend(s_free_queue, &pending, 0);
        s_stream_dropped_frames++;
        c5_audio_tx_stats_record_drop();
    }
}

static bool c5_audio_transport_send_slot(int sock,
                                         const struct sockaddr_in *dest,
                                         const c5_audio_slot_t *slot)
{
    if (slot->kind == C5_AUDIO_SLOT_PRE_ROLL_BEGIN) {
        ESP_LOGI(TAG,
                 "PCM_PRE_ROLL_BEGIN stream_id=%lu frame_count=%lu first_sequence=%lu",
                 (unsigned long)slot->stream_id,
                 (unsigned long)slot->marker_frame_count,
                 (unsigned long)s_sequence);
        return true;
    }
    if (slot->kind == C5_AUDIO_SLOT_LIVE_BEGIN) {
        ESP_LOGI(TAG,
                 "PCM_PRE_ROLL_END stream_id=%lu frames_sent=%lu last_sequence=%lu",
                 (unsigned long)slot->stream_id,
                 (unsigned long)s_wire_pre_roll_frames,
                 (unsigned long)(s_sent_pcm ? s_last_pcm_sequence : s_sequence));
        ESP_LOGI(TAG,
                 "PCM_LIVE_BEGIN stream_id=%lu first_sequence=%lu",
                 (unsigned long)slot->stream_id,
                 (unsigned long)s_sequence);
        return true;
    }

    esp111_protocol_audio_frame_t frame = {
        .type = slot->wire_type,
        .source_id = s_source_id,
        .flags = slot->flags,
        .stream_id = slot->stream_id,
        .sequence = s_sequence,
        .sample_counter = s_sample_counter,
        .sample_rate = 16000U,
        .bits_per_sample = 16U,
        .channels = 1U,
        .frame_samples = slot->frame_samples,
        .payload_length = slot->payload_length,
    };
    uint8_t wire[ESP111_PROTOCOL_AUDIO_STREAM_HEADER_BYTES + ESP111_PROTOCOL_AUDIO_STREAM_MAX_PAYLOAD];
    const size_t wire_length = esp111_protocol_audio_encode(&frame,
                                                             slot->payload_length > 0U ? slot->payload : NULL,
                                                             wire,
                                                             sizeof(wire));
    if (wire_length == 0U) {
        ESP_LOGW(TAG, "PCM_STREAM_DROP reason=encode_failed source=%u stream=%lu",
                 (unsigned int)s_source_id, (unsigned long)slot->stream_id);
        return false;
    }
    /* A sequence becomes visible only when this queue head is encoded for
     * send.  Queue FIFO is therefore the sole ordering authority. */
    errno = 0;
    const int sent = sock >= 0 ? sendto(sock, wire, wire_length, 0,
                                        (const struct sockaddr *)dest, sizeof(*dest)) : -1;
    const int socket_errno = sock >= 0 ? errno : ENOTCONN;
    if (sent != (int)wire_length) {
        __atomic_fetch_add(&s_tx_stats.send_error_count, 1U, __ATOMIC_RELAXED);
        ESP_LOGW(TAG,
                 "PCM_TX_FRAME stage=socket_send stream_id=%lu sequence=%lu wire_type=%u bytes=%u sent=%d result=failed errno=%d",
                 (unsigned long)slot->stream_id,
                 (unsigned long)frame.sequence,
                 (unsigned int)slot->wire_type,
                 (unsigned int)wire_length,
                 sent,
                 socket_errno);
        return false;
    }
    ++s_sequence;
    const uint32_t tx_frames = slot->wire_type == ESP111_PROTOCOL_AUDIO_STREAM_PCM ?
        __atomic_add_fetch(&s_tx_stats.tx_frames, 1U, __ATOMIC_RELAXED) :
        __atomic_load_n(&s_tx_stats.tx_frames, __ATOMIC_RELAXED);
    __atomic_store_n(&s_tx_stats.last_sequence, frame.sequence, __ATOMIC_RELAXED);
    if (slot->wire_type == ESP111_PROTOCOL_AUDIO_STREAM_PCM) {
        s_sample_counter += slot->frame_samples;
        s_last_pcm_sequence = frame.sequence;
        s_sent_pcm = true;
        if ((slot->flags & ESP111_PROTOCOL_AUDIO_STREAM_FLAG_PREROLL) != 0U) {
            ++s_wire_pre_roll_frames;
        } else {
            ++s_wire_live_frames;
        }
    }
    if (slot->wire_type == ESP111_PROTOCOL_AUDIO_STREAM_VOICE_START) {
        ESP_LOGI(TAG, "PCM_STREAM_OPEN stream_id=%lu sequence=%lu",
                 (unsigned long)slot->stream_id, (unsigned long)frame.sequence);
    }
    if ((tx_frames != 0U && (tx_frames % C5_AUDIO_TX_STATS_INTERVAL) == 0U) ||
        slot->wire_type == ESP111_PROTOCOL_AUDIO_STREAM_VOICE_END ||
        slot->wire_type == ESP111_PROTOCOL_AUDIO_STREAM_VOICE_ABORT) {
        c5_audio_tx_stats_log();
    }
    if (slot->wire_type == ESP111_PROTOCOL_AUDIO_STREAM_PCM) {
        c5_audio_transport_pace_pcm(slot->frame_samples);
    }
    return true;
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
        __atomic_fetch_add(&s_tx_stats.queue_pops, 1U, __ATOMIC_RELAXED);
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            if (sock >= 0) {
                dest.sin_family = AF_INET; dest.sin_port = htons(ESP111_PROTOCOL_AUDIO_STREAM_PORT);
                (void)inet_pton(AF_INET, server_comm_get_host(), &dest.sin_addr);
            }
        }
        if (!c5_audio_transport_send_slot(sock, &dest, slot)) {
            s_stream_dropped_frames++;
            c5_audio_tx_stats_record_drop();
            ESP_LOGW(TAG, "PCM_STREAM_DROP reason=send_failed source=%u stream=%lu dropped=%lu queued=%u",
                     (unsigned int)s_source_id,
                     (unsigned long)s_stream_id,
                     (unsigned long)s_stream_dropped_frames,
                     (unsigned int)uxQueueMessagesWaiting(s_send_queue));
            s_active = false;
            /* Keep start admission closed until every queued frame from the
             * failed session has been returned to the free pool. */
            s_closing = true;
            c5_audio_transport_discard_pending();
            s_closing = false;
            c5_audio_tx_stats_log();
        } else if (slot->kind == C5_AUDIO_SLOT_WIRE) {
            c5_audio_transport_complete_close(slot);
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
    s_stream_id = s_stream_id_seed ^ s_stream_generation ^ ((uint32_t)s_source_id << 24U);
    if (s_stream_id == 0U) s_stream_id = 1U;
    s_sequence = 0U;
    s_sample_counter = 0U;
    s_phase_flags = ESP111_PROTOCOL_AUDIO_STREAM_FLAG_PREROLL;
    s_stream_pcm_frames = 0U;
    s_stream_pcm_bytes = 0U;
    s_stream_dropped_frames = 0U;
    s_wire_pre_roll_frames = 0U;
    s_wire_live_frames = 0U;
    s_last_pcm_sequence = 0U;
    s_sent_pcm = false;
    c5_audio_tx_stats_reset(s_stream_id);
    s_active = true;
    ESP_LOGI(TAG,
             "PCM_SEQUENCE_RESET stream_id=%lu initial_sequence=0 reason=new_stream",
             (unsigned long)s_stream_id);
    esp_err_t ret = queue_wire(ESP111_PROTOCOL_AUDIO_STREAM_VOICE_START, 0U, NULL, 0U);
    if (ret == ESP_OK) {
        c5_audio_transport_log_summary("START");
    } else {
        s_active = false;
    }
    return ret;
}

esp_err_t c5_audio_transport_begin_pre_roll(uint32_t frame_count)
{
    if (!s_active || s_closing || s_phase_flags != ESP111_PROTOCOL_AUDIO_STREAM_FLAG_PREROLL) {
        return ESP_ERR_INVALID_STATE;
    }
    return queue_marker(C5_AUDIO_SLOT_PRE_ROLL_BEGIN, frame_count);
}

esp_err_t c5_audio_transport_append_pcm(const int16_t *pcm, size_t samples)
{
    if (!s_active || s_closing || pcm == NULL || samples == 0U || samples > ESP111_PROTOCOL_AUDIO_STREAM_MAX_PAYLOAD / 2U) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = queue_wire(ESP111_PROTOCOL_AUDIO_STREAM_PCM, s_phase_flags, pcm, samples);
    if (ret == ESP_OK) {
        s_stream_pcm_frames++;
        s_stream_pcm_bytes += (uint32_t)(samples * sizeof(int16_t));
    }
    return ret;
}

esp_err_t c5_audio_transport_mark_live(void)
{
    if (!s_active || s_closing) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_phase_flags == ESP111_PROTOCOL_AUDIO_STREAM_FLAG_PREROLL) {
        const esp_err_t ret = queue_marker(C5_AUDIO_SLOT_LIVE_BEGIN, 0U);
        if (ret != ESP_OK) {
            return ret;
        }
        s_phase_flags = 0U;
        c5_audio_transport_log_summary("LIVE");
    }
    return ESP_OK;
}

esp_err_t c5_audio_transport_mark_tail(void)
{
    if (!s_active || s_closing) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_phase_flags != ESP111_PROTOCOL_AUDIO_STREAM_FLAG_TAIL) {
        s_phase_flags = ESP111_PROTOCOL_AUDIO_STREAM_FLAG_TAIL;
        c5_audio_transport_log_summary("TAIL");
    }
    return ESP_OK;
}

esp_err_t c5_audio_transport_finish(void)
{
    if (!s_active || s_closing) return ESP_ERR_INVALID_STATE;
    s_active = false;
    s_closing = true;
    esp_err_t ret = queue_wire(ESP111_PROTOCOL_AUDIO_STREAM_VOICE_END,
                               ESP111_PROTOCOL_AUDIO_STREAM_FLAG_END,
                               NULL,
                               0U);
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
        if (queue_wire(ESP111_PROTOCOL_AUDIO_STREAM_VOICE_ABORT, 0U, NULL, 0U) != ESP_OK) {
            s_closing = false;
        }
    }
    c5_audio_transport_log_summary("ABORT");
    return ESP_OK;
}

bool c5_audio_transport_is_idle(void) { return !s_active && !s_closing; }

uint32_t c5_audio_transport_get_stream_id(void) { return s_stream_id; }

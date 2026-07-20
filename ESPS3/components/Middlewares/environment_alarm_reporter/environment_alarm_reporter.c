#include "environment_alarm_reporter.h"
#include "environment_alarm_delivery.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "environment_alarm_engine.h"
#include "esp111_protocol_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network_worker.h"

#ifdef ESP_PLATFORM
#include "app_stack_monitor.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#define ENV_ALARM_STORAGE_ALLOC(count, size) \
    heap_caps_calloc((count), (size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define ENV_ALARM_COMPLETION_QUEUE_CREATE(depth, item_size) \
    xQueueCreateWithCaps((depth), (item_size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define ENV_ALARM_REPORTER_TASK_CREATE(task, name, stack, arg, priority, handle) \
    xTaskCreateWithCaps((task), (name), (stack), (arg), (priority), (handle), \
                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#else
#include <stdlib.h>
#define ENV_ALARM_STORAGE_ALLOC(count, size) calloc((count), (size))
#define ENV_ALARM_COMPLETION_QUEUE_CREATE(depth, item_size) xQueueCreate((depth), (item_size))
#define ENV_ALARM_REPORTER_TASK_CREATE(task, name, stack, arg, priority, handle) \
    xTaskCreate((task), (name), (stack), (arg), (priority), (handle))
#endif

#define ENV_ALARM_REPORTER_COMPLETION_DEPTH 4U
#define ENV_ALARM_REPORTER_POLL_MS 500U
#define ENV_ALARM_REPORTER_STATS_INTERVAL_MS 60000U
#define ENV_ALARM_REPORTER_JSON_BYTES 1024U
#define ENV_ALARM_REPORTER_TASK_STACK 3072U
#define ENV_ALARM_REPORTER_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)

typedef struct {
    alarm_event_t event;
    uint8_t retry_count;
    bool in_flight;
    uint64_t next_attempt_ms;
} environment_alarm_pending_t;

typedef struct {
    uint64_t event_seq;
    esp_err_t result;
    int http_status;
} environment_alarm_completion_t;

typedef struct {
    uint64_t event_seq;
    int http_status;
    esp_err_t result;
} environment_alarm_dead_letter_t;

static const char *TAG = "ENV_ALARM_REPORT";
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_completion_queue;
static TaskHandle_t s_task;
static bool s_initialized;
typedef struct {
    environment_alarm_pending_t pending[ENVIRONMENT_ALARM_REPORT_QUEUE_CAPACITY];
    environment_alarm_dead_letter_t dead_letters[8];
    alarm_event_t drain_events[ALARM_ENGINE_MAX_EVENTS_PER_UPDATE];
    char json_buffer[ENV_ALARM_REPORTER_JSON_BYTES];
} environment_alarm_reporter_storage_t;

static environment_alarm_reporter_storage_t *s_storage;
#define s_pending (s_storage->pending)
#define s_dead_letters (s_storage->dead_letters)
#define s_drain_events (s_storage->drain_events)
#define s_json_buffer (s_storage->json_buffer)
static size_t s_head;
static size_t s_count;
static size_t s_dead_letter_cursor;
static environment_alarm_reporter_stats_t s_stats;
static int64_t s_last_stats_log_ms;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static environment_alarm_pending_t *pending_at(size_t offset)
{
    return &s_pending[(s_head + offset) % ENVIRONMENT_ALARM_REPORT_QUEUE_CAPACITY];
}

static const char *device_name(alarm_device_id_t device)
{
    switch (device) {
    case ALARM_DEVICE_C51:
        return ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51;
    case ALARM_DEVICE_C52:
        return ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52;
    default:
        return "";
    }
}

static const char *default_room(alarm_device_id_t device)
{
    return device == ALARM_DEVICE_C52 ? "bedroom" : "living_room";
}

static const char *rule_name(alarm_type_t type)
{
    static const char *const names[ALARM_TYPE_COUNT] = {
        "high_temperature", "low_temperature", "fast_temperature_change",
        "high_humidity", "low_humidity", "fast_humidity_change",
        "air_quality_warning", "air_quality_critical", "air_quality_deteriorating",
        "pollution_spike", "environment_unstable", "sensor_degraded",
        "critical_environment",
    };
    return type < ALARM_TYPE_COUNT ? names[type] : "unknown";
}

static const char *level_name(alarm_level_t level)
{
    switch (level) {
    case ALARM_LEVEL_WARNING:
        return "warning";
    case ALARM_LEVEL_HIGH:
        return "error";
    case ALARM_LEVEL_CRITICAL:
        return "critical";
    default:
        return "warning";
    }
}

static const char *state_name(alarm_event_status_t status)
{
    return status == ALARM_STATUS_RECOVERED ? "recovered" : "active";
}

static const char *sensor_state_name(alarm_sensor_state_t state)
{
    switch (state) {
    case ALARM_SENSOR_WARMUP:
        return "WARMUP";
    case ALARM_SENSOR_READY:
        return "READY";
    case ALARM_SENSOR_DEGRADED:
        return "DEGRADED";
    case ALARM_SENSOR_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

static bool pending_contains_locked(uint64_t event_seq)
{
    for (size_t index = 0; index < s_count; ++index) {
        if (pending_at(index)->event.event_seq == event_seq) {
            return true;
        }
    }
    return false;
}

static void remove_head_locked(void)
{
    if (s_count == 0U) {
        return;
    }
    memset(pending_at(0U), 0, sizeof(*pending_at(0U)));
    s_head = (s_head + 1U) % ENVIRONMENT_ALARM_REPORT_QUEUE_CAPACITY;
    --s_count;
}

static void note_dead_letter_locked(const environment_alarm_completion_t *completion)
{
    if (completion == NULL) {
        return;
    }
    s_dead_letters[s_dead_letter_cursor] = (environment_alarm_dead_letter_t){
        .event_seq = completion->event_seq,
        .http_status = completion->http_status,
        .result = completion->result,
    };
    s_dead_letter_cursor = (s_dead_letter_cursor + 1U) %
                           (sizeof(s_dead_letters) / sizeof(s_dead_letters[0]));
    ++s_stats.dead_letter;
}

static void reporter_network_complete(uint64_t event_seq,
                                      esp_err_t result,
                                      int http_status,
                                      void *context)
{
    (void)context;
    if (s_completion_queue == NULL) {
        return;
    }
    const environment_alarm_completion_t completion = {
        .event_seq = event_seq,
        .result = result,
        .http_status = http_status,
    };
    if (xQueueSend(s_completion_queue, &completion, 0) != pdTRUE) {
        ESP_LOGW(TAG,
                 "ENV_ALARM_REPORT event_seq=%" PRIu64 " result=completion_queue_full",
                 event_seq);
        return;
    }
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

static void process_completions(void)
{
    environment_alarm_completion_t completion;
    while (s_completion_queue != NULL && xQueueReceive(s_completion_queue, &completion, 0) == pdTRUE) {
        if (s_lock == NULL) {
            return;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_count == 0U || pending_at(0U)->event.event_seq != completion.event_seq) {
            xSemaphoreGive(s_lock);
            ESP_LOGW(TAG,
                     "ENV_ALARM_REPORT event_seq=%" PRIu64 " result=stale_completion",
                     completion.event_seq);
            continue;
        }

        environment_alarm_pending_t *pending = pending_at(0U);
        const environment_alarm_delivery_outcome_t outcome =
            environment_alarm_delivery_classify(completion.result, completion.http_status);
        if (outcome == ENVIRONMENT_ALARM_DELIVERY_SENT) {
            ++s_stats.events_sent;
            ESP_LOGI(TAG,
                     "ENV_ALARM_REPORT event_seq=%" PRIu64 " rule=%s device=%s attempt=%u result=sent http_status=%d queue_action=remove",
                     pending->event.event_seq,
                     rule_name(pending->event.alarm_type),
                     device_name(pending->event.device_id),
                     (unsigned int)pending->retry_count + 1U,
                     completion.http_status);
            remove_head_locked();
        } else if (outcome == ENVIRONMENT_ALARM_DELIVERY_DEAD_LETTER) {
            note_dead_letter_locked(&completion);
            ESP_LOGW(TAG,
                     "ENV_ALARM_REPORT event_seq=%" PRIu64 " rule=%s device=%s attempt=%u result=dead_letter http_status=%d ret=%s queue_action=remove",
                     pending->event.event_seq,
                     rule_name(pending->event.alarm_type),
                     device_name(pending->event.device_id),
                     (unsigned int)pending->retry_count + 1U,
                     completion.http_status,
                     esp_err_to_name(completion.result));
            remove_head_locked();
        } else {
            if (pending->retry_count < UINT8_MAX) {
                ++pending->retry_count;
            }
            const uint32_t delay_ms = environment_alarm_delivery_retry_delay_ms(pending->retry_count);
            pending->in_flight = false;
            pending->next_attempt_ms = now_ms() + delay_ms;
            ++s_stats.retries;
            ESP_LOGW(TAG,
                     "ENV_ALARM_REPORT event_seq=%" PRIu64 " rule=%s device=%s attempt=%u result=retry http_status=%d ret=%s retry_delay_ms=%u queue_action=retain",
                     pending->event.event_seq,
                     rule_name(pending->event.alarm_type),
                     device_name(pending->event.device_id),
                     (unsigned int)pending->retry_count,
                     completion.http_status,
                     esp_err_to_name(completion.result),
                     (unsigned int)delay_ms);
        }
        s_stats.report_queue_depth = s_count;
        xSemaphoreGive(s_lock);
    }
}

static bool add_number_or_null(cJSON *object, const char *name, bool valid, double value)
{
    return valid ? cJSON_AddNumberToObject(object, name, value) != NULL :
                   cJSON_AddNullToObject(object, name) != NULL;
}

static bool build_payload(const alarm_event_t *event)
{
    if (event == NULL || device_name(event->device_id)[0] == '\0') {
        return false;
    }
    const char *device = device_name(event->device_id);
    const char *room = event->room_id[0] != '\0' ? event->room_id : default_room(event->device_id);
    const char *state = state_name(event->status);
    char alarm_id[24];
    char event_seq[24];
    char local_ingest_seq[24];
    char dedup_key[112];
    char title[96];
    (void)snprintf(alarm_id, sizeof(alarm_id), "%016" PRIx64, event->alarm_id);
    (void)snprintf(event_seq, sizeof(event_seq), "%" PRIu64, event->event_seq);
    (void)snprintf(local_ingest_seq, sizeof(local_ingest_seq), "%" PRIu64, event->local_ingest_seq);
    (void)snprintf(dedup_key,
                   sizeof(dedup_key),
                   "env:%s:%s:%s:%016" PRIx64 ":%016" PRIx64,
                   device,
                   rule_name(event->alarm_type),
                   state,
                   event->alarm_id,
                   event->event_seq);
    (void)snprintf(title,
                   sizeof(title),
                   "Environment %s %s",
                   rule_name(event->alarm_type),
                   state);

    cJSON *root = cJSON_CreateObject();
    cJSON *metadata = root != NULL ? cJSON_AddObjectToObject(root, "payload") : NULL;
    if (root == NULL || metadata == NULL) {
        cJSON_Delete(root);
        return false;
    }
    bool ok = true;
    ok = ok && cJSON_AddStringToObject(root, "device_id", device) != NULL;
    ok = ok && cJSON_AddStringToObject(root, "level", level_name(event->alarm_level)) != NULL;
    ok = ok && cJSON_AddStringToObject(root, "title", title) != NULL;
    ok = ok && cJSON_AddStringToObject(root, "message", event->description[0] != '\0' ? event->description : title) != NULL;
    ok = ok && cJSON_AddBoolToObject(root, "acknowledged", false) != NULL;
    ok = ok && cJSON_AddStringToObject(root, "room_id", room) != NULL;
    ok = ok && cJSON_AddStringToObject(root, "room_name", room) != NULL;
    ok = ok && cJSON_AddStringToObject(root, "source", "s3_environment_alarm") != NULL;
    ok = ok && cJSON_AddStringToObject(metadata, "alarm_id", alarm_id) != NULL;
    ok = ok && cJSON_AddStringToObject(metadata, "dedup_key", dedup_key) != NULL;
    ok = ok && cJSON_AddStringToObject(metadata, "rule_id", rule_name(event->alarm_type)) != NULL;
    ok = ok && cJSON_AddStringToObject(metadata, "alarm_type", rule_name(event->alarm_type)) != NULL;
    ok = ok && cJSON_AddStringToObject(metadata, "severity", level_name(event->alarm_level)) != NULL;
    ok = ok && cJSON_AddStringToObject(metadata, "state", state) != NULL;
    ok = ok && cJSON_AddBoolToObject(metadata, "active", event->status == ALARM_STATUS_ACTIVE) != NULL;
    ok = ok && cJSON_AddBoolToObject(metadata, "recovered", event->status == ALARM_STATUS_RECOVERED) != NULL;
    ok = ok && cJSON_AddStringToObject(metadata, "unit", event->unit[0] != '\0' ? event->unit : "unknown") != NULL;
    ok = ok && cJSON_AddStringToObject(metadata, "source", event->source[0] != '\0' ? event->source : "c5_bme690") != NULL;
    ok = ok && cJSON_AddStringToObject(metadata, "sensor_state", sensor_state_name(event->sensor_state)) != NULL;
    ok = ok && cJSON_AddStringToObject(metadata, "event_seq", event_seq) != NULL;
    ok = ok && cJSON_AddNumberToObject(metadata, "remote_seq", event->remote_seq) != NULL;
    ok = ok && cJSON_AddStringToObject(metadata, "local_ingest_seq", local_ingest_seq) != NULL;
    ok = ok && cJSON_AddNumberToObject(metadata, "monotonic_timestamp_ms", (double)event->event_monotonic_ms) != NULL;
    ok = ok && cJSON_AddNumberToObject(metadata, "observed_value", event->observed_value) != NULL;
    ok = ok && cJSON_AddNumberToObject(metadata, "trigger_threshold", event->trigger_threshold) != NULL;
    ok = ok && cJSON_AddNumberToObject(metadata, "recovery_threshold", event->recovery_threshold) != NULL;
    ok = ok && add_number_or_null(metadata, "stability_score", event->stability_score_valid, event->stability_score);
    ok = ok && add_number_or_null(metadata, "temperature_c", event->temperature_valid, event->temperature);
    ok = ok && add_number_or_null(metadata, "humidity_percent", event->humidity_valid, event->humidity);
    ok = ok && add_number_or_null(metadata, "air_quality_score", event->air_quality_score_valid, event->air_quality_score);
    ok = ok && add_number_or_null(metadata, "gas_ratio", event->gas_ratio_valid, event->gas_ratio);
    if (event->boot_id_valid) {
        ok = ok && cJSON_AddNumberToObject(metadata, "boot_id", event->boot_id) != NULL;
    } else {
        ok = ok && cJSON_AddNullToObject(metadata, "boot_id") != NULL;
    }
    if (event->timestamp_valid) {
        ok = ok && cJSON_AddNumberToObject(metadata, "event_timestamp_ms", (double)event->timestamp_ms) != NULL;
        ok = ok && cJSON_AddStringToObject(metadata, "event_time_source", "c5_time_synced") != NULL;
    } else {
        ok = ok && cJSON_AddNullToObject(metadata, "event_timestamp_ms") != NULL;
        ok = ok && cJSON_AddStringToObject(metadata, "event_time_source", "s3_monotonic") != NULL;
    }
    if (!ok || !cJSON_PrintPreallocated(root, s_json_buffer, sizeof(s_json_buffer), false)) {
        cJSON_Delete(root);
        return false;
    }
    cJSON_Delete(root);
    return true;
}

static void complete_local_submission_failure(uint64_t event_seq, esp_err_t result)
{
    const environment_alarm_completion_t completion = {
        .event_seq = event_seq,
        .result = result,
        .http_status = 0,
    };
    if (s_completion_queue != NULL && xQueueSend(s_completion_queue, &completion, 0) == pdTRUE) {
        return;
    }
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_count > 0U && pending_at(0U)->event.event_seq == event_seq) {
            pending_at(0U)->in_flight = false;
            pending_at(0U)->next_attempt_ms = now_ms() + environment_alarm_delivery_retry_delay_ms(0U);
        }
        xSemaphoreGive(s_lock);
    }
}

static void submit_head_if_due(void)
{
    if (!network_worker_is_server_ready() ||
        network_worker_get_link_state() != NETWORK_WORKER_LINK_STABLE || s_lock == NULL) {
        return;
    }
    environment_alarm_pending_t pending = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_count == 0U || pending_at(0U)->in_flight ||
        pending_at(0U)->next_attempt_ms > now_ms()) {
        xSemaphoreGive(s_lock);
        return;
    }
    pending = *pending_at(0U);
    pending_at(0U)->in_flight = true;
    xSemaphoreGive(s_lock);

    if (!build_payload(&pending.event)) {
        complete_local_submission_failure(pending.event.event_seq, ESP_ERR_INVALID_SIZE);
        return;
    }
    const size_t json_length = strlen(s_json_buffer);
    char *json_body = cJSON_malloc(json_length + 1U);
    if (json_body == NULL) {
        complete_local_submission_failure(pending.event.event_seq, ESP_ERR_NO_MEM);
        return;
    }
    memcpy(json_body, s_json_buffer, json_length + 1U);
    const esp_err_t ret = network_worker_submit_environment_alarm_json(json_body,
                                                                         pending.event.event_seq,
                                                                         reporter_network_complete,
                                                                         NULL,
                                                                         "environment_alarm");
    if (ret != ESP_OK) {
        cJSON_free(json_body);
        complete_local_submission_failure(pending.event.event_seq, ret);
        return;
    }
    ESP_LOGI(TAG,
             "ENV_ALARM_REPORT event_seq=%" PRIu64 " rule=%s device=%s attempt=%u result=submitted queue_action=retain",
             pending.event.event_seq,
             rule_name(pending.event.alarm_type),
             device_name(pending.event.device_id),
             (unsigned int)pending.retry_count + 1U);
}

static void log_stats_if_due(void)
{
    const int64_t now = (int64_t)now_ms();
    if (s_last_stats_log_ms != 0 && now - s_last_stats_log_ms < ENV_ALARM_REPORTER_STATS_INTERVAL_MS) {
        return;
    }
    environment_alarm_reporter_stats_t stats = {0};
    if (environment_alarm_reporter_get_stats(&stats) != ESP_OK) {
        return;
    }
    s_last_stats_log_ms = now;
    ESP_LOGI(TAG,
             "ENV_ALARM_STATS events_enqueued=%" PRIu64 " events_sent=%" PRIu64 " retries=%" PRIu64 " queue_full=%" PRIu64 " dead_letter=%" PRIu64 " engine_queue_depth=%u report_queue_depth=%u",
             stats.events_enqueued,
             stats.events_sent,
             stats.retries,
             stats.queue_full,
             stats.dead_letter,
             (unsigned int)stats.engine_queue_depth,
             (unsigned int)stats.report_queue_depth);
}

static void environment_alarm_reporter_task(void *arg)
{
    (void)arg;
#ifdef ESP_PLATFORM
    app_stack_monitor_report(TAG,
                             "environment_alarm_reporter",
                             ENV_ALARM_REPORTER_TASK_STACK,
                             "entry");
#endif
    for (;;) {
        process_completions();
        /* A freed FIFO slot must also release older engine events when no new BME sample arrives. */
        (void)environment_alarm_reporter_drain_engine();
        submit_head_if_due();
        log_stats_if_due();
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ENV_ALARM_REPORTER_POLL_MS));
    }
}

esp_err_t environment_alarm_reporter_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (s_storage == NULL) {
        s_storage = ENV_ALARM_STORAGE_ALLOC(1U, sizeof(*s_storage));
        if (s_storage == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_completion_queue == NULL) {
        s_completion_queue = ENV_ALARM_COMPLETION_QUEUE_CREATE(ENV_ALARM_REPORTER_COMPLETION_DEPTH,
                                                                sizeof(environment_alarm_completion_t));
        if (s_completion_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_pending, 0, sizeof(s_pending));
    memset(s_dead_letters, 0, sizeof(s_dead_letters));
    memset(&s_stats, 0, sizeof(s_stats));
    s_head = 0U;
    s_count = 0U;
    s_dead_letter_cursor = 0U;
    s_initialized = true;
    s_stats.ready = true;
    xSemaphoreGive(s_lock);
    if (s_task == NULL) {
        BaseType_t created = ENV_ALARM_REPORTER_TASK_CREATE(environment_alarm_reporter_task,
                                                             "env_alarm_report",
                                                             ENV_ALARM_REPORTER_TASK_STACK,
                                                             NULL,
                                                             ENV_ALARM_REPORTER_TASK_PRIORITY,
                                                             &s_task);
        if (created != pdPASS) {
            s_task = NULL;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_initialized = false;
            s_stats.ready = false;
            xSemaphoreGive(s_lock);
            return ESP_ERR_NO_MEM;
        }
#ifdef ESP_PLATFORM
        app_stack_monitor_log_task_created(TAG,
                                           "env_alarm_report",
                                           s_task,
                                           ENV_ALARM_REPORTER_TASK_STACK);
#endif
    }
    ESP_LOGI(TAG,
             "ENV_ALARM_INIT engine_count=2 reporter_ready=1 queue_capacity=%u rules_enabled=%u storage=psram bytes=%u result=ok",
             (unsigned int)ENVIRONMENT_ALARM_REPORT_QUEUE_CAPACITY,
             (unsigned int)ALARM_TYPE_COUNT,
             (unsigned int)sizeof(*s_storage));
    return ESP_OK;
}

esp_err_t environment_alarm_reporter_drain_engine(void)
{
    if (!s_initialized || s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t event_count = alarm_engine_peek_events(s_drain_events,
                                                         ALARM_ENGINE_MAX_EVENTS_PER_UPDATE);
    if (event_count == 0U) {
        return ESP_OK;
    }

    uint64_t ack_through = 0U;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t index = 0U; index < event_count; ++index) {
        const alarm_event_t *event = &s_drain_events[index];
        if (pending_contains_locked(event->event_seq)) {
            ack_through = event->event_seq;
            continue;
        }
        if (s_count >= ENVIRONMENT_ALARM_REPORT_QUEUE_CAPACITY) {
            ++s_stats.queue_full;
            break;
        }
        environment_alarm_pending_t *slot = pending_at(s_count++);
        memset(slot, 0, sizeof(*slot));
        slot->event = *event;
        slot->next_attempt_ms = now_ms();
        ++s_stats.events_enqueued;
        ack_through = event->event_seq;
        ESP_LOGI(TAG,
                 "ENV_ALARM_EVENT alarm_id=%016" PRIx64 " device=%s rule_id=%s severity=%s state=%s event_seq=%" PRIu64 " queue_result=enqueued",
                 event->alarm_id,
                 device_name(event->device_id),
                 rule_name(event->alarm_type),
                 level_name(event->alarm_level),
                 state_name(event->status),
                 event->event_seq);
    }
    s_stats.report_queue_depth = s_count;
    xSemaphoreGive(s_lock);

    if (ack_through == 0U) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t ack_ret = alarm_engine_ack_events(ack_through);
    if (ack_ret != ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ++s_stats.engine_ack_failures;
        xSemaphoreGive(s_lock);
        return ack_ret;
    }
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
    return ack_through == s_drain_events[event_count - 1U].event_seq ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t environment_alarm_reporter_get_stats(environment_alarm_reporter_stats_t *out)
{
    if (out == NULL || s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_stats;
    out->report_queue_depth = s_count;
    out->engine_queue_depth = alarm_engine_get_queue_depth();
    xSemaphoreGive(s_lock);
    return s_initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}

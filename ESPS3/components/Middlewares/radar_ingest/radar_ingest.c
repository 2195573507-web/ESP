#include "radar_ingest.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#include "cJSON.h"
#include "radar_gateway_ingest.h"
#include "radar_registry.h"

#ifndef RADAR_INGEST_HOST_TEST
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

#define RADAR_INGEST_WORKER_STACK 4096U
#define RADAR_INGEST_WORKER_PRIORITY 3U
#define RADAR_INGEST_WORKER_PERIOD_MS 20U

typedef struct {
    bool pending;
    radar_gateway_sample_t sample;
} radar_ingest_pending_t;

typedef struct {
    uint64_t recorded_at_ms;
    uint8_t local_id;
    uint32_t request_sequence;
    uint32_t frame_seq;
    uint8_t target_count;
    radar_gateway_target_t targets[LD2450_MAX_TARGETS];
} radar_ingest_history_entry_t;

static radar_ingest_pending_t s_pending[RADAR_GATEWAY_MAX_REMOTE_SOURCES];
static radar_ingest_history_entry_t *s_history;
static uint16_t s_history_head;
static uint16_t s_history_count;
static bool s_started;
#ifndef RADAR_INGEST_HOST_TEST
static const char *TAG = "radar_ingest";
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_worker;
static bool s_worker_with_caps;
#else
static radar_ingest_history_entry_t s_history_host[RADAR_INGEST_HISTORY_DEPTH];
#endif

#ifndef RADAR_INGEST_HOST_TEST
static const char *source_name(uint8_t local_id)
{
    switch (local_id) {
    case 1U: return "C51";
    case 2U: return "C52";
    default: return "unknown";
    }
}
#endif

static bool object_has_exact_keys(cJSON *object,
                                  const char *const *keys,
                                  size_t key_count)
{
    if (!cJSON_IsObject(object) || keys == NULL || key_count == 0U || key_count > 32U) {
        return false;
    }
    uint32_t seen = 0U;
    for (cJSON *item = object->child; item != NULL; item = item->next) {
        if (item->string == NULL) return false;
        size_t index = 0U;
        while (index < key_count && strcmp(item->string, keys[index]) != 0) ++index;
        if (index == key_count || (seen & (1UL << index)) != 0U) return false;
        seen |= 1UL << index;
    }
    return seen == (key_count == 32U ? UINT32_MAX : ((1UL << key_count) - 1UL));
}

static bool json_integer_in_range(cJSON *item, double min, double max, double *out)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < min || item->valuedouble > max ||
        floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    if (out != NULL) *out = item->valuedouble;
    return true;
}

static radar_ingest_result_t parse_target(cJSON *item, radar_gateway_target_t *out)
{
    static const char *const keys[] = {
        "target_id", "x_mm", "y_mm", "velocity_cm_s", "confidence", "resolution_mm", "distance_mm"
    };
    if (out == NULL || !object_has_exact_keys(item, keys, sizeof(keys) / sizeof(keys[0]))) {
        return RADAR_INGEST_INVALID_TARGETS;
    }
    double id = 0.0, x = 0.0, y = 0.0, velocity = 0.0, confidence = 0.0;
    double resolution = 0.0, distance = 0.0;
    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "target_id"),
                               1.0, LD2450_MAX_TARGETS, &id) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "x_mm"),
                               INT16_MIN, INT16_MAX, &x) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "y_mm"),
                               INT16_MIN, INT16_MAX, &y) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "velocity_cm_s"),
                               INT16_MIN, INT16_MAX, &velocity) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "confidence"),
                               0.0, 100.0, &confidence) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "resolution_mm"),
                               0.0, UINT16_MAX, &resolution) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "distance_mm"),
                               0.0, UINT32_MAX, &distance)) {
        return RADAR_INGEST_INVALID_TARGETS;
    }
    *out = (radar_gateway_target_t){
        .slot = (uint8_t)id - 1U,
        .x_mm = (int16_t)x,
        .y_mm = (int16_t)y,
        .speed_cm_s = (int16_t)velocity,
        .resolution_mm = (uint16_t)resolution,
        .distance_mm = (uint32_t)distance,
        .confidence = (uint8_t)confidence,
        .valid = true,
    };
    return RADAR_INGEST_ACCEPTED;
}

static radar_ingest_result_t parse_json(const char *json,
                                        size_t json_len,
                                        radar_gateway_sample_t *out)
{
    if (out != NULL) memset(out, 0, sizeof(*out));
    if (json == NULL || out == NULL || json_len == 0U) return RADAR_INGEST_INVALID_ARGUMENT;
    if (json_len > RADAR_INGEST_MAX_BODY_BYTES) return RADAR_INGEST_TOO_LARGE;

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, json_len, &parse_end, false);
    if (root == NULL) return RADAR_INGEST_INVALID_JSON;
    while (parse_end != NULL && parse_end < json + json_len && isspace((unsigned char)*parse_end)) {
        ++parse_end;
    }
    if (parse_end == NULL || parse_end != json + json_len) {
        cJSON_Delete(root);
        return RADAR_INGEST_INVALID_JSON;
    }
    static const char *const root_keys[] = {"p", "id", "t", "u", "q", "v"};
    double number = 0.0;
    if (!object_has_exact_keys(root, root_keys, sizeof(root_keys) / sizeof(root_keys[0])) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(root, "p"), 3.0, 3.0, &number) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(root, "id"), 1.0, 2.0, &number)) {
        cJSON_Delete(root);
        return RADAR_INGEST_INVALID_SCHEMA;
    }
    out->local_id = (uint8_t)number;
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "t");
    if (!cJSON_IsString(type) || type->valuestring == NULL || strcmp(type->valuestring, "radar") != 0 ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(root, "u"), 0.0, UINT32_MAX, &number)) {
        cJSON_Delete(root);
        return RADAR_INGEST_INVALID_SCHEMA;
    }
    out->request_uptime_ms = (uint32_t)number;
    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(root, "q"), 1.0, UINT32_MAX, &number)) {
        cJSON_Delete(root);
        return RADAR_INGEST_INVALID_SCHEMA;
    }
    out->request_sequence = (uint32_t)number;

    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "v");
    static const char *const value_keys[] = {
        "device_id", "link_state", "sample_valid", "frame_seq", "frame_uptime_ms", "target_count", "targets"
    };
    if (!object_has_exact_keys(value, value_keys, sizeof(value_keys) / sizeof(value_keys[0]))) {
        cJSON_Delete(root);
        return RADAR_INGEST_INVALID_SCHEMA;
    }
    cJSON *device_id = cJSON_GetObjectItemCaseSensitive(value, "device_id");
    const radar_source_id_t source = radar_registry_source_for_local_id(out->local_id);
    const char *expected_device_id = radar_registry_device_id(source);
    if (!cJSON_IsString(device_id) || device_id->valuestring == NULL || source == RADAR_SOURCE_COUNT ||
        expected_device_id == NULL || strcmp(device_id->valuestring, expected_device_id) != 0) {
        cJSON_Delete(root);
        return RADAR_INGEST_IDENTITY_MISMATCH;
    }
    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(value, "link_state"), 0.0, 7.0, &number)) {
        cJSON_Delete(root);
        return RADAR_INGEST_INVALID_SCHEMA;
    }
    out->link_state = (uint8_t)number;
    double sample_valid = 0.0;
    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(value, "sample_valid"),
                               0.0, 1.0, &sample_valid) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(value, "frame_seq"), 0.0, UINT32_MAX, &number)) {
        cJSON_Delete(root);
        return RADAR_INGEST_INVALID_SCHEMA;
    }
    out->sample_valid = sample_valid != 0.0;
    out->frame_seq = (uint32_t)number;
    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(value, "frame_uptime_ms"),
                               0.0, UINT32_MAX, &number)) {
        cJSON_Delete(root);
        return RADAR_INGEST_INVALID_SCHEMA;
    }
    out->frame_uptime_ms = (uint32_t)number;
    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(value, "target_count"),
                               0.0, LD2450_MAX_TARGETS, &number)) {
        cJSON_Delete(root);
        return RADAR_INGEST_INVALID_TARGETS;
    }
    out->target_count = (uint8_t)number;
    cJSON *targets = cJSON_GetObjectItemCaseSensitive(value, "targets");
    if (!cJSON_IsArray(targets) || cJSON_GetArraySize(targets) != out->target_count) {
        cJSON_Delete(root);
        return RADAR_INGEST_INVALID_TARGETS;
    }
    for (uint8_t i = 0U; i < out->target_count; ++i) {
        const radar_ingest_result_t result = parse_target(cJSON_GetArrayItem(targets, i), &out->targets[i]);
        if (result != RADAR_INGEST_ACCEPTED) {
            cJSON_Delete(root);
            return result;
        }
        for (uint8_t previous = 0U; previous < i; ++previous) {
            if (out->targets[previous].slot == out->targets[i].slot) {
                cJSON_Delete(root);
                return RADAR_INGEST_INVALID_TARGETS;
            }
        }
    }
    if ((!out->sample_valid && out->target_count != 0U) ||
        (out->sample_valid && out->frame_seq == 0U)) {
        cJSON_Delete(root);
        return RADAR_INGEST_INVALID_TARGETS;
    }
    cJSON_Delete(root);
    return RADAR_INGEST_ACCEPTED;
}

static bool ingest_lock(void)
{
#ifdef RADAR_INGEST_HOST_TEST
    return true;
#else
    return s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(5U)) == pdTRUE;
#endif
}

static void ingest_unlock(void)
{
#ifndef RADAR_INGEST_HOST_TEST
    xSemaphoreGive(s_lock);
#endif
}

static void record_history(const radar_gateway_sample_t *sample, uint64_t now_ms)
{
    if (sample == NULL || s_history == NULL || !ingest_lock()) return;
    s_history[s_history_head] = (radar_ingest_history_entry_t){
        .recorded_at_ms = now_ms,
        .local_id = sample->local_id,
        .request_sequence = sample->request_sequence,
        .frame_seq = sample->frame_seq,
        .target_count = sample->target_count,
    };
    memcpy(s_history[s_history_head].targets, sample->targets,
           sizeof(s_history[s_history_head].targets));
    s_history_head = (uint16_t)((s_history_head + 1U) % RADAR_INGEST_HISTORY_DEPTH);
    if (s_history_count < RADAR_INGEST_HISTORY_DEPTH) ++s_history_count;
    ingest_unlock();
}

void radar_ingest_process_pending(uint64_t now_ms)
{
    if (!s_started || now_ms == 0U || !ingest_lock()) return;
    radar_ingest_pending_t pending[RADAR_GATEWAY_MAX_REMOTE_SOURCES] = {0};
    memcpy(pending, s_pending, sizeof(pending));
    memset(s_pending, 0, sizeof(s_pending));
    ingest_unlock();

    for (size_t index = 0U; index < RADAR_GATEWAY_MAX_REMOTE_SOURCES; ++index) {
        if (!pending[index].pending) continue;
        radar_gateway_output_t output = {0};
        const radar_gateway_ingest_result_t result =
            radar_gateway_ingest_admit(&pending[index].sample, 1U, now_ms, &output);
        if (result == RADAR_GATEWAY_INGEST_ACCEPTED || result == RADAR_GATEWAY_INGEST_DUPLICATE) {
            record_history(&pending[index].sample, now_ms);
        }
#ifndef RADAR_INGEST_HOST_TEST
        if (result != RADAR_GATEWAY_INGEST_ACCEPTED && result != RADAR_GATEWAY_INGEST_DUPLICATE) {
            ESP_LOGW(TAG, "RADAR_REMOTE_RADAR_DROP reason=%s",
                     radar_gateway_ingest_result_name(result));
        }
#else
        (void)result;
#endif
    }
    radar_gateway_ingest_poll(now_ms);
}

#ifndef RADAR_INGEST_HOST_TEST
static uint64_t now_ms(void)
{
    const int64_t now_us = esp_timer_get_time();
    return now_us > 0 ? (uint64_t)(now_us / 1000) : 1U;
}

static void radar_worker_task(void *arg)
{
    (void)arg;
    while (true) {
        radar_ingest_process_pending(now_ms());
        vTaskDelay(pdMS_TO_TICKS(RADAR_INGEST_WORKER_PERIOD_MS));
    }
}

static void radar_ingest_rollback_start(void)
{
    if (s_worker != NULL) {
        if (s_worker_with_caps) {
            vTaskDeleteWithCaps(s_worker);
        } else {
            vTaskDelete(s_worker);
        }
        s_worker = NULL;
        s_worker_with_caps = false;
    }
    if (s_lock != NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    heap_caps_free(s_history);
    s_history = NULL;
}
#endif

esp_err_t radar_ingest_start(void)
{
    if (s_started) return ESP_OK;
    memset(s_pending, 0, sizeof(s_pending));
    s_history_head = 0U;
    s_history_count = 0U;
#ifndef RADAR_INGEST_HOST_TEST
    s_history = heap_caps_calloc(RADAR_INGEST_HISTORY_DEPTH, sizeof(*s_history),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_history == NULL) return ESP_ERR_NO_MEM;
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    if (s_lock == NULL) {
        heap_caps_free(s_history);
        s_history = NULL;
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_FREERTOS_UNICORE
    BaseType_t created = xTaskCreateWithCaps(radar_worker_task,
                                             "radar_worker",
                                             RADAR_INGEST_WORKER_STACK,
                                             NULL,
                                             RADAR_INGEST_WORKER_PRIORITY,
                                             &s_worker,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(radar_worker_task,
                                                         "radar_worker",
                                                         RADAR_INGEST_WORKER_STACK,
                                                         NULL,
                                                         RADAR_INGEST_WORKER_PRIORITY,
                                                         &s_worker,
                                                         1,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (created != pdPASS) {
        ESP_LOGW(TAG, "PSRAM task stack allocation failed; falling back to internal RAM");
#if CONFIG_FREERTOS_UNICORE
        created = xTaskCreate(radar_worker_task,
                              "radar_worker",
                              RADAR_INGEST_WORKER_STACK,
                              NULL,
                              RADAR_INGEST_WORKER_PRIORITY,
                              &s_worker);
#else
        created = xTaskCreatePinnedToCore(radar_worker_task,
                                          "radar_worker",
                                          RADAR_INGEST_WORKER_STACK,
                                          NULL,
                                          RADAR_INGEST_WORKER_PRIORITY,
                                          &s_worker,
                                          1);
#endif
        s_worker_with_caps = false;
    } else {
        s_worker_with_caps = true;
    }
    if (created != pdPASS) {
        s_worker = NULL;
        radar_ingest_rollback_start();
        return ESP_ERR_NO_MEM;
    }
#endif
#ifdef RADAR_INGEST_HOST_TEST
    s_history = s_history_host;
    memset(s_history, 0, sizeof(s_history_host));
#endif
    if (radar_gateway_ingest_start() != ESP_OK) {
#ifndef RADAR_INGEST_HOST_TEST
        radar_ingest_rollback_start();
#else
        s_history = NULL;
#endif
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

bool radar_ingest_history_get_stats(radar_ingest_history_stats_t *out)
{
    if (out == NULL || !s_started || !ingest_lock()) return false;
    *out = (radar_ingest_history_stats_t){
        .count = s_history_count,
        .capacity = RADAR_INGEST_HISTORY_DEPTH,
        .psram_backed = s_history != NULL,
    };
    ingest_unlock();
    return true;
}

radar_ingest_result_t radar_ingest_process_json(const char *json,
                                                size_t json_len,
                                                uint64_t received_at_ms)
{
    (void)received_at_ms;
    radar_gateway_sample_t sample = {0};
    const radar_ingest_result_t result = parse_json(json, json_len, &sample);
#ifndef RADAR_INGEST_HOST_TEST
    ESP_LOGI(TAG,
             "RADAR_REMOTE_RADAR_RX content_length=%u source=%s local_id=%u",
             (unsigned int)json_len,
             source_name(sample.local_id),
             (unsigned int)sample.local_id);
#endif
    if (result != RADAR_INGEST_ACCEPTED) {
#ifndef RADAR_INGEST_HOST_TEST
        ESP_LOGW(TAG, "RADAR_REMOTE_RADAR_DROP reason=%s", radar_ingest_result_name(result));
#endif
        return result;
    }
#ifndef RADAR_INGEST_HOST_TEST
    ESP_LOGI(TAG,
             "RADAR_HTTP_INGEST source=%s local_id=%u targets=%u payload_size=%u",
             source_name(sample.local_id),
             (unsigned int)sample.local_id,
             (unsigned int)sample.target_count,
             (unsigned int)json_len);
#endif
    const size_t index = sample.local_id - 1U;
    if (!s_started || index >= RADAR_GATEWAY_MAX_REMOTE_SOURCES || !ingest_lock()) {
#ifndef RADAR_INGEST_HOST_TEST
        ESP_LOGW(TAG, "RADAR_REMOTE_RADAR_DROP reason=%s",
                 radar_ingest_result_name(RADAR_INGEST_UNAVAILABLE));
#endif
        return RADAR_INGEST_UNAVAILABLE;
    }
    s_pending[index] = (radar_ingest_pending_t){.pending = true, .sample = sample};
    ingest_unlock();
#ifndef RADAR_INGEST_HOST_TEST
    ESP_LOGI(TAG,
             "RADAR_REMOTE_RADAR_ACCEPT source=%s targets=%u seq=%lu",
             source_name(sample.local_id),
             (unsigned int)sample.target_count,
             (unsigned long)sample.request_sequence);
#endif
    return RADAR_INGEST_ACCEPTED;
}

const char *radar_ingest_result_name(radar_ingest_result_t result)
{
    switch (result) {
    case RADAR_INGEST_ACCEPTED: return "accepted";
    case RADAR_INGEST_INVALID_ARGUMENT: return "invalid_argument";
    case RADAR_INGEST_TOO_LARGE: return "payload_too_large";
    case RADAR_INGEST_INVALID_JSON: return "invalid_json";
    case RADAR_INGEST_INVALID_SCHEMA: return "invalid_schema";
    case RADAR_INGEST_INVALID_LOCAL_ID: return "invalid_local_id";
    case RADAR_INGEST_IDENTITY_MISMATCH: return "identity_mismatch";
    case RADAR_INGEST_INVALID_TARGETS: return "invalid_targets";
    case RADAR_INGEST_UNAVAILABLE: return "unavailable";
    default: return "unknown";
    }
}

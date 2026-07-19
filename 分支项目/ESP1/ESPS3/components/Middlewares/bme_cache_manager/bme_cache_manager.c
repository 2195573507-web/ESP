/**
 * @file bme_cache_manager.c
 * @brief ESPS3 BME690 RAM ring buffer.
 */

#include "bme_cache_manager.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "cJSON.h"
#include "esp111_protocol_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "bme_cache_manager";

#ifndef BME_CACHE_LOG_INTERVAL_MS
#define BME_CACHE_LOG_INTERVAL_MS 5000LL
#endif

typedef struct {
    bme_cache_record_t records[BME_CACHE_MANAGER_CAPACITY];
    size_t head;
    size_t count;
    uint32_t next_sequence;
    uint32_t overwritten_count;
    int64_t last_push_log_ms;
    int64_t last_size_log_ms;
} bme_cache_state_t;

static bme_cache_state_t s_cache;
static SemaphoreHandle_t s_lock;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void free_record_storage(bme_cache_record_t *record)
{
    if (record == NULL) {
        return;
    }
    if (record->server_json != NULL) {
        cJSON_free(record->server_json);
        record->server_json = NULL;
    }
}

static void clear_record(bme_cache_record_t *record)
{
    free_record_storage(record);
    memset(record, 0, sizeof(*record));
}

static bool log_due(int64_t *last_log_ms)
{
    if (last_log_ms == NULL) {
        return false;
    }
    const int64_t timestamp_ms = now_ms();
    if (*last_log_ms != 0 && timestamp_ms - *last_log_ms < BME_CACHE_LOG_INTERVAL_MS) {
        return false;
    }
    *last_log_ms = timestamp_ms;
    return true;
}

static cJSON *json_object(cJSON *root, const char *key)
{
    cJSON *value = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, key) : NULL;
    return cJSON_IsObject(value) ? value : NULL;
}

static bool json_number(cJSON *root, const char *key, double *out)
{
    if (root == NULL || key == NULL || out == NULL) {
        return false;
    }
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble)) {
        return false;
    }
    *out = value->valuedouble;
    return true;
}

static double json_number_or(cJSON *root, const char *key, double fallback)
{
    double value = fallback;
    (void)json_number(root, key, &value);
    return value;
}

static const char *json_string(cJSON *root, const char *key)
{
    cJSON *value = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, key) : NULL;
    return cJSON_IsString(value) && value->valuestring != NULL ? value->valuestring : "";
}

static const char *json_string_first(cJSON *root, const char *primary, const char *fallback)
{
    const char *value = json_string(root, primary);
    return value[0] != '\0' ? value : json_string(root, fallback);
}

static bool duplicate_json(const char *json, char **out)
{
    if (json == NULL || out == NULL) {
        return false;
    }
    const size_t len = strlen(json);
    char *copy = cJSON_malloc(len + 1U);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, json, len + 1U);
    *out = copy;
    return true;
}

static bool parse_record_from_json(const char *server_json, bme_cache_record_t *out_record)
{
    if (server_json == NULL || server_json[0] == '\0' || out_record == NULL) {
        return false;
    }

    cJSON *root = cJSON_Parse(server_json);
    if (root == NULL) {
        return false;
    }

    cJSON *payload = json_object(root, ESP111_PROTOCOL_JSON_PAYLOAD);
    const char *payload_type = json_string(root, ESP111_PROTOCOL_JSON_PAYLOAD_TYPE);
    const char *device_id = json_string(root, ESP111_PROTOCOL_JSON_DEVICE_ID);
    if (payload == NULL ||
        strcmp(payload_type, ESP111_PROTOCOL_MSG_SENSOR_BME690) != 0 ||
        device_id[0] == '\0') {
        cJSON_Delete(root);
        return false;
    }

    bme_cache_record_t parsed = {0};
    strlcpy(parsed.device_id, device_id, sizeof(parsed.device_id));
    parsed.timestamp_ms = (int64_t)json_number_or(root,
                                                  ESP111_PROTOCOL_JSON_TIMESTAMP_MS,
                                                  0.0);
    if (parsed.timestamp_ms <= 0) {
        parsed.timestamp_ms = (int64_t)json_number_or(payload,
                                                      ESP111_PROTOCOL_JSON_TIMESTAMP_MS,
                                                      0.0);
    }
    if (parsed.timestamp_ms <= 0) {
        parsed.timestamp_ms = (int64_t)json_number_or(root,
                                                      ESP111_PROTOCOL_JSON_UPTIME_MS,
                                                      0.0);
    }
    if (parsed.timestamp_ms <= 0) {
        parsed.timestamp_ms = now_ms();
    }

    bool ok =
        json_number(payload, "temperature_c", &parsed.temperature_c) &&
        json_number(payload, "humidity_percent", &parsed.humidity_percent) &&
        json_number(payload, "pressure_hpa", &parsed.pressure_hpa) &&
        json_number(payload, "gas_resistance_ohm", &parsed.gas_resistance_ohm) &&
        json_number(payload, "air_quality_score", &parsed.air_quality_score);
    parsed.gas_baseline_ohm = json_number_or(payload, "gas_baseline_ohm", 0.0);
    parsed.gas_ratio = json_number_or(payload, "gas_ratio", 0.0);
    parsed.gas_score = json_number_or(payload, "gas_score", parsed.air_quality_score);
    parsed.humidity_score = json_number_or(payload, "humidity_score", 0.0);
    parsed.sample_count = (uint32_t)json_number_or(payload, "sample_count", 0.0);

    strlcpy(parsed.level,
            json_string_first(payload, "level", "air_quality_level"),
            sizeof(parsed.level));
    strlcpy(parsed.confidence,
            json_string_first(payload, "confidence", "air_quality_confidence"),
            sizeof(parsed.confidence));
    strlcpy(parsed.algorithm_version,
            json_string_first(payload, "algorithm_version", "air_quality_algo_version"),
            sizeof(parsed.algorithm_version));

    const bool fields_valid =
        ok &&
        parsed.timestamp_ms > 0 &&
        parsed.humidity_percent >= 0.0 &&
        parsed.humidity_percent <= 100.0 &&
        parsed.pressure_hpa >= 0.0 &&
        parsed.gas_resistance_ohm >= 0.0 &&
        parsed.air_quality_score >= 0.0 &&
        parsed.air_quality_score <= 100.0 &&
        parsed.gas_baseline_ohm >= 0.0 &&
        parsed.gas_ratio >= 0.0 &&
        parsed.gas_score >= 0.0 &&
        parsed.gas_score <= 100.0 &&
        parsed.humidity_score >= 0.0 &&
        parsed.humidity_score <= 100.0 &&
        parsed.level[0] != '\0' &&
        parsed.confidence[0] != '\0' &&
        parsed.algorithm_version[0] != '\0';

    cJSON_Delete(root);
    if (!fields_valid) {
        return false;
    }

    *out_record = parsed;
    return true;
}

static esp_err_t copy_record_for_output(const bme_cache_record_t *src,
                                        bme_cache_record_t *dst)
{
    if (src == NULL || dst == NULL || src->server_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(dst, 0, sizeof(*dst));
    *dst = *src;
    dst->server_json = NULL;
    if (!duplicate_json(src->server_json, &dst->server_json)) {
        memset(dst, 0, sizeof(*dst));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t bme_cache_manager_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < BME_CACHE_MANAGER_CAPACITY; ++i) {
        clear_record(&s_cache.records[i]);
    }
    memset(&s_cache, 0, sizeof(s_cache));
    s_cache.next_sequence = 1U;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG,
             "BME cache initialized capacity=%u",
             (unsigned int)BME_CACHE_MANAGER_CAPACITY);
    return ESP_OK;
}

esp_err_t bme_cache_manager_push_json(const char *server_json,
                                      uint32_t *out_sequence,
                                      size_t *out_size)
{
    if (server_json == NULL || server_json[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bme_cache_record_t parsed = {0};
    if (!parse_record_from_json(server_json, &parsed)) {
        ESP_LOGW(TAG, "BME_CACHE_PUSH rejected reason=invalid_bme_payload");
        return ESP_ERR_INVALID_ARG;
    }
    if (!duplicate_json(server_json, &parsed.server_json)) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_cache.next_sequence == 0U) {
        s_cache.next_sequence = 1U;
    }
    parsed.sequence = s_cache.next_sequence++;

    size_t index = 0;
    if (s_cache.count == BME_CACHE_MANAGER_CAPACITY) {
        index = s_cache.head;
        clear_record(&s_cache.records[index]);
        s_cache.head = (s_cache.head + 1U) % BME_CACHE_MANAGER_CAPACITY;
        ++s_cache.overwritten_count;
    } else {
        index = (s_cache.head + s_cache.count) % BME_CACHE_MANAGER_CAPACITY;
        ++s_cache.count;
    }
    s_cache.records[index] = parsed;
    parsed.server_json = NULL;

    if (out_sequence != NULL) {
        *out_sequence = s_cache.records[index].sequence;
    }
    if (out_size != NULL) {
        *out_size = s_cache.count;
    }

    if (log_due(&s_cache.last_push_log_ms)) {
        ESP_LOGI(TAG,
                 "BME_CACHE_PUSH seq=%lu device_id=%s timestamp_ms=%lld size=%u",
                 (unsigned long)s_cache.records[index].sequence,
                 s_cache.records[index].device_id,
                 (long long)s_cache.records[index].timestamp_ms,
                 (unsigned int)s_cache.count);
    }
    if (log_due(&s_cache.last_size_log_ms)) {
        ESP_LOGI(TAG,
                 "BME_CACHE_SIZE size=%u capacity=%u overwritten=%lu",
                 (unsigned int)s_cache.count,
                 (unsigned int)BME_CACHE_MANAGER_CAPACITY,
                 (unsigned long)s_cache.overwritten_count);
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t bme_cache_manager_peek_oldest(bme_cache_record_t *out_record)
{
    if (out_record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_cache.count == 0U) {
        xSemaphoreGive(s_lock);
        memset(out_record, 0, sizeof(*out_record));
        return ESP_ERR_NOT_FOUND;
    }
    if (s_cache.records[s_cache.head].in_flight) {
        xSemaphoreGive(s_lock);
        memset(out_record, 0, sizeof(*out_record));
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = copy_record_for_output(&s_cache.records[s_cache.head], out_record);
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t bme_cache_manager_peek_oldest_for_device(const char *device_id,
                                                   bme_cache_record_t *out_record)
{
    if (device_id == NULL || device_id[0] == '\0' || out_record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_record, 0, sizeof(*out_record));
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t offset = 0; offset < s_cache.count; ++offset) {
        const size_t index = (s_cache.head + offset) % BME_CACHE_MANAGER_CAPACITY;
        const bme_cache_record_t *record = &s_cache.records[index];
        if (strcmp(record->device_id, device_id) != 0) {
            continue;
        }
        if (record->in_flight) {
            ret = ESP_ERR_INVALID_STATE;
        } else {
            ret = copy_record_for_output(record, out_record);
        }
        break;
    }
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t bme_cache_manager_delete_oldest(uint32_t sequence)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_cache.count == 0U || s_cache.records[s_cache.head].sequence != sequence) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    clear_record(&s_cache.records[s_cache.head]);
    s_cache.head = (s_cache.head + 1U) % BME_CACHE_MANAGER_CAPACITY;
    --s_cache.count;
    if (log_due(&s_cache.last_size_log_ms) || s_cache.count == 0U) {
        ESP_LOGI(TAG,
                 "BME_CACHE_SIZE size=%u capacity=%u overwritten=%lu",
                 (unsigned int)s_cache.count,
                 (unsigned int)BME_CACHE_MANAGER_CAPACITY,
                 (unsigned long)s_cache.overwritten_count);
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t bme_cache_manager_delete_sequence(uint32_t sequence)
{
    if (sequence == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_cache.count == 0U) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    size_t found_offset = BME_CACHE_MANAGER_CAPACITY;
    for (size_t offset = 0; offset < s_cache.count; ++offset) {
        size_t index = (s_cache.head + offset) % BME_CACHE_MANAGER_CAPACITY;
        if (s_cache.records[index].sequence == sequence) {
            found_offset = offset;
            break;
        }
    }

    if (found_offset != BME_CACHE_MANAGER_CAPACITY) {
        size_t delete_index = (s_cache.head + found_offset) % BME_CACHE_MANAGER_CAPACITY;
        clear_record(&s_cache.records[delete_index]);
        for (size_t offset = found_offset; offset + 1U < s_cache.count; ++offset) {
            size_t current = (s_cache.head + offset) % BME_CACHE_MANAGER_CAPACITY;
            size_t next = (s_cache.head + offset + 1U) % BME_CACHE_MANAGER_CAPACITY;
            s_cache.records[current] = s_cache.records[next];
            memset(&s_cache.records[next], 0, sizeof(s_cache.records[next]));
        }
        --s_cache.count;
        if (log_due(&s_cache.last_size_log_ms) || s_cache.count == 0U) {
            ESP_LOGI(TAG,
                     "BME_CACHE_SIZE size=%u capacity=%u overwritten=%lu",
                     (unsigned int)s_cache.count,
                     (unsigned int)BME_CACHE_MANAGER_CAPACITY,
                     (unsigned long)s_cache.overwritten_count);
        }
    }

    xSemaphoreGive(s_lock);
    return found_offset != BME_CACHE_MANAGER_CAPACITY ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t bme_cache_manager_mark_in_flight(uint32_t sequence, bool in_flight)
{
    if (sequence == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t offset = 0; offset < s_cache.count; ++offset) {
        size_t index = (s_cache.head + offset) % BME_CACHE_MANAGER_CAPACITY;
        if (s_cache.records[index].sequence == sequence) {
            s_cache.records[index].in_flight = in_flight;
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

void bme_cache_manager_release_record(bme_cache_record_t *record)
{
    if (record == NULL) {
        return;
    }
    free_record_storage(record);
    memset(record, 0, sizeof(*record));
}

size_t bme_cache_manager_size(void)
{
    if (s_lock == NULL) {
        return 0U;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t size = s_cache.count;
    xSemaphoreGive(s_lock);
    return size;
}

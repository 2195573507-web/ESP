#include "environment_alarm_adapter.h"
#include "environment_alarm_sequence.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "cJSON.h"
#include "environment_alarm_engine.h"
#include "environment_alarm_reporter.h"
#include "esp111_protocol_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define ENV_ALARM_SOURCE_COUNT 2U
#define ENV_ALARM_SAMPLE_LOG_INTERVAL_MS 60000U
#define ENV_ALARM_COMPAT_RESTART_SILENCE_MS 60000U

typedef struct {
    bool initialized;
    alarm_device_id_t engine_device;
    bool has_boot_id;
    uint32_t last_boot_id;
    bool has_remote_seq;
    uint32_t last_remote_seq;
    uint64_t local_ingest_seq;
    uint64_t last_receive_monotonic_ms;
    uint64_t last_valid_sample_ms;
    uint64_t duplicate_count;
    uint64_t out_of_order_count;
    uint64_t sequence_wrap_count;
    uint64_t restart_count;
    uint64_t invalid_count;
    uint64_t missing_field_count;
    uint64_t samples_received;
    uint64_t samples_valid;
    uint64_t last_sample_log_ms;
} environment_alarm_source_state_t;

static const char *TAG = "ENV_ALARM_ADAPTER";
static SemaphoreHandle_t s_lock;
static bool s_initialized;
static environment_alarm_source_state_t s_sources[ENV_ALARM_SOURCE_COUNT] = {
    {.engine_device = ALARM_DEVICE_C51},
    {.engine_device = ALARM_DEVICE_C52},
};

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static environment_alarm_source_state_t *source_for_device(const char *device_id)
{
    if (device_id == NULL) {
        return NULL;
    }
    if (strcmp(device_id, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51) == 0) {
        return &s_sources[0];
    }
    if (strcmp(device_id, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52) == 0) {
        return &s_sources[1];
    }
    return NULL;
}

static bool json_number(cJSON *object, const char *key, float *out)
{
    cJSON *value = object != NULL ? cJSON_GetObjectItemCaseSensitive(object, key) : NULL;
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || out == NULL) {
        return false;
    }
    *out = (float)value->valuedouble;
    return isfinite(*out);
}

static bool json_u32(cJSON *object, const char *key, uint32_t *out)
{
    cJSON *value = object != NULL ? cJSON_GetObjectItemCaseSensitive(object, key) : NULL;
    if (!cJSON_IsNumber(value) || value->valuedouble < 0.0 ||
        value->valuedouble > (double)UINT32_MAX || out == NULL) {
        return false;
    }
    *out = (uint32_t)value->valuedouble;
    return true;
}

static bool json_u64(cJSON *object, const char *key, uint64_t *out)
{
    cJSON *value = object != NULL ? cJSON_GetObjectItemCaseSensitive(object, key) : NULL;
    if (!cJSON_IsNumber(value) || value->valuedouble < 0.0 || out == NULL) {
        return false;
    }
    *out = (uint64_t)value->valuedouble;
    return true;
}

static const char *json_string(cJSON *object, const char *key)
{
    cJSON *value = object != NULL ? cJSON_GetObjectItemCaseSensitive(object, key) : NULL;
    return cJSON_IsString(value) && value->valuestring != NULL ? value->valuestring : NULL;
}

static bool in_range(float value, float low, float high)
{
    return isfinite(value) && value >= low && value <= high;
}

static alarm_air_quality_level_t map_air_quality_level(const char *value)
{
    if (value == NULL) return ALARM_AIR_QUALITY_UNKNOWN;
    if (strcmp(value, "excellent") == 0) return ALARM_AIR_QUALITY_EXCELLENT;
    if (strcmp(value, "good") == 0) return ALARM_AIR_QUALITY_GOOD;
    if (strcmp(value, "moderate") == 0) return ALARM_AIR_QUALITY_MODERATE;
    if (strcmp(value, "poor") == 0) return ALARM_AIR_QUALITY_POOR;
    if (strcmp(value, "bad") == 0) return ALARM_AIR_QUALITY_BAD;
    return ALARM_AIR_QUALITY_UNKNOWN;
}

static alarm_sensor_state_t map_sensor_state(const char *value)
{
    if (value == NULL) return ALARM_SENSOR_UNKNOWN;
    if (strcmp(value, "WARMUP") == 0) return ALARM_SENSOR_WARMUP;
    if (strcmp(value, "READY") == 0) return ALARM_SENSOR_READY;
    if (strcmp(value, "DEGRADED") == 0) return ALARM_SENSOR_DEGRADED;
    return ALARM_SENSOR_UNKNOWN;
}

static bool payload_time_synced(cJSON *payload)
{
    cJSON *value = payload != NULL ? cJSON_GetObjectItemCaseSensitive(payload, "time_synced") : NULL;
    return cJSON_IsTrue(value);
}

static void map_sample(const protocol_adapter_envelope_t *envelope,
                       alarm_environment_sample_t *sample)
{
    memset(sample, 0, sizeof(*sample));
    sample->struct_version = ALARM_ENVIRONMENT_SAMPLE_VERSION;
    sample->remote_seq = envelope->seq;
    strlcpy(sample->room_id, envelope->room_id, sizeof(sample->room_id));
    strlcpy(sample->source, "c5_bme690", sizeof(sample->source));

    cJSON *payload = envelope->payload;
    cJSON *air_quality = payload != NULL ? cJSON_GetObjectItemCaseSensitive(payload, "air_quality") : NULL;
    if (!cJSON_IsObject(air_quality)) {
        air_quality = payload != NULL ? cJSON_GetObjectItemCaseSensitive(payload, "air_quality_json") : NULL;
    }

    float value = 0.0f;
    if (json_number(payload, "temperature_c", &value) && in_range(value, -40.0f, 85.0f)) {
        sample->temperature = value;
        sample->valid_fields |= ALARM_FIELD_TEMPERATURE;
    }
    if (json_number(payload, "humidity_percent", &value) && in_range(value, 0.0f, 100.0f)) {
        sample->humidity = value;
        sample->valid_fields |= ALARM_FIELD_HUMIDITY;
    }
    if (json_number(payload, "pressure_hpa", &value) && in_range(value, 300.0f, 1100.0f)) {
        sample->pressure = value;
        sample->valid_fields |= ALARM_FIELD_PRESSURE;
    }
    if (json_number(payload, "gas_resistance_ohm", &value) && in_range(value, 0.000001f, 1000000000.0f)) {
        sample->gas_resistance = value;
        sample->valid_fields |= ALARM_FIELD_GAS_RESISTANCE;
    }
    value = 0.0f;
    const bool has_score = json_number(air_quality, "score", &value) ||
                           json_number(payload, "air_quality_score", &value);
    if (has_score && in_range(value, 0.0f, 100.0f)) {
        sample->air_quality_score = value;
        sample->valid_fields |= ALARM_FIELD_AIR_QUALITY_SCORE;
    }
    const char *level = json_string(air_quality, "level");
    if (level == NULL) level = json_string(payload, "air_quality_level");
    sample->air_quality_level = map_air_quality_level(level);
    if (sample->air_quality_level != ALARM_AIR_QUALITY_UNKNOWN) {
        sample->valid_fields |= ALARM_FIELD_AIR_QUALITY_LEVEL;
    }
    value = 0.0f;
    const bool has_gas_ratio = json_number(air_quality, "gas_ratio", &value) ||
                               json_number(payload, "gas_ratio", &value);
    if (has_gas_ratio && in_range(value, 0.000001f, 10.0f)) {
        sample->gas_ratio = value;
        sample->valid_fields |= ALARM_FIELD_GAS_RATIO;
    }
    if (json_number(air_quality, "stability_score", &value) && in_range(value, 0.0f, 1.0f)) {
        sample->stability_score = value;
        sample->valid_fields |= ALARM_FIELD_STABILITY_SCORE;
    }
    sample->sensor_state = map_sensor_state(json_string(air_quality, "sensor_state"));

    uint32_t boot_id = 0U;
    if (json_u32(payload, "boot_id", &boot_id) && boot_id != 0U) {
        sample->boot_id_valid = true;
        sample->boot_id = boot_id;
    }

    uint64_t sample_time = 0U;
    if (payload_time_synced(payload) && json_u64(payload, "sample_time_ms", &sample_time) && sample_time > 0U) {
        sample->timestamp_valid = true;
        sample->timestamp_ms = sample_time;
    }
}

static bool sample_has_usable_field(const alarm_environment_sample_t *sample)
{
    return (sample->valid_fields & (ALARM_FIELD_TEMPERATURE |
                                    ALARM_FIELD_HUMIDITY |
                                    ALARM_FIELD_AIR_QUALITY_SCORE)) != 0U;
}

esp_err_t environment_alarm_adapter_init(void)
{
    if (s_initialized) return ESP_OK;
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = alarm_engine_init(NULL);
    if (ret != ESP_OK) return ret;
    ret = environment_alarm_reporter_init();
    if (ret != ESP_OK) return ret;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_sources, 0, sizeof(s_sources));
    s_sources[0].engine_device = ALARM_DEVICE_C51;
    s_sources[1].engine_device = ALARM_DEVICE_C52;
    s_initialized = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t environment_alarm_adapter_ingest(const protocol_adapter_envelope_t *envelope)
{
    if (!s_initialized || s_lock == NULL) return ESP_ERR_INVALID_STATE;
    if (envelope == NULL ||
        protocol_adapter_message_kind(envelope->message_type) != PROTOCOL_ADAPTER_MESSAGE_SENSOR_BME690) {
        return ESP_ERR_INVALID_ARG;
    }

    environment_alarm_source_state_t *source = source_for_device(envelope->device_id);
    if (source == NULL) return ESP_ERR_NOT_FOUND;

    alarm_environment_sample_t sample;
    map_sample(envelope, &sample);
    sample.device_id = source->engine_device;
    const uint64_t received_ms = now_ms();
    bool boot_changed = false;
    esp_err_t result = ESP_OK;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    ++source->samples_received;
    const uint64_t previous_receive_ms = source->last_receive_monotonic_ms;
    source->last_receive_monotonic_ms = received_ms;
    if (!sample_has_usable_field(&sample)) {
        ++source->invalid_count;
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    if (sample.valid_fields != (ALARM_FIELD_TEMPERATURE | ALARM_FIELD_HUMIDITY |
                                ALARM_FIELD_PRESSURE | ALARM_FIELD_GAS_RESISTANCE |
                                ALARM_FIELD_AIR_QUALITY_SCORE | ALARM_FIELD_AIR_QUALITY_LEVEL |
                                ALARM_FIELD_GAS_RATIO | ALARM_FIELD_STABILITY_SCORE)) {
        ++source->invalid_count;
        ++source->missing_field_count;
    }

    if (sample.boot_id_valid) {
        if (source->initialized && source->has_boot_id && source->last_boot_id != sample.boot_id) {
            boot_changed = true;
        }
    }
    if (boot_changed) {
        result = alarm_engine_reset(false, source->engine_device);
        if (result != ESP_OK) {
            xSemaphoreGive(s_lock);
            return result;
        }
        ++source->restart_count;
        source->has_remote_seq = false;
        source->has_boot_id = true;
        source->last_boot_id = sample.boot_id;
    }
    if (!sample.boot_id_valid && source->has_remote_seq) {
        const environment_alarm_sequence_result_t candidate = environment_alarm_sequence_classify(sample.remote_seq,
                                                                                                    source->last_remote_seq);
        if (candidate == ENVIRONMENT_ALARM_SEQUENCE_OUT_OF_ORDER &&
            previous_receive_ms > 0U &&
            received_ms - previous_receive_ms >= ENV_ALARM_COMPAT_RESTART_SILENCE_MS &&
            sample.remote_seq < 1024U) {
            result = alarm_engine_reset(false, source->engine_device);
            if (result != ESP_OK) {
                xSemaphoreGive(s_lock);
                return result;
            }
            boot_changed = true;
            ++source->restart_count;
            source->has_remote_seq = false;
        }
    }

    if (source->has_remote_seq) {
        const environment_alarm_sequence_result_t sequence = environment_alarm_sequence_classify(sample.remote_seq,
                                                                                                   source->last_remote_seq);
        if (sequence == ENVIRONMENT_ALARM_SEQUENCE_DUPLICATE) {
            ++source->duplicate_count;
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
        if (sequence == ENVIRONMENT_ALARM_SEQUENCE_OUT_OF_ORDER) {
            ++source->out_of_order_count;
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
        if (sequence == ENVIRONMENT_ALARM_SEQUENCE_WRAP) {
            ++source->sequence_wrap_count;
        }
    }

    if (source->local_ingest_seq == UINT64_MAX) {
        ++source->invalid_count;
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    sample.local_ingest_seq = source->local_ingest_seq + 1U;
    sample.ingest_seq = sample.local_ingest_seq;
    result = alarm_engine_update(&sample, NULL);
    if (result == ESP_OK) {
        source->initialized = true;
        source->has_boot_id = sample.boot_id_valid;
        source->last_boot_id = sample.boot_id;
        source->has_remote_seq = true;
        source->last_remote_seq = sample.remote_seq;
        source->local_ingest_seq = sample.local_ingest_seq;
        source->last_valid_sample_ms = received_ms;
        ++source->samples_valid;
        if (source->last_sample_log_ms == 0U ||
            received_ms - source->last_sample_log_ms >= ENV_ALARM_SAMPLE_LOG_INTERVAL_MS) {
            source->last_sample_log_ms = received_ms;
            ESP_LOGI(TAG,
                     "ENV_ALARM_SAMPLE device=%s source=%s sensor_state=%d stability_score=%.3f local_seq=%" PRIu64 " remote_seq=%" PRIu32 " boot_changed=%d valid_fields=0x%02" PRIx32 " update_result=ok",
                     envelope->device_id,
                     sample.source,
                     (int)sample.sensor_state,
                     (double)sample.stability_score,
                     sample.local_ingest_seq,
                     sample.remote_seq,
                     boot_changed ? 1 : 0,
                     sample.valid_fields);
        }
    }
    xSemaphoreGive(s_lock);
    if (result != ESP_OK) return result;
    return environment_alarm_reporter_drain_engine();
}

esp_err_t environment_alarm_adapter_get_stats(environment_alarm_adapter_stats_t *out)
{
    if (out == NULL || s_lock == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->ready = s_initialized;
    for (size_t index = 0U; index < ENV_ALARM_SOURCE_COUNT; ++index) {
        const environment_alarm_source_state_t *source = &s_sources[index];
        out->samples_received += source->samples_received;
        out->samples_valid += source->samples_valid;
        out->samples_invalid += source->invalid_count;
        out->duplicates += source->duplicate_count;
        out->out_of_order += source->out_of_order_count;
        out->sequence_wraps += source->sequence_wrap_count;
        out->restarts += source->restart_count;
        out->missing_field += source->missing_field_count;
    }
    xSemaphoreGive(s_lock);
    return s_initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}

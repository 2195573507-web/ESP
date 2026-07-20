#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ALARM_ENVIRONMENT_SAMPLE_VERSION 2U
#define ALARM_ENGINE_MAX_DEVICES 4U
#define ALARM_ENGINE_MAX_EVENTS 64U
#define ALARM_ENGINE_MAX_EVENTS_PER_UPDATE 16U
#define ALARM_ENGINE_MAX_ACTIVE_ALARMS 13U
#define ALARM_ENVIRONMENT_ROOM_ID_LEN 32U
#define ALARM_ENVIRONMENT_SOURCE_LEN 24U

typedef enum {
    ALARM_FIELD_TEMPERATURE = 1U << 0,
    ALARM_FIELD_HUMIDITY = 1U << 1,
    ALARM_FIELD_PRESSURE = 1U << 2,
    ALARM_FIELD_GAS_RESISTANCE = 1U << 3,
    ALARM_FIELD_AIR_QUALITY_SCORE = 1U << 4,
    ALARM_FIELD_AIR_QUALITY_LEVEL = 1U << 5,
    ALARM_FIELD_GAS_RATIO = 1U << 6,
    ALARM_FIELD_STABILITY_SCORE = 1U << 7,
} alarm_environment_field_t;

typedef enum { ALARM_DEVICE_C51 = 0, ALARM_DEVICE_C52, ALARM_DEVICE_INVALID } alarm_device_id_t;
typedef enum { ALARM_AIR_QUALITY_EXCELLENT = 0, ALARM_AIR_QUALITY_GOOD, ALARM_AIR_QUALITY_MODERATE,
               ALARM_AIR_QUALITY_POOR, ALARM_AIR_QUALITY_BAD, ALARM_AIR_QUALITY_UNKNOWN } alarm_air_quality_level_t;
typedef enum {
    ALARM_SENSOR_WARMUP = 0,
    ALARM_SENSOR_READY,
    ALARM_SENSOR_DEGRADED,
    ALARM_SENSOR_UNKNOWN,
} alarm_sensor_state_t;
typedef enum { ALARM_TYPE_HIGH_TEMPERATURE = 0, ALARM_TYPE_LOW_TEMPERATURE,
               ALARM_TYPE_FAST_TEMPERATURE_CHANGE, ALARM_TYPE_HIGH_HUMIDITY,
               ALARM_TYPE_LOW_HUMIDITY, ALARM_TYPE_FAST_HUMIDITY_CHANGE,
               ALARM_TYPE_AIR_QUALITY_WARNING, ALARM_TYPE_AIR_QUALITY_CRITICAL,
               ALARM_TYPE_AIR_QUALITY_DETERIORATING, ALARM_TYPE_POLLUTION_SPIKE,
               ALARM_TYPE_ENVIRONMENT_UNSTABLE, ALARM_TYPE_SENSOR_DEGRADED,
               ALARM_TYPE_CRITICAL_ENVIRONMENT, ALARM_TYPE_COUNT } alarm_type_t;
typedef enum { ALARM_LEVEL_WARNING = 0, ALARM_LEVEL_HIGH, ALARM_LEVEL_CRITICAL } alarm_level_t;
typedef enum { ALARM_STATUS_ACTIVE = 0, ALARM_STATUS_RECOVERED } alarm_event_status_t;
typedef enum { ALARM_REASON_THRESHOLD_CONFIRMED = 0, ALARM_REASON_RECOVERY_CONFIRMED,
               ALARM_REASON_ESCALATED_TO_CRITICAL, ALARM_REASON_LEVEL_ESCALATED,
               ALARM_REASON_COMBINED_CONDITIONS_CONFIRMED,
               ALARM_REASON_COMBINED_CONDITIONS_RECOVERED } alarm_reason_t;

typedef struct {
    uint32_t struct_version;
    alarm_device_id_t device_id;
    uint64_t ingest_seq;
    uint32_t valid_fields;
    bool timestamp_valid;
    uint64_t timestamp_ms;
    bool boot_id_valid;
    uint32_t boot_id;
    uint32_t remote_seq;
    uint64_t local_ingest_seq;
    char room_id[ALARM_ENVIRONMENT_ROOM_ID_LEN];
    char source[ALARM_ENVIRONMENT_SOURCE_LEN];
    float temperature, humidity, pressure, gas_resistance;
    float air_quality_score;
    alarm_air_quality_level_t air_quality_level;
    float gas_ratio, stability_score;
    alarm_sensor_state_t sensor_state;
} alarm_environment_sample_t;

typedef struct {
    uint64_t event_seq, alarm_id;
    alarm_device_id_t device_id;
    alarm_type_t alarm_type;
    alarm_level_t alarm_level;
    bool previous_alarm_level_valid;
    alarm_level_t previous_alarm_level;
    alarm_event_status_t status;
    alarm_reason_t reason;
    uint32_t rule_version;
    uint64_t event_monotonic_ms;
    bool timestamp_valid;
    uint64_t timestamp_ms, activated_at_monotonic_ms;
    bool boot_id_valid;
    uint32_t boot_id;
    uint32_t remote_seq;
    uint64_t local_ingest_seq;
    char room_id[ALARM_ENVIRONMENT_ROOM_ID_LEN];
    char source[ALARM_ENVIRONMENT_SOURCE_LEN];
    float observed_value;
    float trigger_threshold;
    float recovery_threshold;
    char unit[16];
    char description[160];
    bool temperature_valid, humidity_valid, pressure_valid, gas_resistance_valid;
    bool air_quality_score_valid, air_quality_level_valid, gas_ratio_valid, stability_score_valid;
    float temperature, humidity, pressure, gas_resistance, air_quality_score, gas_ratio, stability_score;
    alarm_air_quality_level_t air_quality_level;
    alarm_sensor_state_t sensor_state;
    uint32_t participant_alarm_types, participant_conditions;
} alarm_event_t;

typedef struct { alarm_type_t alarm_type; alarm_level_t alarm_level; uint64_t alarm_id, activated_at_monotonic_ms; } alarm_active_alarm_t;
typedef struct { uint64_t dropped_event_count, invalid_sample_count, duplicate_sample_count,
                 out_of_order_sample_count, unknown_device_count, version_mismatch_count,
                 queue_eviction_count, queue_full_count, events_generated; } alarm_engine_diagnostics_t;
typedef uint64_t (*alarm_engine_clock_fn_t)(void *context);
typedef struct { alarm_engine_clock_fn_t monotonic_clock_ms; void *clock_context; } alarm_engine_options_t;

esp_err_t alarm_engine_init(const alarm_engine_options_t *options);
esp_err_t alarm_engine_update(const alarm_environment_sample_t *sample, size_t *generated_event_count);
size_t alarm_engine_peek_events(alarm_event_t *events, size_t capacity);
esp_err_t alarm_engine_ack_events(uint64_t through_event_seq);
size_t alarm_engine_get_active(alarm_device_id_t device_id, alarm_active_alarm_t *alarms, size_t capacity);
esp_err_t alarm_engine_get_diagnostics(alarm_engine_diagnostics_t *diagnostics);
size_t alarm_engine_get_queue_depth(void);
esp_err_t alarm_engine_reset(bool all_devices, alarm_device_id_t device_id);
void alarm_engine_deinit(void);

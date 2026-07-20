#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ALARM_ENVIRONMENT_SAMPLE_VERSION 1U
#define ALARM_ENGINE_MAX_DEVICES 4U
#define ALARM_ENGINE_MAX_EVENTS 128U
#define ALARM_ENGINE_MAX_EVENTS_PER_UPDATE 16U
#define ALARM_ENGINE_MAX_ACTIVE_ALARMS 13U

typedef enum { ALARM_DEVICE_C51 = 0, ALARM_DEVICE_C52, ALARM_DEVICE_INVALID } alarm_device_id_t;
typedef enum { ALARM_AIR_QUALITY_EXCELLENT = 0, ALARM_AIR_QUALITY_GOOD, ALARM_AIR_QUALITY_MODERATE,
               ALARM_AIR_QUALITY_POOR, ALARM_AIR_QUALITY_BAD, ALARM_AIR_QUALITY_UNKNOWN } alarm_air_quality_level_t;
typedef enum { ALARM_SENSOR_WARMUP = 0, ALARM_SENSOR_READY, ALARM_SENSOR_DEGRADED } alarm_sensor_state_t;
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
    bool timestamp_valid;
    uint64_t timestamp_ms;
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
                 queue_eviction_count; } alarm_engine_diagnostics_t;
typedef uint64_t (*alarm_engine_clock_fn_t)(void *context);
typedef struct { alarm_engine_clock_fn_t monotonic_clock_ms; void *clock_context; } alarm_engine_options_t;

esp_err_t alarm_engine_init(const alarm_engine_options_t *options);
esp_err_t alarm_engine_update(const alarm_environment_sample_t *sample, size_t *generated_event_count);
size_t alarm_engine_peek_events(alarm_event_t *events, size_t capacity);
esp_err_t alarm_engine_ack_events(uint64_t through_event_seq);
size_t alarm_engine_get_active(alarm_device_id_t device_id, alarm_active_alarm_t *alarms, size_t capacity);
esp_err_t alarm_engine_get_diagnostics(alarm_engine_diagnostics_t *diagnostics);
esp_err_t alarm_engine_reset(bool all_devices, alarm_device_id_t device_id);
void alarm_engine_deinit(void);

#pragma once

#include "environment_alarm_engine.h"

#define ALARM_ENGINE_TEMPERATURE_HISTORY_CAPACITY 64U
#define ALARM_ENGINE_HUMIDITY_HISTORY_CAPACITY 64U
#define ALARM_ENGINE_AIR_QUALITY_HISTORY_CAPACITY 128U
#define ALARM_ENGINE_RULE_VERSION 1U

#ifndef ENV_ALARM_EVENT_LOG_ENABLED
#define ENV_ALARM_EVENT_LOG_ENABLED 1
#endif

typedef struct { uint64_t timestamp_ms; float value; } alarm_history_item_t;
typedef struct { alarm_history_item_t *items; uint16_t head, count, capacity; } alarm_history_t;
typedef enum { ALARM_RULE_NORMAL = 0, ALARM_RULE_PENDING, ALARM_RULE_ACTIVE } alarm_rule_state_t;
typedef struct {
    alarm_rule_state_t state;
    uint64_t pending_since_ms, recovery_since_ms, last_valid_ms, alarm_id, activated_at_ms;
    uint16_t pending_sample_count, recovery_sample_count, escalation_sample_count;
    uint32_t cycle_seq, participant_conditions;
    alarm_level_t level;
    float activation_score;
} alarm_rule_runtime_t;
typedef struct {
    bool used, has_ingest_seq;
    alarm_device_id_t device_id;
    uint64_t last_ingest_seq;
    alarm_rule_runtime_t rules[ALARM_TYPE_COUNT];
    alarm_history_t temperature_history, humidity_history, air_quality_history;
} alarm_device_runtime_t;

typedef struct {
    float high_temperature_trigger, high_temperature_recovery, low_temperature_trigger, low_temperature_recovery;
    float high_humidity_trigger, high_humidity_recovery, low_humidity_trigger, low_humidity_recovery;
    float fast_temperature_trigger, fast_temperature_recovery, fast_humidity_trigger, fast_humidity_recovery;
    float aq_warning_trigger, aq_warning_recovery, aq_critical_trigger, aq_critical_recovery;
    float aq_deteriorating_delta, aq_deteriorating_recovery_delta, aq_deteriorating_current_max, aq_deteriorating_gas_ratio_max;
    float pollution_trigger, pollution_critical, pollution_recovery, stability_trigger, stability_recovery;
    float critical_environment_high_temperature, critical_environment_low_temperature;
    float critical_environment_high_humidity, critical_environment_low_humidity;
    uint32_t trigger_60s_ms, trigger_30s_ms, recovery_120s_ms, recovery_10min_ms;
    uint32_t combination_trigger_ms, combination_recovery_ms, max_inter_sample_gap_ms;
    uint32_t temperature_window_ms, air_quality_window_ms, diagnostic_log_interval_ms, rule_version;
    uint16_t queue_capacity, minimum_samples_one, minimum_samples_two, minimum_samples_three;
    alarm_device_id_t allowed_devices[2]; uint8_t allowed_device_count;
} alarm_engine_config_t;

typedef struct {
    alarm_device_id_t device_id;
    alarm_type_t alarm_type;
    alarm_level_t alarm_level;
    alarm_event_status_t status;
    alarm_reason_t reason;
    bool timestamp_valid;
    uint64_t timestamp_ms;
    uint64_t event_monotonic_ms;
    uint64_t queue_eviction_count;
    bool queue_eviction_log;
    float temperature;
    float humidity;
    float air_quality_score;
    float gas_ratio;
    alarm_air_quality_level_t air_quality_level;
} alarm_event_log_snapshot_t;

extern const alarm_engine_config_t g_alarm_config;
typedef struct {
    bool initialized;
    alarm_engine_options_t options;
    uint64_t boot_nonce, next_event_seq;
    alarm_device_runtime_t devices[ALARM_ENGINE_MAX_DEVICES];
    alarm_event_t *queue;
    alarm_event_t *staged;
    size_t staged_count;
    uint16_t queue_head, queue_count;
    alarm_engine_diagnostics_t diagnostics;
} alarm_engine_runtime_t;
extern alarm_engine_runtime_t g_alarm_engine;
bool alarm_config_is_valid(void);
void *alarm_storage_calloc(size_t count, size_t size);
void alarm_storage_free(void *pointer);
void alarm_history_init(alarm_history_t *history, uint16_t capacity);
esp_err_t alarm_history_allocate(alarm_history_t *history, uint16_t capacity);
void alarm_history_release(alarm_history_t *history);
void alarm_history_prune(alarm_history_t *history, uint64_t now_ms, uint32_t window_ms);
void alarm_history_push(alarm_history_t *history, uint64_t timestamp_ms, float value);
bool alarm_history_oldest(const alarm_history_t *history, float *value);
bool alarm_history_max(const alarm_history_t *history, float *value);
void alarm_events_stage_reset(void);
size_t alarm_events_stage_count(void);
void alarm_events_emit(const alarm_environment_sample_t *sample, const alarm_rule_runtime_t *rule,
                       alarm_type_t type, alarm_level_t level, alarm_event_status_t status,
                       alarm_reason_t reason, bool previous_level_valid, alarm_level_t previous_level,
                       uint32_t participant_types, uint32_t participant_conditions, uint64_t now_ms);
void alarm_events_swap_last_two(void);
size_t alarm_events_commit(alarm_event_log_snapshot_t *snapshots, size_t capacity);
void alarm_events_log(const alarm_event_log_snapshot_t *snapshots, size_t count);
size_t alarm_events_peek(alarm_event_t *events, size_t capacity);
esp_err_t alarm_events_ack(uint64_t through_event_seq);
void alarm_rules_process(alarm_device_runtime_t *device, const alarm_environment_sample_t *sample, uint64_t now_ms);

#include "environment_alarm_engine_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

alarm_engine_runtime_t g_alarm_engine;
static alarm_event_t s_staged[ALARM_ENGINE_MAX_EVENTS_PER_UPDATE];
static size_t s_staged_count;
#if ENV_ALARM_EVENT_LOG_ENABLED
static uint64_t s_last_log_ms;
#endif

static bool finite_range(float v, float lo, float hi) { return isfinite(v) && v >= lo && v <= hi; }
static bool level_valid(alarm_air_quality_level_t v) { return v >= ALARM_AIR_QUALITY_EXCELLENT && v <= ALARM_AIR_QUALITY_UNKNOWN; }
#if ENV_ALARM_EVENT_LOG_ENABLED
static const char *type_name(alarm_type_t t) {
    static const char *const n[ALARM_TYPE_COUNT]={"HIGH_TEMPERATURE","LOW_TEMPERATURE","FAST_TEMPERATURE_CHANGE","HIGH_HUMIDITY","LOW_HUMIDITY","FAST_HUMIDITY_CHANGE","AIR_QUALITY_WARNING","AIR_QUALITY_CRITICAL","AIR_QUALITY_DETERIORATING","POLLUTION_SPIKE","ENVIRONMENT_UNSTABLE","SENSOR_DEGRADED","CRITICAL_ENVIRONMENT"};
    return t<ALARM_TYPE_COUNT?n[t]:"UNKNOWN";
}
static const char *reason_name(alarm_reason_t r) {
    static const char *const n[]={"threshold_confirmed","recovery_confirmed","escalated_to_critical","level_escalated","combined_conditions_confirmed","combined_conditions_recovered"};
    return r<=ALARM_REASON_COMBINED_CONDITIONS_RECOVERED?n[r]:"unknown";
}
#if ENV_ALARM_EVENT_LOG_ENABLED
static const char *device_name(alarm_device_id_t id) {
    switch (id) {
    case ALARM_DEVICE_C51:
        return "sensair_shuttle_01";
    case ALARM_DEVICE_C52:
        return "sensair_shuttle_02";
    default:
        return "UNKNOWN";
    }
}
static const char *level_name(alarm_level_t level) {
    static const char *const names[] = {"WARNING", "HIGH", "CRITICAL"};
    return level <= ALARM_LEVEL_CRITICAL ? names[level] : "UNKNOWN";
}
static const char *air_quality_level_name(alarm_air_quality_level_t level) {
    static const char *const names[] = {"excellent", "good", "moderate", "poor", "bad"};
    return level <= ALARM_AIR_QUALITY_BAD ? names[level] : "unknown";
}
#endif
static float threshold(alarm_type_t t, alarm_event_status_t s) {
    const bool recovered=s==ALARM_STATUS_RECOVERED;
    switch (t) {
    case ALARM_TYPE_HIGH_TEMPERATURE: return recovered?g_alarm_config.high_temperature_recovery:g_alarm_config.high_temperature_trigger;
    case ALARM_TYPE_LOW_TEMPERATURE: return recovered?g_alarm_config.low_temperature_recovery:g_alarm_config.low_temperature_trigger;
    case ALARM_TYPE_FAST_TEMPERATURE_CHANGE: return recovered?g_alarm_config.fast_temperature_recovery:g_alarm_config.fast_temperature_trigger;
    case ALARM_TYPE_HIGH_HUMIDITY: return recovered?g_alarm_config.high_humidity_recovery:g_alarm_config.high_humidity_trigger;
    case ALARM_TYPE_LOW_HUMIDITY: return recovered?g_alarm_config.low_humidity_recovery:g_alarm_config.low_humidity_trigger;
    case ALARM_TYPE_FAST_HUMIDITY_CHANGE: return recovered?g_alarm_config.fast_humidity_recovery:g_alarm_config.fast_humidity_trigger;
    case ALARM_TYPE_AIR_QUALITY_WARNING: return recovered?g_alarm_config.aq_warning_recovery:g_alarm_config.aq_warning_trigger;
    case ALARM_TYPE_AIR_QUALITY_CRITICAL: return recovered?g_alarm_config.aq_critical_recovery:g_alarm_config.aq_critical_trigger;
    case ALARM_TYPE_AIR_QUALITY_DETERIORATING: return recovered?g_alarm_config.aq_deteriorating_recovery_delta:g_alarm_config.aq_deteriorating_delta;
    case ALARM_TYPE_POLLUTION_SPIKE: return recovered?g_alarm_config.pollution_recovery:g_alarm_config.pollution_trigger;
    case ALARM_TYPE_ENVIRONMENT_UNSTABLE: return recovered?g_alarm_config.stability_recovery:g_alarm_config.stability_trigger;
    case ALARM_TYPE_CRITICAL_ENVIRONMENT: return (float)(recovered?g_alarm_config.combination_recovery_ms:g_alarm_config.combination_trigger_ms)/1000.0f;
    default: return 0.0f;
    }
}
#endif
static uint32_t participant_types(uint32_t c) {
    uint32_t types=0;
    if (c&3U) types|=1UL<<ALARM_TYPE_AIR_QUALITY_CRITICAL;
    if (c&1U) types|=1UL<<ALARM_TYPE_HIGH_TEMPERATURE;
    if (c&2U) types|=1UL<<ALARM_TYPE_LOW_TEMPERATURE;
    if (c&12U) types|=1UL<<ALARM_TYPE_POLLUTION_SPIKE;
    if (c&4U) types|=1UL<<ALARM_TYPE_HIGH_HUMIDITY;
    if (c&8U) types|=1UL<<ALARM_TYPE_LOW_HUMIDITY;
    return types;
}
void alarm_events_stage_reset(void) { s_staged_count=0; }
size_t alarm_events_stage_count(void) { return s_staged_count; }
void alarm_events_swap_last_two(void) { if (s_staged_count>=2) { alarm_event_t t=s_staged[s_staged_count-1U]; s_staged[s_staged_count-1U]=s_staged[s_staged_count-2U]; s_staged[s_staged_count-2U]=t; } }
void alarm_events_emit(const alarm_environment_sample_t *s, const alarm_rule_runtime_t *r, alarm_type_t type,
                       alarm_level_t level, alarm_event_status_t status, alarm_reason_t reason,
                       bool previous_valid, alarm_level_t previous, uint32_t types, uint32_t conditions, uint64_t now) {
    if (s_staged_count>=ALARM_ENGINE_MAX_EVENTS_PER_UPDATE) return;
    alarm_event_t *e=&s_staged[s_staged_count++]; memset(e,0,sizeof(*e));
    e->event_seq=g_alarm_engine.next_event_seq++; e->alarm_id=r->alarm_id; e->device_id=s->device_id; e->alarm_type=type;
    e->alarm_level=level; e->previous_alarm_level_valid=previous_valid; e->previous_alarm_level=previous; e->status=status; e->reason=reason;
    e->rule_version=g_alarm_config.rule_version; e->event_monotonic_ms=now; e->timestamp_valid=s->timestamp_valid; e->timestamp_ms=s->timestamp_ms; e->activated_at_monotonic_ms=r->activated_at_ms;
    e->temperature_valid=finite_range(s->temperature,-40,85); e->temperature=s->temperature; e->humidity_valid=finite_range(s->humidity,0,100); e->humidity=s->humidity;
    e->pressure_valid=finite_range(s->pressure,300,1100); e->pressure=s->pressure; e->gas_resistance_valid=finite_range(s->gas_resistance,0.000001f,1000000000.0f); e->gas_resistance=s->gas_resistance;
    e->air_quality_score_valid=finite_range(s->air_quality_score,0,100); e->air_quality_score=s->air_quality_score; e->air_quality_level_valid=level_valid(s->air_quality_level); e->air_quality_level=s->air_quality_level;
    e->gas_ratio_valid=finite_range(s->gas_ratio,0.000001f,10); e->gas_ratio=s->gas_ratio; e->stability_score_valid=finite_range(s->stability_score,0,1); e->stability_score=s->stability_score; e->sensor_state=s->sensor_state;
    e->participant_alarm_types=type==ALARM_TYPE_CRITICAL_ENVIRONMENT?participant_types(conditions):types; e->participant_conditions=conditions;
#if ENV_ALARM_EVENT_LOG_ENABLED
    (void)snprintf(e->description,sizeof(e->description),"%s %s reason=%s device=%d threshold=%.2f temperature=%.2f humidity=%.2f score=%.2f conditions=%lu",type_name(type),status==ALARM_STATUS_ACTIVE?"ACTIVE":"RECOVERED",reason_name(reason),(int)s->device_id,(double)threshold(type,status),(double)s->temperature,(double)s->humidity,(double)s->air_quality_score,(unsigned long)conditions);
#else
    strlcpy(e->description, "environment alarm", sizeof(e->description));
#endif
}
static alarm_event_t *at(uint16_t i) { return &g_alarm_engine.queue[(g_alarm_engine.queue_head+i)%g_alarm_config.queue_capacity]; }
static bool victim(const alarm_event_t *e, int priority) { return priority==0?e->reason==ALARM_REASON_LEVEL_ESCALATED:priority==1?e->status==ALARM_STATUS_RECOVERED:e->status==ALARM_STATUS_ACTIVE; }
static void remove_at(uint16_t index) { for (uint16_t i=index;i+1U<g_alarm_engine.queue_count;i++) *at(i)=*at((uint16_t)(i+1U)); --g_alarm_engine.queue_count; }
static bool push(const alarm_event_t *event) {
    bool queue_eviction_log = false;
    if (g_alarm_engine.queue_count==g_alarm_config.queue_capacity) { bool found=false; uint16_t index=0;
        for (int p=0;p<3&&!found;p++) for (uint16_t i=0;i<g_alarm_engine.queue_count;i++) if (victim(at(i),p)) { index=i; found=true; break; }
        ++g_alarm_engine.diagnostics.dropped_event_count; ++g_alarm_engine.diagnostics.queue_eviction_count; if (!found) return false; remove_at(index);
#if ENV_ALARM_EVENT_LOG_ENABLED
        if (s_last_log_ms==0 || event->event_monotonic_ms-s_last_log_ms>=g_alarm_config.diagnostic_log_interval_ms) { queue_eviction_log = true; s_last_log_ms=event->event_monotonic_ms; }
#endif
    }
    *at(g_alarm_engine.queue_count++)=*event;
    return queue_eviction_log;
}
size_t alarm_events_commit(alarm_event_log_snapshot_t *snapshots, size_t capacity) {
    size_t snapshot_count = 0;
    for (size_t i=0; i<s_staged_count; i++) {
        const alarm_event_t *event = &s_staged[i];
#if ENV_ALARM_EVENT_LOG_ENABLED
        if (snapshots != NULL && snapshot_count < capacity) {
            snapshots[snapshot_count++] = (alarm_event_log_snapshot_t){
                .device_id = event->device_id,
                .alarm_type = event->alarm_type,
                .alarm_level = event->alarm_level,
                .status = event->status,
                .reason = event->reason,
                .timestamp_valid = event->timestamp_valid,
                .timestamp_ms = event->timestamp_ms,
                .event_monotonic_ms = event->event_monotonic_ms,
                .temperature = event->temperature,
                .humidity = event->humidity,
                .air_quality_score = event->air_quality_score,
                .gas_ratio = event->gas_ratio,
                .air_quality_level = event->air_quality_level,
            };
        }
#else
        (void)snapshots;
        (void)capacity;
#endif
        const bool queue_eviction_log = push(event);
#if ENV_ALARM_EVENT_LOG_ENABLED
        if (snapshots != NULL && snapshot_count > 0U) {
            alarm_event_log_snapshot_t *snapshot = &snapshots[snapshot_count - 1U];
            snapshot->queue_eviction_log = queue_eviction_log;
            snapshot->queue_eviction_count = g_alarm_engine.diagnostics.queue_eviction_count;
        }
#else
        (void)queue_eviction_log;
#endif
    }
    return snapshot_count;
}

void alarm_events_log(const alarm_event_log_snapshot_t *snapshots, size_t count)
{
#if ENV_ALARM_EVENT_LOG_ENABLED
    for (size_t i = 0; snapshots != NULL && i < count; i++) {
        const alarm_event_log_snapshot_t *event = &snapshots[i];
        if (event->queue_eviction_log) {
            ESP_LOGW("environment_alarm",
                     "event queue eviction count=%llu",
                     (unsigned long long)event->queue_eviction_count);
        }
        if (event->status == ALARM_STATUS_ACTIVE) {
            ESP_LOGW("ENV_ALARM",
                     "device_id=%s rule_name=%s state=ACTIVE current_temp=%.2f "
                     "current_humidity=%.2f current_aq=%.0f current_aq_level=%s current_gas_ratio=%.3f "
                     "threshold=%.2f timestamp_valid=%d timestamp_ms=%llu monotonic_ms=%llu severity=%s reason=%s",
                     device_name(event->device_id),
                     type_name(event->alarm_type),
                     (double)event->temperature,
                     (double)event->humidity,
                     (double)event->air_quality_score,
                     air_quality_level_name(event->air_quality_level),
                     (double)event->gas_ratio,
                     (double)threshold(event->alarm_type, event->status),
                     event->timestamp_valid ? 1 : 0,
                     (unsigned long long)(event->timestamp_valid ? event->timestamp_ms : 0U),
                     (unsigned long long)event->event_monotonic_ms,
                     level_name(event->alarm_level),
                     reason_name(event->reason));
        } else {
            ESP_LOGI("ENV_ALARM",
                     "device_id=%s rule_name=%s state=RECOVERED current_temp=%.2f "
                     "current_humidity=%.2f current_aq=%.0f current_aq_level=%s current_gas_ratio=%.3f "
                     "threshold=%.2f timestamp_valid=%d timestamp_ms=%llu monotonic_ms=%llu reason=%s",
                     device_name(event->device_id),
                     type_name(event->alarm_type),
                     (double)event->temperature,
                     (double)event->humidity,
                     (double)event->air_quality_score,
                     air_quality_level_name(event->air_quality_level),
                     (double)event->gas_ratio,
                     (double)threshold(event->alarm_type, event->status),
                     event->timestamp_valid ? 1 : 0,
                     (unsigned long long)(event->timestamp_valid ? event->timestamp_ms : 0U),
                     (unsigned long long)event->event_monotonic_ms,
                     reason_name(event->reason));
        }
    }
#else
    (void)snapshots;
    (void)count;
#endif
}
size_t alarm_events_peek(alarm_event_t *events, size_t capacity) { const size_t n=g_alarm_engine.queue_count<capacity?g_alarm_engine.queue_count:capacity; for (size_t i=0;i<n;i++) events[i]=*at((uint16_t)i); return n; }
esp_err_t alarm_events_ack(uint64_t through) { while (g_alarm_engine.queue_count&&at(0)->event_seq<=through) { g_alarm_engine.queue_head=(uint16_t)((g_alarm_engine.queue_head+1U)%g_alarm_config.queue_capacity); --g_alarm_engine.queue_count; } return ESP_OK; }

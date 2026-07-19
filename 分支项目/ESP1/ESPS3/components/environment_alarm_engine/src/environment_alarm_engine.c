#include "environment_alarm_engine_internal.h"

#include <math.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_boot_counter;
static bool finite_range(float v,float lo,float hi) { return isfinite(v)&&v>=lo&&v<=hi; }
static bool sensor_valid(alarm_sensor_state_t s) { return s==ALARM_SENSOR_WARMUP||s==ALARM_SENSOR_READY||s==ALARM_SENSOR_DEGRADED; }
static bool device_allowed(alarm_device_id_t id) { for (uint8_t i=0;i<g_alarm_config.allowed_device_count;i++) if (g_alarm_config.allowed_devices[i]==id) return true; return false; }
static uint64_t now_ms(void) { return g_alarm_engine.options.monotonic_clock_ms?g_alarm_engine.options.monotonic_clock_ms(g_alarm_engine.options.clock_context):(uint64_t)(esp_timer_get_time()/1000LL); }
static alarm_device_runtime_t *find_device(alarm_device_id_t id) {
    for (size_t i=0;i<ALARM_ENGINE_MAX_DEVICES;i++) if (g_alarm_engine.devices[i].used&&g_alarm_engine.devices[i].device_id==id) return &g_alarm_engine.devices[i];
    for (size_t i=0;i<ALARM_ENGINE_MAX_DEVICES;i++) if (!g_alarm_engine.devices[i].used) { alarm_device_runtime_t *d=&g_alarm_engine.devices[i]; memset(d,0,sizeof(*d)); d->used=true; d->device_id=id; alarm_history_init(&d->temperature_history,ALARM_ENGINE_TEMPERATURE_HISTORY_CAPACITY); alarm_history_init(&d->humidity_history,ALARM_ENGINE_HUMIDITY_HISTORY_CAPACITY); alarm_history_init(&d->air_quality_history,ALARM_ENGINE_AIR_QUALITY_HISTORY_CAPACITY); return d; }
    return NULL;
}
esp_err_t alarm_engine_init(const alarm_engine_options_t *options) {
    if (!alarm_config_is_valid()) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock); memset(&g_alarm_engine,0,sizeof(g_alarm_engine)); if (options) g_alarm_engine.options=*options; g_alarm_engine.boot_nonce=now_ms()^(++s_boot_counter<<32U); g_alarm_engine.next_event_seq=1; g_alarm_engine.initialized=true; portEXIT_CRITICAL(&s_lock); return ESP_OK;
}
esp_err_t alarm_engine_update(const alarm_environment_sample_t *s,size_t *generated) {
    alarm_event_log_snapshot_t log_snapshots[ALARM_ENGINE_MAX_EVENTS_PER_UPDATE];
    size_t log_snapshot_count = 0;
    if (generated) {
        *generated=0;
    }
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    if (!g_alarm_engine.initialized) { ++g_alarm_engine.diagnostics.invalid_sample_count; portEXIT_CRITICAL(&s_lock); return ESP_ERR_INVALID_STATE; }
    if (s->struct_version!=ALARM_ENVIRONMENT_SAMPLE_VERSION) { ++g_alarm_engine.diagnostics.version_mismatch_count; portEXIT_CRITICAL(&s_lock); return ESP_ERR_INVALID_VERSION; }
    if (!device_allowed(s->device_id)) { ++g_alarm_engine.diagnostics.unknown_device_count; portEXIT_CRITICAL(&s_lock); return ESP_ERR_NOT_FOUND; }
    alarm_device_runtime_t *d=find_device(s->device_id); if (!d) { ++g_alarm_engine.diagnostics.unknown_device_count; portEXIT_CRITICAL(&s_lock); return ESP_ERR_NO_MEM; }
    if (d->has_ingest_seq&&s->ingest_seq<=d->last_ingest_seq) { if (s->ingest_seq==d->last_ingest_seq) ++g_alarm_engine.diagnostics.duplicate_sample_count; else ++g_alarm_engine.diagnostics.out_of_order_sample_count; portEXIT_CRITICAL(&s_lock); return ESP_OK; }
    d->has_ingest_seq=true; d->last_ingest_seq=s->ingest_seq; const uint64_t now=now_ms();
    alarm_history_prune(&d->temperature_history,now,g_alarm_config.temperature_window_ms); alarm_history_prune(&d->humidity_history,now,g_alarm_config.temperature_window_ms); alarm_history_prune(&d->air_quality_history,now,g_alarm_config.air_quality_window_ms);
    if (finite_range(s->temperature,-40,85)) alarm_history_push(&d->temperature_history,now,s->temperature);
    if (finite_range(s->humidity,0,100)) alarm_history_push(&d->humidity_history,now,s->humidity);
    if (finite_range(s->air_quality_score,0,100)&&sensor_valid(s->sensor_state)&&s->sensor_state==ALARM_SENSOR_READY) alarm_history_push(&d->air_quality_history,now,s->air_quality_score);
    alarm_events_stage_reset(); alarm_rules_process(d,s,now); if (generated) *generated=alarm_events_stage_count(); log_snapshot_count=alarm_events_commit(log_snapshots,ALARM_ENGINE_MAX_EVENTS_PER_UPDATE); portEXIT_CRITICAL(&s_lock);
    alarm_events_log(log_snapshots, log_snapshot_count);
    return ESP_OK;
}
size_t alarm_engine_peek_events(alarm_event_t *e,size_t cap) { if (!e||!cap) return 0; portENTER_CRITICAL(&s_lock); const size_t n=g_alarm_engine.initialized?alarm_events_peek(e,cap):0; portEXIT_CRITICAL(&s_lock); return n; }
esp_err_t alarm_engine_ack_events(uint64_t seq) { portENTER_CRITICAL(&s_lock); if (!g_alarm_engine.initialized) { portEXIT_CRITICAL(&s_lock); return ESP_ERR_INVALID_STATE; } const esp_err_t r=alarm_events_ack(seq); portEXIT_CRITICAL(&s_lock); return r; }
size_t alarm_engine_get_active(alarm_device_id_t id,alarm_active_alarm_t *out,size_t cap) { if (!out||!cap) return 0; portENTER_CRITICAL(&s_lock); size_t n=0; if (g_alarm_engine.initialized&&device_allowed(id)) for (size_t i=0;i<ALARM_ENGINE_MAX_DEVICES;i++) if (g_alarm_engine.devices[i].used&&g_alarm_engine.devices[i].device_id==id) for (alarm_type_t t=0;t<ALARM_TYPE_COUNT&&n<cap;t++) { alarm_rule_runtime_t *r=&g_alarm_engine.devices[i].rules[t]; if (r->state==ALARM_RULE_ACTIVE) out[n++]=(alarm_active_alarm_t){t,r->level,r->alarm_id,r->activated_at_ms}; } portEXIT_CRITICAL(&s_lock); return n; }
esp_err_t alarm_engine_get_diagnostics(alarm_engine_diagnostics_t *out) { if (!out) return ESP_ERR_INVALID_ARG; portENTER_CRITICAL(&s_lock); if (!g_alarm_engine.initialized) { portEXIT_CRITICAL(&s_lock); return ESP_ERR_INVALID_STATE; } *out=g_alarm_engine.diagnostics; portEXIT_CRITICAL(&s_lock); return ESP_OK; }
esp_err_t alarm_engine_reset(bool all,alarm_device_id_t id) { portENTER_CRITICAL(&s_lock); if (!g_alarm_engine.initialized) { portEXIT_CRITICAL(&s_lock); return ESP_ERR_INVALID_STATE; } if (all) { memset(g_alarm_engine.devices,0,sizeof(g_alarm_engine.devices)); g_alarm_engine.queue_head=0; g_alarm_engine.queue_count=0; portEXIT_CRITICAL(&s_lock); return ESP_OK; } if (!device_allowed(id)) { portEXIT_CRITICAL(&s_lock); return ESP_ERR_NOT_FOUND; } for (size_t i=0;i<ALARM_ENGINE_MAX_DEVICES;i++) if (g_alarm_engine.devices[i].used&&g_alarm_engine.devices[i].device_id==id) { memset(&g_alarm_engine.devices[i],0,sizeof(g_alarm_engine.devices[i])); break; } portEXIT_CRITICAL(&s_lock); return ESP_OK; }
void alarm_engine_deinit(void) { portENTER_CRITICAL(&s_lock); memset(&g_alarm_engine,0,sizeof(g_alarm_engine)); portEXIT_CRITICAL(&s_lock); }

#include "environment_alarm_engine_internal.h"

#include <math.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_boot_counter;
static bool s_initializing;
static bool finite_range(float v,float lo,float hi) { return isfinite(v)&&v>=lo&&v<=hi; }
static bool sensor_valid(alarm_sensor_state_t s) { return s==ALARM_SENSOR_WARMUP||s==ALARM_SENSOR_READY||s==ALARM_SENSOR_DEGRADED||s==ALARM_SENSOR_UNKNOWN; }
static bool device_allowed(alarm_device_id_t id) { for (uint8_t i=0;i<g_alarm_config.allowed_device_count;i++) if (g_alarm_config.allowed_devices[i]==id) return true; return false; }
static uint64_t now_ms(void) { return g_alarm_engine.options.monotonic_clock_ms?g_alarm_engine.options.monotonic_clock_ms(g_alarm_engine.options.clock_context):(uint64_t)(esp_timer_get_time()/1000LL); }

typedef struct {
    alarm_event_t *queue;
    alarm_event_t *staged;
    alarm_history_t temperature[ALARM_ENGINE_MAX_DEVICES];
    alarm_history_t humidity[ALARM_ENGINE_MAX_DEVICES];
    alarm_history_t air_quality[ALARM_ENGINE_MAX_DEVICES];
} alarm_engine_storage_t;

static void release_device_histories(alarm_device_runtime_t *device)
{
    if (device == NULL) return;
    alarm_history_release(&device->temperature_history);
    alarm_history_release(&device->humidity_history);
    alarm_history_release(&device->air_quality_history);
}

static void release_runtime_storage(void)
{
    for (size_t i = 0U; i < ALARM_ENGINE_MAX_DEVICES; ++i) {
        release_device_histories(&g_alarm_engine.devices[i]);
    }
    alarm_storage_free(g_alarm_engine.staged);
    alarm_storage_free(g_alarm_engine.queue);
    g_alarm_engine.staged = NULL;
    g_alarm_engine.queue = NULL;
    g_alarm_engine.staged_count = 0U;
}

static void release_prepared_storage(alarm_engine_storage_t *storage)
{
    if (storage == NULL) return;
    for (size_t i = 0U; i < ALARM_ENGINE_MAX_DEVICES; ++i) {
        alarm_history_release(&storage->temperature[i]);
        alarm_history_release(&storage->humidity[i]);
        alarm_history_release(&storage->air_quality[i]);
    }
    alarm_storage_free(storage->staged);
    alarm_storage_free(storage->queue);
    memset(storage, 0, sizeof(*storage));
}

static esp_err_t prepare_runtime_storage(alarm_engine_storage_t *storage)
{
    if (storage == NULL) return ESP_ERR_INVALID_ARG;
    storage->queue = alarm_storage_calloc(ALARM_ENGINE_MAX_EVENTS,
                                           sizeof(*storage->queue));
    if (storage->queue == NULL) return ESP_ERR_NO_MEM;
    storage->staged = alarm_storage_calloc(ALARM_ENGINE_MAX_EVENTS_PER_UPDATE,
                                            sizeof(*storage->staged));
    if (storage->staged == NULL) return ESP_ERR_NO_MEM;
    for (size_t i = 0U; i < ALARM_ENGINE_MAX_DEVICES; ++i) {
        esp_err_t ret = alarm_history_allocate(&storage->temperature[i],
                                                ALARM_ENGINE_TEMPERATURE_HISTORY_CAPACITY);
        if (ret != ESP_OK) return ret;
        ret = alarm_history_allocate(&storage->humidity[i],
                                     ALARM_ENGINE_HUMIDITY_HISTORY_CAPACITY);
        if (ret != ESP_OK) return ret;
        ret = alarm_history_allocate(&storage->air_quality[i],
                                     ALARM_ENGINE_AIR_QUALITY_HISTORY_CAPACITY);
        if (ret != ESP_OK) return ret;
    }
    return ESP_OK;
}

static void reset_device(alarm_device_runtime_t *device, alarm_device_id_t id)
{
    if (device == NULL) return;
    alarm_history_t temperature = device->temperature_history;
    alarm_history_t humidity = device->humidity_history;
    alarm_history_t air_quality = device->air_quality_history;
    memset(device, 0, sizeof(*device));
    device->temperature_history = temperature;
    device->humidity_history = humidity;
    device->air_quality_history = air_quality;
    device->device_id = ALARM_DEVICE_INVALID;
    if (id < ALARM_DEVICE_INVALID) {
        device->used = true;
        device->device_id = id;
    }
    alarm_history_init(&device->temperature_history, ALARM_ENGINE_TEMPERATURE_HISTORY_CAPACITY);
    alarm_history_init(&device->humidity_history, ALARM_ENGINE_HUMIDITY_HISTORY_CAPACITY);
    alarm_history_init(&device->air_quality_history, ALARM_ENGINE_AIR_QUALITY_HISTORY_CAPACITY);
}

static alarm_device_runtime_t *find_device(alarm_device_id_t id) {
    for (size_t i=0;i<ALARM_ENGINE_MAX_DEVICES;i++) if (g_alarm_engine.devices[i].used&&g_alarm_engine.devices[i].device_id==id) return &g_alarm_engine.devices[i];
    for (size_t i=0;i<ALARM_ENGINE_MAX_DEVICES;i++) if (!g_alarm_engine.devices[i].used) { alarm_device_runtime_t *d=&g_alarm_engine.devices[i]; reset_device(d, id); return d; }
    return NULL;
}
esp_err_t alarm_engine_init(const alarm_engine_options_t *options) {
    if (!alarm_config_is_valid()) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    if (g_alarm_engine.initialized) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    if (s_initializing) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_initializing = true;
    portEXIT_CRITICAL(&s_lock);

    alarm_engine_storage_t storage = {0};
    esp_err_t ret = prepare_runtime_storage(&storage);
    if (ret != ESP_OK) {
        release_prepared_storage(&storage);
        portENTER_CRITICAL(&s_lock);
        s_initializing = false;
        portEXIT_CRITICAL(&s_lock);
        return ret;
    }

    portENTER_CRITICAL(&s_lock);
    memset(&g_alarm_engine,0,sizeof(g_alarm_engine));
    g_alarm_engine.queue = storage.queue;
    g_alarm_engine.staged = storage.staged;
    storage.queue = NULL;
    storage.staged = NULL;
    for (size_t i = 0U; i < ALARM_ENGINE_MAX_DEVICES; ++i) {
        g_alarm_engine.devices[i].temperature_history = storage.temperature[i];
        g_alarm_engine.devices[i].humidity_history = storage.humidity[i];
        g_alarm_engine.devices[i].air_quality_history = storage.air_quality[i];
        memset(&storage.temperature[i], 0, sizeof(storage.temperature[i]));
        memset(&storage.humidity[i], 0, sizeof(storage.humidity[i]));
        memset(&storage.air_quality[i], 0, sizeof(storage.air_quality[i]));
    }
    if (options) g_alarm_engine.options=*options;
    g_alarm_engine.boot_nonce=now_ms()^(++s_boot_counter<<32U);
    g_alarm_engine.next_event_seq=1;
    g_alarm_engine.initialized=true;
    s_initializing = false;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
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
    if (!sensor_valid(s->sensor_state)) { ++g_alarm_engine.diagnostics.invalid_sample_count; portEXIT_CRITICAL(&s_lock); return ESP_ERR_INVALID_ARG; }
    alarm_device_runtime_t *d=find_device(s->device_id); if (!d) { ++g_alarm_engine.diagnostics.unknown_device_count; portEXIT_CRITICAL(&s_lock); return ESP_ERR_NO_MEM; }
    if (g_alarm_engine.queue_count > g_alarm_config.queue_capacity - ALARM_ENGINE_MAX_EVENTS_PER_UPDATE) {
        ++g_alarm_engine.diagnostics.queue_full_count;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    if (d->has_ingest_seq&&s->ingest_seq<=d->last_ingest_seq) { if (s->ingest_seq==d->last_ingest_seq) ++g_alarm_engine.diagnostics.duplicate_sample_count; else ++g_alarm_engine.diagnostics.out_of_order_sample_count; portEXIT_CRITICAL(&s_lock); return ESP_OK; }
    d->has_ingest_seq=true; d->last_ingest_seq=s->ingest_seq; const uint64_t now=now_ms();
    alarm_history_prune(&d->temperature_history,now,g_alarm_config.temperature_window_ms); alarm_history_prune(&d->humidity_history,now,g_alarm_config.temperature_window_ms); alarm_history_prune(&d->air_quality_history,now,g_alarm_config.air_quality_window_ms);
    if ((s->valid_fields & ALARM_FIELD_TEMPERATURE) != 0U && finite_range(s->temperature,-40,85)) alarm_history_push(&d->temperature_history,now,s->temperature);
    if ((s->valid_fields & ALARM_FIELD_HUMIDITY) != 0U && finite_range(s->humidity,0,100)) alarm_history_push(&d->humidity_history,now,s->humidity);
    if ((s->valid_fields & ALARM_FIELD_AIR_QUALITY_SCORE) != 0U && finite_range(s->air_quality_score,0,100)&&s->sensor_state==ALARM_SENSOR_READY) alarm_history_push(&d->air_quality_history,now,s->air_quality_score);
    alarm_events_stage_reset(); alarm_rules_process(d,s,now); log_snapshot_count=alarm_events_commit(log_snapshots,ALARM_ENGINE_MAX_EVENTS_PER_UPDATE); g_alarm_engine.diagnostics.events_generated += log_snapshot_count; if (generated) *generated=log_snapshot_count; portEXIT_CRITICAL(&s_lock);
    alarm_events_log(log_snapshots, log_snapshot_count);
    return ESP_OK;
}
size_t alarm_engine_peek_events(alarm_event_t *e,size_t cap) { if (!e||!cap) return 0; portENTER_CRITICAL(&s_lock); const size_t n=g_alarm_engine.initialized?alarm_events_peek(e,cap):0; portEXIT_CRITICAL(&s_lock); return n; }
esp_err_t alarm_engine_ack_events(uint64_t seq) { portENTER_CRITICAL(&s_lock); if (!g_alarm_engine.initialized) { portEXIT_CRITICAL(&s_lock); return ESP_ERR_INVALID_STATE; } const esp_err_t r=alarm_events_ack(seq); portEXIT_CRITICAL(&s_lock); return r; }
size_t alarm_engine_get_active(alarm_device_id_t id,alarm_active_alarm_t *out,size_t cap) { if (!out||!cap) return 0; portENTER_CRITICAL(&s_lock); size_t n=0; if (g_alarm_engine.initialized&&device_allowed(id)) for (size_t i=0;i<ALARM_ENGINE_MAX_DEVICES;i++) if (g_alarm_engine.devices[i].used&&g_alarm_engine.devices[i].device_id==id) for (alarm_type_t t=0;t<ALARM_TYPE_COUNT&&n<cap;t++) { alarm_rule_runtime_t *r=&g_alarm_engine.devices[i].rules[t]; if (r->state==ALARM_RULE_ACTIVE) out[n++]=(alarm_active_alarm_t){t,r->level,r->alarm_id,r->activated_at_ms}; } portEXIT_CRITICAL(&s_lock); return n; }
esp_err_t alarm_engine_get_diagnostics(alarm_engine_diagnostics_t *out) { if (!out) return ESP_ERR_INVALID_ARG; portENTER_CRITICAL(&s_lock); if (!g_alarm_engine.initialized) { portEXIT_CRITICAL(&s_lock); return ESP_ERR_INVALID_STATE; } *out=g_alarm_engine.diagnostics; portEXIT_CRITICAL(&s_lock); return ESP_OK; }
size_t alarm_engine_get_queue_depth(void) { portENTER_CRITICAL(&s_lock); const size_t count=g_alarm_engine.initialized?g_alarm_engine.queue_count:0U; portEXIT_CRITICAL(&s_lock); return count; }
esp_err_t alarm_engine_reset(bool all,alarm_device_id_t id) { portENTER_CRITICAL(&s_lock); if (!g_alarm_engine.initialized) { portEXIT_CRITICAL(&s_lock); return ESP_ERR_INVALID_STATE; } if (all) { for (size_t i=0;i<ALARM_ENGINE_MAX_DEVICES;i++) reset_device(&g_alarm_engine.devices[i], ALARM_DEVICE_INVALID); g_alarm_engine.queue_head=0; g_alarm_engine.queue_count=0; g_alarm_engine.staged_count=0; portEXIT_CRITICAL(&s_lock); return ESP_OK; } if (!device_allowed(id)) { portEXIT_CRITICAL(&s_lock); return ESP_ERR_NOT_FOUND; } for (size_t i=0;i<ALARM_ENGINE_MAX_DEVICES;i++) if (g_alarm_engine.devices[i].used&&g_alarm_engine.devices[i].device_id==id) { reset_device(&g_alarm_engine.devices[i], ALARM_DEVICE_INVALID); break; } portEXIT_CRITICAL(&s_lock); return ESP_OK; }
void alarm_engine_deinit(void) {
    portENTER_CRITICAL(&s_lock);
    if (!g_alarm_engine.initialized) { portEXIT_CRITICAL(&s_lock); return; }
    g_alarm_engine.initialized = false;
    s_initializing = true;
    portEXIT_CRITICAL(&s_lock);
    release_runtime_storage();
    portENTER_CRITICAL(&s_lock);
    memset(&g_alarm_engine,0,sizeof(g_alarm_engine));
    s_initializing = false;
    portEXIT_CRITICAL(&s_lock);
}

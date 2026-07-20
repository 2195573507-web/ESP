#include "habit_rule_engine.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "rule_loader.h"
#include "time_provider.h"

#ifdef ESP_PLATFORM
#include "app_stack_monitor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

static const char *TAG = "habit_rule";
static habit_rule_engine_t s_runtime_engine;
static habit_time_provider_t s_runtime_time_provider;
static bool s_runtime_time_provider_set;
static uint32_t s_runtime_event_sequence;
#ifdef ESP_PLATFORM
static TaskHandle_t s_runtime_task;
static StaticSemaphore_t s_runtime_lock_storage;
static SemaphoreHandle_t s_runtime_lock;
static habit_snapshot_provider_t s_snapshot_provider;
static void *s_snapshot_context;
#endif

static const char *event_name(habit_event_type_t type)
{
    static const char *const names[] = {
        "PERSON_ENTER_ROOM", "PERSON_LEAVE_ROOM", "PERSON_LONG_STAY",
        "ROOM_EMPTY_TIMEOUT", "NIGHT_ACTIVITY", "LONG_OCCUPANCY",
    };
    return type < (sizeof(names) / sizeof(names[0])) ? names[type] : "UNKNOWN";
}

static uint8_t count_enabled(const habit_rule_engine_t *engine)
{
    uint8_t count = 0U;
    for (size_t i = 0; i < HABIT_RULE_COUNT; ++i) count += engine->rule_enabled[i] ? 1U : 0U;
    return count;
}

static bool night_hour(const habit_rule_engine_t *engine, uint8_t hour)
{
    if (engine->night_start_hour == engine->night_end_hour) return true;
    if (engine->night_start_hour < engine->night_end_hour) {
        return hour >= engine->night_start_hour && hour < engine->night_end_hour;
    }
    return hour >= engine->night_start_hour || hour < engine->night_end_hour;
}

static void timestamp_text(const habit_wall_clock_t *clock, char out[HABIT_EVENT_TIMESTAMP_BYTES])
{
    if (clock == NULL || !clock->valid) {
        out[0] = '\0';
        return;
    }
    (void)snprintf(out, HABIT_EVENT_TIMESTAMP_BYTES, "%04u-%02u-%02uT%02u:%02u:%02u",
                   (unsigned int)clock->year, (unsigned int)clock->month,
                   (unsigned int)clock->day, (unsigned int)clock->hour,
                   (unsigned int)clock->minute, (unsigned int)clock->second);
}

static bool rule_matches_snapshot(const habit_rule_engine_t *engine, habit_rule_id_t rule,
                                  const habit_room_snapshot_t *snapshot)
{
    const char *scope_type = engine->rule_scope_type[rule];
    const char *scope_id = engine->rule_scope_id[rule];
    if (strcmp(scope_type, "home") == 0) return true;
    if (strcmp(scope_type, "room") == 0) {
        return scope_id[0] != '\0' && strcmp(scope_id, snapshot->room) == 0;
    }
    if (strcmp(scope_type, "zone") == 0) {
        return snapshot->zone != NULL && scope_id[0] != '\0' &&
               strcmp(scope_id, snapshot->zone) == 0;
    }
    return false;
}

static void emit(habit_rule_engine_t *engine, habit_rule_id_t rule, habit_event_type_t type,
                 const habit_room_snapshot_t *snapshot, const habit_wall_clock_t *clock,
                 const char *reason)
{
    const uint8_t capacity = (uint8_t)(sizeof(engine->events) / sizeof(engine->events[0]));
    if (engine->event_count == capacity) {
        engine->event_head = (uint8_t)((engine->event_head + 1U) % capacity);
        --engine->event_count;
        ++engine->diagnostics.dropped_events;
        ESP_LOGW(TAG, "HABIT_EVENT_DROP reason=fifo_full policy=drop_oldest dropped_events=%lu",
                 (unsigned long)engine->diagnostics.dropped_events);
    }
    const uint8_t index = (uint8_t)((engine->event_head + engine->event_count) % capacity);
    habit_event_t *event = &engine->events[index];
    memset(event, 0, sizeof(*event));
    ++s_runtime_event_sequence;
    if (s_runtime_event_sequence == 0U) s_runtime_event_sequence = 1U;
    (void)snprintf(event->event_id, sizeof(event->event_id), "habit-%llu-%lu",
                   (unsigned long long)snapshot->monotonic_ms,
                   (unsigned long)s_runtime_event_sequence);
    (void)snprintf(event->rule_id, sizeof(event->rule_id), "%s", engine->rule_ids[rule]);
    (void)snprintf(event->event_type, sizeof(event->event_type), "%s", event_name(type));
    (void)snprintf(event->room, sizeof(event->room), "%s", snapshot->room);
    (void)snprintf(event->source, sizeof(event->source), "%s",
                   snapshot->source != NULL && snapshot->source[0] != '\0' ? snapshot->source : "unknown");
    event->sequence = s_runtime_event_sequence;
    event->person_count = snapshot->person_count;
    timestamp_text(clock, event->timestamp);
    (void)snprintf(event->reason, sizeof(event->reason), "%s", reason);
    ++engine->event_count;
    ++engine->diagnostics.generated_events;
    if (event->timestamp[0] == '\0') {
        (void)snprintf(event->timestamp, sizeof(event->timestamp), "%s", "unavailable");
    }
    ESP_LOGI(TAG, "HABIT_EVENT id=%s rule_id=%s type=%s room=%s source=%s person_count=%u reason=%s",
             event->event_id, event->rule_id, event->event_type, event->room, event->source,
             (unsigned int)event->person_count, event->reason);
}

static habit_wall_clock_t read_clock(const habit_rule_engine_t *engine)
{
    habit_wall_clock_t clock = {0};
    if (engine->time_provider.get_local_time != NULL &&
        engine->time_provider.get_local_time(engine->time_provider.context, &clock)) {
        clock.valid = true;
    }
    return clock;
}

static int find_room(habit_rule_engine_t *engine, const char *room)
{
    int free_slot = -1;
    for (size_t i = 0; i < HABIT_RULE_MAX_ROOMS; ++i) {
        if (engine->rooms[i].used && strcmp(engine->rooms[i].room, room) == 0) return (int)i;
        if (!engine->rooms[i].used && free_slot < 0) free_slot = (int)i;
    }
    if (free_slot >= 0) {
        engine->rooms[free_slot].used = true;
        (void)snprintf(engine->rooms[free_slot].room, sizeof(engine->rooms[free_slot].room), "%s", room);
    }
    return free_slot;
}

void habit_rule_engine_init(habit_rule_engine_t *engine)
{
    if (engine == NULL) return;
    memset(engine, 0, sizeof(*engine));
    rule_loader_load_defaults(engine);
    engine->time_provider.get_local_time = habit_time_provider_unavailable;
    engine->diagnostics.version = engine->version;
    engine->diagnostics.enabled_rule_count = count_enabled(engine);
}

void habit_rule_engine_set_time_provider(habit_rule_engine_t *engine,
                                         const habit_time_provider_t *provider)
{
    if (engine == NULL) return;
    if (provider != NULL) engine->time_provider = *provider;
    else engine->time_provider = (habit_time_provider_t){ .get_local_time = habit_time_provider_unavailable };
}

esp_err_t habit_rule_engine_load_json(habit_rule_engine_t *engine, const char *json)
{
    const esp_err_t ret = rule_loader_load_json(engine, json);
    if (ret == ESP_OK) {
        engine->diagnostics.version = engine->version;
        engine->diagnostics.enabled_rule_count = count_enabled(engine);
        ESP_LOGI(TAG, "HABIT_RULE version=%lu enabled=%u", (unsigned long)engine->version,
                 (unsigned int)engine->diagnostics.enabled_rule_count);
    }
    return ret;
}

void habit_rule_engine_process(habit_rule_engine_t *engine, const habit_room_snapshot_t *snapshot)
{
    if (engine == NULL || snapshot == NULL || !snapshot->occupied_known ||
        snapshot->room == NULL || snapshot->room[0] == '\0' || snapshot->monotonic_ms == 0U) return;
    const int index = find_room(engine, snapshot->room);
    if (index < 0) return;
    habit_room_runtime_t *room = &engine->rooms[index];
    const habit_wall_clock_t clock = read_clock(engine);
    const bool first_sample = room->occupied_since_ms == 0U && room->empty_since_ms == 0U;

    if (first_sample) {
        room->occupied = snapshot->occupied;
        room->occupied_since_ms = snapshot->occupied ? snapshot->monotonic_ms : 0U;
        room->empty_since_ms = snapshot->occupied ? 0U : snapshot->monotonic_ms;
        return;
    }

    if (room->occupied != snapshot->occupied) {
        const bool entering = snapshot->occupied;
        room->occupied = entering;
        room->long_stay_emitted = false;
        room->long_occupancy_emitted = false;
        room->empty_timeout_emitted = false;
        room->occupied_since_ms = entering ? snapshot->monotonic_ms : 0U;
        room->empty_since_ms = entering ? 0U : snapshot->monotonic_ms;
        if (entering) {
            if (engine->rule_enabled[HABIT_RULE_PERSON_ENTER_ROOM] &&
                rule_matches_snapshot(engine, HABIT_RULE_PERSON_ENTER_ROOM, snapshot)) {
                emit(engine, HABIT_RULE_PERSON_ENTER_ROOM, HABIT_EVENT_PERSON_ENTER_ROOM,
                     snapshot, &clock, "occupied_false_to_true");
            }
            if (engine->rule_enabled[HABIT_RULE_NIGHT_ACTIVITY] &&
                rule_matches_snapshot(engine, HABIT_RULE_NIGHT_ACTIVITY, snapshot)) {
                if (!clock.valid) {
                    ++engine->diagnostics.time_unavailable_skips;
                    ESP_LOGW(TAG, "HABIT_RULE_SKIP reason=time_unavailable room=%s", snapshot->room);
                } else if (night_hour(engine, clock.hour)) {
                    emit(engine, HABIT_RULE_NIGHT_ACTIVITY, HABIT_EVENT_NIGHT_ACTIVITY,
                         snapshot, &clock, "entered_during_night");
                }
            }
        } else if (engine->rule_enabled[HABIT_RULE_PERSON_LEAVE_ROOM] &&
                   rule_matches_snapshot(engine, HABIT_RULE_PERSON_LEAVE_ROOM, snapshot)) {
            emit(engine, HABIT_RULE_PERSON_LEAVE_ROOM, HABIT_EVENT_PERSON_LEAVE_ROOM,
                 snapshot, &clock, "occupied_true_to_false");
        }
    }

    if (room->occupied && room->occupied_since_ms > 0U && snapshot->monotonic_ms >= room->occupied_since_ms) {
        const uint64_t duration = snapshot->monotonic_ms - room->occupied_since_ms;
        if (engine->rule_enabled[HABIT_RULE_PERSON_LONG_STAY] && !room->long_stay_emitted &&
            rule_matches_snapshot(engine, HABIT_RULE_PERSON_LONG_STAY, snapshot) &&
            duration >= engine->long_stay_ms) {
            room->long_stay_emitted = true;
            emit(engine, HABIT_RULE_PERSON_LONG_STAY, HABIT_EVENT_PERSON_LONG_STAY,
                 snapshot, &clock, "occupied_for_long_stay_threshold");
        }
        if (engine->rule_enabled[HABIT_RULE_LONG_OCCUPANCY] && !room->long_occupancy_emitted &&
            rule_matches_snapshot(engine, HABIT_RULE_LONG_OCCUPANCY, snapshot) &&
            duration >= engine->long_occupancy_ms) {
            room->long_occupancy_emitted = true;
            emit(engine, HABIT_RULE_LONG_OCCUPANCY, HABIT_EVENT_LONG_OCCUPANCY,
                 snapshot, &clock, "occupied_for_long_occupancy_threshold");
        }
    }
    if (!room->occupied && room->empty_since_ms > 0U && snapshot->monotonic_ms >= room->empty_since_ms &&
        engine->rule_enabled[HABIT_RULE_ROOM_EMPTY_TIMEOUT] && !room->empty_timeout_emitted &&
        rule_matches_snapshot(engine, HABIT_RULE_ROOM_EMPTY_TIMEOUT, snapshot) &&
        snapshot->monotonic_ms - room->empty_since_ms >= engine->empty_timeout_ms) {
        room->empty_timeout_emitted = true;
        emit(engine, HABIT_RULE_ROOM_EMPTY_TIMEOUT, HABIT_EVENT_ROOM_EMPTY_TIMEOUT,
             snapshot, &clock, "empty_for_timeout_threshold");
    }
}

bool habit_rule_engine_pop_event(habit_rule_engine_t *engine, habit_event_t *out)
{
    if (engine == NULL || out == NULL || engine->event_count == 0U) return false;
    *out = engine->events[engine->event_head];
    engine->event_head = (uint8_t)((engine->event_head + 1U) %
                                   (sizeof(engine->events) / sizeof(engine->events[0])));
    --engine->event_count;
    return true;
}

void habit_rule_engine_get_diagnostics(const habit_rule_engine_t *engine,
                                       habit_rule_diagnostics_t *out)
{
    if (engine != NULL && out != NULL) *out = engine->diagnostics;
}

#ifdef ESP_PLATFORM
static void habit_rule_task(void *arg)
{
    (void)arg;
    for (;;) {
        habit_room_snapshot_t snapshot = {0};
        if (s_snapshot_provider != NULL && s_snapshot_provider(s_snapshot_context, &snapshot)) {
            xSemaphoreTake(s_runtime_lock, portMAX_DELAY);
            habit_rule_engine_process(&s_runtime_engine, &snapshot);
            xSemaphoreGive(s_runtime_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(250U));
    }
}
#endif

esp_err_t habit_rule_engine_start(habit_snapshot_provider_t provider, void *context)
{
    if (provider == NULL) return ESP_ERR_INVALID_ARG;
#ifdef ESP_PLATFORM
    if (s_runtime_task != NULL) return ESP_OK;
    habit_rule_engine_init(&s_runtime_engine);
    s_runtime_lock = xSemaphoreCreateMutexStatic(&s_runtime_lock_storage);
    if (s_runtime_lock == NULL) return ESP_ERR_NO_MEM;
    if (s_runtime_time_provider_set) {
        habit_rule_engine_set_time_provider(&s_runtime_engine, &s_runtime_time_provider);
    }
    s_snapshot_provider = provider;
    s_snapshot_context = context;
    ESP_LOGI(TAG, "HABIT_RULE version=%lu enabled=%u", (unsigned long)s_runtime_engine.version,
             (unsigned int)s_runtime_engine.diagnostics.enabled_rule_count);
    if (xTaskCreateWithCaps(habit_rule_task,
                            "habit_rule",
                            4096U,
                            NULL,
                            3U,
                            &s_runtime_task,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        s_runtime_task = NULL;
        s_snapshot_provider = NULL;
        return ESP_ERR_NO_MEM;
    }
    app_stack_monitor_log_task_created(TAG, "habit_rule", s_runtime_task, 4096U);
    return ESP_OK;
#else
    (void)provider;
    (void)context;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void habit_rule_engine_runtime_set_time_provider(const habit_time_provider_t *provider)
{
    s_runtime_time_provider = provider != NULL ? *provider : (habit_time_provider_t){
        .get_local_time = habit_time_provider_unavailable,
    };
    s_runtime_time_provider_set = true;
#ifdef ESP_PLATFORM
    if (s_runtime_task != NULL) {
        habit_rule_engine_set_time_provider(&s_runtime_engine, &s_runtime_time_provider);
    }
#endif
}

void habit_rule_engine_runtime_get_diagnostics(habit_rule_diagnostics_t *out)
{
    habit_rule_engine_get_diagnostics(&s_runtime_engine, out);
}

bool habit_rule_engine_runtime_pop_event(habit_event_t *out)
{
#ifdef ESP_PLATFORM
    if (out == NULL || s_runtime_lock == NULL) return false;
    xSemaphoreTake(s_runtime_lock, portMAX_DELAY);
    const bool found = habit_rule_engine_pop_event(&s_runtime_engine, out);
    xSemaphoreGive(s_runtime_lock);
    return found;
#else
    (void)out;
    return false;
#endif
}

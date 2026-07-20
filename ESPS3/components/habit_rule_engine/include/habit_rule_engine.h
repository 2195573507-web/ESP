#ifndef HABIT_RULE_ENGINE_H
#define HABIT_RULE_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HABIT_RULE_MAX_ROOMS 8U
#define HABIT_RULE_ROOM_NAME_BYTES 32U
#define HABIT_EVENT_TYPE_BYTES 32U
#define HABIT_RULE_ID_BYTES 128U
#define HABIT_EVENT_ID_BYTES 48U
#define HABIT_EVENT_SOURCE_BYTES 16U
#define HABIT_EVENT_TIMESTAMP_BYTES 32U
#define HABIT_EVENT_REASON_BYTES 64U

typedef enum {
    HABIT_EVENT_PERSON_ENTER_ROOM = 0,
    HABIT_EVENT_PERSON_LEAVE_ROOM,
    HABIT_EVENT_PERSON_LONG_STAY,
    HABIT_EVENT_ROOM_EMPTY_TIMEOUT,
    HABIT_EVENT_NIGHT_ACTIVITY,
    HABIT_EVENT_LONG_OCCUPANCY,
} habit_event_type_t;

typedef enum {
    HABIT_RULE_PERSON_ENTER_ROOM = 0,
    HABIT_RULE_PERSON_LEAVE_ROOM,
    HABIT_RULE_PERSON_LONG_STAY,
    HABIT_RULE_ROOM_EMPTY_TIMEOUT,
    HABIT_RULE_NIGHT_ACTIVITY,
    HABIT_RULE_LONG_OCCUPANCY,
    HABIT_RULE_COUNT,
} habit_rule_id_t;

typedef struct {
    bool valid;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} habit_wall_clock_t;

typedef struct {
    bool (*get_local_time)(void *context, habit_wall_clock_t *out);
    void *context;
} habit_time_provider_t;

typedef struct {
    bool occupied_known;
    bool occupied;
    uint8_t person_count;
    const char *room;
    const char *source;
    const char *zone;
    uint64_t monotonic_ms;
} habit_room_snapshot_t;

typedef struct {
    char event_id[HABIT_EVENT_ID_BYTES];
    char rule_id[HABIT_RULE_ID_BYTES];
    char event_type[HABIT_EVENT_TYPE_BYTES];
    char room[HABIT_RULE_ROOM_NAME_BYTES];
    char source[HABIT_EVENT_SOURCE_BYTES];
    uint32_t sequence;
    uint8_t person_count;
    char timestamp[HABIT_EVENT_TIMESTAMP_BYTES];
    char reason[HABIT_EVENT_REASON_BYTES];
} habit_event_t;

typedef struct {
    uint32_t version;
    uint8_t enabled_rule_count;
    uint32_t generated_events;
    uint32_t dropped_events;
    uint32_t time_unavailable_skips;
} habit_rule_diagnostics_t;

typedef struct {
    bool used;
    char room[HABIT_RULE_ROOM_NAME_BYTES];
    bool occupied;
    uint64_t occupied_since_ms;
    uint64_t empty_since_ms;
    bool long_stay_emitted;
    bool long_occupancy_emitted;
    bool empty_timeout_emitted;
} habit_room_runtime_t;

typedef struct habit_rule_engine {
    uint32_t version;
    bool rule_enabled[HABIT_RULE_COUNT];
    char rule_ids[HABIT_RULE_COUNT][HABIT_RULE_ID_BYTES];
    char rule_scope_type[HABIT_RULE_COUNT][8];
    char rule_scope_id[HABIT_RULE_COUNT][HABIT_RULE_ROOM_NAME_BYTES];
    uint32_t long_stay_ms;
    uint32_t empty_timeout_ms;
    uint32_t long_occupancy_ms;
    uint8_t night_start_hour;
    uint8_t night_end_hour;
    habit_time_provider_t time_provider;
    habit_room_runtime_t rooms[HABIT_RULE_MAX_ROOMS];
    habit_event_t events[16];
    uint8_t event_head;
    uint8_t event_count;
    habit_rule_diagnostics_t diagnostics;
} habit_rule_engine_t;

void habit_rule_engine_init(habit_rule_engine_t *engine);
void habit_rule_engine_set_time_provider(habit_rule_engine_t *engine,
                                         const habit_time_provider_t *provider);
esp_err_t habit_rule_engine_load_json(habit_rule_engine_t *engine, const char *json);
void habit_rule_engine_process(habit_rule_engine_t *engine, const habit_room_snapshot_t *snapshot);
bool habit_rule_engine_pop_event(habit_rule_engine_t *engine, habit_event_t *out);
void habit_rule_engine_get_diagnostics(const habit_rule_engine_t *engine,
                                       habit_rule_diagnostics_t *out);

typedef bool (*habit_snapshot_provider_t)(void *context, habit_room_snapshot_t *out);
esp_err_t habit_rule_engine_start(habit_snapshot_provider_t provider, void *context);
void habit_rule_engine_runtime_set_time_provider(const habit_time_provider_t *provider);
void habit_rule_engine_runtime_get_diagnostics(habit_rule_diagnostics_t *out);
bool habit_rule_engine_runtime_pop_event(habit_event_t *out);

#ifdef __cplusplus
}
#endif

#endif

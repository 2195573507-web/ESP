#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "habit_rule_engine.h"
#include "radar_home_snapshot.h"

static radar_protocol_payload_t remote_payload(uint8_t local_id, uint32_t sequence,
                                               radar_presence_state_t state)
{
    return (radar_protocol_payload_t){
        .schema_version = RADAR_PROTOCOL_SCHEMA_VERSION,
        .local_id = local_id,
        .sequence = sequence,
        .uptime_ms = sequence * 100U,
        .state = state,
        .target_count = state == RADAR_STATE_PRESENT ? 1U : 0U,
        .uart_online = true,
        .frame_fresh = true,
    };
}

static radar_count_summary_t person_count(uint8_t count)
{
    return (radar_count_summary_t){
        .visible_person_count = count,
        .source_person_count = count,
        .count_state = RADAR_PERSON_COUNT_OBSERVED,
    };
}

static void update_remote(radar_source_id_t source, uint8_t local_id, uint32_t sequence,
                          uint64_t timestamp_ms, radar_presence_state_t state, uint8_t count)
{
    bool changed = false;
    const radar_protocol_payload_t payload = remote_payload(local_id, sequence, state);
    const radar_count_summary_t counts = person_count(count);
    assert(radar_registry_update_remote(source, &payload, &counts, 1U,
                                        timestamp_ms, &changed) ==
           RADAR_REGISTRY_UPDATE_ACCEPTED);
}

static void update_local(uint64_t timestamp_ms, radar_presence_state_t state, uint8_t count)
{
    bool changed = false;
    const radar_snapshot_t snapshot = {
        .state = state,
        .uart_online = true,
        .frame_fresh = true,
        .last_valid_frame_ms = timestamp_ms,
    };
    const radar_count_summary_t counts = person_count(count);
    assert(radar_registry_update_local(&snapshot, &counts, NULL, timestamp_ms,
                                       &changed));
}

static const radar_home_snapshot_room_t *room(const radar_home_snapshot_t *snapshot,
                                              const char *room_id)
{
    for (uint8_t index = 0U; index < snapshot->room_count; ++index) {
        if (strcmp(snapshot->rooms[index].room_id, room_id) == 0) return &snapshot->rooms[index];
    }
    return NULL;
}

static void process_room(habit_rule_engine_t *engine, const radar_home_snapshot_room_t *value)
{
    const habit_room_snapshot_t input = {
        .occupied_known = value->known,
        .occupied = value->occupied,
        .person_count = value->person_count,
        .room = value->room_id,
        .source = value->dominant_source,
        .monotonic_ms = value->timestamp_ms,
    };
    habit_rule_engine_process(engine, &input);
}

static void assert_enter_event(habit_rule_engine_t *engine, const char *expected_room,
                               const char *expected_source)
{
    habit_event_t event = {0};
    assert(habit_rule_engine_pop_event(engine, &event));
    assert(strcmp(event.rule_id, "person_enter_room") == 0);
    assert(strcmp(event.event_type, "PERSON_ENTER_ROOM") == 0);
    assert(strcmp(event.room, expected_room) == 0);
    assert(strcmp(event.source, expected_source) == 0);
}

static void test_c51_bedroom_occupied(void)
{
    habit_rule_engine_t engine;
    habit_rule_engine_init(&engine);
    update_remote(RADAR_SOURCE_C51, 1U, 1U, 10U, RADAR_STATE_VACANT_INFERRED, 0U);
    radar_home_snapshot_t snapshot = {0};
    assert(radar_home_snapshot_get(&snapshot));
    process_room(&engine, room(&snapshot, "living_room"));
    update_remote(RADAR_SOURCE_C51, 1U, 2U, 20U, RADAR_STATE_PRESENT, 1U);
    assert(radar_home_snapshot_get(&snapshot));
    const radar_home_snapshot_room_t *living = room(&snapshot, "living_room");
    assert(living != NULL && living->occupied && living->person_count == 1U);
    assert(strcmp(living->dominant_source, "C51") == 0);
    process_room(&engine, living);
    assert_enter_event(&engine, "living_room", "C51");
}

static void test_c52_bedroom_occupied(void)
{
    habit_rule_engine_t engine;
    habit_rule_engine_init(&engine);
    update_remote(RADAR_SOURCE_C52, 2U, 1U, 30U, RADAR_STATE_VACANT_INFERRED, 0U);
    radar_home_snapshot_t snapshot = {0};
    assert(radar_home_snapshot_get(&snapshot));
    process_room(&engine, room(&snapshot, "bedroom"));
    update_remote(RADAR_SOURCE_C52, 2U, 2U, 40U, RADAR_STATE_PRESENT, 1U);
    assert(radar_home_snapshot_get(&snapshot));
    const radar_home_snapshot_room_t *bedroom = room(&snapshot, "bedroom");
    assert(bedroom != NULL && bedroom->occupied && bedroom->person_count == 1U);
    assert(strcmp(bedroom->dominant_source, "C52") == 0);
    process_room(&engine, bedroom);
    assert_enter_event(&engine, "bedroom", "C52");
}

static void test_s3_local_living_room_occupied(void)
{
    habit_rule_engine_t engine;
    habit_rule_engine_init(&engine);
    update_local(50U, RADAR_STATE_VACANT_INFERRED, 0U);
    radar_home_snapshot_t snapshot = {0};
    assert(radar_home_snapshot_get(&snapshot));
    process_room(&engine, room(&snapshot, "s3_local"));
    update_local(60U, RADAR_STATE_PRESENT, 1U);
    assert(radar_home_snapshot_get(&snapshot));
    const radar_home_snapshot_room_t *local = room(&snapshot, "s3_local");
    assert(local != NULL && local->occupied && local->person_count == 1U);
    assert(strcmp(local->dominant_source, "S3_LOCAL") == 0);
    process_room(&engine, local);
    assert_enter_event(&engine, "s3_local", "S3_LOCAL");
}

static void test_multiple_people_home_snapshot(void)
{
    update_remote(RADAR_SOURCE_C51, 1U, 3U, 70U, RADAR_STATE_PRESENT, 2U);
    update_remote(RADAR_SOURCE_C52, 2U, 3U, 70U, RADAR_STATE_PRESENT, 1U);
    update_local(70U, RADAR_STATE_PRESENT, 1U);
    radar_home_snapshot_t snapshot = {0};
    assert(radar_home_snapshot_get(&snapshot));
    assert(snapshot.occupancy_known && snapshot.occupied);
    assert(snapshot.room_count == 3U);
    assert(snapshot.person_count == 4U);
}

int main(void)
{
    assert(radar_registry_init());
    test_c51_bedroom_occupied();
    test_c52_bedroom_occupied();
    test_s3_local_living_room_occupied();
    test_multiple_people_home_snapshot();
    puts("habit HOME snapshot integration host tests: PASS");
    return 0;
}

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "habit_rule_engine.h"

static habit_wall_clock_t s_clock;

static bool fake_time(void *context, habit_wall_clock_t *out)
{
    (void)context;
    *out = s_clock;
    return s_clock.valid;
}

static void set_time(uint8_t hour, uint8_t minute)
{
    s_clock = (habit_wall_clock_t){
        .valid = true, .year = 2026U, .month = 7U, .day = 19U,
        .hour = hour, .minute = minute,
    };
}

static void process(habit_rule_engine_t *engine, const char *room, bool occupied,
                    uint8_t count, uint64_t monotonic_ms)
{
    const habit_room_snapshot_t snapshot = {
        .occupied_known = true,
        .occupied = occupied,
        .person_count = count,
        .room = room,
        .monotonic_ms = monotonic_ms,
    };
    habit_rule_engine_process(engine, &snapshot);
}

static habit_event_t next_event(habit_rule_engine_t *engine)
{
    habit_event_t event;
    assert(habit_rule_engine_pop_event(engine, &event));
    return event;
}

static void prepare(habit_rule_engine_t *engine, bool clock_available)
{
    habit_rule_engine_init(engine);
    if (clock_available) {
        const habit_time_provider_t provider = { .get_local_time = fake_time };
        habit_rule_engine_set_time_provider(engine, &provider);
    }
}

static void assert_event(const habit_event_t *event, const char *type, const char *room)
{
    assert(strcmp(event->event_type, type) == 0);
    assert(strcmp(event->room, room) == 0);
}

static void test_enter_leave(void)
{
    habit_rule_engine_t engine;
    set_time(12U, 0U);
    prepare(&engine, true);
    process(&engine, "living_room", false, 0U, 10U);
    process(&engine, "living_room", true, 1U, 100U);
    habit_event_t event = next_event(&engine);
    assert_event(&event, "PERSON_ENTER_ROOM", "living_room");
    process(&engine, "living_room", false, 0U, 200U);
    event = next_event(&engine);
    assert_event(&event, "PERSON_LEAVE_ROOM", "living_room");
}

static void test_long_stay_and_empty_timeout(void)
{
    habit_rule_engine_t engine;
    set_time(12U, 0U);
    prepare(&engine, true);
    assert(habit_rule_engine_load_json(&engine,
        "{\"version\":1,\"rules\":[{\"id\":\"long_stay\",\"enabled\":true,\"parameters\":{\"minutes\":1}},"
        "{\"id\":\"empty_timeout\",\"enabled\":true,\"parameters\":{\"minutes\":1}}]}") == ESP_OK);
    process(&engine, "living_room", false, 0U, 10U);
    process(&engine, "living_room", true, 1U, 100U);
    (void)next_event(&engine);
    process(&engine, "living_room", true, 1U, 60100U);
    habit_event_t event = next_event(&engine);
    assert_event(&event, "PERSON_LONG_STAY", "living_room");
    process(&engine, "living_room", false, 0U, 70000U);
    (void)next_event(&engine);
    process(&engine, "living_room", false, 0U, 130000U);
    event = next_event(&engine);
    assert_event(&event, "ROOM_EMPTY_TIMEOUT", "living_room");
}

static void test_night_window(void)
{
    struct {
        uint8_t hour;
        uint8_t minute;
        bool expected;
    } cases[] = {
        {22U, 0U, true}, {21U, 59U, false}, {5U, 59U, true}, {6U, 1U, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        habit_rule_engine_t engine;
        set_time(cases[i].hour, cases[i].minute);
        prepare(&engine, true);
        process(&engine, "living_room", false, 0U, 10U);
        process(&engine, "living_room", true, 1U, 100U);
        const habit_event_t enter = next_event(&engine);
        assert_event(&enter, "PERSON_ENTER_ROOM", "living_room");
        habit_event_t event;
        const bool has_night = habit_rule_engine_pop_event(&engine, &event);
        assert(has_night == cases[i].expected);
        if (has_night) assert_event(&event, "NIGHT_ACTIVITY", "living_room");
    }
}

static void test_time_unavailable_skips_night(void)
{
    habit_rule_engine_t engine;
    prepare(&engine, false);
    process(&engine, "living_room", false, 0U, 10U);
    process(&engine, "living_room", true, 1U, 100U);
    habit_event_t event = next_event(&engine);
    assert_event(&event, "PERSON_ENTER_ROOM", "living_room");
    assert(!habit_rule_engine_pop_event(&engine, &event));
    habit_rule_diagnostics_t diagnostics;
    habit_rule_engine_get_diagnostics(&engine, &diagnostics);
    assert(diagnostics.time_unavailable_skips == 1U);
}

static void test_rooms_are_independent(void)
{
    habit_rule_engine_t engine;
    set_time(12U, 0U);
    prepare(&engine, true);
    assert(habit_rule_engine_load_json(&engine,
        "{\"version\":2,\"rules\":[{\"id\":\"empty_timeout\",\"enabled\":true,\"parameters\":{\"minutes\":1}}]}") == ESP_OK);
    process(&engine, "living_room", false, 0U, 10U);
    process(&engine, "bedroom", false, 0U, 20U);
    process(&engine, "living_room", true, 1U, 100U);
    process(&engine, "bedroom", true, 1U, 200U);
    process(&engine, "living_room", false, 0U, 300U);
    process(&engine, "bedroom", true, 1U, 60300U);
    process(&engine, "living_room", false, 0U, 60300U);
    habit_event_t event;
    bool found_living_timeout = false;
    while (habit_rule_engine_pop_event(&engine, &event)) {
        if (strcmp(event.event_type, "ROOM_EMPTY_TIMEOUT") == 0) {
            assert(strcmp(event.room, "living_room") == 0);
            found_living_timeout = true;
        }
    }
    assert(found_living_timeout);
}

int main(void)
{
    test_enter_leave();
    test_long_stay_and_empty_timeout();
    test_night_window();
    test_time_unavailable_skips_night();
    test_rooms_are_independent();
    puts("habit rule engine host tests: PASS");
    return 0;
}

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "radar_config.h"
#include "radar_person_continuity.h"

static radar_person_continuity_config_t continuity_config(void)
{
    return (radar_person_continuity_config_t){
        .still_hold_ms = RADAR_CONFIG_PERSON_STILL_HOLD_MS,
        .dormant_timeout_ms = RADAR_CONFIG_PERSON_DORMANT_TIMEOUT_MS,
        .reacquire_gate_mm = RADAR_CONFIG_PERSON_REACQUIRE_GATE_MM,
        .same_zone_bonus_mm = 250U,
        .adjacent_zone_allow = true,
        .new_confirm_frames = 2U,
        .new_near_dormant_confirm_frames = 3U,
        .velocity_decay_start_ms = 400U,
        .velocity_decay_ms = 600U,
    };
}

static radar_track_snapshot_t track(uint32_t id, int32_t x_mm, int32_t y_mm, uint8_t zone)
{
    return (radar_track_snapshot_t){
        .active = true,
        .visible = true,
        .lifecycle = RADAR_TRACK_CONFIRMED,
        .track_id = id,
        .filtered_x_mm = x_mm,
        .filtered_y_mm = y_mm,
        .x_mm = x_mm,
        .y_mm = y_mm,
        .zone_id = zone,
        .confidence = 100U,
    };
}

static void step(radar_person_continuity_t *continuity,
                 radar_track_snapshot_t *tracks,
                 size_t track_count,
                 bool source_valid,
                 uint64_t now_ms)
{
    radar_person_continuity_update(continuity, tracks, track_count,
                                   source_valid, false, now_ms);
}

static void init(radar_person_continuity_t *continuity)
{
    const radar_person_continuity_config_t config = continuity_config();
    radar_person_continuity_init(continuity, &config, 0U);
}

static const radar_person_snapshot_t *person(const radar_person_continuity_t *continuity,
                                             uint32_t person_id)
{
    for (size_t i = 0U; i < RADAR_PERSON_CONTINUITY_MAX_PERSONS; ++i) {
        if (continuity->persons[i].person_id == person_id) {
            return &continuity->persons[i];
        }
    }
    return NULL;
}

static void counts(const radar_person_continuity_t *continuity,
                   uint8_t visible,
                   uint8_t retained,
                   uint8_t business,
                   radar_person_count_state_t state)
{
    assert(continuity->visible_person_count == visible);
    assert(continuity->retained_person_count == retained);
    assert(continuity->source_person_count == business);
    assert(continuity->count_state == state);
}

static void establish_single(radar_person_continuity_t *continuity, uint32_t track_id,
                             int32_t x_mm, uint64_t now_ms)
{
    radar_track_snapshot_t current = track(track_id, x_mm, 1000, 1U);
    step(continuity, &current, 1U, true, now_ms);
    step(continuity, &current, 1U, true, now_ms + 100U);
    counts(continuity, 1U, 0U, 1U, RADAR_PERSON_COUNT_OBSERVED);
}

static void test_continuous_walk(void)
{
    radar_person_continuity_t continuity;
    init(&continuity);
    radar_track_snapshot_t current = track(1U, 0, 1000, 1U);
    step(&continuity, &current, 1U, true, 100U);
    current.filtered_x_mm = current.x_mm = 200;
    step(&continuity, &current, 1U, true, 200U);
    current.filtered_x_mm = current.x_mm = 400;
    step(&continuity, &current, 1U, true, 300U);
    counts(&continuity, 1U, 0U, 1U, RADAR_PERSON_COUNT_OBSERVED);
    assert(continuity.persons[0].person_id == 1U && continuity.persons[0].attached_track_id == 1U);
    puts("PASS: replay 1 continuous single-person walk");
}

static void test_short_stops_and_reacquire(void)
{
    const uint32_t stop_ms[] = {500U, 1000U, 2000U, 3000U, 5000U, 11000U};
    for (size_t i = 0U; i < sizeof(stop_ms) / sizeof(stop_ms[0]); ++i) {
        radar_person_continuity_t continuity;
        init(&continuity);
        establish_single(&continuity, 1U, 0, 100U);
        step(&continuity, NULL, 0U, true, 100U + stop_ms[i]);
        radar_track_snapshot_t restored = track(100U + (uint32_t)i, 100, 1000, 1U);
        step(&continuity, &restored, 1U, true, 200U + stop_ms[i]);
        counts(&continuity, 1U, 0U, 1U, RADAR_PERSON_COUNT_OBSERVED);
        assert(continuity.persons[0].person_id == 1U);
        assert(continuity.persons[0].attached_track_id == restored.track_id);
    }
    puts("PASS: replay 2-7 household stop/reacquire through 11 seconds");
}

static void test_timeout_creates_new_person(void)
{
    radar_person_continuity_t continuity;
    init(&continuity);
    establish_single(&continuity, 1U, 0, 100U);
    step(&continuity, NULL, 0U, true, 12201U);
    counts(&continuity, 0U, 0U, 0U, RADAR_PERSON_COUNT_VACANT_INFERRED);
    radar_track_snapshot_t replacement = track(2U, 0, 1000, 1U);
    step(&continuity, &replacement, 1U, true, 12300U);
    step(&continuity, &replacement, 1U, true, 12400U);
    counts(&continuity, 1U, 0U, 1U, RADAR_PERSON_COUNT_OBSERVED);
    assert(continuity.persons[0].person_id == 2U);
    puts("PASS: replay 5 timeout releases old person and allows new person");
}

static void test_drift_and_gate(void)
{
    radar_person_continuity_t continuity;
    init(&continuity);
    establish_single(&continuity, 1U, 0, 100U);
    step(&continuity, NULL, 0U, true, 600U);
    radar_track_snapshot_t drift = track(2U, 500, 1000, 1U);
    step(&continuity, &drift, 1U, true, 700U);
    assert(continuity.persons[0].person_id == 1U);

    init(&continuity);
    establish_single(&continuity, 1U, 0, 100U);
    step(&continuity, NULL, 0U, true, 600U);
    radar_track_snapshot_t within_gate = track(2U, 1990, 1000, 1U);
    step(&continuity, &within_gate, 1U, true, 700U);
    assert(continuity.persons[0].person_id == 1U);
    counts(&continuity, 1U, 0U, 1U, RADAR_PERSON_COUNT_OBSERVED);
    puts("PASS: replay 8 filtered drift and 2 m reacquire gate");
}

static void test_far_new_person(void)
{
    radar_person_continuity_t continuity;
    init(&continuity);
    establish_single(&continuity, 1U, 0, 100U);
    step(&continuity, NULL, 0U, true, 1500U);
    radar_track_snapshot_t far = track(2U, 3000, 1000, 1U);
    step(&continuity, &far, 1U, true, 1600U);
    step(&continuity, &far, 1U, true, 1700U);
    counts(&continuity, 1U, 1U, 2U, RADAR_PERSON_COUNT_OBSERVED);
    assert(person(&continuity, 1U) != NULL && person(&continuity, 2U) != NULL);
    puts("PASS: replay 7 far entrant is not merged with departing person");
}

static void establish_pair(radar_person_continuity_t *continuity, uint64_t now_ms)
{
    radar_track_snapshot_t pair[2] = {track(1U, -800, 1000, 1U), track(2U, 800, 1000, 1U)};
    step(continuity, pair, 2U, true, now_ms);
    step(continuity, pair, 2U, true, now_ms + 100U);
    counts(continuity, 2U, 0U, 2U, RADAR_PERSON_COUNT_OBSERVED);
}

static void test_two_people_and_crossing(void)
{
    radar_person_continuity_t continuity;
    init(&continuity);
    establish_pair(&continuity, 100U);
    radar_track_snapshot_t one_moves[2] = {track(1U, -400, 1000, 1U), track(2U, 900, 1000, 1U)};
    step(&continuity, one_moves, 2U, true, 300U);
    counts(&continuity, 2U, 0U, 2U, RADAR_PERSON_COUNT_OBSERVED);

    init(&continuity);
    establish_pair(&continuity, 100U);
    step(&continuity, NULL, 0U, true, 700U);
    radar_track_snapshot_t swapped[2] = {track(3U, 760, 1000, 1U), track(4U, -760, 1000, 1U)};
    step(&continuity, swapped, 2U, true, 800U);
    counts(&continuity, 2U, 0U, 2U, RADAR_PERSON_COUNT_OBSERVED);
    assert(person(&continuity, 1U)->attached_track_id == 4U);
    assert(person(&continuity, 2U)->attached_track_id == 3U);
    puts("PASS: replay 8 two-person stop/move and replay 10 crossing/slot exchange");
}

static void test_both_recover_and_false_targets(void)
{
    radar_person_continuity_t continuity;
    init(&continuity);
    establish_pair(&continuity, 100U);
    step(&continuity, NULL, 0U, true, 900U);
    radar_track_snapshot_t restored[2] = {track(3U, -700, 1000, 1U), track(4U, 700, 1000, 1U)};
    step(&continuity, restored, 2U, true, 1000U);
    counts(&continuity, 2U, 0U, 2U, RADAR_PERSON_COUNT_OBSERVED);

    counts(&continuity, 2U, 0U, 2U, RADAR_PERSON_COUNT_OBSERVED);

    init(&continuity);
    establish_single(&continuity, 3U, -700, 1050U);
    radar_track_snapshot_t single_with_false[2] = {track(3U, -600, 1000, 1U), track(9U, 3000, 3000, 1U)};
    step(&continuity, single_with_false, 2U, true, 1100U);
    counts(&continuity, 1U, 0U, 1U, RADAR_PERSON_COUNT_OBSERVED);
    radar_track_snapshot_t single = track(3U, -500, 1000, 1U);
    step(&continuity, &single, 1U, true, 1200U);
    counts(&continuity, 1U, 0U, 1U, RADAR_PERSON_COUNT_OBSERVED);
    puts("PASS: replay 9 both recover and replay 11 one-frame false target");
}

static void test_stale_offline_history_and_dedup(void)
{
    radar_person_continuity_t continuity;
    init(&continuity);
    establish_single(&continuity, 1U, 0, 100U);
    step(&continuity, NULL, 0U, false, 300U);
    counts(&continuity, 0U, 0U, 0U, RADAR_PERSON_COUNT_UNKNOWN);
    step(&continuity, NULL, 0U, true, 500U);
    counts(&continuity, 0U, 1U, 1U, RADAR_PERSON_COUNT_ESTIMATED);
    step(&continuity, NULL, 0U, true, 700U);
    assert(continuity.persons[0].state == RADAR_PERSON_STILL_HOLD);
    radar_track_snapshot_t restored = track(2U, 100, 1000, 1U);
    step(&continuity, &restored, 1U, true, 800U);
    assert(continuity.persons[0].person_id == 1U && continuity.persons[0].attached_track_id == 2U);
    counts(&continuity, 1U, 0U, 1U, RADAR_PERSON_COUNT_OBSERVED);
    step(&continuity, NULL, 0U, true, 1200U);
    counts(&continuity, 0U, 1U, 1U, RADAR_PERSON_COUNT_ESTIMATED);
    puts("PASS: replay 12 stale/offline UNKNOWN and replay 13/14 history/dedup");
}

static void test_no_heap_or_id_semantics(void)
{
    assert(sizeof(radar_person_continuity_t) < 2048U);
    radar_person_continuity_t continuity;
    init(&continuity);
    establish_single(&continuity, 7U, 0, 100U);
    assert(continuity.persons[0].person_id != continuity.persons[0].attached_track_id);
    printf("PASS: fixed-capacity continuity storage=%zu bytes and separate person/track IDs\n",
           sizeof(radar_person_continuity_t));
}

static void test_three_target_processing_budget(void)
{
    radar_person_continuity_t continuity;
    init(&continuity);
    radar_track_snapshot_t tracks[3] = {
        track(1U, -1000, 1200, 1U),
        track(2U, 0, 1500, 1U),
        track(3U, 1000, 1200, 1U),
    };
    step(&continuity, tracks, 3U, true, 100U);
    step(&continuity, tracks, 3U, true, 200U);
    double max_ms = 0.0;
    for (uint32_t i = 0U; i < 1000U; ++i) {
        const clock_t started = clock();
        step(&continuity, tracks, 3U, true, 300U + (uint64_t)i * 20U);
        const double elapsed_ms = 1000.0 * (double)(clock() - started) / CLOCKS_PER_SEC;
        if (elapsed_ms > max_ms) {
            max_ms = elapsed_ms;
        }
    }
    assert(max_ms < 5.0);
    printf("PASS: three-target continuity max host CPU %.3f ms\n", max_ms);
}

int main(void)
{
    test_continuous_walk();
    test_short_stops_and_reacquire();
    test_timeout_creates_new_person();
    test_drift_and_gate();
    test_far_new_person();
    test_two_people_and_crossing();
    test_both_recover_and_false_targets();
    test_stale_offline_history_and_dedup();
    test_no_heap_or_id_semantics();
    test_three_target_processing_budget();
    puts("radar person continuity replay tests: PASS");
    return 0;
}

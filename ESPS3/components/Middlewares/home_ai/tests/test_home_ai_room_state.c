#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "home_ai_room_state.h"

static radar_registry_entry_t entry_for(radar_source_id_t source,
                                        radar_presence_state_t state,
                                        radar_person_count_state_t count_state,
                                        uint8_t business_count,
                                        uint64_t now_ms)
{
    radar_registry_entry_t entry = {0};
    entry.source = source;
    entry.source_online = true;
    entry.snapshot.state = state;
    entry.snapshot.frame_fresh = true;
    entry.last_report_ms = now_ms;
    entry.count_summary.count_state = count_state;
    entry.count_summary.business_person_count = business_count;
    return entry;
}

static home_ai_room_state_t state_for(radar_source_id_t source)
{
    home_ai_room_state_t state = {0};
    assert(home_ai_room_state_get(source, &state));
    return state;
}

static void update_one(radar_registry_entry_t *entry, uint64_t now_ms)
{
    entry->last_report_ms = now_ms;
    home_ai_room_state_update(entry, 1U, now_ms);
}

static void test_presence_debounce_and_mapping(void)
{
    assert(home_ai_room_state_init());
    radar_registry_entry_t living = entry_for(RADAR_SOURCE_S3_LOCAL,
                                              RADAR_STATE_PRESENT,
                                              RADAR_PERSON_COUNT_OBSERVED,
                                              1U,
                                              100U);
    update_one(&living, 100U);
    assert(state_for(RADAR_SOURCE_S3_LOCAL).presence_state == HOME_AI_ROOM_PRESENCE_UNKNOWN);
    update_one(&living, 1599U);
    assert(state_for(RADAR_SOURCE_S3_LOCAL).presence_state == HOME_AI_ROOM_PRESENCE_UNKNOWN);
    update_one(&living, 1600U);
    home_ai_room_state_t state = state_for(RADAR_SOURCE_S3_LOCAL);
    assert(state.presence_state == HOME_AI_ROOM_PRESENCE_OCCUPIED);
    assert(strcmp(state.room_id, "living_room") == 0);
    assert(state.radar_fresh);

    living.snapshot.state = RADAR_STATE_VACANT_INFERRED;
    living.count_summary.count_state = RADAR_PERSON_COUNT_VACANT_INFERRED;
    living.count_summary.business_person_count = 0U;
    update_one(&living, 1700U);
    assert(state_for(RADAR_SOURCE_S3_LOCAL).presence_state == HOME_AI_ROOM_PRESENCE_OCCUPIED);
    update_one(&living, 61699U);
    assert(state_for(RADAR_SOURCE_S3_LOCAL).presence_state == HOME_AI_ROOM_PRESENCE_OCCUPIED);
    update_one(&living, 61700U);
    state = state_for(RADAR_SOURCE_S3_LOCAL);
    assert(state.presence_state == HOME_AI_ROOM_PRESENCE_VACANT);
    assert(state.occupancy_mode == HOME_AI_ROOM_OCCUPANCY_UNKNOWN);
    assert(state.stable_target_count == 0U);
}

static void test_count_debounce_and_conservative_multiple(void)
{
    assert(home_ai_room_state_init());
    radar_registry_entry_t bedroom = entry_for(RADAR_SOURCE_C51,
                                               RADAR_STATE_PRESENT,
                                               RADAR_PERSON_COUNT_OBSERVED,
                                               2U,
                                               100U);
    update_one(&bedroom, 100U);
    update_one(&bedroom, 1600U);
    assert(state_for(RADAR_SOURCE_C51).presence_state == HOME_AI_ROOM_PRESENCE_OCCUPIED);
    update_one(&bedroom, 3099U);
    assert(state_for(RADAR_SOURCE_C51).occupancy_mode == HOME_AI_ROOM_OCCUPANCY_UNKNOWN);
    update_one(&bedroom, 3100U);
    home_ai_room_state_t state = state_for(RADAR_SOURCE_C51);
    assert(state.occupancy_mode == HOME_AI_ROOM_OCCUPANCY_MULTIPLE);
    assert(state.stable_target_count == 2U);

    bedroom.count_summary.business_person_count = 1U;
    update_one(&bedroom, 3200U);
    update_one(&bedroom, 13199U);
    assert(state_for(RADAR_SOURCE_C51).occupancy_mode == HOME_AI_ROOM_OCCUPANCY_MULTIPLE);
    update_one(&bedroom, 13200U);
    state = state_for(RADAR_SOURCE_C51);
    assert(state.occupancy_mode == HOME_AI_ROOM_OCCUPANCY_SINGLE);
    assert(state.stable_target_count == 1U);
}

static void test_stale_and_untrusted_data_degrade_to_unknown(void)
{
    assert(home_ai_room_state_init());
    radar_registry_entry_t bedroom = entry_for(RADAR_SOURCE_C52,
                                               RADAR_STATE_PRESENT,
                                               RADAR_PERSON_COUNT_OBSERVED,
                                               2U,
                                               100U);
    update_one(&bedroom, 100U);
    update_one(&bedroom, 3100U);
    assert(state_for(RADAR_SOURCE_C52).occupancy_mode == HOME_AI_ROOM_OCCUPANCY_MULTIPLE);

    bedroom.source_online = false;
    bedroom.snapshot.frame_fresh = false;
    update_one(&bedroom, 3200U);
    home_ai_room_state_t state = state_for(RADAR_SOURCE_C52);
    assert(state.presence_state == HOME_AI_ROOM_PRESENCE_UNKNOWN);
    assert(state.occupancy_mode == HOME_AI_ROOM_OCCUPANCY_UNKNOWN);
    assert(!state.radar_fresh);

    bedroom.source_online = true;
    bedroom.snapshot.frame_fresh = true;
    bedroom.snapshot.state = RADAR_STATE_PRESENT;
    bedroom.count_summary.count_state = RADAR_PERSON_COUNT_UNKNOWN;
    bedroom.count_summary.business_person_count = 2U;
    update_one(&bedroom, 3300U);
    update_one(&bedroom, 4800U);
    state = state_for(RADAR_SOURCE_C52);
    assert(state.presence_state == HOME_AI_ROOM_PRESENCE_OCCUPIED);
    assert(state.occupancy_mode == HOME_AI_ROOM_OCCUPANCY_UNKNOWN);
}

static void test_null_snapshot_fails_closed(void)
{
    assert(home_ai_room_state_init());
    radar_registry_entry_t bedroom = entry_for(RADAR_SOURCE_C51,
                                               RADAR_STATE_PRESENT,
                                               RADAR_PERSON_COUNT_OBSERVED,
                                               1U,
                                               100U);
    update_one(&bedroom, 100U);
    update_one(&bedroom, 1600U);
    assert(state_for(RADAR_SOURCE_C51).presence_state == HOME_AI_ROOM_PRESENCE_OCCUPIED);

    home_ai_room_state_update(NULL, 1U, 1700U);
    assert(state_for(RADAR_SOURCE_C51).presence_state == HOME_AI_ROOM_PRESENCE_OCCUPIED);

    home_ai_room_state_update(NULL, 0U, 1800U);
    assert(state_for(RADAR_SOURCE_C51).presence_state == HOME_AI_ROOM_PRESENCE_UNKNOWN);
    assert(!state_for(RADAR_SOURCE_C51).radar_fresh);
}

static void test_config_rebind_resets_old_room_state(void)
{
    assert(home_ai_room_state_init());
    home_ai_room_state_config_t configs[HOME_AI_ROOM_STATE_COUNT] = {0};
    for (radar_source_id_t source = RADAR_SOURCE_S3_LOCAL;
         source < RADAR_SOURCE_COUNT;
         source = (radar_source_id_t)(source + 1)) {
        assert(home_ai_room_state_get_config(source, &configs[source]));
    }
    assert(configs[RADAR_SOURCE_S3_LOCAL].voice_terminal_device_id[0] == '\0');
    assert(strcmp(configs[RADAR_SOURCE_C51].voice_terminal_device_id,
                  "sensair_shuttle_01") == 0);
    assert(strcmp(configs[RADAR_SOURCE_C52].voice_terminal_device_id,
                  "sensair_shuttle_02") == 0);
    strcpy(configs[RADAR_SOURCE_C51].room_id, "guest_room");
    strcpy(configs[RADAR_SOURCE_C51].voice_terminal_device_id, "sensair_shuttle_02");
    configs[RADAR_SOURCE_C52].voice_terminal_device_id[0] = '\0';
    configs[RADAR_SOURCE_C51].occupied_confirm_ms = 500U;
    configs[RADAR_SOURCE_C51].quiet_start_minute = 22U * 60U;
    configs[RADAR_SOURCE_C51].quiet_end_minute = 6U * 60U;
    assert(home_ai_room_state_set_environment_fresh(RADAR_SOURCE_C51, true, 100U));
    assert(home_ai_room_state_set_config(configs, HOME_AI_ROOM_STATE_COUNT));
    home_ai_room_state_config_t rebound = {0};
    assert(home_ai_room_state_get_config(RADAR_SOURCE_C51, &rebound));
    assert(strcmp(rebound.voice_terminal_device_id, "sensair_shuttle_02") == 0);

    radar_registry_entry_t bedroom = entry_for(RADAR_SOURCE_C51,
                                               RADAR_STATE_PRESENT,
                                               RADAR_PERSON_COUNT_OBSERVED,
                                               1U,
                                               200U);
    update_one(&bedroom, 200U);
    update_one(&bedroom, 700U);
    const home_ai_room_state_t state = state_for(RADAR_SOURCE_C51);
    assert(strcmp(state.room_id, "guest_room") == 0);
    assert(state.presence_state == HOME_AI_ROOM_PRESENCE_OCCUPIED);
    assert(!state.environment_fresh);
    assert(home_ai_room_state_apply_quiet_schedule(RADAR_SOURCE_C51, true, 800U));
    assert(state_for(RADAR_SOURCE_C51).quiet_state == HOME_AI_ROOM_QUIET_SCHEDULED);
    assert(home_ai_room_state_set_quiet_state(RADAR_SOURCE_C51,
                                              HOME_AI_ROOM_QUIET_TEMPORARY_AWAKE,
                                              900U));
    assert(home_ai_room_state_apply_quiet_schedule(RADAR_SOURCE_C51, true, 1000U));
    assert(state_for(RADAR_SOURCE_C51).quiet_state == HOME_AI_ROOM_QUIET_TEMPORARY_AWAKE);
    configs[RADAR_SOURCE_C52].source = RADAR_SOURCE_C51;
    assert(!home_ai_room_state_set_config(configs, HOME_AI_ROOM_STATE_COUNT));
}

int main(void)
{
    test_presence_debounce_and_mapping();
    test_count_debounce_and_conservative_multiple();
    test_stale_and_untrusted_data_degrade_to_unknown();
    test_null_snapshot_fails_closed();
    test_config_rebind_resets_old_room_state();
    puts("home ai room state host tests: PASS");
    return 0;
}

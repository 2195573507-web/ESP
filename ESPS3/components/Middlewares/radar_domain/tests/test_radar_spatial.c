#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "radar_coordinate_transform.h"
#include "radar_spatial_state.h"
#include "radar_rate_manager.h"
#include "radar_target_tracker.h"
#include "radar_uart_recovery.h"
#include "radar_zone_map.h"

static radar_target_t raw_target(int16_t x, int16_t y, int16_t speed)
{
    return (radar_target_t){
        .valid = true,
        .x_mm = x,
        .y_mm = y,
        .speed_cm_s = speed,
        .resolution_mm = 100U,
        .confidence = 100U,
    };
}

static radar_spatial_target_t spatial_target(int32_t x, int32_t y, int16_t speed)
{
    return (radar_spatial_target_t){
        .valid = true,
        .x_mm = x,
        .y_mm = y,
        .speed_cm_s = speed,
        .distance_mm = 1000U,
    };
}

static radar_frame_t frame(uint32_t seq, uint64_t now_ms, int16_t x, int16_t y, int16_t speed)
{
    radar_frame_t value = {.frame_seq = seq, .received_at_ms = now_ms, .target_count = 1U};
    value.targets[0] = raw_target(x, y, speed);
    return value;
}

static void test_transform_and_zone_map(void)
{
    radar_spatial_config_t config = radar_spatial_default_config();
    radar_spatial_target_t target;
    radar_target_t raw = raw_target(1000, 2000, 10);
    assert(radar_coordinate_transform_target(&config.installation, &raw, &target));
    assert(target.x_mm == 1000 && target.y_mm == 2000);
    assert(target.distance_mm == 2236U);
    assert(target.angle_deg == 27);

    config.installation.flip_x = true;
    config.installation.rotation_deg = 90;
    assert(radar_coordinate_transform_target(&config.installation, &raw, &target));
    assert(target.x_mm == -2000 && target.y_mm == -1000);

    config.installation.flip_x = false;
    config.installation.rotation_deg = 0;
    config.installation.zone_count = 3U;
    config.installation.zones[0] = (radar_zone_definition_t){1U, RADAR_ZONE_ACTIVE, true,
        {-1000, 1000, 0, 3000}, 100U};
    config.installation.zones[1] = (radar_zone_definition_t){2U, RADAR_ZONE_ENTRY, true,
        {1100, 1600, 0, 3000}, 0U};
    config.installation.zones[2] = (radar_zone_definition_t){3U, RADAR_ZONE_IGNORE, true,
        {-200, 200, 500, 1500}, 0U};
    radar_zone_map_t map;
    radar_zone_map_init(&map, &config.installation);
    uint8_t zone = 0U;
    radar_zone_type_t type = RADAR_ZONE_NONE;
    assert(!radar_zone_map_resolve(&map, 0, 1000, 0U, &zone, &type));
    assert(radar_zone_map_resolve(&map, 1050, 1000, 1U, &zone, &type));
    assert(zone == 1U && type == RADAR_ZONE_ACTIVE);
    assert(radar_zone_map_resolve(&map, 1200, 1000, 1U, &zone, &type));
    assert(zone == 2U && type == RADAR_ZONE_ENTRY);
}

static void test_tracker_association_crossing_and_lifecycle(void)
{
    radar_spatial_config_t config = radar_spatial_default_config();
    config.thresholds.association_gate_mm = 700U;
    config.thresholds.track_timeout_ms = 1000U;
    radar_zone_map_t map;
    radar_zone_map_init(&map, &config.installation);
    radar_target_tracker_t tracker;
    radar_target_tracker_init(&tracker, &config.thresholds);
    radar_spatial_target_t first[] = {spatial_target(-300, 1000, 20), spatial_target(300, 1000, -20)};
    radar_target_tracker_update(&tracker, first, 2U, &map, 100U);
    assert(radar_target_tracker_active_count(&tracker) == 0U);
    radar_spatial_target_t confirmed[] = {spatial_target(-280, 1000, 20),
                                          spatial_target(280, 1000, -20)};
    radar_target_tracker_update(&tracker, confirmed, 2U, &map, 200U);
    assert(radar_target_tracker_active_count(&tracker) == 1U);
    radar_spatial_target_t second_candidate[] = {spatial_target(-260, 1000, 20),
                                                 spatial_target(260, 1000, -20)};
    radar_target_tracker_update(&tracker, second_candidate, 2U, &map, 300U);
    radar_spatial_target_t second_confirmed[] = {spatial_target(-240, 1000, 20),
                                                 spatial_target(240, 1000, -20)};
    radar_target_tracker_update(&tracker, second_confirmed, 2U, &map, 400U);
    assert(radar_target_tracker_active_count(&tracker) == 2U);
    const uint32_t id_a = tracker.tracks[0].track_id;
    const uint32_t id_b = tracker.tracks[1].track_id;
    radar_spatial_target_t crossed[] = {spatial_target(120, 1000, -20), spatial_target(-120, 1000, 20)};
    radar_target_tracker_update(&tracker, crossed, 2U, &map, 500U);
    assert(radar_target_tracker_active_count(&tracker) == 2U);
    assert(tracker.tracks[0].track_id == id_a && tracker.tracks[1].track_id == id_b);
    radar_target_tracker_update(&tracker, NULL, 0U, &map, 600U);
    assert(radar_target_tracker_active_count(&tracker) == 0U);
    assert(radar_target_tracker_visible_count(&tracker) == 0U);
    radar_target_tracker_expire(&tracker, 1301U);
    assert(radar_target_tracker_stale_count(&tracker) == 2U);
    radar_target_tracker_expire(&tracker, 3501U);
    assert(radar_target_tracker_active_count(&tracker) == 0U);

    config.installation.zone_count = 1U;
    config.installation.zones[0].enabled = true;
    config.installation.zones[0].type = RADAR_ZONE_ACTIVE;
    config.installation.zones[0].rect = (radar_rect_t){-500, 500, 0, 3000};
    config.thresholds.association_gate_mm = 2000U;
    radar_zone_map_init(&map, &config.installation);
    radar_target_tracker_init(&tracker, &config.thresholds);
    radar_spatial_target_t in_zone = spatial_target(0, 1000, 0);
    radar_target_tracker_update(&tracker, &in_zone, 1U, &map, 100U);
    radar_target_tracker_update(&tracker, &in_zone, 1U, &map, 200U);
    radar_spatial_target_t outside_zone = spatial_target(800, 1000, 0);
    radar_target_tracker_update(&tracker, &outside_zone, 1U, &map, 500U);
    assert(tracker.tracks[0].zone_changed);
    assert(tracker.tracks[0].zone_left);
}

static void test_spatial_semantics(void)
{
    radar_spatial_config_t config = radar_spatial_default_config();
    config.thresholds.track_timeout_ms = 1000U;
    config.thresholds.motion_displacement_mm = 2000U;
    radar_spatial_state_t state;
    radar_spatial_state_init(&state, &config, 0U);
    radar_spatial_snapshot_t snapshot;
    radar_spatial_state_poll(&state, RADAR_UART_RECOVERY_VALID, 1U);
    radar_spatial_state_get_snapshot(&state, &snapshot);
    assert(snapshot.sensor_state == RADAR_SENSOR_STALE);
    assert(snapshot.frame_age_ms == 0U);
    for (uint32_t i = 1U; i <= 3U; ++i) {
        radar_frame_t value = frame(i, i * 100U, 300, 1200, 0);
        radar_spatial_state_on_frame(&state, &value, true, i * 100U);
    }
    radar_spatial_state_get_snapshot(&state, &snapshot);
    assert(snapshot.sensor_state == RADAR_SENSOR_VALID);
    assert(snapshot.occupancy_state == RADAR_OCCUPANCY_PRESENT);
    assert(snapshot.motion_state == RADAR_MOTION_STILL_CANDIDATE);

    radar_frame_t moving = frame(4U, 400U, 600, 1200, 30);
    radar_spatial_state_on_frame(&state, &moving, true, 400U);
    radar_frame_t moving_2 = frame(5U, 500U, 900, 1200, 30);
    radar_frame_t moving_3 = frame(6U, 600U, 1200, 1200, 30);
    radar_spatial_state_on_frame(&state, &moving_2, true, 500U);
    radar_spatial_state_on_frame(&state, &moving_3, true, 600U);
    radar_spatial_state_get_snapshot(&state, &snapshot);
    assert(snapshot.motion_state == RADAR_MOTION_MOVING);
    assert(snapshot.tracks[0].raw_x_mm == 1200);
    assert(snapshot.tracks[0].filtered_x_mm == 1012);

    for (uint32_t seq = 7U; seq <= 14U; ++seq) {
        radar_frame_t still = frame(seq, seq * 100U, 1200, 1200, 0);
        radar_spatial_state_on_frame(&state, &still, true, seq * 100U);
    }
    radar_spatial_state_get_snapshot(&state, &snapshot);
    assert(snapshot.motion_state == RADAR_MOTION_STILL_CANDIDATE);

    radar_frame_t empty = {.frame_seq = 15U, .received_at_ms = 1500U, .target_count = 0U};
    radar_spatial_state_on_frame(&state, &empty, true, 1500U);
    radar_spatial_state_poll(&state, RADAR_UART_RECOVERY_VALID, 1600U);
    radar_spatial_state_get_snapshot(&state, &snapshot);
    assert(snapshot.occupancy_state == RADAR_OCCUPANCY_HOLD);
    assert(snapshot.occupancy_confidence < 100U);
    radar_spatial_state_poll(&state, RADAR_UART_RECOVERY_VALID, 4401U);
    radar_spatial_state_get_snapshot(&state, &snapshot);
    assert(snapshot.occupancy_state == RADAR_OCCUPANCY_VACANT_INFERRED);

    radar_spatial_state_poll(&state, RADAR_UART_RECOVERY_VALID, 100U);
    radar_spatial_state_get_snapshot(&state, &snapshot);
    assert(snapshot.sensor_state == RADAR_SENSOR_STALE);
    assert(snapshot.frame_age_ms == 0U);

    radar_spatial_state_init(&state, &config, 0U);
    radar_frame_t initial = frame(1U, 100U, 100, 1000, 0);
    radar_frame_t initial_confirm = frame(2U, 150U, 100, 1000, 0);
    radar_frame_t jump = frame(3U, 200U, 2500, 1000, 0);
    radar_spatial_state_on_frame(&state, &initial, true, 100U);
    radar_spatial_state_on_frame(&state, &initial_confirm, true, 150U);
    radar_spatial_state_on_frame(&state, &jump, true, 200U);
    radar_spatial_state_get_snapshot(&state, &snapshot);
    assert(snapshot.accepted_target_count == 1U);
    assert(snapshot.active_track_count == 0U);
    assert(snapshot.diagnostics.tracker.dropped_target_count >= 1U);
    radar_frame_t outlier = frame(4U, 300U, 6500, 1000, 0);
    radar_spatial_state_on_frame(&state, &outlier, true, 300U);
    radar_spatial_state_get_snapshot(&state, &snapshot);
    assert(snapshot.accepted_target_count == 0U);
    assert(snapshot.diagnostics.tracker.coordinate_outliers == 1U);
}

static radar_frame_t single_person_frame(uint32_t sequence, uint64_t now_ms)
{
    const double phase = (double)sequence * 0.01;
    radar_frame_t value = {
        .frame_seq = sequence,
        .received_at_ms = now_ms,
        .target_count = 1U,
    };
    value.targets[0] = raw_target((int16_t)lround(900.0 * sin(phase)),
                                  (int16_t)lround(1800.0 + 100.0 * cos(phase)),
                                  12);

    /* A single-frame reflected slot must never receive a second persistent ID. */
    if (sequence % 47U == 0U) {
        value.target_count = 2U;
        value.targets[1] = raw_target(3400, 3400, 0);
    }
    /* Domain filtering must also reject an invalid slot even if a caller marked it valid. */
    if (sequence % 113U == 0U) {
        value.target_count = 2U;
        value.targets[1] = raw_target(0, 0, 0);
        value.targets[1].resolution_mm = 0U;
    }
    return value;
}

static void test_single_person_five_minute_stability(void)
{
    radar_spatial_state_t state;
    radar_spatial_state_init(&state, NULL, 0U);

    radar_spatial_snapshot_t snapshot;
    for (uint32_t sequence = 1U; sequence <= 3000U; ++sequence) {
        const uint64_t now_ms = (uint64_t)sequence * 100U;
        const radar_frame_t value = single_person_frame(sequence, now_ms);
        radar_spatial_state_on_frame(&state, &value, true, now_ms);
        radar_spatial_state_get_snapshot(&state, &snapshot);
        if (sequence >= 2U) {
            assert(snapshot.active_track_count == 1U);
            assert(snapshot.current_targets[0].track_id == 1U);
        }
    }

    assert(state.tracker.next_track_id == 2U);
    assert(snapshot.diagnostics.tracker.new_track_count == 1U);
    assert(snapshot.diagnostics.tracker.velocity_outliers == 0U);

    /* No new UART frame must stale the current output, not retain its last
     * visible point until the sensor-level timeout. */
    radar_spatial_state_poll(&state, RADAR_UART_RECOVERY_VALID, 300801U);
    radar_spatial_state_get_snapshot(&state, &snapshot);
    assert(snapshot.active_track_count == 0U);
    assert(snapshot.history_target_count == 1U);
    assert(snapshot.history_targets[0].track_id == 1U);
    assert(snapshot.diagnostics.tracker.stale_track_count == 1U);

    radar_spatial_state_poll(&state, RADAR_UART_RECOVERY_VALID, 303001U);
    radar_spatial_state_get_snapshot(&state, &snapshot);
    assert(snapshot.active_track_count == 0U);
    assert(snapshot.history_target_count == 1U);
    assert(snapshot.diagnostics.tracker.deleted_track_count == 1U);
}

static void test_uart_recovery(void)
{
    radar_uart_recovery_t recovery;
    radar_uart_recovery_init(&recovery, NULL, 0U);
    radar_uart_recovery_note_init_result(&recovery, true, 0U);
    for (uint32_t i = 0U; i < 30U; ++i) radar_uart_recovery_note_no_valid(&recovery, 100U + i);
    assert(recovery.state == RADAR_UART_RECOVERY_WAITING_VALID);
    radar_uart_recovery_note_rx_bytes(&recovery, 1000U, 8U);
    radar_uart_recovery_note_timeout(&recovery, 3900U);
    assert(recovery.state == RADAR_UART_RECOVERY_WAITING_VALID);
    radar_uart_recovery_note_timeout(&recovery, 4000U);
    assert(recovery.state == RADAR_UART_RECOVERY_BACKOFF);

    radar_uart_recovery_init(&recovery, NULL, 0U);
    radar_uart_recovery_note_init_result(&recovery, true, 0U);
    for (uint32_t i = 0U; i < 3U; ++i) radar_uart_recovery_note_valid_frame(&recovery, i * 100U);
    assert(recovery.state == RADAR_UART_RECOVERY_VALID);
    for (uint32_t i = 0U; i < 3U; ++i) radar_uart_recovery_note_error(&recovery, 400U + i);
    assert(recovery.state == RADAR_UART_RECOVERY_BACKOFF);
    assert(radar_uart_recovery_should_stop_rx(&recovery));
    assert(!radar_uart_recovery_retry_due(&recovery, 651U));
    assert(radar_uart_recovery_retry_due(&recovery, 652U));
    radar_uart_recovery_note_init_result(&recovery, false, 652U);
    assert(recovery.init_failure_count == 1U);
    assert(recovery.current_backoff_ms == 500U);

    radar_uart_recovery_init(&recovery, NULL, 0U);
    radar_uart_recovery_note_init_result(&recovery, true, 0U);
    radar_uart_recovery_note_overflow(&recovery, 100U);
    assert(recovery.state == RADAR_UART_RECOVERY_BACKOFF);
}

static void test_adaptive_rate_state_machine(void)
{
    radar_rate_manager_t manager;
    radar_rate_manager_init(&manager, 0U);
    assert(manager.mode == RADAR_RATE_IDLE);
    assert(manager.policy.parser_period_ms == 100U);
    assert(!radar_rate_manager_update(&manager, 0U, 0U, 0U, 4000U));
    assert(radar_rate_manager_update(&manager, 1U, 0U, 0U, 5000U));
    assert(manager.mode == RADAR_RATE_DETECTING);
    assert(radar_rate_manager_update(&manager, 1U, 1U, 1U, 5100U));
    assert(manager.mode == RADAR_RATE_TRACKING && manager.policy.tracker_period_ms == 67U);
    assert(!radar_rate_manager_update(&manager, 1U, 1U, 1U, 5200U));
    assert(manager.mode == RADAR_RATE_TRACKING);
    assert(radar_rate_manager_update(&manager, 0U, 0U, 1U, 5300U));
    assert(manager.mode == RADAR_RATE_LOST_PENDING && manager.policy.tracker_period_ms == 200U);
    assert(radar_rate_manager_update(&manager, 1U, 1U, 1U, 5400U));
    assert(manager.mode == RADAR_RATE_TRACKING);
    assert(radar_rate_manager_update(&manager, 0U, 0U, 1U, 5500U));
    assert(manager.mode == RADAR_RATE_LOST_PENDING);
    assert(radar_rate_manager_update(&manager, 0U, 0U, 0U, 8500U));
    assert(manager.mode == RADAR_RATE_LOST);
    assert(radar_rate_manager_update(&manager, 0U, 0U, 0U, 13500U));
    assert(manager.mode == RADAR_RATE_IDLE);
}

static void test_occlusion_and_velocity_outlier_policy(void)
{
    radar_spatial_state_t state;
    radar_spatial_snapshot_t snapshot;
    radar_spatial_state_init(&state, NULL, 0U);
    radar_frame_t first_frame = frame(1U, 100U, 100, 1000, 0);
    radar_frame_t second_frame = frame(2U, 200U, 100, 1000, 0);
    radar_spatial_state_on_frame(&state, &first_frame, true, 100U);
    radar_spatial_state_on_frame(&state, &second_frame, true, 200U);
    radar_spatial_state_get_snapshot(&state, &snapshot);
    assert(snapshot.current_targets[0].track_id == 1U);
    radar_spatial_state_on_frame(&state, &(radar_frame_t){.frame_seq = 3U, .received_at_ms = 300U}, true, 300U);
    radar_spatial_state_poll(&state, RADAR_UART_RECOVERY_VALID, 1000U);
    radar_spatial_state_get_snapshot(&state, &snapshot);
    assert(snapshot.tracks[0].track_id == 1U);
    assert(snapshot.tracks[0].lifecycle == RADAR_TRACK_HOLD);
    radar_frame_t restored_frame = frame(4U, 2500U, 150, 1000, 0);
    radar_spatial_state_on_frame(&state, &restored_frame, true, 2500U);
    radar_spatial_state_get_snapshot(&state, &snapshot);
    assert(snapshot.current_targets[0].track_id == 1U);

    radar_target_tracker_t tracker;
    radar_spatial_config_t config = radar_spatial_default_config();
    radar_target_tracker_init(&tracker, &config.thresholds);
    radar_spatial_target_t stable = spatial_target(100, 1000, 0);
    radar_target_tracker_update(&tracker, &stable, 1U, NULL, 100U);
    radar_target_tracker_update(&tracker, &stable, 1U, NULL, 200U);
    radar_spatial_target_t first = spatial_target(600, 1000, 0);
    radar_spatial_target_t second = spatial_target(1100, 1000, 0);
    radar_spatial_target_t third = spatial_target(1600, 1000, 0);
    radar_target_tracker_update(&tracker, &first, 1U, NULL, 300U);
    radar_target_tracker_update(&tracker, &second, 1U, NULL, 400U);
    assert(tracker.tracks[0].consecutive_velocity_outliers == 2U);
    radar_target_tracker_update(&tracker, &third, 1U, NULL, 500U);
    assert(tracker.tracks[0].raw_x_mm == 1100);
    assert(tracker.diagnostics.velocity_outliers >= 3U);
}

int main(void)
{
    test_transform_and_zone_map();
    test_tracker_association_crossing_and_lifecycle();
    test_spatial_semantics();
    test_single_person_five_minute_stability();
    test_uart_recovery();
    test_adaptive_rate_state_machine();
    test_occlusion_and_velocity_outlier_policy();
    puts("radar spatial/recovery host tests: PASS");
    return 0;
}

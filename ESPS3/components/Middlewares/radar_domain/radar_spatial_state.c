#include "radar_spatial_state.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "radar_config.h"

/*
 * 空间状态机以变换后的目标和跟踪器为输入，分别推导传感器有效性、占用和运动。
 * 三种语义拆分后，UART 离线、数据过期和“有人但静止”不会被压缩为同一状态。
 * 这些域内常量不改变 UART、协议或安装坐标配置接口。
 */

static uint32_t age_ms(uint64_t timestamp_ms, uint64_t now_ms)
{
    if (timestamp_ms == 0U || now_ms < timestamp_ms) return 0U;
    const uint64_t age = now_ms - timestamp_ms;
    return age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
}

radar_spatial_config_t radar_spatial_default_config(void)
{
    radar_spatial_config_t config;
    memset(&config, 0, sizeof(config));
    config.installation.flip_x = RADAR_CONFIG_FLIP_X != 0;
    config.installation.flip_y = RADAR_CONFIG_FLIP_Y != 0;
    config.installation.rotation_deg = RADAR_CONFIG_ROTATION_DEG;
    config.installation.origin_offset_x_mm = RADAR_CONFIG_ORIGIN_OFFSET_X_MM;
    config.installation.origin_offset_y_mm = RADAR_CONFIG_ORIGIN_OFFSET_Y_MM;
    config.installation.max_detection_distance_mm = RADAR_CONFIG_MAX_DETECTION_DISTANCE_MM;
    config.installation.room_bounds = (radar_rect_t){
        RADAR_CONFIG_ROOM_MIN_X_MM,
        RADAR_CONFIG_ROOM_MAX_X_MM,
        RADAR_CONFIG_ROOM_MIN_Y_MM,
        RADAR_CONFIG_ROOM_MAX_Y_MM,
    };
    config.installation.zone_count = RADAR_CONFIG_ZONE_COUNT;
    config.installation.zones[0] = (radar_zone_definition_t){
        .zone_id = RADAR_CONFIG_ZONE_0_ID,
        .type = (radar_zone_type_t)RADAR_CONFIG_ZONE_0_TYPE,
        .enabled = RADAR_CONFIG_ZONE_0_ENABLED != 0,
        .rect = {
            RADAR_CONFIG_ZONE_0_MIN_X_MM,
            RADAR_CONFIG_ZONE_0_MAX_X_MM,
            RADAR_CONFIG_ZONE_0_MIN_Y_MM,
            RADAR_CONFIG_ZONE_0_MAX_Y_MM,
        },
        .hysteresis_mm = RADAR_CONFIG_ZONE_0_HYSTERESIS_MM,
    };
    config.thresholds = (radar_spatial_thresholds_t){
        .sensor_stale_ms = RADAR_CONFIG_SENSOR_STALE_MS,
        .association_gate_mm = RADAR_CONFIG_ASSOCIATION_GATE_MM,
        .association_gate_min_mm = RADAR_CONFIG_ASSOCIATION_GATE_MIN_MM,
        .association_gate_max_mm = RADAR_CONFIG_ASSOCIATION_GATE_MAX_MM,
        .track_timeout_ms = RADAR_CONFIG_TRACK_TIMEOUT_MS,
        .track_confirm_frames = RADAR_CONFIG_TRACK_CONFIRM_FRAMES,
        .track_hold_ms = RADAR_CONFIG_TRACK_HOLD_MS,
        .motion_speed_cm_s = RADAR_CONFIG_MOTION_SPEED_CM_S,
        .motion_displacement_mm = RADAR_CONFIG_MOTION_DISPLACEMENT_MM,
        .still_candidate_frames = RADAR_CONFIG_STILL_CANDIDATE_FRAMES,
        .hold_decay_ms = RADAR_CONFIG_HOLD_DECAY_MS,
        .hold_confidence_loss_per_s = RADAR_CONFIG_HOLD_CONFIDENCE_LOSS_PER_S,
        .confidence_initial = RADAR_CONFIG_CONFIDENCE_INITIAL,
        .confidence_gain_per_frame = RADAR_CONFIG_CONFIDENCE_GAIN_PER_FRAME,
        .confidence_loss_per_miss = RADAR_CONFIG_CONFIDENCE_LOSS_PER_MISS,
        .track_confidence_min = RADAR_CONFIG_TRACK_CONFIDENCE_MIN,
        .max_velocity_mm_s = RADAR_CONFIG_MAX_VELOCITY_MM_S,
        .ema_alpha_percent = RADAR_CONFIG_EMA_ALPHA_PERCENT,
        .motion_enter_frames = RADAR_CONFIG_MOTION_ENTER_FRAMES,
        .motion_exit_frames = RADAR_CONFIG_MOTION_EXIT_FRAMES,
        .target_jump_max_mm = RADAR_CONFIG_TARGET_JUMP_MAX_MM,
    };
    config.person_continuity = (radar_person_continuity_config_t){
        .still_hold_ms = RADAR_CONFIG_PERSON_STILL_HOLD_MS,
        .dormant_timeout_ms = RADAR_CONFIG_PERSON_DORMANT_TIMEOUT_MS,
        .reacquire_gate_mm = RADAR_CONFIG_PERSON_REACQUIRE_GATE_MM,
        .same_zone_bonus_mm = RADAR_CONFIG_PERSON_SAME_ZONE_BONUS_MM,
        .adjacent_zone_allow = RADAR_CONFIG_PERSON_ADJACENT_ZONE_ALLOW != 0,
        .new_confirm_frames = RADAR_CONFIG_PERSON_NEW_CONFIRM_FRAMES,
        .new_near_dormant_confirm_frames =
            RADAR_CONFIG_PERSON_NEW_NEAR_DORMANT_CONFIRM_FRAMES,
        .velocity_decay_start_ms = RADAR_CONFIG_PERSON_VELOCITY_DECAY_START_MS,
        .velocity_decay_ms = RADAR_CONFIG_PERSON_VELOCITY_DECAY_MS,
    };
    return config;
}

static void refresh_snapshot(radar_spatial_state_t *state, uint64_t now_ms)
{
    radar_spatial_snapshot_t *snapshot = &state->snapshot;
    snapshot->captured_at_ms = now_ms;
    snapshot->frame_age_ms = age_ms(snapshot->latest_frame_ms, now_ms);
    radar_target_tracker_copy(&state->tracker, snapshot->tracks, RADAR_TRACKER_MAX_TRACKS);
    memset(snapshot->current_targets, 0, sizeof(snapshot->current_targets));
    snapshot->visible_track_count = radar_target_tracker_copy_active(&state->tracker,
                                                                       snapshot->current_targets,
                                                                       RADAR_TRACKER_MAX_TRACKS);
    /* Existing consumers use active_track_count for current visible tracks.
     * Keep that compatibility alias, but do not use it as a person count. */
    snapshot->active_track_count = snapshot->visible_track_count;
    snapshot->confirmed_active_track_count =
        radar_target_tracker_confirmed_count(&state->tracker);
    radar_person_continuity_get_counts(&state->person_continuity,
                                       &snapshot->visible_person_count,
                                       &snapshot->retained_person_count,
                                       &snapshot->source_person_count,
                                       &snapshot->count_state);
    radar_person_continuity_copy_persons(&state->person_continuity,
                                         snapshot->persons,
                                         RADAR_PERSON_CONTINUITY_MAX_PERSONS);
    snapshot->diagnostics.tracker = state->tracker.diagnostics;
    snapshot->diagnostics.tracker.active_track_count = snapshot->active_track_count;
    snapshot->diagnostics.tracker.stale_track_count =
        radar_target_tracker_stale_count(&state->tracker);
    snapshot->dominant_zone_id = 0U;
    snapshot->occupancy_confidence = 0U;
    for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS; ++i) {
        const radar_track_snapshot_t *track = &snapshot->tracks[i];
        if (!track->active) continue;
        if (track->confidence >= snapshot->occupancy_confidence) {
            snapshot->occupancy_confidence = track->confidence;
        }
    }
}

static bool has_unresolved_observation(const radar_spatial_state_t *state)
{
    if (state == NULL) {
        return false;
    }
    if (state->snapshot.accepted_target_count > 0U &&
        radar_target_tracker_visible_count(&state->tracker) == 0U) {
        return true;
    }
    for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS; ++i) {
        const radar_track_snapshot_t *track = &state->tracker.tracks[i];
        if (track->active && track->lifecycle == RADAR_TRACK_TENTATIVE) {
            return true;
        }
    }
    return false;
}

static bool raw_target_is_domain_valid(const radar_target_t *target)
{
    return target != NULL && target->valid && target->resolution_mm != 0U &&
           target->confidence != 0U && !(target->x_mm == 0 && target->y_mm == 0) &&
           target->x_mm != INT16_MIN &&
           target->x_mm != -32704 && target->y_mm != INT16_MIN &&
           target->y_mm != -32704;
}

static void record_stale_history(radar_spatial_state_t *state)
{
    for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS; ++i) {
        radar_track_snapshot_t *track = &state->tracker.tracks[i];
        if (!track->active || track->track_id == 0U ||
            track->lifecycle != RADAR_TRACK_HOLD || track->history_recorded) {
            continue;
        }
        state->snapshot.history_targets[state->history_write_index] = *track;
        state->history_write_index = (uint8_t)((state->history_write_index + 1U) %
                                                RADAR_TRACKER_HISTORY_MAX_TARGETS);
        if (state->snapshot.history_target_count < RADAR_TRACKER_HISTORY_MAX_TARGETS) {
            ++state->snapshot.history_target_count;
        }
        track->history_recorded = true;
    }
}

static bool latest_frame_has_motion(const radar_spatial_state_t *state)
{
    for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS; ++i) {
        const radar_track_snapshot_t *track = &state->tracker.tracks[i];
        if (!track->active || !track->visible ||
            track->lifecycle != RADAR_TRACK_CONFIRMED) {
            continue;
        }
        if ((uint32_t)abs(track->speed_cm_s) >= state->config.thresholds.motion_speed_cm_s ||
            track->last_displacement_mm >= state->config.thresholds.motion_displacement_mm) {
            return true;
        }
    }
    return false;
}

static void update_motion_hysteresis(radar_spatial_state_t *state)
{
    const bool moving = latest_frame_has_motion(state);
    if (moving) {
        state->motion_exit_streak = 0U;
        if (state->motion_enter_streak < UINT32_MAX) {
            ++state->motion_enter_streak;
        }
        if (state->motion_enter_streak >= state->config.thresholds.motion_enter_frames) {
            state->motion_confirmed = true;
        }
        return;
    }

    state->motion_enter_streak = 0U;
    if (!state->motion_confirmed) {
        return;
    }
    if (state->motion_exit_streak < UINT32_MAX) {
        ++state->motion_exit_streak;
    }
    if (state->motion_exit_streak >= state->config.thresholds.motion_exit_frames) {
        state->motion_confirmed = false;
        state->motion_exit_streak = 0U;
    }
}

static void update_semantics(radar_spatial_state_t *state, uint64_t now_ms)
{
    (void)now_ms;
    radar_spatial_snapshot_t *snapshot = &state->snapshot;
    if (snapshot->sensor_state != RADAR_SENSOR_VALID) {
        snapshot->occupancy_state = RADAR_OCCUPANCY_UNKNOWN;
        snapshot->motion_state = RADAR_MOTION_UNKNOWN;
        return;
    }
    if (snapshot->count_state == RADAR_PERSON_COUNT_OBSERVED) {
        bool still = false;
        for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS; ++i) {
            const radar_track_snapshot_t *track = &state->tracker.tracks[i];
            if (!track->active || !track->visible) continue;
            if (track->consecutive_seen >= state->config.thresholds.still_candidate_frames) {
                still = true;
            }
        }
        snapshot->occupancy_state = RADAR_OCCUPANCY_PRESENT;
        snapshot->motion_state = state->motion_confirmed ? RADAR_MOTION_MOVING :
                                 (still ? RADAR_MOTION_STILL_CANDIDATE : RADAR_MOTION_NONE);
    } else if (snapshot->count_state == RADAR_PERSON_COUNT_ESTIMATED) {
        snapshot->occupancy_state = RADAR_OCCUPANCY_HOLD;
        snapshot->motion_state = RADAR_MOTION_NONE;
        snapshot->occupancy_confidence = 0U;
        for (size_t i = 0U; i < RADAR_PERSON_CONTINUITY_MAX_PERSONS; ++i) {
            const radar_person_snapshot_t *person = &snapshot->persons[i];
            if (person->state == RADAR_PERSON_STILL_HOLD ||
                person->state == RADAR_PERSON_DORMANT) {
                if (person->quality > snapshot->occupancy_confidence) {
                    snapshot->occupancy_confidence = person->quality;
                }
            }
        }
    } else if (snapshot->count_state == RADAR_PERSON_COUNT_VACANT_INFERRED) {
        snapshot->occupancy_state = RADAR_OCCUPANCY_VACANT_INFERRED;
        snapshot->motion_state = RADAR_MOTION_NONE;
    } else {
        snapshot->occupancy_state = RADAR_OCCUPANCY_UNKNOWN;
        snapshot->motion_state = RADAR_MOTION_UNKNOWN;
    }
}

void radar_spatial_state_init(radar_spatial_state_t *state,
                              const radar_spatial_config_t *config,
                              uint64_t now_ms)
{
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
    state->config = config != NULL ? *config : radar_spatial_default_config();
    radar_zone_map_init(&state->zone_map, &state->config.installation);
    radar_target_tracker_init(&state->tracker, &state->config.thresholds);
    radar_person_continuity_init(&state->person_continuity,
                                 &state->config.person_continuity,
                                 UINT8_MAX);
    state->snapshot.captured_at_ms = now_ms;
    state->snapshot.sensor_state = RADAR_SENSOR_OFFLINE;
    state->snapshot.occupancy_state = RADAR_OCCUPANCY_UNKNOWN;
    state->snapshot.motion_state = RADAR_MOTION_UNKNOWN;
    state->snapshot.count_state = RADAR_PERSON_COUNT_UNKNOWN;
}

void radar_spatial_state_set_source(radar_spatial_state_t *state, uint8_t source_id)
{
    if (state != NULL) {
        radar_person_continuity_set_source(&state->person_continuity, source_id);
    }
}

void radar_spatial_state_on_frame(radar_spatial_state_t *state,
                                  const radar_frame_t *frame,
                                  bool uart_recovered,
                                  uint64_t now_ms)
{
    if (state == NULL || frame == NULL || frame->frame_seq == state->last_frame_seq) return;
    state->last_frame_seq = frame->frame_seq;
    radar_person_continuity_set_sequence(&state->person_continuity, frame->frame_seq);
    state->snapshot.latest_frame_ms = frame->received_at_ms;
    state->snapshot.raw_target_count = frame->target_count;
    state->snapshot.accepted_target_count = 0U;
    memcpy(state->snapshot.raw_targets, frame->targets, sizeof(state->snapshot.raw_targets));
    memset(state->snapshot.accepted_targets, 0, sizeof(state->snapshot.accepted_targets));
    radar_spatial_target_t accepted[RADAR_TRACKER_MAX_TRACKS];
    memset(accepted, 0, sizeof(accepted));
    for (size_t i = 0U; i < LD2450_MAX_TARGETS; ++i) {
        radar_spatial_target_t transformed;
        if (!raw_target_is_domain_valid(&frame->targets[i])) {
            if (frame->targets[i].valid) {
                ++state->tracker.diagnostics.dropped_target_count;
            }
            continue;
        }
        if (!radar_coordinate_transform_target(&state->config.installation, &frame->targets[i], &transformed) ||
            !radar_coordinate_transform_in_room(&state->config.installation, &transformed)) {
            if (frame->targets[i].valid) {
                ++state->tracker.diagnostics.coordinate_outliers;
            }
            continue;
        }
        if (state->snapshot.accepted_target_count < LD2450_MAX_TARGETS) {
            accepted[state->snapshot.accepted_target_count++] = transformed;
        } else {
            ++state->tracker.diagnostics.dropped_target_count;
        }
    }
    memcpy(state->snapshot.accepted_targets, accepted, sizeof(accepted));
#if CONFIG_S3_RADAR_ZONE_ACTIVE
    radar_target_tracker_update(&state->tracker, accepted, state->snapshot.accepted_target_count,
                                &state->zone_map, now_ms);
#else
    radar_target_tracker_update(&state->tracker, accepted, state->snapshot.accepted_target_count,
                                NULL, now_ms);
#endif
    record_stale_history(state);
    update_motion_hysteresis(state);
    state->snapshot.sensor_state = uart_recovered ? RADAR_SENSOR_VALID : RADAR_SENSOR_STALE;
    radar_person_continuity_update(&state->person_continuity,
                                   state->tracker.tracks,
                                   RADAR_TRACKER_MAX_TRACKS,
                                   state->snapshot.sensor_state == RADAR_SENSOR_VALID,
                                   has_unresolved_observation(state),
                                   now_ms);
    refresh_snapshot(state, now_ms);
    update_semantics(state, now_ms);
}

void radar_spatial_state_poll(radar_spatial_state_t *state,
                             radar_uart_recovery_state_t recovery_state,
                             uint64_t now_ms)
{
    if (state == NULL) return;
    if (state->snapshot.latest_frame_ms != 0U && now_ms >= state->snapshot.latest_frame_ms &&
        now_ms - state->snapshot.latest_frame_ms >= state->config.thresholds.track_hold_ms) {
        radar_target_tracker_mark_missing(&state->tracker, now_ms);
    }
    radar_target_tracker_expire(&state->tracker, now_ms);
    record_stale_history(state);
    if (recovery_state == RADAR_UART_RECOVERY_OFFLINE ||
        recovery_state == RADAR_UART_RECOVERY_BACKOFF) {
        state->snapshot.sensor_state = RADAR_SENSOR_OFFLINE;
    } else if (state->snapshot.latest_frame_ms == 0U ||
               now_ms < state->snapshot.latest_frame_ms ||
               age_ms(state->snapshot.latest_frame_ms, now_ms) > state->config.thresholds.sensor_stale_ms ||
               recovery_state != RADAR_UART_RECOVERY_VALID) {
        state->snapshot.sensor_state = RADAR_SENSOR_STALE;
    } else {
        state->snapshot.sensor_state = RADAR_SENSOR_VALID;
    }
    radar_person_continuity_update(&state->person_continuity,
                                   state->tracker.tracks,
                                   RADAR_TRACKER_MAX_TRACKS,
                                   state->snapshot.sensor_state == RADAR_SENSOR_VALID,
                                   has_unresolved_observation(state),
                                   now_ms);
    refresh_snapshot(state, now_ms);
    update_semantics(state, now_ms);
}

void radar_spatial_state_set_diagnostics(radar_spatial_state_t *state,
                                         const ld2450_parser_diagnostics_t *parser,
                                         const ld2450_uart_diagnostics_t *uart,
                                         const radar_uart_recovery_t *recovery)
{
    if (state == NULL) return;
    if (parser != NULL) state->snapshot.diagnostics.parser = *parser;
    if (uart != NULL) state->snapshot.diagnostics.uart = *uart;
    if (recovery != NULL) state->snapshot.diagnostics.recovery = *recovery;
}

void radar_spatial_state_get_snapshot(const radar_spatial_state_t *state,
                                      radar_spatial_snapshot_t *out)
{
    if (state != NULL && out != NULL) *out = state->snapshot;
}

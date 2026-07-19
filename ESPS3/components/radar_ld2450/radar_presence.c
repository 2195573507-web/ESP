#include "radar_presence.h"

#include <limits.h>
#include <string.h>

/*
 * 存在状态由连续帧证据驱动：有效目标累积运动票数，短暂缺帧进入保持期，
 * 仅在证据过期后推断无人。这样可把 UART 抖动与真实离开明确区分。
 */

static void sat_inc_u32(uint32_t *value)
{
    if (value != NULL && *value < UINT32_MAX) {
        ++(*value);
    }
}

radar_presence_config_t radar_presence_default_config(void)
{
    radar_presence_config_t config = {
        .frame_period_expected_ms = RADAR_FRAME_PERIOD_EXPECTED_MS,
        .online_timeout_ms = RADAR_ONLINE_TIMEOUT_MS,
        .enter_window_frames = RADAR_ENTER_WINDOW_FRAMES,
        .enter_required_frames = RADAR_ENTER_REQUIRED_FRAMES,
        .short_gap_ms = RADAR_SHORT_GAP_MS,
        .hold_timeout_ms = RADAR_HOLD_TIMEOUT_MS,
    };
    return config;
}

static radar_presence_config_t sanitize_config(const radar_presence_config_t *input)
{
    radar_presence_config_t config =
        input != NULL ? *input : radar_presence_default_config();

    if (config.frame_period_expected_ms == 0U) {
        config.frame_period_expected_ms = RADAR_FRAME_PERIOD_EXPECTED_MS;
    }
    if (config.online_timeout_ms == 0U) {
        config.online_timeout_ms = RADAR_ONLINE_TIMEOUT_MS;
    }
    if (config.enter_window_frames == 0U ||
        config.enter_window_frames > RADAR_ENTER_WINDOW_FRAMES) {
        config.enter_window_frames = RADAR_ENTER_WINDOW_FRAMES;
    }
    if (config.enter_required_frames == 0U ||
        config.enter_required_frames > config.enter_window_frames) {
        config.enter_required_frames = RADAR_ENTER_REQUIRED_FRAMES;
    }
    if (config.short_gap_ms == 0U) {
        config.short_gap_ms = RADAR_SHORT_GAP_MS;
    }
    if (config.hold_timeout_ms < config.short_gap_ms) {
        config.hold_timeout_ms = RADAR_HOLD_TIMEOUT_MS;
    }
    return config;
}

static void set_state(radar_presence_t *presence,
                      radar_presence_state_t state,
                      uint64_t now_ms)
{
    if (presence->snapshot.state == state) {
        return;
    }
    presence->snapshot.state = state;
    presence->snapshot.state_since_ms = now_ms;
    sat_inc_u32(&presence->snapshot.state_seq);
    sat_inc_u32(&presence->diagnostics.state_change_count);
}

static void reset_evidence(radar_presence_t *presence)
{
    memset(presence->motion_history, 0, sizeof(presence->motion_history));
    presence->history_count = 0U;
    presence->history_index = 0U;
    presence->empty_frame_streak = 0U;
}

static bool time_is_monotonic(radar_presence_t *presence, uint64_t now_ms)
{
    if (!presence->has_observed_time) {
        presence->has_observed_time = true;
        presence->last_observed_ms = now_ms;
        return true;
    }
    if (now_ms >= presence->last_observed_ms) {
        presence->last_observed_ms = now_ms;
        return true;
    }

    sat_inc_u32(&presence->diagnostics.time_rollback_count);
    reset_evidence(presence);
    presence->snapshot.last_valid_frame_ms = 0U;
    presence->snapshot.last_motion_ms = 0U;
    presence->snapshot.frame_fresh = false;
    set_state(presence, RADAR_STATE_UNKNOWN, now_ms);
    presence->last_observed_ms = now_ms;
    return false;
}

void radar_presence_init(radar_presence_t *presence,
                         const radar_presence_config_t *config,
                         uint64_t now_ms)
{
    if (presence == NULL) {
        return;
    }
    memset(presence, 0, sizeof(*presence));
    presence->config = sanitize_config(config);
    presence->snapshot.state = RADAR_STATE_UNKNOWN;
    presence->snapshot.state_since_ms = now_ms;
    presence->last_observed_ms = now_ms;
    presence->has_observed_time = true;
}

static uint8_t motion_votes(const radar_presence_t *presence)
{
    uint8_t votes = 0U;
    for (uint8_t i = 0U; i < presence->history_count; ++i) {
        if (presence->motion_history[i]) {
            ++votes;
        }
    }
    return votes;
}

static void append_motion_evidence(radar_presence_t *presence, bool has_target)
{
    presence->motion_history[presence->history_index] = has_target;
    presence->history_index =
        (uint8_t)((presence->history_index + 1U) % presence->config.enter_window_frames);
    if (presence->history_count < presence->config.enter_window_frames) {
        ++presence->history_count;
    }

    if (has_target) {
        presence->empty_frame_streak = 0U;
    } else if (presence->empty_frame_streak < UINT32_MAX) {
        ++presence->empty_frame_streak;
    }
}

void radar_presence_on_frame(radar_presence_t *presence,
                             const radar_frame_t *frame,
                             bool uart_online,
                             uint64_t now_ms)
{
    if (presence == NULL || frame == NULL) {
        return;
    }
    (void)time_is_monotonic(presence, now_ms);

    presence->snapshot.uart_online = uart_online;
    if (!uart_online) {
        presence->snapshot.frame_fresh = false;
        set_state(presence, RADAR_STATE_UNKNOWN, now_ms);
        return;
    }

    presence->snapshot.last_valid_frame_ms = now_ms;
    presence->snapshot.frame_fresh = true;
    presence->snapshot.current_target_count =
        frame->target_count <= LD2450_MAX_TARGETS ? frame->target_count : LD2450_MAX_TARGETS;
    memcpy(presence->snapshot.targets, frame->targets, sizeof(presence->snapshot.targets));

    const bool has_target = presence->snapshot.current_target_count > 0U;
    append_motion_evidence(presence, has_target);

    if ((presence->snapshot.state == RADAR_STATE_HOLD ||
         presence->snapshot.state == RADAR_STATE_VACANT_INFERRED) && has_target) {
        presence->snapshot.last_motion_ms = now_ms;
        set_state(presence, RADAR_STATE_MOTION, now_ms);
        return;
    }

    if (has_target &&
        presence->history_count >= presence->config.enter_required_frames &&
        motion_votes(presence) >= presence->config.enter_required_frames) {
        presence->snapshot.last_motion_ms = now_ms;
        set_state(presence, RADAR_STATE_MOTION, now_ms);
        return;
    }

    if (presence->snapshot.state == RADAR_STATE_MOTION &&
        presence->snapshot.last_motion_ms <= now_ms &&
        now_ms - presence->snapshot.last_motion_ms >= presence->config.short_gap_ms) {
        set_state(presence, RADAR_STATE_HOLD, now_ms);
    }

    if (presence->snapshot.state == RADAR_STATE_HOLD &&
        presence->empty_frame_streak > 0U &&
        presence->snapshot.last_motion_ms <= now_ms &&
        now_ms - presence->snapshot.last_motion_ms >= presence->config.hold_timeout_ms) {
        set_state(presence, RADAR_STATE_VACANT_INFERRED, now_ms);
    }
}

void radar_presence_poll(radar_presence_t *presence,
                         bool uart_online,
                         uint64_t now_ms)
{
    if (presence == NULL) {
        return;
    }
    (void)time_is_monotonic(presence, now_ms);
    presence->snapshot.uart_online = uart_online;

    const bool has_valid_frame = presence->snapshot.last_valid_frame_ms > 0U;
    const bool timestamp_valid =
        has_valid_frame && now_ms >= presence->snapshot.last_valid_frame_ms;
    const bool fresh =
        timestamp_valid &&
        now_ms - presence->snapshot.last_valid_frame_ms <= presence->config.online_timeout_ms;
    presence->snapshot.frame_fresh = fresh;

    if (!uart_online || !fresh) {
        reset_evidence(presence);
        set_state(presence, RADAR_STATE_UNKNOWN, now_ms);
    }
}

void radar_presence_note_uart_error(radar_presence_t *presence, uint64_t now_ms)
{
    if (presence == NULL) {
        return;
    }
    (void)time_is_monotonic(presence, now_ms);
    sat_inc_u32(&presence->diagnostics.uart_error_count);
    presence->snapshot.uart_online = false;
    presence->snapshot.frame_fresh = false;
    reset_evidence(presence);
    set_state(presence, RADAR_STATE_UNKNOWN, now_ms);
}

void radar_presence_get_snapshot(const radar_presence_t *presence,
                                 radar_snapshot_t *out)
{
    if (presence != NULL && out != NULL) {
        *out = presence->snapshot;
    }
}

void radar_presence_get_diagnostics(const radar_presence_t *presence,
                                    radar_presence_diagnostics_t *out)
{
    if (presence != NULL && out != NULL) {
        *out = presence->diagnostics;
    }
}

const char *radar_presence_state_name(radar_presence_state_t state)
{
    switch (state) {
    case RADAR_STATE_VACANT_INFERRED:
        return "vacant_inferred";
    case RADAR_STATE_HOLD:
        return "hold";
    case RADAR_STATE_MOTION:
        return "motion";
    case RADAR_STATE_PRESENT:
        return "present";
    case RADAR_STATE_UNKNOWN:
    default:
        return "unknown";
    }
}

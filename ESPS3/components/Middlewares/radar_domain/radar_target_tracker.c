#include "radar_target_tracker.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * 跟踪器以最近距离匹配保留目标身份，并通过置信度和失帧计数过滤瞬时噪声。
 * 它不改变原始目标数组，供上层同时保留协议原始值和稳定的空间轨迹。
 */

#define RADAR_PI 3.14159265358979323846

static void sat_inc(uint32_t *value)
{
    if (value != NULL && *value < UINT32_MAX) ++(*value);
}

static uint32_t distance_between(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    const double dx = (double)x1 - x2;
    const double dy = (double)y1 - y2;
    const double result = sqrt(dx * dx + dy * dy);
    return result > UINT32_MAX ? UINT32_MAX : (uint32_t)lround(result);
}

static void predicted_position(const radar_track_snapshot_t *track,
                               uint64_t now_ms,
                               int32_t *out_x,
                               int32_t *out_y)
{
    int32_t x = track->filtered_x_mm;
    int32_t y = track->filtered_y_mm;
    if (track->last_seen_ms != 0U && now_ms > track->last_seen_ms) {
        const uint64_t dt_ms = now_ms - track->last_seen_ms;
        x += (int32_t)((int64_t)track->velocity_x_mm_s * (int64_t)dt_ms / 1000);
        y += (int32_t)((int64_t)track->velocity_y_mm_s * (int64_t)dt_ms / 1000);
    }
    *out_x = x;
    *out_y = y;
}

static bool target_exceeds_velocity_limit(const radar_target_tracker_t *tracker,
                                          const radar_track_snapshot_t *track,
                                          const radar_spatial_target_t *target,
                                          uint64_t now_ms)
{
    if (tracker == NULL || track == NULL || target == NULL || !track->active ||
        track->last_seen_ms == 0U || now_ms <= track->last_seen_ms ||
        tracker->thresholds.max_velocity_mm_s == 0U) {
        return false;
    }
    const uint64_t dt_ms = now_ms - track->last_seen_ms;
    const uint64_t displacement = distance_between(track->raw_x_mm, track->raw_y_mm,
                                                    target->x_mm, target->y_mm);
    return displacement * 1000U > (uint64_t)tracker->thresholds.max_velocity_mm_s * dt_ms;
}

static bool track_is_current(const radar_target_tracker_t *tracker,
                             const radar_track_snapshot_t *track)
{
    return tracker != NULL && track != NULL && track->active && track->visible &&
           track->lifecycle == RADAR_TRACK_CONFIRMED && track->track_id != 0U &&
           track->confidence >= tracker->thresholds.track_confidence_min;
}

typedef struct {
    const radar_target_tracker_t *tracker;
    const radar_spatial_target_t *targets;
    size_t target_count;
    uint64_t now_ms;
    int current[RADAR_TRACKER_MAX_TRACKS];
    int best[RADAR_TRACKER_MAX_TRACKS];
    bool used[RADAR_TRACKER_MAX_TRACKS];
    uint8_t best_matches;
    uint64_t best_cost;
} assignment_search_t;

static void search_assignments(assignment_search_t *search, size_t track_index,
                               uint8_t matches, uint64_t cost)
{
    if (track_index == RADAR_TRACKER_MAX_TRACKS) {
        if (matches > search->best_matches ||
            (matches == search->best_matches && cost < search->best_cost)) {
            search->best_matches = matches;
            search->best_cost = cost;
            memcpy(search->best, search->current, sizeof(search->best));
        }
        return;
    }

    const radar_track_snapshot_t *track = &search->tracker->tracks[track_index];
    search->current[track_index] = -1;
    if (!track->active) {
        search_assignments(search, track_index + 1U, matches, cost);
        return;
    }
    search_assignments(search, track_index + 1U, matches, cost);
    int32_t predicted_x = 0;
    int32_t predicted_y = 0;
    predicted_position(track, search->now_ms, &predicted_x, &predicted_y);
    const uint32_t gate = search->tracker->thresholds.association_gate_mm;
    for (size_t target_index = 0U; target_index < search->target_count; ++target_index) {
        if (search->used[target_index] || !search->targets[target_index].valid) {
            continue;
        }
        const uint32_t distance = distance_between(predicted_x, predicted_y,
                                                   search->targets[target_index].x_mm,
                                                   search->targets[target_index].y_mm);
        if (distance >= gate) {
            continue;
        }
        if (target_exceeds_velocity_limit(search->tracker, track,
                                          &search->targets[target_index], search->now_ms) &&
            track->consecutive_velocity_outliers >= 2U) {
            continue;
        }
        search->used[target_index] = true;
        search->current[track_index] = (int)target_index;
        search_assignments(search, track_index + 1U, matches + 1U, cost + distance);
        search->used[target_index] = false;
        search->current[track_index] = -1;
    }
}

static uint32_t clamp_confidence(uint32_t value)
{
    return value > 100U ? 100U : value;
}

static void release_track(radar_target_tracker_t *tracker, radar_track_snapshot_t *track)
{
    const bool was_confirmed = track->track_id != 0U;
    memset(track, 0, sizeof(*track));
    sat_inc(&tracker->diagnostics.released_track_count);
    if (was_confirmed) {
        sat_inc(&tracker->diagnostics.deleted_track_count);
    }
}

void radar_target_tracker_init(radar_target_tracker_t *tracker,
                               const radar_spatial_thresholds_t *thresholds)
{
    if (tracker == NULL) return;
    memset(tracker, 0, sizeof(*tracker));
    if (thresholds != NULL) tracker->thresholds = *thresholds;
    tracker->next_track_id = 1U;
}

void radar_target_tracker_expire(radar_target_tracker_t *tracker, uint64_t now_ms)
{
    if (tracker == NULL) return;
    for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS; ++i) {
        radar_track_snapshot_t *track = &tracker->tracks[i];
        if (!track->active) continue;
        if (now_ms < track->last_seen_ms) {
            continue;
        }
        const uint64_t missing_ms = now_ms - track->last_seen_ms;
        if (missing_ms >= tracker->thresholds.track_timeout_ms) {
            release_track(tracker, track);
        } else if (track->lifecycle == RADAR_TRACK_TENTATIVE &&
                   missing_ms >= tracker->thresholds.track_hold_ms) {
            release_track(tracker, track);
        } else if (track->track_id != 0U && !track->visible &&
                   missing_ms >= tracker->thresholds.track_hold_ms) {
            track->lifecycle = RADAR_TRACK_HOLD;
        }
    }
}

static void update_track(radar_target_tracker_t *tracker,
                         radar_track_snapshot_t *track,
                         const radar_spatial_target_t *target,
                         const radar_zone_map_t *zone_map,
                         uint64_t now_ms)
{
    const int32_t previous_x = track->filtered_x_mm;
    const int32_t previous_y = track->filtered_y_mm;
    const uint64_t previous_seen_ms = track->last_seen_ms;
    const uint32_t displacement = track->active
        ? distance_between(previous_x, previous_y, target->x_mm, target->y_mm) : 0U;
    uint8_t zone_id = 0U;
    radar_zone_type_t zone_type = RADAR_ZONE_NONE;
    if (zone_map != NULL &&
        !radar_zone_map_resolve(zone_map, target->x_mm, target->y_mm, track->zone_id,
                                &zone_id, &zone_type)) {
        return;
    }
    const bool new_track = !track->active;
    const bool velocity_outlier = !new_track &&
        target_exceeds_velocity_limit(tracker, track, target, now_ms);
    if (new_track) {
        memset(track, 0, sizeof(*track));
        track->active = true;
        track->lifecycle = RADAR_TRACK_TENTATIVE;
        track->first_seen_ms = now_ms;
        track->confidence = tracker->thresholds.confidence_initial;
    } else {
        sat_inc(&tracker->diagnostics.association_count);
    }
    track->zone_changed = false;
    track->zone_left = false;
    if (track->zone_id != zone_id) {
        if (!new_track) {
            sat_inc(&tracker->diagnostics.zone_switch_count);
            track->zone_changed = true;
            track->previous_zone_id = track->zone_id;
            track->zone_left = track->zone_id != 0U && zone_id == 0U;
        }
        track->zone_id = zone_id;
        track->zone_entered_ms = now_ms;
    }
    track->visible = true;
    if (!new_track && track->lifecycle == RADAR_TRACK_HOLD) {
        track->lifecycle = RADAR_TRACK_CONFIRMED;
    }
    track->raw_x_mm = target->x_mm;
    track->raw_y_mm = target->y_mm;
    if (new_track) {
        track->filtered_x_mm = target->x_mm;
        track->filtered_y_mm = target->y_mm;
    } else {
        track->filtered_x_mm += (target->x_mm - track->filtered_x_mm) *
            (int32_t)tracker->thresholds.ema_alpha_percent / 100;
        track->filtered_y_mm += (target->y_mm - track->filtered_y_mm) *
            (int32_t)tracker->thresholds.ema_alpha_percent / 100;
    }
    track->x_mm = track->filtered_x_mm;
    track->y_mm = track->filtered_y_mm;
    track->speed_cm_s = target->speed_cm_s;
    track->resolution_mm = target->resolution_mm;
    track->distance_mm = target->distance_mm;
    track->angle_deg = target->angle_deg;
    if (velocity_outlier) {
        sat_inc(&tracker->diagnostics.velocity_outliers);
        if (track->consecutive_velocity_outliers < UINT8_MAX) {
            ++track->consecutive_velocity_outliers;
        }
        track->confidence = track->confidence > tracker->thresholds.confidence_loss_per_miss
            ? track->confidence - tracker->thresholds.confidence_loss_per_miss : 0U;
    } else if (!new_track) {
        track->consecutive_velocity_outliers = 0U;
        track->confidence = clamp_confidence(track->confidence +
                                             tracker->thresholds.confidence_gain_per_frame);
    }
    if (!new_track && previous_seen_ms != 0U && now_ms > previous_seen_ms) {
        const uint64_t dt_ms = now_ms - previous_seen_ms;
        track->velocity_x_mm_s = (int32_t)((int64_t)(track->filtered_x_mm - previous_x) *
                                            1000 / (int64_t)dt_ms);
        track->velocity_y_mm_s = (int32_t)((int64_t)(track->filtered_y_mm - previous_y) *
                                            1000 / (int64_t)dt_ms);
        if (track->velocity_x_mm_s != 0 || track->velocity_y_mm_s != 0) {
            track->direction_deg = (int16_t)lround(atan2((double)track->velocity_x_mm_s,
                                                         (double)track->velocity_y_mm_s) *
                                                   (180.0 / RADAR_PI));
        }
    }
    track->last_displacement_mm = displacement;
    track->last_match_distance_mm = displacement;
    track->last_seen_ms = now_ms;
    track->dwell_ms = now_ms >= track->zone_entered_ms ? now_ms - track->zone_entered_ms : 0U;
    track->missed_frames = 0U;
    sat_inc(&track->consecutive_seen);
    if (track->lifecycle == RADAR_TRACK_TENTATIVE &&
        track->consecutive_seen >= tracker->thresholds.track_confirm_frames &&
        track->confidence >= tracker->thresholds.track_confidence_min) {
        track->lifecycle = RADAR_TRACK_CONFIRMED;
        track->track_id = tracker->next_track_id++;
        if (tracker->next_track_id == 0U) tracker->next_track_id = 1U;
        sat_inc(&tracker->diagnostics.new_track_count);
    }
    (void)zone_type;
}

void radar_target_tracker_update(radar_target_tracker_t *tracker,
                                 const radar_spatial_target_t *targets,
                                 size_t target_count,
                                 const radar_zone_map_t *zone_map,
                                 uint64_t now_ms)
{
    if (tracker == NULL || (targets == NULL && target_count > 0U)) return;
    bool used_targets[RADAR_TRACKER_MAX_TRACKS] = {false};
    if (target_count > RADAR_TRACKER_MAX_TRACKS) target_count = RADAR_TRACKER_MAX_TRACKS;
    for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS; ++i) {
        if (tracker->tracks[i].active) {
            tracker->tracks[i].visible = false;
            tracker->tracks[i].zone_changed = false;
            tracker->tracks[i].zone_left = false;
        }
    }
    assignment_search_t search = {
        .tracker = tracker,
        .targets = targets,
        .target_count = target_count,
        .now_ms = now_ms,
        .best_cost = UINT64_MAX,
    };
    for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS; ++i) {
        search.current[i] = -1;
        search.best[i] = -1;
    }
    search_assignments(&search, 0U, 0U, 0U);
    bool confirmed_matched = false;
    for (size_t track_index = 0U; track_index < RADAR_TRACKER_MAX_TRACKS; ++track_index) {
        if (search.best[track_index] < 0) {
            continue;
        }
        const size_t target_index = (size_t)search.best[track_index];
        const bool was_confirmed = tracker->tracks[track_index].track_id != 0U;
        update_track(tracker, &tracker->tracks[track_index], &targets[target_index], zone_map, now_ms);
        if (was_confirmed) {
            confirmed_matched = true;
        }
        used_targets[target_index] = true;
    }
    for (size_t i = 0U; i < target_count; ++i) {
        if (used_targets[i] || !targets[i].valid) continue;
        const uint8_t confirmed_count = radar_target_tracker_confirmed_count(tracker);
        bool has_tentative = false;
        for (size_t j = 0U; j < RADAR_TRACKER_MAX_TRACKS; ++j) {
            if (tracker->tracks[j].active &&
                tracker->tracks[j].lifecycle == RADAR_TRACK_TENTATIVE) {
                has_tentative = true;
                break;
            }
        }
        /* A replacement measurement cannot create a second identity while its
         * original track is absent.  Additional people enter only beside an
         * already observed confirmed track, and candidates serialize one at a time. */
        if (has_tentative || (confirmed_count > 0U && !confirmed_matched)) {
            if (!confirmed_matched) {
                for (size_t j = 0U; j < RADAR_TRACKER_MAX_TRACKS; ++j) {
                    const radar_track_snapshot_t *track = &tracker->tracks[j];
                    if (track->track_id != 0U &&
                        target_exceeds_velocity_limit(tracker, track, &targets[i], now_ms)) {
                        sat_inc(&tracker->diagnostics.velocity_outliers);
                        break;
                    }
                }
            }
            sat_inc(&tracker->diagnostics.dropped_target_count);
            continue;
        }
        radar_track_snapshot_t *slot = NULL;
        for (size_t j = 0U; j < RADAR_TRACKER_MAX_TRACKS; ++j) {
            if (!tracker->tracks[j].active) { slot = &tracker->tracks[j]; break; }
        }
        if (slot == NULL) { sat_inc(&tracker->diagnostics.dropped_target_count); continue; }
        update_track(tracker, slot, &targets[i], zone_map, now_ms);
    }
    for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS; ++i) {
        radar_track_snapshot_t *track = &tracker->tracks[i];
        if (track->active && !track->visible) {
            sat_inc(&track->missed_frames);
            track->confidence = track->confidence > tracker->thresholds.confidence_loss_per_miss
                ? track->confidence - tracker->thresholds.confidence_loss_per_miss : 0U;
        }
    }
    radar_target_tracker_expire(tracker, now_ms);
}

void radar_target_tracker_mark_missing(radar_target_tracker_t *tracker, uint64_t now_ms)
{
    if (tracker == NULL) return;
    for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS; ++i) {
        radar_track_snapshot_t *track = &tracker->tracks[i];
        if (!track->active || !track->visible || now_ms < track->last_seen_ms) {
            continue;
        }
        track->visible = false;
        sat_inc(&track->missed_frames);
        track->confidence = track->confidence > tracker->thresholds.confidence_loss_per_miss
            ? track->confidence - tracker->thresholds.confidence_loss_per_miss : 0U;
    }
}

uint8_t radar_target_tracker_active_count(const radar_target_tracker_t *tracker)
{
    uint8_t count = 0U;
    if (tracker == NULL) return 0U;
    for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS; ++i)
        if (track_is_current(tracker, &tracker->tracks[i])) ++count;
    return count;
}

uint8_t radar_target_tracker_visible_count(const radar_target_tracker_t *tracker)
{
    uint8_t count = 0U;
    if (tracker == NULL) return 0U;
    for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS; ++i)
        if (track_is_current(tracker, &tracker->tracks[i])) ++count;
    return count;
}

uint8_t radar_target_tracker_confirmed_count(const radar_target_tracker_t *tracker)
{
    uint8_t count = 0U;
    if (tracker == NULL) return 0U;
    for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS; ++i) {
        const radar_track_snapshot_t *track = &tracker->tracks[i];
        if (track->active && track->track_id != 0U &&
            (track->lifecycle == RADAR_TRACK_CONFIRMED ||
             track->lifecycle == RADAR_TRACK_HOLD)) {
            ++count;
        }
    }
    return count;
}

uint8_t radar_target_tracker_stale_count(const radar_target_tracker_t *tracker)
{
    uint8_t count = 0U;
    if (tracker == NULL) return 0U;
    for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS; ++i) {
        const radar_track_snapshot_t *track = &tracker->tracks[i];
        if (track->active && track->track_id != 0U &&
            track->lifecycle == RADAR_TRACK_HOLD) {
            ++count;
        }
    }
    return count;
}

void radar_target_tracker_copy(const radar_target_tracker_t *tracker,
                               radar_track_snapshot_t *out,
                               size_t capacity)
{
    if (tracker == NULL || out == NULL || capacity == 0U) return;
    const size_t count = capacity < RADAR_TRACKER_MAX_TRACKS ? capacity : RADAR_TRACKER_MAX_TRACKS;
    memcpy(out, tracker->tracks, count * sizeof(*out));
}

uint8_t radar_target_tracker_copy_active(const radar_target_tracker_t *tracker,
                                         radar_track_snapshot_t *out,
                                         size_t capacity)
{
    if (tracker == NULL || out == NULL || capacity == 0U) return 0U;
    uint8_t count = 0U;
    for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS && count < capacity; ++i) {
        if (!track_is_current(tracker, &tracker->tracks[i])) continue;
        out[count++] = tracker->tracks[i];
    }
    return count;
}

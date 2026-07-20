#include "radar_person_continuity.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "radar_source_context.h"
#endif

/* This layer keeps short business continuity only.  It deliberately does not
 * alter tracker allocation, slot meaning, or long-term identity semantics. */

#define TAG "radar_person"

typedef struct {
    radar_person_continuity_t *continuity;
    const radar_track_snapshot_t *tracks;
    size_t track_count;
    uint64_t now_ms;
    int current[RADAR_PERSON_CONTINUITY_MAX_PERSONS];
    int best[RADAR_PERSON_CONTINUITY_MAX_PERSONS];
    bool used_tracks[RADAR_TRACKER_MAX_TRACKS];
    uint8_t best_matches;
    uint64_t best_cost;
} person_assignment_search_t;

static uint32_t age_ms(uint64_t timestamp_ms, uint64_t now_ms)
{
    if (timestamp_ms == 0U || now_ms < timestamp_ms) {
        return 0U;
    }
    const uint64_t age = now_ms - timestamp_ms;
    return age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
}

static uint32_t distance_between(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    const double dx = (double)x1 - (double)x2;
    const double dy = (double)y1 - (double)y2;
    const double distance = sqrt(dx * dx + dy * dy);
    return distance > UINT32_MAX ? UINT32_MAX : (uint32_t)lround(distance);
}

static bool elapsed(uint64_t now_ms, uint64_t previous_ms, uint32_t interval_ms)
{
    return previous_ms == 0U || now_ms < previous_ms || now_ms - previous_ms >= interval_ms;
}

static bool track_is_visible_confirmed(const radar_track_snapshot_t *track)
{
    return track != NULL && track->active && track->visible &&
           track->lifecycle == RADAR_TRACK_CONFIRMED && track->track_id != 0U;
}

static bool person_is_retained(const radar_person_snapshot_t *person)
{
    return person != NULL && (person->state == RADAR_PERSON_STILL_HOLD ||
                               person->state == RADAR_PERSON_DORMANT);
}

const char *radar_person_state_name(radar_person_state_t state)
{
    switch (state) {
    case RADAR_PERSON_VISIBLE: return "VISIBLE";
    case RADAR_PERSON_STILL_HOLD: return "STILL_HOLD";
    case RADAR_PERSON_DORMANT: return "DORMANT";
    case RADAR_PERSON_RELEASED: return "RELEASED";
    case RADAR_PERSON_EMPTY:
    default: return "EMPTY";
    }
}

const char *radar_person_count_state_name(radar_person_count_state_t state)
{
    switch (state) {
    case RADAR_PERSON_COUNT_OBSERVED: return "OBSERVED";
    case RADAR_PERSON_COUNT_ESTIMATED: return "ESTIMATED";
    case RADAR_PERSON_COUNT_VACANT_INFERRED: return "VACANT_INFERRED";
    case RADAR_PERSON_COUNT_UNKNOWN:
    default: return "UNKNOWN";
    }
}

static void refresh_counts(radar_person_continuity_t *continuity)
{
    if (continuity == NULL) {
        return;
    }

    continuity->visible_person_count = 0U;
    continuity->retained_person_count = 0U;
    continuity->source_person_count = 0U;
    if (!continuity->source_valid) {
        continuity->count_state = RADAR_PERSON_COUNT_UNKNOWN;
        return;
    }

    for (size_t i = 0U; i < RADAR_PERSON_CONTINUITY_MAX_PERSONS; ++i) {
        const radar_person_snapshot_t *person = &continuity->persons[i];
        if (person->state == RADAR_PERSON_VISIBLE) {
            ++continuity->visible_person_count;
            ++continuity->source_person_count;
        } else if (person_is_retained(person)) {
            ++continuity->retained_person_count;
            ++continuity->source_person_count;
        }
    }

    if (continuity->visible_person_count > 0U) {
        continuity->count_state = RADAR_PERSON_COUNT_OBSERVED;
    } else if (continuity->retained_person_count > 0U) {
        continuity->count_state = RADAR_PERSON_COUNT_ESTIMATED;
    } else if (continuity->unresolved_observation) {
        /* A tentative/new motion segment is not a person or a vacancy claim. */
        continuity->count_state = RADAR_PERSON_COUNT_UNKNOWN;
    } else {
        continuity->count_state = RADAR_PERSON_COUNT_VACANT_INFERRED;
    }
}

static void log_event(const radar_person_continuity_t *continuity,
                      const char *event,
                      const radar_person_snapshot_t *person,
                      uint32_t old_track_id,
                      uint32_t new_track_id,
                      uint32_t distance_mm,
                      uint64_t now_ms,
                      uint8_t zone_id,
                      const char *reason)
{
#ifdef ESP_PLATFORM
    if (continuity == NULL || event == NULL) {
        return;
    }
    const uint32_t person_id = person != NULL ? person->person_id : 0U;
    const uint32_t person_age = person != NULL ? age_ms(person->first_seen_ms, now_ms) : 0U;
    const RadarSourceContext *context = radar_source_context_get(
        (radar_source_id_t)continuity->source_id);
    ESP_LOGI(TAG,
             "RADAR_PERSON_UPDATE event=%s source_id=%u source=%s device_id=%s room=%s sequence=%lu timestamp_ms=%llu person_id=%lu old_track_id=%lu new_track_id=%lu distance_mm=%lu age_ms=%lu zone=%u visible_person_count=%u retained_person_count=%u source_person_count=%u count_state=%s reason=%s",
             event,
             (unsigned int)continuity->source_id,
             context != NULL ? context->source_name : "UNKNOWN",
             context != NULL ? context->device_id : "unknown",
             context != NULL ? context->room_id : "unknown",
             (unsigned long)continuity->source_sequence,
             (unsigned long long)now_ms,
             (unsigned long)person_id,
             (unsigned long)old_track_id,
             (unsigned long)new_track_id,
             (unsigned long)distance_mm,
             (unsigned long)person_age,
             (unsigned int)zone_id,
             (unsigned int)continuity->visible_person_count,
             (unsigned int)continuity->retained_person_count,
             (unsigned int)continuity->source_person_count,
             radar_person_count_state_name(continuity->count_state),
             reason != NULL ? reason : "unspecified");
#else
    (void)continuity;
    (void)event;
    (void)person;
    (void)old_track_id;
    (void)new_track_id;
    (void)distance_mm;
    (void)now_ms;
    (void)zone_id;
    (void)reason;
#endif
}

static bool zones_are_compatible(const radar_person_continuity_t *continuity,
                                 uint8_t person_zone,
                                 uint8_t track_zone)
{
    if (person_zone == 0U || track_zone == 0U || person_zone == track_zone) {
        return true;
    }
    if (continuity == NULL || !continuity->config.adjacent_zone_allow) {
        return false;
    }
    return person_zone + 1U == track_zone || track_zone + 1U == person_zone;
}

static void predicted_position(const radar_person_continuity_t *continuity,
                               const radar_person_snapshot_t *person,
                               uint64_t now_ms,
                               int32_t *out_x_mm,
                               int32_t *out_y_mm)
{
    int32_t x_mm = person->last_filtered_x_mm;
    int32_t y_mm = person->last_filtered_y_mm;
    const uint32_t age = age_ms(person->last_visible_ms, now_ms);
    const uint32_t start = continuity->config.velocity_decay_start_ms;
    const uint32_t decay = continuity->config.velocity_decay_ms;
    uint64_t effective_ms = age;

    if (age > start) {
        const uint32_t after_start = age - start;
        if (decay == 0U || after_start >= decay) {
            effective_ms = (uint64_t)start + (uint64_t)decay / 2U;
        } else {
            /* Integrate a linear velocity decay to avoid a late, unbounded
             * extrapolation while preserving a smooth short occlusion gate. */
            effective_ms = (uint64_t)start + after_start -
                ((uint64_t)after_start * after_start) / ((uint64_t)decay * 2U);
        }
    }
    x_mm += (int32_t)((int64_t)person->velocity_x_mm_s * (int64_t)effective_ms / 1000);
    y_mm += (int32_t)((int64_t)person->velocity_y_mm_s * (int64_t)effective_ms / 1000);
    *out_x_mm = x_mm;
    *out_y_mm = y_mm;
}

static bool attached_track_is_current(const radar_track_snapshot_t *tracks,
                                      size_t track_count,
                                      uint32_t attached_track_id)
{
    if (tracks == NULL || attached_track_id == 0U) {
        return false;
    }
    for (size_t i = 0U; i < track_count; ++i) {
        if (track_is_visible_confirmed(&tracks[i]) && tracks[i].track_id == attached_track_id) {
            return true;
        }
    }
    return false;
}

static bool person_can_match_track(const radar_person_continuity_t *continuity,
                                   const radar_person_snapshot_t *person,
                                   const radar_track_snapshot_t *track,
                                   const radar_track_snapshot_t *tracks,
                                   size_t track_count,
                                   uint64_t now_ms,
                                   uint32_t *out_distance_mm)
{
    if (continuity == NULL || person == NULL || track == NULL ||
        !track_is_visible_confirmed(track) || person->person_id == 0U ||
        person->state == RADAR_PERSON_EMPTY || person->state == RADAR_PERSON_RELEASED ||
        age_ms(person->last_visible_ms, now_ms) >= continuity->config.dormant_timeout_ms) {
        return false;
    }
    if (person->attached_track_id == track->track_id) {
        if (out_distance_mm != NULL) {
            *out_distance_mm = 0U;
        }
        return true;
    }
    if (person->state == RADAR_PERSON_VISIBLE &&
        attached_track_is_current(tracks, track_count, person->attached_track_id)) {
        return false;
    }
    if (!zones_are_compatible(continuity, person->last_zone_id, track->zone_id)) {
        return false;
    }
    int32_t predicted_x_mm = 0;
    int32_t predicted_y_mm = 0;
    predicted_position(continuity, person, now_ms, &predicted_x_mm, &predicted_y_mm);
    const uint32_t distance = distance_between(predicted_x_mm,
                                               predicted_y_mm,
                                               track->filtered_x_mm,
                                               track->filtered_y_mm);
    if (distance > continuity->config.reacquire_gate_mm) {
        return false;
    }
    if (out_distance_mm != NULL) {
        *out_distance_mm = distance;
    }
    return true;
}

static uint32_t match_cost(const radar_person_continuity_t *continuity,
                           const radar_person_snapshot_t *person,
                           const radar_track_snapshot_t *track,
                           const radar_track_snapshot_t *tracks,
                           size_t track_count,
                           uint64_t now_ms)
{
    uint32_t distance = 0U;
    if (!person_can_match_track(continuity, person, track, tracks, track_count, now_ms, &distance)) {
        return UINT32_MAX;
    }
    if (person->attached_track_id == track->track_id) {
        return 0U;
    }
    if (person->last_zone_id != 0U && person->last_zone_id == track->zone_id &&
        distance > continuity->config.same_zone_bonus_mm) {
        distance -= continuity->config.same_zone_bonus_mm;
    } else if (person->last_zone_id != 0U && person->last_zone_id == track->zone_id) {
        distance = 0U;
    }
    return distance;
}

static void search_assignments(person_assignment_search_t *search,
                               size_t person_index,
                               uint8_t matches,
                               uint64_t cost)
{
    if (person_index == RADAR_PERSON_CONTINUITY_MAX_PERSONS) {
        if (matches > search->best_matches ||
            (matches == search->best_matches && cost < search->best_cost)) {
            search->best_matches = matches;
            search->best_cost = cost;
            memcpy(search->best, search->current, sizeof(search->best));
        }
        return;
    }

    search->current[person_index] = -1;
    search_assignments(search, person_index + 1U, matches, cost);

    const radar_person_snapshot_t *person = &search->continuity->persons[person_index];
    for (size_t track_index = 0U; track_index < search->track_count; ++track_index) {
        if (search->used_tracks[track_index]) {
            continue;
        }
        const uint32_t candidate_cost = match_cost(search->continuity,
                                                   person,
                                                   &search->tracks[track_index],
                                                   search->tracks,
                                                   search->track_count,
                                                   search->now_ms);
        if (candidate_cost == UINT32_MAX || UINT64_MAX - cost < candidate_cost) {
            continue;
        }
        search->used_tracks[track_index] = true;
        search->current[person_index] = (int)track_index;
        search_assignments(search, person_index + 1U, matches + 1U, cost + candidate_cost);
        search->used_tracks[track_index] = false;
        search->current[person_index] = -1;
    }
}

static void release_person(radar_person_continuity_t *continuity,
                           radar_person_snapshot_t *person,
                           uint64_t now_ms,
                           const char *reason)
{
    const uint32_t old_track_id = person->attached_track_id;
    person->state = RADAR_PERSON_RELEASED;
    person->attached_track_id = 0U;
    refresh_counts(continuity);
    log_event(continuity, "PERSON_RELEASE", person, old_track_id, 0U, 0U, now_ms,
              person->last_zone_id, reason);
}

static void update_unmatched_person(radar_person_continuity_t *continuity,
                                    radar_person_snapshot_t *person,
                                    uint64_t now_ms)
{
    if (person->person_id == 0U || person->state == RADAR_PERSON_EMPTY ||
        person->state == RADAR_PERSON_RELEASED) {
        return;
    }
    const uint32_t age = age_ms(person->last_visible_ms, now_ms);
    if (person->quality > 0U) {
        --person->quality;
    }
    if (age >= continuity->config.dormant_timeout_ms) {
        release_person(continuity, person, now_ms, "continuity_timeout");
    } else if (age >= continuity->config.still_hold_ms) {
        if (person->state != RADAR_PERSON_DORMANT) {
            person->state = RADAR_PERSON_DORMANT;
            person->dormant_since_ms = now_ms;
            refresh_counts(continuity);
            log_event(continuity, "PERSON_DORMANT", person, person->attached_track_id,
                      person->attached_track_id, 0U, now_ms, person->last_zone_id,
                      "visible_gap_exceeded_still_hold");
        }
    } else if (person->state != RADAR_PERSON_STILL_HOLD) {
        person->state = RADAR_PERSON_STILL_HOLD;
        refresh_counts(continuity);
        log_event(continuity, "PERSON_STILL_HOLD", person, person->attached_track_id,
                  person->attached_track_id, 0U, now_ms, person->last_zone_id,
                  "short_visible_gap");
    }
}

static void attach_person(radar_person_continuity_t *continuity,
                          radar_person_snapshot_t *person,
                          const radar_track_snapshot_t *track,
                          uint32_t distance_mm,
                          uint64_t now_ms,
                          bool creating)
{
    const uint32_t old_track_id = person->attached_track_id;
    const radar_person_state_t old_state = person->state;
    person->state = RADAR_PERSON_VISIBLE;
    person->attached_track_id = track->track_id;
    person->last_filtered_x_mm = track->filtered_x_mm;
    person->last_filtered_y_mm = track->filtered_y_mm;
    person->last_zone_id = track->zone_id;
    person->last_visible_ms = now_ms;
    person->last_attached_ms = now_ms;
    person->dormant_since_ms = 0U;
    person->quality = track->confidence;
    person->velocity_x_mm_s = track->velocity_x_mm_s;
    person->velocity_y_mm_s = track->velocity_y_mm_s;
    person->last_velocity_ms = now_ms;
    refresh_counts(continuity);

    if (creating) {
        log_event(continuity, "PERSON_CREATE", person, 0U, track->track_id, distance_mm,
                  now_ms, track->zone_id, "confirmed_new_motion_segment");
    }
    const bool track_attachment_changed = creating || old_track_id != track->track_id ||
        old_state != RADAR_PERSON_VISIBLE;
    if (track_attachment_changed) {
        log_event(continuity, "PERSON_ATTACH_TRACK", person, old_track_id, track->track_id,
                  distance_mm, now_ms, track->zone_id,
                  creating ? "new_person_confirmed" : "existing_person_match");
    }
    if (!creating && old_track_id != 0U && old_track_id != track->track_id) {
        log_event(continuity, "PERSON_REACQUIRE", person, old_track_id, track->track_id,
                  distance_mm, now_ms, track->zone_id,
                  old_state == RADAR_PERSON_DORMANT ? "dormant_track_reacquire" :
                  (old_state == RADAR_PERSON_STILL_HOLD ? "still_hold_track_reacquire" :
                                                        "visible_track_replacement"));
    }
}

static bool near_retained_person(const radar_person_continuity_t *continuity,
                                 const radar_track_snapshot_t *track,
                                 uint64_t now_ms)
{
    for (size_t i = 0U; i < RADAR_PERSON_CONTINUITY_MAX_PERSONS; ++i) {
        const radar_person_snapshot_t *person = &continuity->persons[i];
        if (!person_is_retained(person) ||
            age_ms(person->last_visible_ms, now_ms) >= continuity->config.dormant_timeout_ms ||
            !zones_are_compatible(continuity, person->last_zone_id, track->zone_id)) {
            continue;
        }
        int32_t predicted_x_mm = 0;
        int32_t predicted_y_mm = 0;
        predicted_position(continuity, person, now_ms, &predicted_x_mm, &predicted_y_mm);
        if (distance_between(predicted_x_mm, predicted_y_mm,
                             track->filtered_x_mm, track->filtered_y_mm) <=
            continuity->config.reacquire_gate_mm) {
            return true;
        }
    }
    return false;
}

static radar_person_candidate_t *candidate_for_track(radar_person_continuity_t *continuity,
                                                      uint32_t track_id)
{
    radar_person_candidate_t *available = NULL;
    for (size_t i = 0U; i < RADAR_PERSON_CONTINUITY_MAX_CANDIDATES; ++i) {
        radar_person_candidate_t *candidate = &continuity->candidates[i];
        if (candidate->track_id == track_id) {
            return candidate;
        }
        if (candidate->track_id == 0U && available == NULL) {
            available = candidate;
        }
    }
    return available;
}

static void clear_candidate_for_track(radar_person_continuity_t *continuity, uint32_t track_id)
{
    if (track_id == 0U) {
        return;
    }
    for (size_t i = 0U; i < RADAR_PERSON_CONTINUITY_MAX_CANDIDATES; ++i) {
        if (continuity->candidates[i].track_id == track_id) {
            memset(&continuity->candidates[i], 0, sizeof(continuity->candidates[i]));
        }
    }
}

static radar_person_snapshot_t *available_person_slot(radar_person_continuity_t *continuity)
{
    for (size_t i = 0U; i < RADAR_PERSON_CONTINUITY_MAX_PERSONS; ++i) {
        if (continuity->persons[i].state == RADAR_PERSON_EMPTY ||
            continuity->persons[i].state == RADAR_PERSON_RELEASED) {
            return &continuity->persons[i];
        }
    }
    return NULL;
}

static void consider_new_person(radar_person_continuity_t *continuity,
                                const radar_track_snapshot_t *track,
                                uint64_t now_ms)
{
    radar_person_candidate_t *candidate = candidate_for_track(continuity, track->track_id);
    if (candidate == NULL) {
        return;
    }
    const bool near_retained = near_retained_person(continuity, track, now_ms);
    if (candidate->track_id != track->track_id ||
        (candidate->last_seen_ms != 0U &&
         (now_ms < candidate->last_seen_ms ||
          now_ms - candidate->last_seen_ms >= continuity->config.still_hold_ms))) {
        *candidate = (radar_person_candidate_t){.track_id = track->track_id,
                                                .last_seen_ms = now_ms,
                                                .consecutive_confirmed_frames = 1U,
                                                .near_retained_person = near_retained};
    } else {
        if (now_ms == candidate->last_seen_ms) {
            return;
        }
        candidate->last_seen_ms = now_ms;
        candidate->near_retained_person = candidate->near_retained_person || near_retained;
        if (candidate->consecutive_confirmed_frames < UINT8_MAX) {
            ++candidate->consecutive_confirmed_frames;
        }
    }
    const uint32_t required = candidate->near_retained_person
        ? continuity->config.new_near_dormant_confirm_frames
        : continuity->config.new_confirm_frames;
    if (candidate->consecutive_confirmed_frames < required) {
        return;
    }
    radar_person_snapshot_t *person = available_person_slot(continuity);
    if (person == NULL) {
        return;
    }
    memset(person, 0, sizeof(*person));
    person->person_id = continuity->next_person_id++;
    if (continuity->next_person_id == 0U) {
        continuity->next_person_id = 1U;
    }
    person->first_seen_ms = now_ms;
    attach_person(continuity, person, track, 0U, now_ms, true);
    clear_candidate_for_track(continuity, track->track_id);
}

static void expire_candidates(radar_person_continuity_t *continuity, uint64_t now_ms)
{
    for (size_t i = 0U; i < RADAR_PERSON_CONTINUITY_MAX_CANDIDATES; ++i) {
        radar_person_candidate_t *candidate = &continuity->candidates[i];
        if (candidate->track_id != 0U &&
            age_ms(candidate->last_seen_ms, now_ms) >= continuity->config.still_hold_ms) {
            memset(candidate, 0, sizeof(*candidate));
        }
    }
}

void radar_person_continuity_init(radar_person_continuity_t *continuity,
                                  const radar_person_continuity_config_t *config,
                                  uint8_t source_id)
{
    if (continuity == NULL) {
        return;
    }
    memset(continuity, 0, sizeof(*continuity));
    if (config != NULL) {
        continuity->config = *config;
    }
    continuity->next_person_id = 1U;
    continuity->source_id = source_id;
    continuity->count_state = RADAR_PERSON_COUNT_UNKNOWN;
}

void radar_person_continuity_set_source(radar_person_continuity_t *continuity,
                                        uint8_t source_id)
{
    if (continuity != NULL) {
        continuity->source_id = source_id;
    }
}

void radar_person_continuity_set_sequence(radar_person_continuity_t *continuity,
                                          uint32_t sequence)
{
    if (continuity != NULL) {
        continuity->source_sequence = sequence;
    }
}

void radar_person_continuity_update(radar_person_continuity_t *continuity,
                                    const radar_track_snapshot_t *tracks,
                                    size_t track_count,
                                    bool source_valid,
                                    bool unresolved_observation,
                                    uint64_t now_ms)
{
    if (continuity == NULL || (tracks == NULL && track_count > 0U)) {
        return;
    }
    if (track_count > RADAR_TRACKER_MAX_TRACKS) {
        track_count = RADAR_TRACKER_MAX_TRACKS;
    }
    const uint8_t previous_visible = continuity->visible_person_count;
    const uint8_t previous_retained = continuity->retained_person_count;
    const uint8_t previous_business = continuity->source_person_count;
    const radar_person_count_state_t previous_state = continuity->count_state;
    continuity->source_valid = source_valid;
    continuity->unresolved_observation = unresolved_observation;

    if (!source_valid) {
        refresh_counts(continuity);
        if (previous_state != continuity->count_state) {
            log_event(continuity, "PERSON_COUNT_CHANGE", NULL, 0U, 0U, 0U, now_ms, 0U,
                      "source_not_observable");
        }
        return;
    }

    for (size_t i = 0U; i < RADAR_PERSON_CONTINUITY_MAX_PERSONS; ++i) {
        radar_person_snapshot_t *person = &continuity->persons[i];
        if (person->person_id != 0U && person->state != RADAR_PERSON_EMPTY &&
            person->state != RADAR_PERSON_RELEASED &&
            age_ms(person->last_visible_ms, now_ms) >= continuity->config.dormant_timeout_ms) {
            release_person(continuity, person, now_ms, "continuity_timeout_before_match");
        }
    }

    person_assignment_search_t search = {
        .continuity = continuity,
        .tracks = tracks,
        .track_count = track_count,
        .now_ms = now_ms,
        .best_cost = UINT64_MAX,
    };
    for (size_t i = 0U; i < RADAR_PERSON_CONTINUITY_MAX_PERSONS; ++i) {
        search.current[i] = -1;
        search.best[i] = -1;
    }
    search_assignments(&search, 0U, 0U, 0U);

    bool matched_tracks[RADAR_TRACKER_MAX_TRACKS] = {false};
    for (size_t person_index = 0U; person_index < RADAR_PERSON_CONTINUITY_MAX_PERSONS; ++person_index) {
        const int track_index = search.best[person_index];
        radar_person_snapshot_t *person = &continuity->persons[person_index];
        if (track_index < 0) {
            update_unmatched_person(continuity, person, now_ms);
            continue;
        }
        const radar_track_snapshot_t *track = &tracks[(size_t)track_index];
        uint32_t distance_mm = 0U;
        (void)person_can_match_track(continuity, person, track, tracks, track_count,
                                     now_ms, &distance_mm);
        attach_person(continuity, person, track, distance_mm, now_ms, false);
        matched_tracks[(size_t)track_index] = true;
        clear_candidate_for_track(continuity, track->track_id);
    }

    for (size_t track_index = 0U; track_index < track_count; ++track_index) {
        if (!matched_tracks[track_index] && track_is_visible_confirmed(&tracks[track_index])) {
            continuity->unresolved_observation = true;
            consider_new_person(continuity, &tracks[track_index], now_ms);
        }
    }
    expire_candidates(continuity, now_ms);
    refresh_counts(continuity);

    if (previous_visible != continuity->visible_person_count ||
        previous_retained != continuity->retained_person_count ||
        previous_business != continuity->source_person_count ||
        previous_state != continuity->count_state) {
        log_event(continuity, "PERSON_COUNT_CHANGE", NULL, 0U, 0U, 0U, now_ms, 0U,
                  "continuity_state_change");
    }

    uint8_t visible_track_count = 0U;
    for (size_t i = 0U; i < track_count; ++i) {
        if (track_is_visible_confirmed(&tracks[i])) {
            ++visible_track_count;
        }
    }
    if (visible_track_count != continuity->visible_person_count &&
        elapsed(now_ms, continuity->last_mismatch_log_ms, 1000U)) {
        continuity->last_mismatch_log_ms = now_ms;
        log_event(continuity, "TRACK_COUNT_MISMATCH", NULL, 0U, 0U, 0U, now_ms, 0U,
                  "visible_tracks_vs_visible_persons");
    }
}

void radar_person_continuity_copy_persons(const radar_person_continuity_t *continuity,
                                          radar_person_snapshot_t *out,
                                          size_t capacity)
{
    if (continuity == NULL || out == NULL || capacity == 0U) {
        return;
    }
    const size_t count = capacity < RADAR_PERSON_CONTINUITY_MAX_PERSONS
        ? capacity : RADAR_PERSON_CONTINUITY_MAX_PERSONS;
    memcpy(out, continuity->persons, count * sizeof(*out));
}

void radar_person_continuity_get_counts(const radar_person_continuity_t *continuity,
                                        uint8_t *visible_person_count,
                                        uint8_t *retained_person_count,
                                        uint8_t *source_person_count,
                                        radar_person_count_state_t *count_state)
{
    if (visible_person_count != NULL) {
        *visible_person_count = continuity != NULL ? continuity->visible_person_count : 0U;
    }
    if (retained_person_count != NULL) {
        *retained_person_count = continuity != NULL ? continuity->retained_person_count : 0U;
    }
    if (source_person_count != NULL) {
        *source_person_count = continuity != NULL ? continuity->source_person_count : 0U;
    }
    if (count_state != NULL) {
        *count_state = continuity != NULL ? continuity->count_state : RADAR_PERSON_COUNT_UNKNOWN;
    }
}

#include "radar_edge_filter.h"

#include <limits.h>
#include <string.h>

static bool target_is_accepted(const radar_target_t *target)
{
    return target != NULL && target->valid && target->slot < LD2450_MAX_TARGETS &&
           target->distance_mm <= RADAR_EDGE_FILTER_MAX_DISTANCE_MM;
}

static int16_t smooth_speed(int16_t previous, int16_t current)
{
    const int32_t mixed = (int32_t)previous * 2 + current;
    const int32_t rounded = mixed >= 0 ? (mixed + 1) / 3 : (mixed - 1) / 3;
    return rounded > INT16_MAX ? INT16_MAX :
           rounded < INT16_MIN ? INT16_MIN : (int16_t)rounded;
}

static uint8_t confidence_for_continuity(uint8_t continuity)
{
    const uint16_t value = 50U + (uint16_t)continuity * 10U;
    return value > 100U ? 100U : (uint8_t)value;
}

static void sort_by_distance(radar_target_sample_t *sample)
{
    for (uint8_t i = 1U; i < sample->target_count; ++i) {
        radar_target_t current = sample->targets[i];
        uint8_t index = i;
        while (index > 0U && sample->targets[index - 1U].distance_mm > current.distance_mm) {
            sample->targets[index] = sample->targets[index - 1U];
            --index;
        }
        sample->targets[index] = current;
    }
}

void radar_edge_filter_init(radar_edge_filter_t *filter)
{
    if (filter != NULL) {
        memset(filter, 0, sizeof(*filter));
    }
}

void radar_edge_filter_apply(radar_edge_filter_t *filter, radar_target_sample_t *sample)
{
    if (filter == NULL || sample == NULL) {
        return;
    }
    if (!sample->sample_valid) {
        sample->target_count = 0U;
        memset(sample->targets, 0, sizeof(sample->targets));
        filter->has_latest = false;
        return;
    }
    if (filter->has_latest && sample->frame_seq == filter->latest_frame_seq) {
        const uint8_t link_state = sample->link_state;
        *sample = filter->latest;
        sample->link_state = link_state;
        return;
    }

    radar_target_sample_t filtered = *sample;
    filtered.target_count = 0U;
    memset(filtered.targets, 0, sizeof(filtered.targets));
    bool observed[LD2450_MAX_TARGETS] = {false};
    for (uint8_t index = 0U; index < sample->target_count; ++index) {
        radar_target_t target = sample->targets[index];
        if (!target_is_accepted(&target) || observed[target.slot]) {
            continue;
        }
        observed[target.slot] = true;
        const uint8_t slot = target.slot;
        target.speed_cm_s = filter->has_speed[slot]
            ? smooth_speed(filter->smoothed_speed_cm_s[slot], target.speed_cm_s)
            : target.speed_cm_s;
        filter->has_speed[slot] = true;
        filter->smoothed_speed_cm_s[slot] = target.speed_cm_s;
        if (filter->continuity[slot] < 5U) {
            ++filter->continuity[slot];
        }
        /* This is edge continuity confidence, not a vendor RF signal-quality claim. */
        target.confidence = confidence_for_continuity(filter->continuity[slot]);
        filtered.targets[filtered.target_count++] = target;
    }
    for (uint8_t slot = 0U; slot < LD2450_MAX_TARGETS; ++slot) {
        if (!observed[slot]) {
            filter->has_speed[slot] = false;
            filter->continuity[slot] = 0U;
        }
    }
    sort_by_distance(&filtered);
    filter->latest = filtered;
    filter->latest_frame_seq = filtered.frame_seq;
    filter->has_latest = true;
    *sample = filtered;
}

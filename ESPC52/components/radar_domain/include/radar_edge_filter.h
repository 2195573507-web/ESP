#ifndef RADAR_EDGE_FILTER_H
#define RADAR_EDGE_FILTER_H

#include "radar_target_sample.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RADAR_EDGE_FILTER_MAX_DISTANCE_MM 6000U

typedef struct {
    bool has_speed[LD2450_MAX_TARGETS];
    int16_t smoothed_speed_cm_s[LD2450_MAX_TARGETS];
    uint8_t continuity[LD2450_MAX_TARGETS];
    bool has_latest;
    uint32_t latest_frame_seq;
    radar_target_sample_t latest;
} radar_edge_filter_t;

void radar_edge_filter_init(radar_edge_filter_t *filter);
void radar_edge_filter_apply(radar_edge_filter_t *filter, radar_target_sample_t *sample);

#ifdef __cplusplus
}
#endif

#endif

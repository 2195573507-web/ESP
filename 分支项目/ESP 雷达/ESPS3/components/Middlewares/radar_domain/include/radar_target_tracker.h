#ifndef RADAR_TARGET_TRACKER_H
#define RADAR_TARGET_TRACKER_H

#include <stddef.h>

#include "radar_spatial_types.h"
#include "radar_zone_map.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* 跟踪 ID 单调递增；轨迹释放后不会重新分配旧 ID。 */
    radar_track_snapshot_t tracks[RADAR_TRACKER_MAX_TRACKS];
    uint32_t next_track_id;
    radar_tracker_diagnostics_t diagnostics;
    radar_spatial_thresholds_t thresholds;
} radar_target_tracker_t;

void radar_target_tracker_init(radar_target_tracker_t *tracker,
                               const radar_spatial_thresholds_t *thresholds);
void radar_target_tracker_update(radar_target_tracker_t *tracker,
                                 const radar_spatial_target_t *targets,
                                 size_t target_count,
                                 const radar_zone_map_t *zone_map,
                                 uint64_t now_ms);
void radar_target_tracker_mark_missing(radar_target_tracker_t *tracker, uint64_t now_ms);
void radar_target_tracker_expire(radar_target_tracker_t *tracker, uint64_t now_ms);
uint8_t radar_target_tracker_active_count(const radar_target_tracker_t *tracker);
uint8_t radar_target_tracker_visible_count(const radar_target_tracker_t *tracker);
uint8_t radar_target_tracker_confirmed_count(const radar_target_tracker_t *tracker);
uint8_t radar_target_tracker_stale_count(const radar_target_tracker_t *tracker);
void radar_target_tracker_copy(const radar_target_tracker_t *tracker,
                               radar_track_snapshot_t *out,
                               size_t capacity);
uint8_t radar_target_tracker_copy_active(const radar_target_tracker_t *tracker,
                                         radar_track_snapshot_t *out,
                                         size_t capacity);

#ifdef __cplusplus
}
#endif

#endif

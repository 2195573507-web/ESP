#ifndef RADAR_SOURCE_CONTEXT_H
#define RADAR_SOURCE_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>

#include "radar_spatial_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RADAR_SOURCE_CONTEXT_NAME_LEN 16U
#define RADAR_SOURCE_CONTEXT_DEVICE_ID_LEN 48U
#define RADAR_SOURCE_CONTEXT_ROOM_ID_LEN 32U

/* The source identity is part of the data contract, not a caller convention. */
typedef enum {
    RADAR_SOURCE_S3_LOCAL = 0,
    RADAR_SOURCE_C51 = 1,
    RADAR_SOURCE_C52 = 2,
    RADAR_SOURCE_COUNT,
} radar_source_id_t;

typedef enum {
    RADAR_TRANSPORT_S3_LOCAL_UART = 0,
    RADAR_TRANSPORT_C5_BLE_HTTP,
} radar_transport_type_t;

typedef struct {
    int32_t x_mm;
    int32_t y_mm;
} radar_point_mm_t;

/* Published state for exactly one physical radar.  This is a snapshot, not a
 * mutable aggregation target: consumers must never write a different source's
 * room, presence, tracks, persons, or timestamp through it. */
typedef struct {
    radar_source_id_t source_id;
    char device_id[RADAR_SOURCE_CONTEXT_DEVICE_ID_LEN];
    char room_id[RADAR_SOURCE_CONTEXT_ROOM_ID_LEN];
    bool online;
    radar_presence_state_t presence;
    radar_motion_state_t motion;
    radar_track_snapshot_t tracks[RADAR_TRACKER_MAX_TRACKS];
    radar_person_snapshot_t persons[RADAR_PERSON_CONTINUITY_MAX_PERSONS];
    radar_count_summary_t count_summary;
    uint64_t timestamp_ms;
    uint32_t sequence;
} RadarSourceState;

/*
 * One object owns the complete state boundary for one physical radar.
 * spatial_storage is the owner; the tracker/person/spatial pointers are
 * intentionally aliases into that object so no second state copy can drift.
 */
typedef struct radar_source_context {
    radar_source_id_t source_id;
    char source_name[RADAR_SOURCE_CONTEXT_NAME_LEN];
    char device_id[RADAR_SOURCE_CONTEXT_DEVICE_ID_LEN];
    char room_id[RADAR_SOURCE_CONTEXT_ROOM_ID_LEN];
    radar_transport_type_t transport_type;
    bool online_state;
    radar_presence_state_t presence_state;
    uint32_t state_version;
    uint64_t last_update_timestamp;
    uint64_t last_frame_time;

    radar_target_t raw_targets[LD2450_MAX_TARGETS];
    radar_spatial_target_t filtered_targets[LD2450_MAX_TARGETS];
    radar_track_snapshot_t history[RADAR_TRACKER_HISTORY_MAX_TARGETS];
    uint8_t history_count;

    radar_point_mm_t mount_position;
    radar_point_mm_t offset;
    int16_t rotation;
    radar_installation_config_t coordinate_config;

    radar_spatial_state_t spatial_storage;
    radar_spatial_state_t *spatial_state;
    radar_target_tracker_t *tracker_state;
    radar_person_continuity_t *person_state;
    radar_spatial_diagnostics_t diagnostics_state;
    radar_spatial_snapshot_t snapshot;
    radar_count_summary_t count_summary;
    RadarSourceState source_state;
    uint32_t sequence;
    uint32_t frame_sequence;
} RadarSourceContext;

typedef RadarSourceContext radar_source_context_t;

/* A lock-protected value view for asynchronous diagnostics/logging.  It
 * deliberately contains no mutable tracker/person pointers. */
typedef struct {
    radar_source_id_t source_id;
    char source_name[RADAR_SOURCE_CONTEXT_NAME_LEN];
    char device_id[RADAR_SOURCE_CONTEXT_DEVICE_ID_LEN];
    char room_id[RADAR_SOURCE_CONTEXT_ROOM_ID_LEN];
    uint32_t sequence;
    uint32_t frame_sequence;
    radar_spatial_snapshot_t snapshot;
} radar_source_context_log_view_t;

bool radar_source_context_init(uint64_t now_ms);
bool radar_source_context_is_valid(radar_source_id_t source);
RadarSourceContext *radar_source_context_mutable(radar_source_id_t source);
const RadarSourceContext *radar_source_context_get(radar_source_id_t source);
const char *radar_source_context_source_name(radar_source_id_t source);
const char *radar_source_context_device_id(radar_source_id_t source);
const char *radar_source_context_room_id(radar_source_id_t source);
radar_transport_type_t radar_source_context_transport(radar_source_id_t source);
const char *radar_source_context_transport_name(radar_transport_type_t transport);
bool radar_source_context_get_state(radar_source_id_t source, RadarSourceState *out);
bool radar_source_context_get_log_view(radar_source_id_t source,
                                       radar_source_context_log_view_t *out);

void radar_source_context_reset(RadarSourceContext *context, uint64_t now_ms);
void radar_source_context_publish(RadarSourceContext *context,
                                  const radar_spatial_snapshot_t *snapshot,
                                  const radar_count_summary_t *count_summary,
                                  bool online,
                                  uint32_t sequence,
                                  uint64_t frame_time_ms);
void radar_source_context_commit_state(RadarSourceContext *context,
                                       radar_presence_state_t presence_state,
                                       bool online,
                                       uint32_t sequence,
                                       uint64_t update_timestamp_ms,
                                       const radar_count_summary_t *count_summary);

#ifdef __cplusplus
}
#endif

#endif

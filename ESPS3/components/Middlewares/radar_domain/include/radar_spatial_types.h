#ifndef RADAR_SPATIAL_TYPES_H
#define RADAR_SPATIAL_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#include "ld2450_parser.h"
#include "ld2450_types.h"
#include "ld2450_uart_diagnostics.h"
#include "radar_uart_recovery.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RADAR_ZONE_MAP_MAX_ZONES 8U
#define RADAR_TRACKER_MAX_TRACKS LD2450_MAX_TARGETS
#define RADAR_TRACKER_HISTORY_MAX_TARGETS 8U

typedef enum {
    RADAR_SENSOR_OFFLINE = 0,
    RADAR_SENSOR_STALE,
    RADAR_SENSOR_VALID,
} radar_sensor_state_t;

typedef enum {
    RADAR_OCCUPANCY_UNKNOWN = 0,
    RADAR_OCCUPANCY_PRESENT,
    RADAR_OCCUPANCY_HOLD,
    RADAR_OCCUPANCY_VACANT_INFERRED,
} radar_occupancy_state_t;

typedef enum {
    RADAR_MOTION_UNKNOWN = 0,
    RADAR_MOTION_STILL_CANDIDATE,
    RADAR_MOTION_MOVING,
    RADAR_MOTION_NONE,
} radar_motion_state_t;

typedef enum {
    RADAR_TRACK_EMPTY = 0,
    RADAR_TRACK_TENTATIVE,
    RADAR_TRACK_CONFIRMED,
    RADAR_TRACK_HOLD,
} radar_track_lifecycle_t;

typedef enum {
    RADAR_ZONE_NONE = 0,
    RADAR_ZONE_ACTIVE,
    RADAR_ZONE_ENTRY,
    RADAR_ZONE_IGNORE,
} radar_zone_type_t;

typedef struct {
    int32_t min_x_mm;
    int32_t max_x_mm;
    int32_t min_y_mm;
    int32_t max_y_mm;
} radar_rect_t;

typedef struct {
    uint8_t zone_id;
    radar_zone_type_t type;
    bool enabled;
    radar_rect_t rect;
    uint32_t hysteresis_mm;
} radar_zone_definition_t;

/* 唯一的安装坐标契约；默认值保持 LD2450 原始坐标轴。 */
typedef struct {
    bool flip_x;
    bool flip_y;
    int16_t rotation_deg;
    int32_t origin_offset_x_mm;
    int32_t origin_offset_y_mm;
    uint32_t max_detection_distance_mm;
    radar_rect_t room_bounds;
    uint8_t zone_count;
    radar_zone_definition_t zones[RADAR_ZONE_MAP_MAX_ZONES];
} radar_installation_config_t;

typedef struct {
    uint32_t sensor_stale_ms;
    uint32_t association_gate_mm;
    uint32_t association_gate_min_mm;
    uint32_t association_gate_max_mm;
    uint32_t track_timeout_ms;
    uint32_t track_confirm_frames;
    uint32_t track_hold_ms;
    uint32_t motion_speed_cm_s;
    uint32_t motion_displacement_mm;
    uint32_t still_candidate_frames;
    uint32_t hold_decay_ms;
    uint32_t hold_confidence_loss_per_s;
    uint32_t confidence_initial;
    uint32_t confidence_gain_per_frame;
    uint32_t confidence_loss_per_miss;
    uint32_t track_confidence_min;
    uint32_t max_velocity_mm_s;
    uint32_t ema_alpha_percent;
    uint32_t motion_enter_frames;
    uint32_t motion_exit_frames;
    uint32_t target_jump_max_mm;
} radar_spatial_thresholds_t;

typedef struct {
    radar_installation_config_t installation;
    radar_spatial_thresholds_t thresholds;
} radar_spatial_config_t;

typedef struct {
    bool valid;
    int32_t x_mm;
    int32_t y_mm;
    int16_t speed_cm_s;
    uint16_t resolution_mm;
    uint32_t distance_mm;
    int16_t angle_deg;
    uint8_t zone_id;
    radar_zone_type_t zone_type;
} radar_spatial_target_t;

typedef struct {
    bool active;
    bool visible;
    radar_track_lifecycle_t lifecycle;
    uint32_t track_id;
    int32_t raw_x_mm;
    int32_t raw_y_mm;
    int32_t filtered_x_mm;
    int32_t filtered_y_mm;
    /* x/y remain filtered compatibility aliases for internal consumers. */
    int32_t x_mm;
    int32_t y_mm;
    int16_t speed_cm_s;
    uint16_t resolution_mm;
    int16_t direction_deg;
    int32_t velocity_x_mm_s;
    int32_t velocity_y_mm_s;
    uint32_t distance_mm;
    int16_t angle_deg;
    uint64_t first_seen_ms;
    uint64_t last_seen_ms;
    uint32_t consecutive_seen;
    uint32_t missed_frames;
    uint8_t zone_id;
    uint8_t previous_zone_id;
    bool zone_changed;
    bool zone_left;
    uint64_t zone_entered_ms;
    uint64_t dwell_ms;
    uint32_t confidence;
    uint32_t last_displacement_mm;
    uint32_t last_match_distance_mm;
    uint8_t consecutive_velocity_outliers;
    bool history_recorded;
} radar_track_snapshot_t;

typedef struct {
    uint32_t association_count;
    uint32_t new_track_count;
    uint32_t released_track_count;
    uint32_t dropped_target_count;
    uint32_t zone_switch_count;
    uint32_t coordinate_outliers;
    uint32_t jump_outliers;
    uint32_t velocity_outliers;
    uint32_t deleted_track_count;
    uint32_t active_track_count;
    uint32_t stale_track_count;
} radar_tracker_diagnostics_t;

typedef struct {
    ld2450_parser_diagnostics_t parser;
    ld2450_uart_diagnostics_t uart;
    radar_uart_recovery_t recovery;
    radar_tracker_diagnostics_t tracker;
} radar_spatial_diagnostics_t;

typedef struct {
    uint64_t captured_at_ms;
    uint64_t latest_frame_ms;
    uint32_t frame_age_ms;
    radar_sensor_state_t sensor_state;
    radar_occupancy_state_t occupancy_state;
    radar_motion_state_t motion_state;
    uint32_t occupancy_confidence;
    uint8_t raw_target_count;
    uint8_t accepted_target_count;
    uint8_t active_track_count;
    uint8_t history_target_count;
    uint8_t dominant_zone_id;
    radar_target_t raw_targets[LD2450_MAX_TARGETS];
    radar_spatial_target_t accepted_targets[LD2450_MAX_TARGETS];
    radar_track_snapshot_t tracks[RADAR_TRACKER_MAX_TRACKS];
    /* Current targets exclude TENTATIVE and HOLD tracks. */
    radar_track_snapshot_t current_targets[RADAR_TRACKER_MAX_TRACKS];
    /* Stale tracks are retained independently for diagnostics and replay only. */
    radar_track_snapshot_t history_targets[RADAR_TRACKER_HISTORY_MAX_TARGETS];
    radar_spatial_diagnostics_t diagnostics;
} radar_spatial_snapshot_t;

/* Public, zone-free copy for local UI and future read-only consumers. */
typedef struct {
    uint32_t track_id;
    int32_t raw_x_mm;
    int32_t raw_y_mm;
    int32_t filtered_x_mm;
    int32_t filtered_y_mm;
    uint32_t distance_mm;
    int16_t angle_deg;
    int16_t speed_cm_s;
    uint32_t confidence;
    bool visible;
    uint64_t timestamp_ms;
} radar_readonly_track_t;

typedef struct {
    uint64_t timestamp_ms;
    uint64_t latest_frame_ms;
    uint32_t frame_age_ms;
    radar_sensor_state_t sensor_state;
    radar_occupancy_state_t occupancy_state;
    radar_motion_state_t motion_state;
    uint8_t track_count;
    radar_readonly_track_t tracks[RADAR_TRACKER_MAX_TRACKS];
} radar_readonly_snapshot_t;

#ifdef __cplusplus
}
#endif

#endif

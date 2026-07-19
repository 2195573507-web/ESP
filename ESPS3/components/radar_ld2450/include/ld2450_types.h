#ifndef LD2450_TYPES_H
#define LD2450_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LD2450_FRAME_SIZE 30U
#define LD2450_HEADER_SIZE 4U
#define LD2450_TARGET_SIZE 8U
#define LD2450_MAX_TARGETS 3U

typedef struct {
    bool valid;
    int16_t x_mm;
    int16_t y_mm;
    int16_t speed_cm_s;
    uint16_t resolution_mm;
    uint32_t distance_mm;
    uint8_t confidence;
} radar_target_t;

typedef struct {
    uint32_t frame_seq;
    uint64_t received_at_ms;
    uint8_t target_count;
    radar_target_t targets[LD2450_MAX_TARGETS];
} radar_frame_t;

typedef enum {
    RADAR_STATE_UNKNOWN = 0,
    RADAR_STATE_VACANT_INFERRED = 1,
    RADAR_STATE_HOLD = 2,
    RADAR_STATE_MOTION = 3,
    /* S3 本地兼容状态：确认有人，但不据此断言正在移动。 */
    RADAR_STATE_PRESENT = 4,
} radar_presence_state_t;

typedef struct {
    radar_presence_state_t state;
    uint8_t current_target_count;
    uint32_t state_seq;
    uint64_t state_since_ms;
    uint64_t last_valid_frame_ms;
    uint64_t last_motion_ms;
    bool uart_online;
    bool frame_fresh;
    radar_target_t targets[LD2450_MAX_TARGETS];
} radar_snapshot_t;

#ifdef __cplusplus
}
#endif

#endif

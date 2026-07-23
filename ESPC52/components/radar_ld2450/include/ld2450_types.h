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
    uint8_t slot;
    int16_t x_mm;
    int16_t y_mm;
    int16_t speed_cm_s;
    uint16_t resolution_mm;
    uint32_t distance_mm;
    /* Reserved wire field. C5 forwards no inferred confidence, so this remains zero. */
    uint8_t confidence;
} radar_target_t;

typedef struct {
    uint32_t frame_seq;
    uint64_t received_at_ms;
    uint8_t target_count;
    radar_target_t targets[LD2450_MAX_TARGETS];
} radar_frame_t;

#ifdef __cplusplus
}
#endif

#endif

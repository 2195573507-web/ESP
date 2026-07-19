#ifndef RADAR_TARGET_SAMPLE_H
#define RADAR_TARGET_SAMPLE_H

#include <stdbool.h>
#include <stdint.h>

#include "ld2450_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* C5 -> S3 boundary: only parsed, bounded target data crosses this type. */
typedef struct {
    uint8_t local_id;
    uint8_t link_state;
    bool sample_valid;
    uint32_t frame_seq;
    uint32_t frame_uptime_ms;
    uint8_t target_count;
    radar_target_t targets[LD2450_MAX_TARGETS];
} radar_target_sample_t;

#ifdef __cplusplus
}
#endif

#endif

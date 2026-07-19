#ifndef RADAR_STATE_CODEC_H
#define RADAR_STATE_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "ld2450_types.h"
#include "radar_target_sample.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RADAR_RESULT_JSON_MAX_BYTES 1024U

int radar_result_encode_json(const radar_target_sample_t *sample,
                             uint32_t request_uptime_ms,
                             uint32_t request_sequence,
                             char *out,
                             size_t out_size);

#ifdef __cplusplus
}
#endif

#endif

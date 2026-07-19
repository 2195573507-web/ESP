#ifndef RADAR_STATE_CODEC_H
#define RADAR_STATE_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "ld2450_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RADAR_STATE_JSON_MAX_BYTES 768U

int radar_state_encode_json(const radar_snapshot_t *snapshot,
                            uint8_t local_id,
                            uint32_t sequence,
                            uint64_t uptime_ms,
                            char *out,
                            size_t out_size);

#ifdef __cplusplus
}
#endif

#endif


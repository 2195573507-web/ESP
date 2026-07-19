#ifndef RADAR_STATE_CLIENT_H
#define RADAR_STATE_CLIENT_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t accepted_count;
    uint32_t failed_count;
    uint32_t coalesced_count;
    uint32_t encode_error_count;
    uint32_t last_body_bytes;
} radar_state_client_stats_t;

esp_err_t radar_state_client_start(void);
void radar_state_client_get_stats(radar_state_client_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif


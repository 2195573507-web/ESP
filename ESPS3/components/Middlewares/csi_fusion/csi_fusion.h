#ifndef CSI_FUSION_H
#define CSI_FUSION_H

/**
 * @file csi_fusion.h
 * @brief ESPS3 CSI feature fusion and state machine.
 *
 * S3 owns CSI motion_score and IDLE/MOTION/HOLD state decisions. Inputs are C5
 * feature frames only; raw CSI and subcarrier payloads are rejected before this
 * module is called.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CSI_FUSION_TEXT_LEN 32U

typedef enum {
    CSI_FUSION_STATE_IDLE = 0,
    CSI_FUSION_STATE_MOTION = 1,
    CSI_FUSION_STATE_HOLD = 2,
} csi_fusion_state_t;

typedef struct {
    char device_id[CSI_FUSION_TEXT_LEN];
    char link_id[CSI_FUSION_TEXT_LEN];
    float frame_energy;
    float variance;
    float quality;
    int rssi;
    uint32_t frame_seq;
    uint64_t timestamp_ms;
} csi_fusion_feature_t;

typedef struct {
    bool valid;
    char device_id[CSI_FUSION_TEXT_LEN];
    char link_id[CSI_FUSION_TEXT_LEN];
    csi_fusion_state_t state;
    float frame_energy;
    float variance;
    int rssi;
    float motion_score;
    uint64_t timestamp_ms;
    uint8_t active_link_count;
} csi_fusion_fact_t;

void csi_fusion_init(void);

esp_err_t csi_fusion_update(const csi_fusion_feature_t *feature,
                            csi_fusion_fact_t *out_fact);

const char *csi_fusion_state_to_string(csi_fusion_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* CSI_FUSION_H */

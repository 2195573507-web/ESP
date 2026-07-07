#ifndef CSI_FUSION_H
#define CSI_FUSION_H

/**
 * @file csi_fusion.h
 * @brief ESPS3 CSI tick-aligned canonical event fusion and state machine.
 *
 * S3 owns only time alignment, link pairing, IDLE/MOTION/HOLD state decisions,
 * and CanonicalEvent v2 generation. Inputs are C5-provided state/confidence
 * observations; raw CSI, subcarrier, energy, variance, and CV math stay out of
 * this module.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CSI_FUSION_TEXT_LEN 32U
#define CSI_FUSION_TRACE_ID_LEN 48U
#define CSI_FUSION_SCHEMA_VERSION 2U
#define CSI_FUSION_LINK_COUNT 2U

#ifndef CSI_FUSION_TICK_MS
#define CSI_FUSION_TICK_MS 100U
#endif

typedef enum {
    CSI_FUSION_STATE_IDLE = 0,
    CSI_FUSION_STATE_MOTION = 1,
    CSI_FUSION_STATE_HOLD = 2,
} csi_fusion_state_t;

typedef struct {
    char device_id[CSI_FUSION_TEXT_LEN];
    char link_id[CSI_FUSION_TEXT_LEN];
    char trace_id[CSI_FUSION_TRACE_ID_LEN];
    bool has_state;
    csi_fusion_state_t state;
    float confidence;
    float quality;
    int rssi;
    uint32_t frame_seq;
    uint64_t tick_id;
    uint64_t timestamp_ms;
} csi_fusion_feature_t;

typedef struct {
    bool valid;
    char device_id[CSI_FUSION_TEXT_LEN];
    char link_id[CSI_FUSION_TEXT_LEN];
    char trace_id[CSI_FUSION_TRACE_ID_LEN];
    bool has_state;
    csi_fusion_state_t state;
    float confidence;
    float quality;
    int rssi;
    uint32_t frame_seq;
    uint64_t tick_id;
    uint64_t timestamp_ms;
} csi_fusion_link_state_t;

typedef struct {
    bool valid;
    uint8_t schema_version;
    char trace_id[CSI_FUSION_TRACE_ID_LEN];
    uint64_t tick_id;
    char links[CSI_FUSION_LINK_COUNT][CSI_FUSION_TEXT_LEN];
    csi_fusion_state_t fused_state;
    float confidence;
    uint64_t timestamp_ms;
    uint8_t active_link_count;
} csi_fusion_canonical_event_t;

typedef csi_fusion_canonical_event_t csi_fusion_telemetry_t;
typedef csi_fusion_canonical_event_t csi_fusion_fact_t;

void csi_fusion_init(void);

esp_err_t csi_fusion_update(const csi_fusion_feature_t *feature,
                            csi_fusion_fact_t *out_fact,
                            csi_fusion_telemetry_t *out_telemetry);

esp_err_t csi_fusion_flush(csi_fusion_fact_t *out_fact,
                           csi_fusion_telemetry_t *out_telemetry);

esp_err_t csi_fusion_format_telemetry_json(const csi_fusion_telemetry_t *telemetry,
                                           char *out,
                                           size_t out_size);

const char *csi_fusion_state_to_string(csi_fusion_state_t state);

bool csi_fusion_state_from_string(const char *value, csi_fusion_state_t *out_state);

const char *csi_fusion_link_state_name(size_t index);

#ifdef __cplusplus
}
#endif

#endif /* CSI_FUSION_H */

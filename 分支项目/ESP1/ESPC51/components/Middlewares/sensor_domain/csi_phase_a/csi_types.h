#ifndef CSI_TYPES_H
#define CSI_TYPES_H

/**
 * @file csi_types.h
 * @brief C5 CSI edge feature data structures.
 *
 * C5 owns raw CSI only inside local callback/feature extraction. Public output from
 * this module is a low-dimensional feature frame for S3; it does not contain raw
 * CSI, I/Q arrays, phase arrays, or subcarrier-level payloads.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "envelope_builder.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CSI_PHASE_A_MAX_RAW_SUBCARRIERS 64U
#define CSI_PHASE_A_MAX_SELECTED_SUBCARRIERS 40U
#define CSI_PHASE_A_MIN_SELECTED_SUBCARRIERS 20U
#define CSI_PHASE_A_MAX_CALIBRATION_ENERGY_SAMPLES 96U
#define CSI_FEATURE_LINK_ID_MAX_LEN 16U

typedef enum {
    CSI_PROCESSOR_STATE_INIT = 0,
    CSI_PROCESSOR_STATE_CALIBRATION = 1,
    CSI_PROCESSOR_STATE_RUN = 2,
} csi_processor_state_t;

typedef enum {
    CSI_SAMPLE_QUALITY_INVALID = 0,
    CSI_SAMPLE_QUALITY_CALIBRATING = 1,
    CSI_SAMPLE_QUALITY_WEAK = 2,
    CSI_SAMPLE_QUALITY_GOOD = 3,
} csi_sample_quality_t;

typedef struct {
    int16_t i;
    int16_t q;
} csi_iq_sample_t;

typedef struct {
    uint64_t timestamp_ms;
    int8_t rssi;
    uint8_t subcarrier_count;
    uint16_t amplitude[CSI_PHASE_A_MAX_RAW_SUBCARRIERS];
} csi_frame_sample_t;

typedef struct {
    float frame_energy;
    float variance;
    float cv;
    int rssi;
    float quality;
} csi_feature_metrics_t;

typedef struct {
    char link_id[CSI_FEATURE_LINK_ID_MAX_LEN];
    uint64_t timestamp_ms;
    csi_feature_metrics_t metrics;
    envelope_state_hint_t state_hint;
    float motion_score;
    float confidence;
    csi_sample_quality_t quality_state;
} csi_feature_frame_t;

#ifdef __cplusplus
}
#endif

#endif /* CSI_TYPES_H */

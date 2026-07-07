#ifndef CSI_FEATURE_H
#define CSI_FEATURE_H

/**
 * @file csi_feature.h
 * @brief C5 calibration and low-dimensional CSI feature extraction.
 *
 * Active output is csi_feature_frame_t only. The processor keeps bounded
 * calibration aggregates and EWMA state, never raw CSI history or uploadable
 * subcarrier arrays.
 */

#include "csi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t calibration_duration_ms;
    uint32_t calibration_converged_ms;
    uint16_t min_calibration_samples;
    float calibration_variance_epsilon;
    float ewma_alpha;
    uint8_t guard_subcarriers;
    uint8_t min_selected_subcarriers;
    uint8_t max_selected_subcarriers;
    int8_t min_rssi;
} csi_feature_config_t;

typedef struct {
    csi_processor_state_t state;
    csi_feature_config_t config;
    uint64_t calibration_started_ms;
    uint16_t calibration_samples;
    uint8_t observed_subcarrier_count;
    uint8_t selected_count;
    uint16_t energy_sample_count;
    uint32_t baseline_sum[CSI_PHASE_A_MAX_RAW_SUBCARRIERS];
    uint64_t variance_sum[CSI_PHASE_A_MAX_RAW_SUBCARRIERS];
    uint64_t variance_sum_sq[CSI_PHASE_A_MAX_RAW_SUBCARRIERS];
    float calibration_last_variance[CSI_PHASE_A_MAX_RAW_SUBCARRIERS];
    uint64_t calibration_stable_since_ms;
    float baseline[CSI_PHASE_A_MAX_RAW_SUBCARRIERS];
    float previous_clean[CSI_PHASE_A_MAX_RAW_SUBCARRIERS];
    float smoothed_delta[CSI_PHASE_A_MAX_RAW_SUBCARRIERS];
    uint8_t selected_indices[CSI_PHASE_A_MAX_SELECTED_SUBCARRIERS];
    float energy_samples[CSI_PHASE_A_MAX_CALIBRATION_ENERGY_SAMPLES];
    float baseline_energy;
    float energy_sigma;
    float noise_floor;
    float delta_noise_estimate;
    uint64_t last_timestamp_ms;
    bool has_previous_clean;
} csi_feature_processor_t;

void csi_feature_default_config(csi_feature_config_t *config);

void csi_feature_processor_init(csi_feature_processor_t *processor,
                                const csi_feature_config_t *config);

uint16_t csi_feature_amplitude_from_iq(int16_t i, int16_t q);

bool csi_feature_processor_push(csi_feature_processor_t *processor,
                                const csi_frame_sample_t *frame,
                                csi_feature_frame_t *out_feature);

bool csi_feature_processor_ready(const csi_feature_processor_t *processor);

uint8_t csi_feature_processor_selected_count(const csi_feature_processor_t *processor);

const char *csi_feature_quality_to_string(csi_sample_quality_t quality);

#ifdef __cplusplus
}
#endif

#endif /* CSI_FEATURE_H */

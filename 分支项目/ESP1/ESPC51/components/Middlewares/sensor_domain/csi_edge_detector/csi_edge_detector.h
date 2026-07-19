#ifndef CSI_EDGE_DETECTOR_H
#define CSI_EDGE_DETECTOR_H

/**
 * @file csi_edge_detector.h
 * @brief C5 CSI edge motion detector.
 *
 * The detector consumes low-dimensional csi_feature_frame_t values. It keeps a
 * bounded sliding window and emits a local IDLE/MOTION hint, motion_score, and
 * confidence without storing raw CSI, I/Q samples, phase, or subcarrier payloads.
 */

#include <stdbool.h>
#include <stdint.h>

#include "csi_feature.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CSI_EDGE_DETECTOR_DEFAULT_WINDOW_SIZE 32U
#define CSI_EDGE_DETECTOR_MAX_WINDOW_SIZE 32U
#define CSI_EDGE_DETECTOR_DEFAULT_EMA_ALPHA 0.25f
#define CSI_EDGE_DETECTOR_DEFAULT_MOTION_ENTER 0.55f
#define CSI_EDGE_DETECTOR_DEFAULT_MOTION_EXIT 0.35f
#define CSI_EDGE_DETECTOR_DEFAULT_MOTION_ENTER_DURATION_MS 300U
#define CSI_EDGE_DETECTOR_DEFAULT_MOTION_EXIT_DURATION_MS 500U

typedef enum {
    CSI_EDGE_STATE_IDLE = 0,
    CSI_EDGE_STATE_MOTION = 1,
} csi_edge_state_t;

typedef struct {
    uint8_t window_size;
    float ema_alpha;
    float motion_enter_threshold;
    float motion_exit_threshold;
    uint32_t motion_enter_duration_ms;
    uint32_t motion_exit_duration_ms;
} csi_edge_detector_config_t;

typedef struct {
    csi_edge_state_t local_state_hint;
    uint64_t timestamp_ms;
    float motion_score;
    float confidence;
    uint8_t window_count;
    float window_energy_mean;
    float window_variance_mean;
    float window_cv_mean;
} csi_edge_detection_t;

typedef struct {
    uint64_t timestamp_ms;
    float energy;
    float variance;
    float cv;
    float quality;
    float activity_score;
} csi_edge_window_sample_t;

typedef struct {
    csi_edge_detector_config_t config;
    csi_edge_window_sample_t window[CSI_EDGE_DETECTOR_MAX_WINDOW_SIZE];
    uint8_t write_index;
    uint8_t count;
    float smoothed_motion_score;
    csi_edge_state_t state;
    uint64_t state_since_ms;
    uint64_t motion_enter_since_ms;
    uint64_t motion_exit_since_ms;
    bool has_smoothed_score;
    bool motion_enter_pending;
    bool motion_exit_pending;
} csi_edge_detector_t;

void csi_edge_detector_default_config(csi_edge_detector_config_t *config);

void csi_edge_detector_init(csi_edge_detector_t *detector,
                            const csi_edge_detector_config_t *config);

bool csi_edge_detector_push(csi_edge_detector_t *detector,
                            const csi_feature_frame_t *feature,
                            csi_edge_detection_t *out_detection);

const char *csi_edge_state_to_string(csi_edge_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* CSI_EDGE_DETECTOR_H */

/**
 * @file csi_edge_detector.c
 * @brief C5 CSI edge motion detector implementation.
 */

#include "csi_edge_detector.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define CSI_EDGE_VARIANCE_IDLE 80.0f
#define CSI_EDGE_VARIANCE_MOTION 260.0f
#define CSI_EDGE_CV_IDLE 0.08f
#define CSI_EDGE_CV_MOTION 0.22f

static float csi_edge_clamp01(float value)
{
    if (!isfinite(value) || value <= 0.0f) {
        return 0.0f;
    }
    if (value >= 1.0f) {
        return 1.0f;
    }
    return value;
}

static float csi_edge_finite_or_zero(float value)
{
    return isfinite(value) ? value : 0.0f;
}

static float csi_edge_normalize(float value, float low, float high)
{
    if (high <= low) {
        return 0.0f;
    }
    return csi_edge_clamp01((value - low) / (high - low));
}

static uint8_t csi_edge_sanitize_window_size(uint8_t window_size)
{
    if (window_size == 0U) {
        return CSI_EDGE_DETECTOR_DEFAULT_WINDOW_SIZE;
    }
    if (window_size > CSI_EDGE_DETECTOR_MAX_WINDOW_SIZE) {
        return CSI_EDGE_DETECTOR_MAX_WINDOW_SIZE;
    }
    return window_size;
}

static float csi_edge_sanitize_alpha(float alpha)
{
    if (!isfinite(alpha) || alpha <= 0.0f || alpha > 1.0f) {
        return CSI_EDGE_DETECTOR_DEFAULT_EMA_ALPHA;
    }
    return alpha;
}

static void csi_edge_window_means(const csi_edge_detector_t *detector,
                                  float *out_energy_mean,
                                  float *out_variance_mean,
                                  float *out_cv_mean)
{
    if (out_energy_mean == NULL || out_variance_mean == NULL || out_cv_mean == NULL) {
        return;
    }
    *out_energy_mean = 0.0f;
    *out_variance_mean = 0.0f;
    *out_cv_mean = 0.0f;
    if (detector == NULL || detector->count == 0U) {
        return;
    }

    float energy_sum = 0.0f;
    float variance_sum = 0.0f;
    float cv_sum = 0.0f;
    for (uint8_t i = 0; i < detector->count; ++i) {
        energy_sum += detector->window[i].energy;
        variance_sum += detector->window[i].variance;
        cv_sum += detector->window[i].cv;
    }

    float count = (float)detector->count;
    *out_energy_mean = energy_sum / count;
    *out_variance_mean = variance_sum / count;
    *out_cv_mean = cv_sum / count;
}

static float csi_edge_energy_activity(const csi_edge_detector_t *detector, float energy)
{
    float energy_mean = 0.0f;
    float variance_mean = 0.0f;
    float cv_mean = 0.0f;
    csi_edge_window_means(detector, &energy_mean, &variance_mean, &cv_mean);
    (void)variance_mean;
    (void)cv_mean;

    if (detector == NULL || detector->count < 4U) {
        return 0.0f;
    }

    float denominator = fabsf(energy_mean) > 1.0f ? fabsf(energy_mean) : 1.0f;
    float relative_delta = fabsf(energy - energy_mean) / denominator;
    return csi_edge_clamp01(relative_delta * 4.0f);
}

static float csi_edge_activity_score(const csi_edge_detector_t *detector,
                                     const csi_feature_frame_t *feature)
{
    float energy = csi_edge_finite_or_zero(feature->metrics.frame_energy);
    float variance = csi_edge_finite_or_zero(feature->metrics.variance);
    float cv = csi_edge_finite_or_zero(feature->metrics.cv);

    float energy_score = csi_edge_energy_activity(detector, energy);
    float variance_score =
        csi_edge_normalize(variance, CSI_EDGE_VARIANCE_IDLE, CSI_EDGE_VARIANCE_MOTION);
    float cv_score = csi_edge_normalize(cv, CSI_EDGE_CV_IDLE, CSI_EDGE_CV_MOTION);

    return csi_edge_clamp01((variance_score * 0.45f) +
                            (cv_score * 0.35f) +
                            (energy_score * 0.20f));
}

static void csi_edge_window_push(csi_edge_detector_t *detector,
                                 const csi_feature_frame_t *feature,
                                 float activity_score)
{
    if (detector == NULL || feature == NULL) {
        return;
    }

    csi_edge_window_sample_t *slot = &detector->window[detector->write_index];
    slot->timestamp_ms = feature->timestamp_ms;
    slot->energy = csi_edge_finite_or_zero(feature->metrics.frame_energy);
    slot->variance = csi_edge_finite_or_zero(feature->metrics.variance);
    slot->cv = csi_edge_finite_or_zero(feature->metrics.cv);
    slot->quality = csi_edge_clamp01(feature->metrics.quality);
    slot->activity_score = activity_score;

    detector->write_index = (uint8_t)((detector->write_index + 1U) %
                                      detector->config.window_size);
    if (detector->count < detector->config.window_size) {
        detector->count++;
    }
}

static void csi_edge_update_state(csi_edge_detector_t *detector,
                                  uint64_t timestamp_ms,
                                  float motion_score,
                                  float quality)
{
    if (detector == NULL) {
        return;
    }

    if (detector->state == CSI_EDGE_STATE_IDLE) {
        detector->motion_exit_pending = false;
        bool enter_candidate =
            motion_score >= detector->config.motion_enter_threshold && quality > 0.0f;
        if (!enter_candidate) {
            detector->motion_enter_pending = false;
            return;
        }
        if (!detector->motion_enter_pending) {
            detector->motion_enter_pending = true;
            detector->motion_enter_since_ms = timestamp_ms;
        }
        uint64_t enter_elapsed_ms =
            timestamp_ms >= detector->motion_enter_since_ms
                ? timestamp_ms - detector->motion_enter_since_ms
                : 0U;
        if (enter_elapsed_ms >= detector->config.motion_enter_duration_ms) {
            detector->state = CSI_EDGE_STATE_MOTION;
            detector->state_since_ms = timestamp_ms;
            detector->motion_enter_pending = false;
        }
        return;
    }

    detector->motion_enter_pending = false;
    bool exit_candidate =
        motion_score <= detector->config.motion_exit_threshold || quality <= 0.0f;
    if (!exit_candidate) {
        detector->motion_exit_pending = false;
        return;
    }
    if (!detector->motion_exit_pending) {
        detector->motion_exit_pending = true;
        detector->motion_exit_since_ms = timestamp_ms;
    }
    uint64_t exit_elapsed_ms =
        timestamp_ms >= detector->motion_exit_since_ms
            ? timestamp_ms - detector->motion_exit_since_ms
            : 0U;
    if (exit_elapsed_ms >= detector->config.motion_exit_duration_ms) {
        detector->state = CSI_EDGE_STATE_IDLE;
        detector->state_since_ms = timestamp_ms;
        detector->motion_exit_pending = false;
    }
}

void csi_edge_detector_default_config(csi_edge_detector_config_t *config)
{
    if (config == NULL) {
        return;
    }
    config->window_size = CSI_EDGE_DETECTOR_DEFAULT_WINDOW_SIZE;
    config->ema_alpha = CSI_EDGE_DETECTOR_DEFAULT_EMA_ALPHA;
    config->motion_enter_threshold = CSI_EDGE_DETECTOR_DEFAULT_MOTION_ENTER;
    config->motion_exit_threshold = CSI_EDGE_DETECTOR_DEFAULT_MOTION_EXIT;
    config->motion_enter_duration_ms = CSI_EDGE_DETECTOR_DEFAULT_MOTION_ENTER_DURATION_MS;
    config->motion_exit_duration_ms = CSI_EDGE_DETECTOR_DEFAULT_MOTION_EXIT_DURATION_MS;
}

void csi_edge_detector_init(csi_edge_detector_t *detector,
                            const csi_edge_detector_config_t *config)
{
    if (detector == NULL) {
        return;
    }

    csi_edge_detector_config_t default_config;
    csi_edge_detector_default_config(&default_config);
    memset(detector, 0, sizeof(*detector));
    detector->config = config != NULL ? *config : default_config;
    detector->config.window_size =
        csi_edge_sanitize_window_size(detector->config.window_size);
    detector->config.ema_alpha = csi_edge_sanitize_alpha(detector->config.ema_alpha);
    if (!isfinite(detector->config.motion_enter_threshold) ||
        detector->config.motion_enter_threshold <= 0.0f ||
        detector->config.motion_enter_threshold > 1.0f) {
        detector->config.motion_enter_threshold =
            CSI_EDGE_DETECTOR_DEFAULT_MOTION_ENTER;
    }
    if (!isfinite(detector->config.motion_exit_threshold) ||
        detector->config.motion_exit_threshold < 0.0f ||
        detector->config.motion_exit_threshold >= detector->config.motion_enter_threshold) {
        detector->config.motion_exit_threshold =
            CSI_EDGE_DETECTOR_DEFAULT_MOTION_EXIT;
    }
    if (detector->config.motion_enter_duration_ms <
        CSI_EDGE_DETECTOR_DEFAULT_MOTION_ENTER_DURATION_MS) {
        detector->config.motion_enter_duration_ms =
            CSI_EDGE_DETECTOR_DEFAULT_MOTION_ENTER_DURATION_MS;
    }
    if (detector->config.motion_exit_duration_ms <
        CSI_EDGE_DETECTOR_DEFAULT_MOTION_EXIT_DURATION_MS) {
        detector->config.motion_exit_duration_ms =
            CSI_EDGE_DETECTOR_DEFAULT_MOTION_EXIT_DURATION_MS;
    }
    detector->state = CSI_EDGE_STATE_IDLE;
}

bool csi_edge_detector_push(csi_edge_detector_t *detector,
                            const csi_feature_frame_t *feature,
                            csi_edge_detection_t *out_detection)
{
    if (detector == NULL || feature == NULL || out_detection == NULL) {
        return false;
    }

    float quality = csi_edge_clamp01(feature->metrics.quality);
    float activity_score = csi_edge_activity_score(detector, feature);
    if (!detector->has_smoothed_score) {
        detector->smoothed_motion_score = activity_score;
        detector->has_smoothed_score = true;
    } else {
        detector->smoothed_motion_score =
            (detector->config.ema_alpha * activity_score) +
            ((1.0f - detector->config.ema_alpha) * detector->smoothed_motion_score);
    }

    float motion_score = csi_edge_clamp01(detector->smoothed_motion_score);
    csi_edge_window_push(detector, feature, activity_score);
    csi_edge_update_state(detector, feature->timestamp_ms, motion_score, quality);

    float energy_mean = 0.0f;
    float variance_mean = 0.0f;
    float cv_mean = 0.0f;
    csi_edge_window_means(detector, &energy_mean, &variance_mean, &cv_mean);

    float window_readiness =
        0.50f + (0.50f * ((float)detector->count /
                          (float)detector->config.window_size));
    float state_separation = detector->state == CSI_EDGE_STATE_MOTION
                                 ? motion_score
                                 : 1.0f - motion_score;
    float confidence = csi_edge_clamp01(quality * window_readiness *
                                        (0.50f + (0.50f * csi_edge_clamp01(state_separation))));

    out_detection->local_state_hint = detector->state;
    out_detection->timestamp_ms = feature->timestamp_ms;
    out_detection->motion_score = motion_score;
    out_detection->confidence = confidence;
    out_detection->window_count = detector->count;
    out_detection->window_energy_mean = energy_mean;
    out_detection->window_variance_mean = variance_mean;
    out_detection->window_cv_mean = cv_mean;
    return true;
}

const char *csi_edge_state_to_string(csi_edge_state_t state)
{
    switch (state) {
    case CSI_EDGE_STATE_MOTION:
        return "MOTION";
    case CSI_EDGE_STATE_IDLE:
    default:
        return "IDLE";
    }
}

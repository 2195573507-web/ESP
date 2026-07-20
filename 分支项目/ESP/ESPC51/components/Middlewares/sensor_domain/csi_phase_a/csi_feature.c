/**
 * @file csi_feature.c
 * @brief C5 CSI calibration and feature extraction implementation.
 */

#include "csi_feature.h"

#include <string.h>

typedef struct {
    uint8_t index;
    float variance;
    float stability;
    float score;
} csi_subcarrier_variance_t;

static uint64_t csi_abs_i64(int64_t value)
{
    return (value < 0) ? (uint64_t)(-value) : (uint64_t)value;
}

static float csi_absf(float value)
{
    return value < 0.0f ? -value : value;
}

static uint32_t csi_isqrt_u64(uint64_t value)
{
    uint64_t bit = 1ULL << 62;
    uint64_t result = 0;

    while (bit > value) {
        bit >>= 2;
    }

    while (bit != 0U) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }

    return (uint32_t)result;
}

static float csi_sqrtf_approx(float value)
{
    if (value <= 0.0f) {
        return 0.0f;
    }

    float guess = (value > 1.0f) ? value : 1.0f;
    for (int i = 0; i < 6; ++i) {
        guess = 0.5f * (guess + value / guess);
    }
    return guess;
}

static void sort_float(float *values, size_t count)
{
    for (size_t i = 1; i < count; ++i) {
        float key = values[i];
        size_t j = i;
        while (j > 0U && values[j - 1U] > key) {
            values[j] = values[j - 1U];
            --j;
        }
        values[j] = key;
    }
}

static void sort_subcarrier_variance(csi_subcarrier_variance_t *values, size_t count)
{
    for (size_t i = 1; i < count; ++i) {
        csi_subcarrier_variance_t key = values[i];
        size_t j = i;
        while (j > 0U && values[j - 1U].variance > key.variance) {
            values[j] = values[j - 1U];
            --j;
        }
        values[j] = key;
    }
}

static void sort_subcarrier_score_desc(csi_subcarrier_variance_t *values, size_t count)
{
    for (size_t i = 1; i < count; ++i) {
        csi_subcarrier_variance_t key = values[i];
        size_t j = i;
        while (j > 0U && values[j - 1U].score < key.score) {
            values[j] = values[j - 1U];
            --j;
        }
        values[j] = key;
    }
}

static float clamp01f(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

static float csi_feature_cv(float frame_energy, float variance)
{
    return frame_energy > 0.001f ? csi_sqrtf_approx(variance) / frame_energy : 0.0f;
}

static envelope_state_hint_t csi_feature_state_hint(float variance,
                                                    float cv,
                                                    float quality)
{
    if (quality <= 0.0f) {
        return ENVELOPE_STATE_HINT_IDLE;
    }
    if (variance >= 260.0f || cv >= 0.22f) {
        return ENVELOPE_STATE_HINT_MOTION;
    }
    if (variance >= 80.0f || cv >= 0.08f) {
        return ENVELOPE_STATE_HINT_HOLD;
    }
    return ENVELOPE_STATE_HINT_IDLE;
}

static float median_float(float *values, size_t count)
{
    if (count == 0U) {
        return 0.0f;
    }

    sort_float(values, count);
    if ((count % 2U) != 0U) {
        return values[count / 2U];
    }
    return (values[(count / 2U) - 1U] + values[count / 2U]) * 0.5f;
}

static float frame_energy_for_count(const csi_frame_sample_t *frame, uint8_t count)
{
    if (frame == NULL || count == 0U || frame->subcarrier_count == 0U) {
        return 0.0f;
    }

    uint8_t capped = count;
    if (capped > frame->subcarrier_count) {
        capped = frame->subcarrier_count;
    }

    uint32_t sum = 0;
    for (uint8_t i = 0; i < capped; ++i) {
        sum += frame->amplitude[i];
    }
    return (float)sum / (float)capped;
}

static bool is_guard_or_dc(uint8_t index, uint8_t count, uint8_t guard)
{
    if (count == 0U) {
        return true;
    }
    if (index < guard || index + guard >= count) {
        return true;
    }
    uint8_t dc = (uint8_t)(count / 2U);
    return index == dc || index + 1U == dc || (dc > 0U && index == dc - 1U);
}

static float calibration_variance(const csi_feature_processor_t *processor, uint8_t index)
{
    if (processor == NULL || processor->calibration_samples == 0U) {
        return 0.0f;
    }

    float n = (float)processor->calibration_samples;
    float sum = (float)processor->variance_sum[index];
    float sum_sq = (float)processor->variance_sum_sq[index];
    float mean = sum / n;
    float variance = (sum_sq / n) - (mean * mean);
    return variance > 0.0f ? variance : 0.0f;
}

static bool calibration_variance_converged(csi_feature_processor_t *processor,
                                           uint64_t timestamp_ms)
{
    if (processor == NULL || processor->calibration_samples < 2U ||
        processor->observed_subcarrier_count == 0U) {
        return false;
    }

    float change_sum = 0.0f;
    uint8_t compared = 0U;
    uint8_t count = processor->observed_subcarrier_count;
    for (uint8_t i = 0; i < count; ++i) {
        if (is_guard_or_dc(i, count, processor->config.guard_subcarriers)) {
            continue;
        }
        float variance = calibration_variance(processor, i);
        change_sum += csi_absf(variance - processor->calibration_last_variance[i]);
        processor->calibration_last_variance[i] = variance;
        ++compared;
    }

    if (compared == 0U) {
        processor->calibration_stable_since_ms = 0U;
        return false;
    }

    float mean_change = change_sum / (float)compared;
    if (mean_change <= processor->config.calibration_variance_epsilon) {
        if (processor->calibration_stable_since_ms == 0U) {
            processor->calibration_stable_since_ms = timestamp_ms;
        }
        return timestamp_ms >= processor->calibration_stable_since_ms &&
               timestamp_ms - processor->calibration_stable_since_ms >=
                   processor->config.calibration_converged_ms;
    }

    processor->calibration_stable_since_ms = 0U;
    return false;
}

static void compute_energy_baseline(csi_feature_processor_t *processor)
{
    if (processor == NULL || processor->energy_sample_count == 0U) {
        return;
    }

    float sum = 0.0f;
    for (uint16_t i = 0; i < processor->energy_sample_count; ++i) {
        sum += processor->energy_samples[i];
    }
    processor->baseline_energy = sum / (float)processor->energy_sample_count;

    float squared = 0.0f;
    for (uint16_t i = 0; i < processor->energy_sample_count; ++i) {
        float diff = processor->energy_samples[i] - processor->baseline_energy;
        squared += diff * diff;
    }
    processor->energy_sigma = csi_sqrtf_approx(squared / (float)processor->energy_sample_count);

    float deviations[CSI_PHASE_A_MAX_CALIBRATION_ENERGY_SAMPLES] = {0};
    for (uint16_t i = 0; i < processor->energy_sample_count; ++i) {
        deviations[i] = csi_absf(processor->energy_samples[i] - processor->baseline_energy);
    }
    processor->noise_floor = median_float(deviations, processor->energy_sample_count);
}

static void select_subcarriers(csi_feature_processor_t *processor)
{
    if (processor == NULL || processor->observed_subcarrier_count == 0U) {
        return;
    }

    csi_subcarrier_variance_t candidates[CSI_PHASE_A_MAX_RAW_SUBCARRIERS] = {0};
    size_t candidate_count = 0;
    uint8_t count = processor->observed_subcarrier_count;

    for (uint8_t i = 0; i < count; ++i) {
        if (is_guard_or_dc(i, count, processor->config.guard_subcarriers)) {
            continue;
        }
        candidates[candidate_count].index = i;
        candidates[candidate_count].variance = calibration_variance(processor, i);
        candidates[candidate_count].stability = 0.0f;
        candidates[candidate_count].score = 0.0f;
        ++candidate_count;
    }

    if (candidate_count == 0U) {
        return;
    }

    sort_subcarrier_variance(candidates, candidate_count);
    size_t p30 = (candidate_count * 30U) / 100U;
    size_t p70 = (candidate_count * 70U) / 100U;
    if (p70 < p30) {
        p70 = p30;
    }

    uint8_t target = processor->config.max_selected_subcarriers;
    if (target > CSI_PHASE_A_MAX_SELECTED_SUBCARRIERS) {
        target = CSI_PHASE_A_MAX_SELECTED_SUBCARRIERS;
    }
    if (target < processor->config.min_selected_subcarriers) {
        target = processor->config.min_selected_subcarriers;
    }

    if (p70 >= candidate_count) {
        p70 = candidate_count - 1U;
    }

    float min_variance = candidates[p30].variance;
    float max_variance = candidates[p30].variance;
    for (size_t i = p30; i <= p70; ++i) {
        if (candidates[i].variance < min_variance) {
            min_variance = candidates[i].variance;
        }
        if (candidates[i].variance > max_variance) {
            max_variance = candidates[i].variance;
        }
    }
    float variance_range = max_variance - min_variance;
    float stability_denominator = max_variance + processor->config.calibration_variance_epsilon;
    for (size_t i = p30; i <= p70; ++i) {
        float variance_weight = variance_range > 0.0f
                                    ? 1.0f - ((candidates[i].variance - min_variance) / variance_range)
                                    : 1.0f;
        float temporal_stability_weight =
            1.0f - clamp01f(candidates[i].variance / stability_denominator);
        candidates[i].stability = temporal_stability_weight;
        candidates[i].score = variance_weight + temporal_stability_weight;
    }
    sort_subcarrier_score_desc(&candidates[p30], (p70 - p30) + 1U);

    processor->selected_count = 0U;
    for (size_t i = p30; i <= p70 && i < candidate_count &&
                       processor->selected_count < target;
        ++i) {
        uint8_t slot = processor->selected_count;
        processor->selected_indices[slot] = candidates[i].index;
        processor->selected_count++;
    }

    if (processor->selected_count < processor->config.min_selected_subcarriers) {
        for (size_t i = 0; i < candidate_count &&
                           processor->selected_count < processor->config.min_selected_subcarriers &&
                           processor->selected_count < CSI_PHASE_A_MAX_SELECTED_SUBCARRIERS;
             ++i) {
            bool already_selected = false;
            for (uint8_t j = 0; j < processor->selected_count; ++j) {
                if (processor->selected_indices[j] == candidates[i].index) {
                    already_selected = true;
                    break;
                }
            }
            if (!already_selected) {
                uint8_t slot = processor->selected_count;
                processor->selected_indices[slot] = candidates[i].index;
                processor->selected_count++;
            }
        }
    }
}

static void finish_calibration(csi_feature_processor_t *processor)
{
    if (processor == NULL || processor->calibration_samples == 0U) {
        return;
    }

    for (uint8_t i = 0; i < processor->observed_subcarrier_count; ++i) {
        processor->baseline[i] =
            (float)processor->baseline_sum[i] / (float)processor->calibration_samples;
    }
    compute_energy_baseline(processor);
    select_subcarriers(processor);
    memset(processor->previous_clean, 0, sizeof(processor->previous_clean));
    memset(processor->smoothed_delta, 0, sizeof(processor->smoothed_delta));
    processor->has_previous_clean = false;
    processor->delta_noise_estimate = processor->noise_floor;
    processor->state = CSI_PROCESSOR_STATE_RUN;
}

static void add_calibration_frame(csi_feature_processor_t *processor,
                                  const csi_frame_sample_t *frame)
{
    if (processor == NULL || frame == NULL || frame->subcarrier_count == 0U) {
        return;
    }

    uint8_t count = frame->subcarrier_count;
    if (count > CSI_PHASE_A_MAX_RAW_SUBCARRIERS) {
        count = CSI_PHASE_A_MAX_RAW_SUBCARRIERS;
    }
    if (processor->observed_subcarrier_count == 0U ||
        count < processor->observed_subcarrier_count) {
        processor->observed_subcarrier_count = count;
    }

    for (uint8_t i = 0; i < count; ++i) {
        uint32_t amp = frame->amplitude[i];
        processor->baseline_sum[i] += amp;
        processor->variance_sum[i] += amp;
        processor->variance_sum_sq[i] += (uint64_t)amp * (uint64_t)amp;
    }

    if (processor->energy_sample_count < CSI_PHASE_A_MAX_CALIBRATION_ENERGY_SAMPLES) {
        processor->energy_samples[processor->energy_sample_count++] =
            frame_energy_for_count(frame, count);
    }
    ++processor->calibration_samples;
    processor->last_timestamp_ms = frame->timestamp_ms;
}

void csi_feature_default_config(csi_feature_config_t *config)
{
    if (config == NULL) {
        return;
    }

    config->calibration_duration_ms = 7000U;
    config->calibration_converged_ms = 2000U;
    config->min_calibration_samples = 50U;
    config->calibration_variance_epsilon = 0.75f;
    config->ewma_alpha = 0.25f;
    config->guard_subcarriers = 2U;
    config->min_selected_subcarriers = CSI_PHASE_A_MIN_SELECTED_SUBCARRIERS;
    config->max_selected_subcarriers = CSI_PHASE_A_MAX_SELECTED_SUBCARRIERS;
    config->min_rssi = -82;
}

void csi_feature_processor_init(csi_feature_processor_t *processor,
                                const csi_feature_config_t *config)
{
    if (processor == NULL) {
        return;
    }

    csi_feature_config_t default_config;
    csi_feature_default_config(&default_config);
    memset(processor, 0, sizeof(*processor));
    processor->config = config != NULL ? *config : default_config;
    if (processor->config.ewma_alpha < 0.01f || processor->config.ewma_alpha > 1.0f) {
        processor->config.ewma_alpha = default_config.ewma_alpha;
    }
    if (processor->config.calibration_duration_ms < 5000U) {
        processor->config.calibration_duration_ms = 5000U;
    }
    if (processor->config.calibration_duration_ms > 10000U) {
        processor->config.calibration_duration_ms = 10000U;
    }
    if (processor->config.min_calibration_samples < 50U) {
        processor->config.min_calibration_samples = 50U;
    }
    if (processor->config.calibration_converged_ms < 500U) {
        processor->config.calibration_converged_ms = default_config.calibration_converged_ms;
    }
    if (processor->config.calibration_variance_epsilon <= 0.0f) {
        processor->config.calibration_variance_epsilon =
            default_config.calibration_variance_epsilon;
    }
    processor->state = CSI_PROCESSOR_STATE_INIT;
}

uint16_t csi_feature_amplitude_from_iq(int16_t i, int16_t q)
{
    int64_t i64 = (int64_t)i;
    int64_t q64 = (int64_t)q;
    uint64_t power = csi_abs_i64(i64 * i64) + csi_abs_i64(q64 * q64);
    uint32_t amplitude = csi_isqrt_u64(power);

    return (amplitude > UINT16_MAX) ? UINT16_MAX : (uint16_t)amplitude;
}

bool csi_feature_processor_push(csi_feature_processor_t *processor,
                                const csi_frame_sample_t *frame,
                                csi_feature_frame_t *out_feature)
{
    if (processor == NULL || frame == NULL || out_feature == NULL ||
        frame->subcarrier_count == 0U) {
        return false;
    }

    memset(out_feature, 0, sizeof(*out_feature));
    out_feature->metrics.rssi = frame->rssi;
    out_feature->timestamp_ms = frame->timestamp_ms;
    out_feature->metrics.cv = 0.0f;
    out_feature->metrics.quality = 0.0f;
    out_feature->state_hint = ENVELOPE_STATE_HINT_IDLE;
    out_feature->motion_score = 0.0f;
    out_feature->confidence = 0.0f;
    out_feature->quality_state = CSI_SAMPLE_QUALITY_INVALID;

    if (processor->state == CSI_PROCESSOR_STATE_INIT) {
        processor->state = CSI_PROCESSOR_STATE_CALIBRATION;
        processor->calibration_started_ms = frame->timestamp_ms;
    }

    if (processor->state == CSI_PROCESSOR_STATE_CALIBRATION) {
        add_calibration_frame(processor, frame);
        out_feature->quality_state = CSI_SAMPLE_QUALITY_CALIBRATING;

        uint64_t elapsed = frame->timestamp_ms >= processor->calibration_started_ms
                               ? frame->timestamp_ms - processor->calibration_started_ms
                               : 0U;
        bool converged = calibration_variance_converged(processor, frame->timestamp_ms);
        bool duration_ready = elapsed >= processor->config.calibration_duration_ms;
        bool samples_ready =
            processor->calibration_samples >= processor->config.min_calibration_samples;
        if (duration_ready && samples_ready && converged) {
            finish_calibration(processor);
        }
        return false;
    }

    if (processor->state != CSI_PROCESSOR_STATE_RUN || processor->selected_count == 0U ||
        frame->rssi < processor->config.min_rssi) {
        out_feature->quality_state = CSI_SAMPLE_QUALITY_WEAK;
        return false;
    }

    float energy_sum = 0.0f;
    float delta_sum = 0.0f;
    float delta_sq_sum = 0.0f;
    uint8_t used = 0U;
    for (uint8_t i = 0; i < processor->selected_count; ++i) {
        uint8_t index = processor->selected_indices[i];
        if (index >= frame->subcarrier_count ||
            index >= CSI_PHASE_A_MAX_RAW_SUBCARRIERS) {
            continue;
        }
        float clean = (float)frame->amplitude[index] - processor->baseline[index];
        float delta = processor->has_previous_clean ? clean - processor->previous_clean[index] : 0.0f;
        processor->smoothed_delta[index] =
            (processor->config.ewma_alpha * delta) +
            ((1.0f - processor->config.ewma_alpha) * processor->smoothed_delta[index]);
        processor->previous_clean[index] = clean;

        float magnitude = csi_absf(processor->smoothed_delta[index]);
        energy_sum += magnitude;
        delta_sum += magnitude;
        delta_sq_sum += magnitude * magnitude;
        ++used;
    }

    if (used == 0U) {
        out_feature->quality_state = CSI_SAMPLE_QUALITY_WEAK;
        return false;
    }

    processor->has_previous_clean = true;
    processor->last_timestamp_ms = frame->timestamp_ms;

    float mean_delta = delta_sum / (float)used;
    float variance = (delta_sq_sum / (float)used) - (mean_delta * mean_delta);
    if (variance < 0.0f) {
        variance = 0.0f;
    }

    processor->delta_noise_estimate =
        (processor->config.ewma_alpha * mean_delta) +
        ((1.0f - processor->config.ewma_alpha) * processor->delta_noise_estimate);
    float quality = 1.0f / (1.0f + variance + processor->delta_noise_estimate);

    out_feature->metrics.frame_energy = energy_sum / (float)used;
    out_feature->metrics.variance = variance;
    out_feature->metrics.cv =
        csi_feature_cv(out_feature->metrics.frame_energy, out_feature->metrics.variance);
    out_feature->metrics.quality = clamp01f(quality);
    out_feature->state_hint = csi_feature_state_hint(out_feature->metrics.variance,
                                                     out_feature->metrics.cv,
                                                     out_feature->metrics.quality);
    out_feature->quality_state = CSI_SAMPLE_QUALITY_GOOD;
    return true;
}

bool csi_feature_processor_ready(const csi_feature_processor_t *processor)
{
    return processor != NULL && processor->state == CSI_PROCESSOR_STATE_RUN &&
           processor->selected_count > 0U;
}

uint8_t csi_feature_processor_selected_count(const csi_feature_processor_t *processor)
{
    return processor != NULL ? processor->selected_count : 0U;
}

const char *csi_feature_quality_to_string(csi_sample_quality_t quality)
{
    switch (quality) {
    case CSI_SAMPLE_QUALITY_GOOD:
        return "good";
    case CSI_SAMPLE_QUALITY_WEAK:
        return "weak";
    case CSI_SAMPLE_QUALITY_CALIBRATING:
        return "calibrating";
    case CSI_SAMPLE_QUALITY_INVALID:
    default:
        return "invalid";
    }
}

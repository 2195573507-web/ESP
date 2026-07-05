/**
 * @file csi_feature.c
 * @brief 阶段 A CSI 纯函数算法实现。
 *
 * 本文件只做本地计算：振幅提取、Hampel/中值滤波、滑动窗口方差和 CV。
 * 所有内存均为调用方传入的固定大小结构体；没有任务、队列、HTTP 或 raw CSI 上传。
 */

#include "csi_feature.h"

#include <string.h>

static uint64_t csi_abs_i64(int64_t value)
{
    return (value < 0) ? (uint64_t)(-value) : (uint64_t)value;
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

static void csi_sort_u16(uint16_t *values, size_t count)
{
    for (size_t i = 1; i < count; ++i) {
        uint16_t key = values[i];
        size_t j = i;
        while (j > 0U && values[j - 1U] > key) {
            values[j] = values[j - 1U];
            --j;
        }
        values[j] = key;
    }
}

static uint16_t csi_median_u16(uint16_t *values, size_t count)
{
    if (count == 0U) {
        return 0U;
    }

    csi_sort_u16(values, count);
    if ((count % 2U) != 0U) {
        return values[count / 2U];
    }

    return (uint16_t)(((uint32_t)values[(count / 2U) - 1U] +
                       (uint32_t)values[count / 2U]) /
                      2U);
}

static float csi_frame_mean_amplitude(const csi_frame_sample_t *frame)
{
    if (frame == NULL || frame->subcarrier_count == 0U) {
        return 0.0f;
    }

    uint32_t sum = 0;
    for (uint8_t i = 0; i < frame->subcarrier_count; ++i) {
        sum += frame->amplitude[i];
    }

    return (float)sum / (float)frame->subcarrier_count;
}

void csi_feature_default_config(csi_feature_config_t *config)
{
    if (config == NULL) {
        return;
    }

    config->median_radius = 2U;
    config->hampel_threshold_q8 = 768U;
    config->min_subcarriers = 4U;
    config->min_samples_for_good_quality = 8U;
    config->min_rssi = -82;
}

uint16_t csi_feature_amplitude_from_iq(int16_t i, int16_t q)
{
    int64_t i64 = (int64_t)i;
    int64_t q64 = (int64_t)q;
    uint64_t power = csi_abs_i64(i64 * i64) + csi_abs_i64(q64 * q64);
    uint32_t amplitude = csi_isqrt_u64(power);

    return (amplitude > UINT16_MAX) ? UINT16_MAX : (uint16_t)amplitude;
}

bool csi_feature_hampel_filter_frame(csi_frame_sample_t *frame,
                                     const csi_feature_config_t *config)
{
    if (frame == NULL || config == NULL || frame->subcarrier_count == 0U ||
        frame->subcarrier_count > CSI_PHASE_A_MAX_SELECTED_SUBCARRIERS) {
        return false;
    }

    uint16_t filtered[CSI_PHASE_A_MAX_SELECTED_SUBCARRIERS] = {0};

    for (uint8_t i = 0; i < frame->subcarrier_count; ++i) {
        uint16_t local[CSI_PHASE_A_MAX_SELECTED_SUBCARRIERS] = {0};
        size_t local_count = 0;
        uint8_t start = (i > config->median_radius) ? (uint8_t)(i - config->median_radius) : 0U;
        uint8_t end = (uint8_t)(i + config->median_radius);
        if (end >= frame->subcarrier_count) {
            end = (uint8_t)(frame->subcarrier_count - 1U);
        }

        for (uint8_t j = start; j <= end; ++j) {
            local[local_count++] = frame->amplitude[j];
        }

        uint16_t median = csi_median_u16(local, local_count);
        uint16_t deviations[CSI_PHASE_A_MAX_SELECTED_SUBCARRIERS] = {0};
        for (size_t j = 0; j < local_count; ++j) {
            uint16_t value = local[j];
            deviations[j] = (value > median) ? (uint16_t)(value - median)
                                             : (uint16_t)(median - value);
        }

        uint16_t mad = csi_median_u16(deviations, local_count);
        uint32_t tolerance = ((uint32_t)mad * (uint32_t)config->hampel_threshold_q8) / 256U;
        if (tolerance == 0U) {
            tolerance = ((uint32_t)median / 4U) + 1U;
        }

        uint16_t original = frame->amplitude[i];
        uint32_t diff = (original > median) ? (uint32_t)(original - median)
                                            : (uint32_t)(median - original);
        filtered[i] = (diff > tolerance) ? median : original;
    }

    memcpy(frame->amplitude, filtered, sizeof(filtered));
    return true;
}

void csi_feature_window_init(csi_feature_window_t *window, uint8_t max_samples)
{
    if (window == NULL) {
        return;
    }

    memset(window, 0, sizeof(*window));
    if (max_samples == 0U || max_samples > CSI_PHASE_A_MAX_WINDOW_SAMPLES) {
        window->max_samples = CSI_PHASE_A_MAX_WINDOW_SAMPLES;
    } else {
        window->max_samples = max_samples;
    }
}

bool csi_feature_window_push(csi_feature_window_t *window,
                             const csi_frame_sample_t *frame)
{
    if (window == NULL || frame == NULL || window->max_samples == 0U ||
        frame->subcarrier_count == 0U) {
        return false;
    }

    uint8_t slot = window->next_index;
    window->frame_energy[slot] = csi_frame_mean_amplitude(frame);
    window->rssi[slot] = frame->rssi;
    window->timestamp_ms[slot] = frame->timestamp_ms;

    window->next_index = (uint8_t)((window->next_index + 1U) % window->max_samples);
    if (window->count < window->max_samples) {
        ++window->count;
    }

    return true;
}

bool csi_feature_window_compute_stats(const csi_feature_window_t *window,
                                      const csi_feature_config_t *config,
                                      csi_window_stats_t *out_stats)
{
    if (window == NULL || config == NULL || out_stats == NULL || window->max_samples == 0U) {
        return false;
    }

    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->quality = CSI_SAMPLE_QUALITY_INVALID;

    if (window->count == 0U) {
        return true;
    }

    float sum = 0.0f;
    int32_t rssi_sum = 0;
    uint64_t newest_ts = 0;
    for (uint8_t i = 0; i < window->count; ++i) {
        sum += window->frame_energy[i];
        rssi_sum += window->rssi[i];
        if (window->timestamp_ms[i] > newest_ts) {
            newest_ts = window->timestamp_ms[i];
        }
    }

    float mean = sum / (float)window->count;
    float squared_diff_sum = 0.0f;
    for (uint8_t i = 0; i < window->count; ++i) {
        float diff = window->frame_energy[i] - mean;
        squared_diff_sum += diff * diff;
    }

    float variance = squared_diff_sum / (float)window->count;
    int8_t avg_rssi = (int8_t)(rssi_sum / (int32_t)window->count);

    out_stats->mean_amplitude = mean;
    out_stats->variance = variance;
    out_stats->cv = (mean > 0.001f) ? (csi_sqrtf_approx(variance) / mean) : 0.0f;
    out_stats->sample_count = window->count;
    out_stats->rssi = avg_rssi;
    out_stats->updated_at_ms = newest_ts;

    if (window->count < config->min_samples_for_good_quality || avg_rssi < config->min_rssi) {
        out_stats->quality = CSI_SAMPLE_QUALITY_WEAK;
    } else {
        out_stats->quality = CSI_SAMPLE_QUALITY_GOOD;
    }

    return true;
}

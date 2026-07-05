#ifndef CSI_FEATURE_H
#define CSI_FEATURE_H

/**
 * @file csi_feature.h
 * @brief 阶段 A CSI 振幅、滤波和滑动窗口统计接口。
 *
 * 调用方式：
 * - csi_feature_amplitude_from_iq()：把单个 I/Q 样本转换为振幅。
 * - csi_feature_hampel_filter_frame()：对单帧已选子载波振幅做 Hampel/中值滤波。
 * - csi_feature_window_push()：把滤波后的帧放入固定大小滑动窗口。
 * - csi_feature_window_compute_stats()：输出 variance、CV、sample_count、quality。
 *
 * 输入：csi_frame_sample。
 * 输出：csi_window_stats。输出只包含摘要统计，不包含 raw CSI 或 I/Q 数组。
 */

#include "csi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t median_radius;
    uint16_t hampel_threshold_q8;
    uint8_t min_subcarriers;
    uint16_t min_samples_for_good_quality;
    int8_t min_rssi;
} csi_feature_config_t;

typedef struct {
    uint8_t max_samples;
    uint8_t count;
    uint8_t next_index;
    float frame_energy[CSI_PHASE_A_MAX_WINDOW_SAMPLES];
    int8_t rssi[CSI_PHASE_A_MAX_WINDOW_SAMPLES];
    uint64_t timestamp_ms[CSI_PHASE_A_MAX_WINDOW_SAMPLES];
} csi_feature_window_t;

void csi_feature_default_config(csi_feature_config_t *config);

uint16_t csi_feature_amplitude_from_iq(int16_t i, int16_t q);

bool csi_feature_hampel_filter_frame(csi_frame_sample_t *frame,
                                     const csi_feature_config_t *config);

void csi_feature_window_init(csi_feature_window_t *window, uint8_t max_samples);

bool csi_feature_window_push(csi_feature_window_t *window,
                             const csi_frame_sample_t *frame);

bool csi_feature_window_compute_stats(const csi_feature_window_t *window,
                                      const csi_feature_config_t *config,
                                      csi_window_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* CSI_FEATURE_H */

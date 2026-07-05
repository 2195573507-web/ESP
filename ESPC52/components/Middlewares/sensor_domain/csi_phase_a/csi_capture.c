/**
 * @file csi_capture.c
 * @brief 阶段 A CSI 离线样本归一化实现。
 *
 * 本文件只把调用方注入的离线 I/Q 样本转成 csi_frame_sample。它不注册 WiFi
 * callback，不启动任务，不访问 S3/Server，也不保存或上传 raw CSI。
 */

#include "csi_capture.h"

#include <string.h>

#include "csi_feature.h"

bool csi_capture_build_frame_from_iq(const csi_iq_sample_t *iq_samples,
                                     size_t iq_count,
                                     const uint8_t *selected_indices,
                                     size_t selected_count,
                                     int8_t rssi,
                                     uint64_t timestamp_ms,
                                     csi_frame_sample_t *out_frame)
{
    if (iq_samples == NULL || selected_indices == NULL || out_frame == NULL ||
        selected_count == 0U ||
        selected_count > CSI_PHASE_A_MAX_SELECTED_SUBCARRIERS) {
        return false;
    }

    memset(out_frame, 0, sizeof(*out_frame));
    out_frame->timestamp_ms = timestamp_ms;
    out_frame->rssi = rssi;

    for (size_t i = 0; i < selected_count; ++i) {
        uint8_t source_index = selected_indices[i];
        if ((size_t)source_index >= iq_count) {
            return false;
        }
        out_frame->amplitude[i] =
            csi_feature_amplitude_from_iq(iq_samples[source_index].i,
                                          iq_samples[source_index].q);
    }

    out_frame->subcarrier_count = (uint8_t)selected_count;
    return true;
}

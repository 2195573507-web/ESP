/**
 * @file csi_capture.c
 * @brief C5 CSI I/Q 到幅值帧转换。
 *
 * 本模块不保存 raw CSI，只把调用方持有的 I/Q buffer 转成 amplitude-only frame，
 * 输出长度受 CSI_PHASE_A_MAX_RAW_SUBCARRIERS 限制。
 */

#include "csi_capture.h"

#include <string.h>

#include "csi_feature.h"

bool csi_capture_build_frame_from_iq(const csi_iq_sample_t *iq_samples,
                                     size_t iq_count,
                                     int8_t rssi,
                                     uint64_t timestamp_ms,
                                     csi_frame_sample_t *out_frame)
{
    if (iq_samples == NULL || out_frame == NULL || iq_count == 0U) {
        return false;
    }

    size_t capped_count = iq_count;
    if (capped_count > CSI_PHASE_A_MAX_RAW_SUBCARRIERS) {
        capped_count = CSI_PHASE_A_MAX_RAW_SUBCARRIERS;
    }

    memset(out_frame, 0, sizeof(*out_frame));
    out_frame->timestamp_ms = timestamp_ms;
    out_frame->rssi = rssi;
    out_frame->subcarrier_count = (uint8_t)capped_count;

    for (size_t i = 0; i < capped_count; ++i) {
        out_frame->amplitude[i] =
            csi_feature_amplitude_from_iq(iq_samples[i].i, iq_samples[i].q);
    }

    return true;
}

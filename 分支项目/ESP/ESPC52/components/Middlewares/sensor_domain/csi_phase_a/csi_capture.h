#ifndef CSI_CAPTURE_H
#define CSI_CAPTURE_H

/**
 * @file csi_capture.h
 * @brief 阶段 A CSI 样本注入与单帧归一化接口。
 *
 * 本文件不是 WiFi CSI callback，也不会调用 esp_wifi_set_csi()。阶段 A 只允许
 * 离线样本或测试代码显式调用 csi_capture_build_frame_from_iq()，把 I/Q 样本
 * 转换成振幅帧。
 *
 * 输入：离线 I/Q 数组、RSSI、时间戳。
 * 输出：csi_frame_sample，只包含振幅、RSSI 和时间戳，不保留 raw I/Q。
 */

#include "csi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool csi_capture_build_frame_from_iq(const csi_iq_sample_t *iq_samples,
                                     size_t iq_count,
                                     int8_t rssi,
                                     uint64_t timestamp_ms,
                                     csi_frame_sample_t *out_frame);

#ifdef __cplusplus
}
#endif

#endif /* CSI_CAPTURE_H */

#ifndef CSI_TYPES_H
#define CSI_TYPES_H

/**
 * @file csi_types.h
 * @brief 阶段 A CSI 独立算法的数据结构定义。
 *
 * 本文件只定义 C5 本地离线算法会用到的结构体和枚举，不接入
 * app_orchestrator，不启动 WiFi CSI callback，也不描述任何 HTTP 上传行为。
 *
 * 调用方式：
 * 1. csi_capture 把离线 I/Q 样本转换成 csi_frame_sample；
 * 2. csi_feature 对 csi_frame_sample 做滤波和窗口统计，输出 csi_window_stats；
 * 3. csi_presence 把 csi_window_stats 转换成 csi_presence_result；
 * 4. csi_result_codec 只生成结构体或日志摘要。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 阶段 A 固定内存上限：只选择少量子载波，避免保存 raw CSI。 */
#define CSI_PHASE_A_MAX_SELECTED_SUBCARRIERS 16U
#define CSI_PHASE_A_MAX_WINDOW_SAMPLES 64U

typedef enum {
    CSI_PRESENCE_STATE_UNKNOWN = 0,
    CSI_PRESENCE_STATE_VACANT = 1,
    CSI_PRESENCE_STATE_OCCUPIED = 2,
} csi_presence_state_t;

typedef enum {
    CSI_SAMPLE_QUALITY_INVALID = 0,
    CSI_SAMPLE_QUALITY_WEAK = 1,
    CSI_SAMPLE_QUALITY_GOOD = 2,
} csi_sample_quality_t;

typedef struct {
    int16_t i;
    int16_t q;
} csi_iq_sample_t;

typedef struct {
    uint64_t timestamp_ms;
    int8_t rssi;
    uint8_t subcarrier_count;
    uint16_t amplitude[CSI_PHASE_A_MAX_SELECTED_SUBCARRIERS];
} csi_frame_sample_t;

typedef struct {
    float variance;
    float cv;
    float mean_amplitude;
    uint16_t sample_count;
    csi_sample_quality_t quality;
    int8_t rssi;
    uint64_t updated_at_ms;
} csi_window_stats_t;

typedef struct {
    csi_presence_state_t state;
    float motion_score;
    float variance;
    float cv;
    int8_t rssi;
    uint16_t sample_count;
    uint64_t updated_at_ms;
} csi_presence_result_t;

#ifdef __cplusplus
}
#endif

#endif /* CSI_TYPES_H */

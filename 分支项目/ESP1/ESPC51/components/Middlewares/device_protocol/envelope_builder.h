#ifndef ENVELOPE_BUILDER_H
#define ENVELOPE_BUILDER_H

/**
 * @file envelope_builder.h
 * @brief C5 feature v2 envelope 序列化器。
 *
 * 本模块只负责把 C5 已计算的轻量 feature 写成 canonical JSON。它不做 CSI 判定、
 * 不保存历史窗口，也不生成 raw/subcarrier 字段；S3 csi_fusion 才负责状态机。
 */

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_PROTOCOL_VERSION 2
#define ENVELOPE_BUILDER_SCHEMA_VERSION "v2"
#define ENVELOPE_BUILDER_SOURCE_CSI_PHASE_A "csi_phase_a"
#define ENVELOPE_BUILDER_JSON_MAX_BYTES 512U
#define ENVELOPE_BUILDER_TRACE_ID_LEN 37U

typedef enum {
    ENVELOPE_STATE_HINT_IDLE = 0,
    ENVELOPE_STATE_HINT_MOTION = 1,
    ENVELOPE_STATE_HINT_HOLD = 2,
} envelope_state_hint_t;

/** C5 边缘 CSI feature 指标；字段保持轻量，便于 S3 双链路融合。 */
typedef struct {
    float frame_energy;
    float variance;
    float cv;
    int rssi;
    float quality;
} envelope_metrics_t;

/** envelope_builder_format() 的完整输入；字符串指针由调用方持有。 */
typedef struct {
    const char *local_id;
    const char *device_id;
    const char *link_id;
    int64_t timestamp_ms;
    envelope_metrics_t metrics;
    envelope_state_hint_t state_hint;
    float motion_score;
    float confidence;
    const char *source;
} envelope_builder_input_t;

/** @brief 将 C5 状态提示枚举转为 v2 envelope 的稳定字符串。 */
const char *envelope_builder_state_hint_to_string(envelope_state_hint_t state_hint);

/** @brief 格式化 v2 feature envelope；out 由调用方提供，失败不会写入动态缓存。 */
esp_err_t envelope_builder_format(const envelope_builder_input_t *input,
                                  char *out,
                                  size_t out_size);

/** @brief 格式化 S3 /local/v1/csi/result 使用的轻量 CSI feature report。 */
esp_err_t envelope_builder_format_local_csi_report(const envelope_builder_input_t *input,
                                                   char *out,
                                                   size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* ENVELOPE_BUILDER_H */

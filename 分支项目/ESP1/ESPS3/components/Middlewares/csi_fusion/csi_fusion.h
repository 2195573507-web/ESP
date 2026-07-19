#ifndef CSI_FUSION_H
#define CSI_FUSION_H

/**
 * @file csi_fusion.h
 * @brief ESPS3 CSI tick 对齐融合和 canonical event 状态机。
 *
 * S3 只负责时间对齐、链路配对、IDLE/MOTION/HOLD 状态决策和 CanonicalEvent v2
 * 生成。输入来自 C5 上报的 motion_score、quality、RSSI 和低维 metrics；C5 state
 * 只作为 local_hint 日志，不直接决定 S3 状态。raw CSI、I/Q、phase 和 subcarrier
 * 数据都不进入本模块。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CSI_FUSION_TEXT_LEN 32U
#define CSI_FUSION_TRACE_ID_LEN 48U
#define CSI_FUSION_SCHEMA_VERSION 2U
#define CSI_FUSION_LINK_COUNT 2U

#ifndef CSI_FUSION_TICK_MS
#define CSI_FUSION_TICK_MS 100U
#endif

typedef enum {
    CSI_FUSION_STATE_IDLE = 0,
    CSI_FUSION_STATE_MOTION = 1,
    CSI_FUSION_STATE_HOLD = 2,
} csi_fusion_state_t;

typedef struct {
    char device_id[CSI_FUSION_TEXT_LEN];
    char link_id[CSI_FUSION_TEXT_LEN];
    char trace_id[CSI_FUSION_TRACE_ID_LEN];
    bool has_state;
    csi_fusion_state_t state;
    float motion_score;
    float confidence;
    float quality;
    int rssi;
    bool has_metrics;
    float energy;
    float variance;
    float cv;
    uint32_t frame_seq;
    uint64_t tick_id;
    uint64_t timestamp_ms;
    uint64_t child_timestamp_ms;
} csi_fusion_feature_t;

typedef struct {
    bool valid;
    char device_id[CSI_FUSION_TEXT_LEN];
    char link_id[CSI_FUSION_TEXT_LEN];
    char trace_id[CSI_FUSION_TRACE_ID_LEN];
    bool has_state;
    csi_fusion_state_t state;
    float motion_score;
    float confidence;
    float quality;
    int rssi;
    bool has_metrics;
    float energy;
    float variance;
    float cv;
    uint32_t frame_seq;
    uint64_t tick_id;
    uint64_t timestamp_ms;
} csi_fusion_link_state_t;

typedef struct {
    bool valid;
    uint8_t schema_version;
    char trace_id[CSI_FUSION_TRACE_ID_LEN];
    uint64_t tick_id;
    char links[CSI_FUSION_LINK_COUNT][CSI_FUSION_TEXT_LEN];
    csi_fusion_state_t fused_state;
    float motion_score;
    float confidence;
    uint64_t timestamp_ms;
    uint8_t active_link_count;
} csi_fusion_canonical_event_t;

typedef csi_fusion_canonical_event_t csi_fusion_telemetry_t;
typedef csi_fusion_canonical_event_t csi_fusion_fact_t;

void csi_fusion_init(void);

/** @brief 立即停用指定 C5 的融合链路并清空实时样本。调用方负责串行化。 */
esp_err_t csi_fusion_suspend_link(const char *device_id);

/** @brief 恢复指定 C5 的融合链路并从 5 个不同 tick 的 warmup 重新开始。 */
esp_err_t csi_fusion_restore_link(const char *device_id);

/** @brief Query whether the restored link has accepted its full warmup sample set. */
bool csi_fusion_link_warmup_complete(const char *device_id);

esp_err_t csi_fusion_update(const csi_fusion_feature_t *feature,
                            csi_fusion_fact_t *out_fact,
                            csi_fusion_telemetry_t *out_telemetry);

esp_err_t csi_fusion_flush(csi_fusion_fact_t *out_fact,
                           csi_fusion_telemetry_t *out_telemetry);

esp_err_t csi_fusion_format_telemetry_json(const csi_fusion_telemetry_t *telemetry,
                                           char *out,
                                           size_t out_size);

const char *csi_fusion_state_to_string(csi_fusion_state_t state);

bool csi_fusion_state_from_string(const char *value, csi_fusion_state_t *out_state);

const char *csi_fusion_link_state_name(size_t index);

#ifdef __cplusplus
}
#endif

#endif /* CSI_FUSION_H */

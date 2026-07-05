#ifndef CSI_PRESENCE_H
#define CSI_PRESENCE_H

/**
 * @file csi_presence.h
 * @brief 阶段 A CSI 有人/无人阈值状态机接口。
 *
 * 调用方式：
 * - csi_presence_state_machine_init() 初始化状态机；
 * - csi_presence_update() 输入窗口统计，输出 csi_presence_result。
 *
 * 输入：csi_window_stats。
 * 输出：unknown/vacant/occupied、motion_score 和诊断摘要。
 * 本模块只做阈值和滞回，不做动作识别、人数识别、呼吸/心率或深度学习模型。
 */

#include "csi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float vacant_threshold;
    float occupied_threshold;
    float variance_score_scale;
    float cv_score_scale;
    uint8_t confirm_windows;
    uint16_t min_samples;
    int8_t min_rssi;
} csi_presence_config_t;

typedef struct {
    csi_presence_state_t current_state;
    csi_presence_state_t pending_state;
    uint8_t pending_count;
} csi_presence_state_machine_t;

void csi_presence_default_config(csi_presence_config_t *config);

void csi_presence_state_machine_init(csi_presence_state_machine_t *machine);

bool csi_presence_update(csi_presence_state_machine_t *machine,
                         const csi_presence_config_t *config,
                         const csi_window_stats_t *stats,
                         csi_presence_result_t *out_result);

const char *csi_presence_state_to_string(csi_presence_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* CSI_PRESENCE_H */

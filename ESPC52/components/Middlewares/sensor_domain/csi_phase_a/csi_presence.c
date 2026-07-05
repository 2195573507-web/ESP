/**
 * @file csi_presence.c
 * @brief 阶段 A CSI 阈值状态机实现。
 *
 * 本文件根据窗口 variance/CV/RSSI 输出 unknown/vacant/occupied。状态切换使用
 * occupied_threshold > vacant_threshold 的滞回和连续窗口确认，减少抖动。
 */

#include "csi_presence.h"

#include <string.h>

static float csi_clampf(float value, float min_value, float max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static float csi_presence_motion_score(const csi_presence_config_t *config,
                                       const csi_window_stats_t *stats)
{
    float variance_part = (config->variance_score_scale > 0.001f)
                              ? (stats->variance / config->variance_score_scale)
                              : 0.0f;
    float cv_part = (config->cv_score_scale > 0.001f) ? (stats->cv / config->cv_score_scale)
                                                      : 0.0f;

    return csi_clampf((variance_part * 0.65f) + (cv_part * 0.35f), 0.0f, 1.0f);
}

void csi_presence_default_config(csi_presence_config_t *config)
{
    if (config == NULL) {
        return;
    }

    config->vacant_threshold = 0.18f;
    config->occupied_threshold = 0.55f;
    config->variance_score_scale = 260.0f;
    config->cv_score_scale = 0.22f;
    config->confirm_windows = 2U;
    config->min_samples = 8U;
    config->min_rssi = -82;
}

void csi_presence_state_machine_init(csi_presence_state_machine_t *machine)
{
    if (machine == NULL) {
        return;
    }

    memset(machine, 0, sizeof(*machine));
    machine->current_state = CSI_PRESENCE_STATE_UNKNOWN;
    machine->pending_state = CSI_PRESENCE_STATE_UNKNOWN;
}

bool csi_presence_update(csi_presence_state_machine_t *machine,
                         const csi_presence_config_t *config,
                         const csi_window_stats_t *stats,
                         csi_presence_result_t *out_result)
{
    if (machine == NULL || config == NULL || stats == NULL || out_result == NULL ||
        config->occupied_threshold <= config->vacant_threshold) {
        return false;
    }

    memset(out_result, 0, sizeof(*out_result));
    out_result->mean_amplitude = stats->mean_amplitude;
    out_result->variance = stats->variance;
    out_result->cv = stats->cv;
    out_result->quality = stats->quality;
    out_result->rssi = stats->rssi;
    out_result->sample_count = stats->sample_count;
    out_result->updated_at_ms = stats->updated_at_ms;

    bool quality_bad = (stats->quality == CSI_SAMPLE_QUALITY_INVALID) ||
                       (stats->sample_count < config->min_samples) ||
                       (stats->rssi < config->min_rssi);
    if (quality_bad) {
        machine->current_state = CSI_PRESENCE_STATE_UNKNOWN;
        machine->pending_state = CSI_PRESENCE_STATE_UNKNOWN;
        machine->pending_count = 0U;
        out_result->state = CSI_PRESENCE_STATE_UNKNOWN;
        out_result->motion_score = 0.0f;
        return true;
    }

    float score = csi_presence_motion_score(config, stats);
    csi_presence_state_t target = machine->current_state;

    if (score >= config->occupied_threshold) {
        target = CSI_PRESENCE_STATE_OCCUPIED;
    } else if (score <= config->vacant_threshold) {
        target = CSI_PRESENCE_STATE_VACANT;
    } else if (target == CSI_PRESENCE_STATE_UNKNOWN) {
        target = CSI_PRESENCE_STATE_UNKNOWN;
    }

    if (target == machine->current_state) {
        machine->pending_state = target;
        machine->pending_count = 0U;
    } else if (target != machine->pending_state) {
        machine->pending_state = target;
        machine->pending_count = 1U;
    } else {
        ++machine->pending_count;
    }

    uint8_t confirm_windows = (config->confirm_windows == 0U) ? 1U : config->confirm_windows;
    if (machine->pending_count >= confirm_windows) {
        machine->current_state = machine->pending_state;
        machine->pending_count = 0U;
    }

    out_result->state = machine->current_state;
    out_result->motion_score = score;
    return true;
}

const char *csi_presence_state_to_string(csi_presence_state_t state)
{
    switch (state) {
    case CSI_PRESENCE_STATE_VACANT:
        return "vacant";
    case CSI_PRESENCE_STATE_OCCUPIED:
        return "occupied";
    case CSI_PRESENCE_STATE_UNKNOWN:
    default:
        return "unknown";
    }
}

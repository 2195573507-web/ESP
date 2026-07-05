/**
 * @file csi_result_codec.c
 * @brief 阶段 A CSI 结果摘要编码实现。
 *
 * 输出字符串用于离线日志验证，字段只有 state、motion_score、mean_amplitude、
 * variance、cv、rssi、quality、sample_count、updated_at_ms。这里没有 HTTP client，
 * 也不会上传 raw CSI。
 */

#include "csi_result_codec.h"

#include <inttypes.h>
#include <stdio.h>

#include "csi_presence.h"

static uint8_t csi_result_state_code(csi_presence_state_t state)
{
    switch (state) {
    case CSI_PRESENCE_STATE_OCCUPIED:
        return 1U;
    case CSI_PRESENCE_STATE_VACANT:
        return 2U;
    case CSI_PRESENCE_STATE_UNKNOWN:
    default:
        return 0U;
    }
}

static const char *csi_result_quality_to_string(uint8_t quality_code)
{
    switch (quality_code) {
    case CSI_SAMPLE_QUALITY_GOOD:
        return "good";
    case CSI_SAMPLE_QUALITY_WEAK:
        return "weak";
    case CSI_SAMPLE_QUALITY_INVALID:
    default:
        return "weak";
    }
}

bool csi_result_codec_from_presence(const csi_presence_result_t *result,
                                    csi_result_codec_summary_t *out_summary)
{
    if (result == NULL || out_summary == NULL) {
        return false;
    }

    float score = result->motion_score;
    if (score < 0.0f) {
        score = 0.0f;
    } else if (score > 1.0f) {
        score = 1.0f;
    }

    out_summary->schema_version = 1U;
    out_summary->state_code = csi_result_state_code(result->state);
    out_summary->motion_score_pct = (uint8_t)((score * 100.0f) + 0.5f);
    out_summary->mean_amplitude = result->mean_amplitude;
    out_summary->variance = result->variance;
    out_summary->cv = result->cv;
    out_summary->quality_code = (uint8_t)result->quality;
    out_summary->rssi = result->rssi;
    out_summary->sample_count = result->sample_count;
    out_summary->updated_at_ms = result->updated_at_ms;
    return true;
}

int csi_result_codec_format_summary(const csi_result_codec_summary_t *summary,
                                    char *buffer,
                                    size_t buffer_size)
{
    if (summary == NULL || buffer == NULL || buffer_size == 0U) {
        return -1;
    }

    csi_presence_state_t state = CSI_PRESENCE_STATE_UNKNOWN;
    if (summary->state_code == 1U) {
        state = CSI_PRESENCE_STATE_OCCUPIED;
    } else if (summary->state_code == 2U) {
        state = CSI_PRESENCE_STATE_VACANT;
    }

    return snprintf(buffer,
                    buffer_size,
                    "{\"schema\":%u,\"state\":\"%s\",\"motion_score\":%.2f,"
                    "\"mean_amplitude\":%.2f,\"variance\":%.2f,\"cv\":%.3f,"
                    "\"rssi\":%d,\"quality\":\"%s\","
                    "\"sample_count\":%u,\"updated_at_ms\":%" PRIu64 "}",
                    (unsigned int)summary->schema_version,
                    csi_presence_state_to_string(state),
                    (double)summary->motion_score_pct / 100.0,
                    (double)summary->mean_amplitude,
                    (double)summary->variance,
                    (double)summary->cv,
                    (int)summary->rssi,
                    csi_result_quality_to_string(summary->quality_code),
                    (unsigned int)summary->sample_count,
                    summary->updated_at_ms);
}

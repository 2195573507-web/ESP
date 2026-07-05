/**
 * @file csi_phase_a_tests.c
 * @brief 阶段 A CSI 离线样本和日志级测试实现。
 *
 * 测试入口覆盖振幅提取、子载波选择、Hampel/中值滤波、窗口方差/CV、状态机和
 * result codec。它们只返回摘要字符串，不打印 raw CSI 大数组，不发 HTTP。
 */

#include "csi_phase_a_tests.h"

#include <stdio.h>
#include <string.h>

#include "csi_capture.h"
#include "csi_feature.h"
#include "csi_presence.h"
#include "csi_result_codec.h"

static const uint8_t k_test_subcarriers[] = {4U, 8U, 12U, 16U, 20U, 24U, 28U, 32U};

static void csi_test_fill_iq(csi_iq_sample_t *iq_samples,
                             size_t iq_count,
                             int16_t base_i,
                             int16_t spike_i,
                             uint8_t spike_index)
{
    for (size_t i = 0; i < iq_count; ++i) {
        iq_samples[i].i = base_i;
        iq_samples[i].q = 0;
    }
    if ((size_t)spike_index < iq_count) {
        iq_samples[spike_index].i = spike_i;
        iq_samples[spike_index].q = spike_i;
    }
}

bool csi_feature_test(char *summary, size_t summary_size)
{
    csi_iq_sample_t iq_samples[40] = {0};
    csi_frame_sample_t frame;
    csi_feature_config_t feature_config;
    csi_feature_window_t window;
    csi_window_stats_t stats;

    csi_feature_default_config(&feature_config);
    csi_test_fill_iq(iq_samples, 40U, 100, 1000, 16U);

    bool ok = csi_capture_build_frame_from_iq(iq_samples,
                                              40U,
                                              k_test_subcarriers,
                                              sizeof(k_test_subcarriers),
                                              -48,
                                              1000U,
                                              &frame);
    uint16_t spike_before = frame.amplitude[3];
    ok = ok && csi_feature_hampel_filter_frame(&frame, &feature_config);
    uint16_t spike_after = frame.amplitude[3];

    csi_feature_window_init(&window, 16U);
    for (uint8_t i = 0; i < 16U; ++i) {
        frame.timestamp_ms = 1000U + ((uint64_t)i * 100U);
        frame.rssi = -48;
        ok = ok && csi_feature_window_push(&window, &frame);
    }
    ok = ok && csi_feature_window_compute_stats(&window, &feature_config, &stats);
    ok = ok && (spike_after < spike_before) && (stats.sample_count == 16U) &&
         (stats.quality == CSI_SAMPLE_QUALITY_GOOD);

    if (summary != NULL && summary_size > 0U) {
        snprintf(summary,
                 summary_size,
                 "feature ok=%d selected=%u spike_before=%u spike_after=%u "
                 "variance=%.2f cv=%.3f samples=%u quality=%u",
                 ok ? 1 : 0,
                 (unsigned int)frame.subcarrier_count,
                 (unsigned int)spike_before,
                 (unsigned int)spike_after,
                 (double)stats.variance,
                 (double)stats.cv,
                 (unsigned int)stats.sample_count,
                 (unsigned int)stats.quality);
    }

    return ok;
}

bool csi_presence_test(char *summary, size_t summary_size)
{
    csi_presence_config_t config;
    csi_presence_state_machine_t machine;
    csi_presence_result_t result;
    csi_window_stats_t stats = {
        .variance = 0.0f,
        .cv = 0.0f,
        .mean_amplitude = 100.0f,
        .sample_count = 16U,
        .quality = CSI_SAMPLE_QUALITY_GOOD,
        .rssi = -50,
        .updated_at_ms = 2000U,
    };

    csi_presence_default_config(&config);
    csi_presence_state_machine_init(&machine);

    bool ok = csi_presence_update(&machine, &config, &stats, &result);
    ok = ok && (result.state == CSI_PRESENCE_STATE_UNKNOWN);
    ok = ok && csi_presence_update(&machine, &config, &stats, &result);
    ok = ok && (result.state == CSI_PRESENCE_STATE_VACANT);

    stats.variance = 900.0f;
    stats.cv = 0.55f;
    stats.updated_at_ms = 3000U;
    ok = ok && csi_presence_update(&machine, &config, &stats, &result);
    ok = ok && (result.state == CSI_PRESENCE_STATE_VACANT);
    ok = ok && csi_presence_update(&machine, &config, &stats, &result);
    ok = ok && (result.state == CSI_PRESENCE_STATE_OCCUPIED) &&
         (result.motion_score >= config.occupied_threshold);

    stats.sample_count = 1U;
    stats.quality = CSI_SAMPLE_QUALITY_WEAK;
    ok = ok && csi_presence_update(&machine, &config, &stats, &result);
    ok = ok && (result.state == CSI_PRESENCE_STATE_UNKNOWN);

    if (summary != NULL && summary_size > 0U) {
        snprintf(summary,
                 summary_size,
                 "presence ok=%d final_state=%s motion=%.2f variance=%.2f "
                 "samples=%u rssi=%d",
                 ok ? 1 : 0,
                 csi_presence_state_to_string(result.state),
                 (double)result.motion_score,
                 (double)result.variance,
                 (unsigned int)result.sample_count,
                 (int)result.rssi);
    }

    return ok;
}

bool csi_result_codec_test(char *summary, size_t summary_size)
{
    csi_presence_result_t result = {
        .state = CSI_PRESENCE_STATE_OCCUPIED,
        .motion_score = 0.73f,
        .mean_amplitude = 128.0f,
        .variance = 512.0f,
        .cv = 0.42f,
        .quality = CSI_SAMPLE_QUALITY_GOOD,
        .rssi = -49,
        .sample_count = 32U,
        .updated_at_ms = 4567U,
    };
    csi_result_codec_summary_t codec_summary;
    char encoded[256] = {0};

    bool ok = csi_result_codec_from_presence(&result, &codec_summary);
    int written = csi_result_codec_format_summary(&codec_summary, encoded, sizeof(encoded));
    ok = ok && (written > 0) && (codec_summary.state_code == 1U) &&
         (codec_summary.motion_score_pct == 73U) &&
         (strstr(encoded, "\"state\":\"occupied\"") != NULL) &&
         (strstr(encoded, "\"quality\":\"good\"") != NULL) &&
         (strstr(encoded, "\"sample_count\":32") != NULL);

    if (summary != NULL && summary_size > 0U) {
        snprintf(summary,
                 summary_size,
                 "codec ok=%d %s",
                 ok ? 1 : 0,
                 encoded);
    }

    return ok;
}

bool csi_phase_a_run_offline_tests(char *summary, size_t summary_size)
{
    char feature_summary[192] = {0};
    char presence_summary[192] = {0};
    char codec_summary[320] = {0};

    bool feature_ok = csi_feature_test(feature_summary, sizeof(feature_summary));
    bool presence_ok = csi_presence_test(presence_summary, sizeof(presence_summary));
    bool codec_ok = csi_result_codec_test(codec_summary, sizeof(codec_summary));
    bool ok = feature_ok && presence_ok && codec_ok;

    if (summary != NULL && summary_size > 0U) {
        snprintf(summary,
                 summary_size,
                 "phase_a ok=%d | %s | %s | %s",
                 ok ? 1 : 0,
                 feature_summary,
                 presence_summary,
                 codec_summary);
    }

    return ok;
}

#ifdef CSI_PHASE_A_TEST_MAIN
int main(void)
{
    char summary[768] = {0};
    bool ok = csi_phase_a_run_offline_tests(summary, sizeof(summary));
    printf("%s\n", summary);
    return ok ? 0 : 1;
}
#endif

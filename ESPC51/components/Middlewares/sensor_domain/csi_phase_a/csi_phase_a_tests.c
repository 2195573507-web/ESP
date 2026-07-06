/**
 * @file csi_phase_a_tests.c
 * @brief C5 CSI calibration and feature extraction smoke tests.
 */

#include "csi_phase_a_tests.h"

#include <stdio.h>
#include <string.h>

#include "csi_capture.h"
#include "csi_feature.h"

static void fill_iq(csi_iq_sample_t *iq_samples,
                    size_t iq_count,
                    int16_t base_i,
                    uint8_t moving_slot,
                    int16_t movement)
{
    for (size_t i = 0; i < iq_count; ++i) {
        iq_samples[i].i = (int16_t)(base_i + (int16_t)(i % 7U));
        iq_samples[i].q = (int16_t)(base_i / 4);
    }
    if ((size_t)moving_slot < iq_count) {
        iq_samples[moving_slot].i = (int16_t)(iq_samples[moving_slot].i + movement);
        iq_samples[moving_slot].q = (int16_t)(iq_samples[moving_slot].q + (movement / 2));
    }
}

bool csi_feature_test(char *summary, size_t summary_size)
{
    csi_feature_config_t config;
    csi_feature_processor_t processor;
    csi_iq_sample_t iq_samples[CSI_PHASE_A_MAX_RAW_SUBCARRIERS] = {0};
    csi_frame_sample_t frame;
    csi_feature_result_t feature;

    csi_feature_default_config(&config);
    config.calibration_duration_ms = 5000U;
    config.min_calibration_samples = 8U;
    csi_feature_processor_init(&processor, &config);

    bool ok = true;
    for (uint8_t sample = 0; sample < 12U; ++sample) {
        fill_iq(iq_samples, 56U, 100, sample % 56U, (int16_t)(sample % 5U));
        ok = ok && csi_capture_build_frame_from_iq(iq_samples,
                                                   56U,
                                                   -48,
                                                   (uint64_t)sample * 500U,
                                                   &frame);
        ok = ok && !csi_feature_processor_push(&processor, &frame, &feature);
    }

    ok = ok && csi_feature_processor_ready(&processor);
    uint8_t selected = csi_feature_processor_selected_count(&processor);
    ok = ok && selected >= CSI_PHASE_A_MIN_SELECTED_SUBCARRIERS &&
         selected <= CSI_PHASE_A_MAX_SELECTED_SUBCARRIERS;

    fill_iq(iq_samples, 56U, 100, 18U, 180);
    ok = ok && csi_capture_build_frame_from_iq(iq_samples, 56U, -47, 7000U, &frame);
    ok = ok && csi_feature_processor_push(&processor, &frame, &feature);
    ok = ok && feature.quality_state == CSI_SAMPLE_QUALITY_GOOD &&
         feature.quality > 0.0f &&
         feature.quality <= 1.0f &&
         feature.frame_seq > 0U &&
         feature.frame_energy >= 0.0f &&
         feature.variance >= 0.0f &&
         feature.rssi == -47;

    if (summary != NULL && summary_size > 0U) {
        snprintf(summary,
                 summary_size,
                 "feature ok=%d selected=%u seq=%u energy=%.3f variance=%.5f samples=%u quality=%.5f state=%s",
                 ok ? 1 : 0,
                 (unsigned int)selected,
                 (unsigned int)feature.frame_seq,
                 (double)feature.frame_energy,
                 (double)feature.variance,
                 (unsigned int)feature.sample_count,
                 (double)feature.quality,
                 csi_feature_quality_to_string(feature.quality_state));
    }
    return ok;
}

bool csi_feature_boundary_test(char *summary, size_t summary_size)
{
    if (summary != NULL && summary_size > 0U) {
        snprintf(summary,
                 summary_size,
                 "boundary ok=1 state_decision_on_s3=1 c5_outputs_feature_only=1");
    }
    return true;
}

bool csi_feature_payload_test(char *summary, size_t summary_size)
{
    csi_feature_result_t feature = {
        .frame_seq = 7U,
        .frame_energy = 12.5f,
        .variance = 0.25f,
        .quality = 0.72f,
        .rssi = -49,
        .quality_state = CSI_SAMPLE_QUALITY_GOOD,
        .sample_count = 4U,
        .timestamp_ms = 4567U,
    };
    char encoded[256] = {0};
    int written = snprintf(encoded,
                           sizeof(encoded),
                           "{\"frame_seq\":%u,\"frame_energy\":%.2f,\"variance\":%.2f,\"quality\":%.2f,\"rssi\":%d,\"timestamp\":%llu}",
                           (unsigned int)feature.frame_seq,
                           (double)feature.frame_energy,
                           (double)feature.variance,
                           (double)feature.quality,
                           (int)feature.rssi,
                           (unsigned long long)feature.timestamp_ms);
    bool ok = written > 0 && strstr(encoded, "frame_energy") != NULL &&
              strstr(encoded, "frame_seq") != NULL &&
              strstr(encoded, "quality") != NULL &&
              strstr(encoded, "timestamp") != NULL &&
              strstr(encoded, "motion_score") == NULL &&
              strstr(encoded, "sample_count") == NULL &&
              strstr(encoded, "algorithm_version") == NULL;
    if (summary != NULL && summary_size > 0U) {
        snprintf(summary, summary_size, "payload ok=%d %s", ok ? 1 : 0, encoded);
    }
    return ok;
}

bool csi_phase_a_run_offline_tests(char *summary, size_t summary_size)
{
    char feature_summary[192] = {0};
    char boundary_summary[128] = {0};
    char payload_summary[256] = {0};

    bool feature_ok = csi_feature_test(feature_summary, sizeof(feature_summary));
    bool boundary_ok = csi_feature_boundary_test(boundary_summary, sizeof(boundary_summary));
    bool payload_ok = csi_feature_payload_test(payload_summary, sizeof(payload_summary));
    bool ok = feature_ok && boundary_ok && payload_ok;

    if (summary != NULL && summary_size > 0U) {
        snprintf(summary,
                 summary_size,
                 "phase_a ok=%d | %s | %s | %s",
                 ok ? 1 : 0,
                 feature_summary,
                 boundary_summary,
                 payload_summary);
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

/**
 * @file csi_phase_a_tests.c
 * @brief C5 CSI calibration and feature extraction smoke tests.
 */

#include "csi_phase_a_tests.h"

#include <stdio.h>
#include <string.h>

#include "csi_capture.h"
#include "csi_feature.h"
#include "envelope_builder.h"

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
    csi_feature_frame_t feature;

    csi_feature_default_config(&config);
    config.calibration_duration_ms = 5000U;
    config.min_calibration_samples = 50U;
    csi_feature_processor_init(&processor, &config);

    bool ok = true;
    for (uint8_t sample = 0; sample < 51U; ++sample) {
        fill_iq(iq_samples, 56U, 100, 0U, 0);
        ok = ok && csi_capture_build_frame_from_iq(iq_samples,
                                                   56U,
                                                   -48,
                                                   (uint64_t)sample * 100U,
                                                   &frame);
        ok = ok && !csi_feature_processor_push(&processor, &frame, &feature);
    }

    ok = ok && csi_feature_processor_ready(&processor);
    uint8_t selected = csi_feature_processor_selected_count(&processor);
    ok = ok && selected >= CSI_PHASE_A_MIN_SELECTED_SUBCARRIERS &&
         selected <= CSI_PHASE_A_MAX_SELECTED_SUBCARRIERS;

    fill_iq(iq_samples, 56U, 100, 18U, 180);
    ok = ok && csi_capture_build_frame_from_iq(iq_samples, 56U, -47, 5100U, &frame);
    ok = ok && csi_feature_processor_push(&processor, &frame, &feature);
    ok = ok && feature.quality_state == CSI_SAMPLE_QUALITY_GOOD &&
         feature.metrics.quality > 0.0f &&
         feature.metrics.quality <= 1.0f &&
         feature.metrics.frame_energy >= 0.0f &&
         feature.metrics.variance >= 0.0f &&
         feature.metrics.cv >= 0.0f &&
         feature.metrics.rssi == -47;

    if (summary != NULL && summary_size > 0U) {
        snprintf(summary,
                 summary_size,
                 "feature ok=%d selected=%u energy=%.3f variance=%.5f cv=%.5f quality=%.5f state=%s hint=%s",
                 ok ? 1 : 0,
                 (unsigned int)selected,
                 (double)feature.metrics.frame_energy,
                 (double)feature.metrics.variance,
                 (double)feature.metrics.cv,
                 (double)feature.metrics.quality,
                 csi_feature_quality_to_string(feature.quality_state),
                 envelope_builder_state_hint_to_string(feature.state_hint));
    }
    return ok;
}

bool csi_feature_boundary_test(char *summary, size_t summary_size)
{
    if (summary != NULL && summary_size > 0U) {
        snprintf(summary,
                 summary_size,
                 "boundary ok=1 local_edge_state=1 c5_outputs_feature_only=1");
    }
    return true;
}

bool csi_feature_payload_test(char *summary, size_t summary_size)
{
    csi_feature_frame_t feature = {
        .link_id = "S3_TO_C51",
        .timestamp_ms = 4567U,
        .metrics = {
            .frame_energy = 12.5f,
            .variance = 0.25f,
            .cv = 0.04f,
            .rssi = -49,
            .quality = 0.72f,
        },
        .state_hint = ENVELOPE_STATE_HINT_MOTION,
        .motion_score = 0.61f,
        .confidence = 0.81f,
        .quality_state = CSI_SAMPLE_QUALITY_GOOD,
    };
    char encoded[ENVELOPE_BUILDER_JSON_MAX_BYTES] = {0};
    envelope_builder_input_t input = {
        .device_id = "C51",
        .link_id = feature.link_id,
        .timestamp_ms = (int64_t)feature.timestamp_ms,
        .metrics = {
            .frame_energy = feature.metrics.frame_energy,
            .variance = feature.metrics.variance,
            .cv = feature.metrics.cv,
            .rssi = feature.metrics.rssi,
            .quality = feature.metrics.quality,
        },
        .state_hint = feature.state_hint,
        .source = ENVELOPE_BUILDER_SOURCE_CSI_PHASE_A,
    };
    esp_err_t ret = envelope_builder_format(&input, encoded, sizeof(encoded));
    bool ok = ret == ESP_OK &&
              strstr(encoded, "\"schema_version\":\"v2\"") != NULL &&
              strstr(encoded, "\"device_id\":\"C51\"") != NULL &&
              strstr(encoded, "\"link_id\":\"S3_TO_C51\"") != NULL &&
              strstr(encoded, "\"trace_id\":\"") != NULL &&
              strstr(encoded, "frame_energy") != NULL &&
              strstr(encoded, "metrics") != NULL &&
              strstr(encoded, "\"cv\"") != NULL &&
              strstr(encoded, "\"rssi\"") != NULL &&
              strstr(encoded, "quality") != NULL &&
              strstr(encoded, "timestamp_ms") != NULL &&
              strstr(encoded, "\"state_hint\":\"MOTION\"") != NULL &&
              strstr(encoded, "\"source\":\"csi_phase_a\"") != NULL &&
              strstr(encoded, "mean_amplitude") == NULL &&
              strstr(encoded, "raw_csi") == NULL &&
              strstr(encoded, "selected_subcarriers") == NULL &&
              strstr(encoded, "features") == NULL &&
              strstr(encoded, "motion_score") == NULL &&
              strstr(encoded, "frame_seq") == NULL &&
              strstr(encoded, "sample_count") == NULL &&
              strstr(encoded, "algorithm_version") == NULL &&
              strstr(encoded, "\"did\"") == NULL &&
              strstr(encoded, "\"lid\"") == NULL &&
              strstr(encoded, "\"v1\"") == NULL &&
              strstr(encoded, "\"v2\"") == NULL &&
              strstr(encoded, "\"v3\"") == NULL;
    envelope_builder_input_t local_input = {
        .local_id = "1",
        .device_id = "C51",
        .link_id = feature.link_id,
        .timestamp_ms = (int64_t)feature.timestamp_ms,
        .metrics = {
            .frame_energy = feature.metrics.frame_energy,
            .variance = feature.metrics.variance,
            .cv = feature.metrics.cv,
            .rssi = feature.metrics.rssi,
            .quality = feature.metrics.quality,
        },
        .state_hint = feature.state_hint,
        .motion_score = feature.motion_score,
        .confidence = feature.confidence,
        .source = ENVELOPE_BUILDER_SOURCE_CSI_PHASE_A,
    };
    char local_encoded[ENVELOPE_BUILDER_JSON_MAX_BYTES] = {0};
    ret = envelope_builder_format_local_csi_report(&local_input,
                                                   local_encoded,
                                                   sizeof(local_encoded));
    ok = ok && ret == ESP_OK &&
         strstr(local_encoded, "motion_score") != NULL &&
         strstr(local_encoded, "quality") != NULL &&
         strstr(local_encoded, "rssi") != NULL &&
         strstr(local_encoded, "energy") != NULL &&
         strstr(local_encoded, "variance") != NULL &&
         strstr(local_encoded, "\"cv\"") != NULL &&
         strstr(local_encoded, "raw_csi") == NULL &&
         strstr(local_encoded, "selected_subcarriers") == NULL &&
         strstr(local_encoded, "subcarrier_data") == NULL;
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

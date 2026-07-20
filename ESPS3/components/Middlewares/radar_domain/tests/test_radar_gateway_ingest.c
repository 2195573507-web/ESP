#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "radar_gateway_ingest.h"

static radar_gateway_sample_t sample(uint8_t local_id,
                                     uint32_t request_q,
                                     uint32_t frame_seq,
                                     int16_t x_mm)
{
    radar_gateway_sample_t value = {
        .local_id = local_id,
        .link_state = 5U,
        .sample_valid = true,
        .request_uptime_ms = request_q * 100U,
        .request_sequence = request_q,
        .frame_seq = frame_seq,
        .frame_uptime_ms = frame_seq * 100U,
        .target_count = 1U,
    };
    value.targets[0] = (radar_gateway_target_t){
        .slot = 0U,
        .x_mm = x_mm,
        .y_mm = 1000,
        .speed_cm_s = 20,
        .resolution_mm = 320U,
        .distance_mm = 1000U,
        .valid = true,
    };
    return value;
}

static void test_schema_and_identity_boundary(void)
{
    const char valid_json[] =
        "{\"p\":2,\"id\":2,\"t\":\"radar\",\"u\":123456,\"q\":10,\"v\":"
        "{\"link_state\":5,\"sample_valid\":1,\"frame_seq\":234,\"frame_uptime_ms\":123450,"
        "\"target_count\":1,\"targets\":[{\"slot\":0,\"x_mm\":1200,\"y_mm\":800,"
        "\"speed_cm_s\":15,\"resolution_mm\":320,\"distance_mm\":1442}]}}";
    radar_gateway_sample_t parsed = {0};
    radar_gateway_output_t output = {0};
    assert(radar_gateway_ingest_process_json(valid_json,
                                             strlen(valid_json),
                                             "sensair_shuttle_02",
                                             1000U,
                                             &parsed,
                                             &output) == RADAR_GATEWAY_INGEST_ACCEPTED);
    assert(parsed.local_id == 2U && parsed.request_sequence == 10U);
    assert(parsed.targets[0].slot == 0U && parsed.targets[0].resolution_mm == 320U);
    assert(output.local_id == 2U && output.radar_online);
    assert(output.count_summary.raw_target_count == 1U);
    assert(output.count_summary.accepted_target_count == 0U);
    assert(output.count_summary.visible_track_count == 0U);
    assert(output.count_summary.source_person_count == 0U);
    assert(radar_gateway_ingest_process_json(valid_json,
                                             strlen(valid_json),
                                             "sensair_shuttle_01",
                                             1010U,
                                             NULL,
                                             NULL) == RADAR_GATEWAY_INGEST_IDENTITY_MISMATCH);

    const char forbidden_confidence[] =
        "{\"p\":2,\"id\":1,\"t\":\"radar\",\"u\":1,\"q\":1,\"v\":"
        "{\"link_state\":5,\"sample_valid\":1,\"frame_seq\":1,\"frame_uptime_ms\":1,"
        "\"target_count\":1,\"targets\":[{\"slot\":0,\"x_mm\":1,\"y_mm\":1,"
        "\"speed_cm_s\":0,\"resolution_mm\":1,\"distance_mm\":1,\"confidence\":100}]}}";
    assert(radar_gateway_ingest_process_json(forbidden_confidence,
                                             strlen(forbidden_confidence),
                                             "sensair_shuttle_01",
                                             1020U,
                                             NULL,
                                             NULL) == RADAR_GATEWAY_INGEST_INVALID_TARGETS);
}

static void test_sequence_and_source_isolation(void)
{
    radar_gateway_output_t output = {0};
    radar_gateway_sample_t c51_first = sample(1U, 1U, 1U, 100);
    assert(radar_gateway_ingest_admit(&c51_first, 1U, 2000U, &output) ==
           RADAR_GATEWAY_INGEST_ACCEPTED);
    assert(output.local_id == 1U);
    assert(radar_gateway_ingest_admit(&c51_first, 1U, 2010U, &output) ==
           RADAR_GATEWAY_INGEST_DUPLICATE);

    radar_gateway_sample_t c51_conflict = c51_first;
    c51_conflict.targets[0].x_mm = 200;
    assert(radar_gateway_ingest_admit(&c51_conflict, 1U, 2020U, NULL) ==
           RADAR_GATEWAY_INGEST_SEQUENCE_CONFLICT);

    radar_gateway_sample_t c51_second = sample(1U, 2U, 2U, 200);
    assert(radar_gateway_ingest_admit(&c51_second, 1U, 2100U, &output) ==
           RADAR_GATEWAY_INGEST_ACCEPTED);
    radar_gateway_sample_t c51_old = sample(1U, 1U, 1U, 100);
    assert(radar_gateway_ingest_admit(&c51_old, 1U, 2200U, NULL) ==
           RADAR_GATEWAY_INGEST_SEQUENCE_BACKWARD);

    radar_gateway_output_t c52 = {0};
    assert(radar_gateway_ingest_get_output(2U, &c52));
    assert(c52.local_id == 2U && c52.radar_online);

    radar_gateway_sample_t c51_offline = sample(1U, 3U, 2U, 0);
    c51_offline.link_state = 6U;
    c51_offline.sample_valid = false;
    c51_offline.target_count = 0U;
    memset(c51_offline.targets, 0, sizeof(c51_offline.targets));
    assert(radar_gateway_ingest_admit(&c51_offline, 1U, 2300U, &output) ==
           RADAR_GATEWAY_INGEST_ACCEPTED);
    assert(!output.radar_online && output.occupancy == RADAR_OCCUPANCY_UNKNOWN);
    radar_gateway_output_t c52_after = {0};
    assert(radar_gateway_ingest_get_output(2U, &c52_after));
    assert(c52_after.radar_online && c52_after.local_id == 2U);
}

static void test_radar_source_context_boundaries(void)
{
    RadarSourceContext *s3 = radar_source_context_mutable(RADAR_SOURCE_S3_LOCAL);
    const RadarSourceContext *c51 = radar_source_context_get(RADAR_SOURCE_C51);
    const RadarSourceContext *c52 = radar_source_context_get(RADAR_SOURCE_C52);
    assert(s3 != NULL && c51 != NULL && c52 != NULL);
    assert(s3->spatial_state != c51->spatial_state && c51->spatial_state != c52->spatial_state);
    assert(s3->tracker_state != c51->tracker_state && c51->tracker_state != c52->tracker_state);
    assert(s3->person_state != c51->person_state && c51->person_state != c52->person_state);
    assert(s3->history != c51->history && c51->history != c52->history);
    assert(s3->raw_targets != c51->raw_targets && c51->raw_targets != c52->raw_targets);
    assert(s3->filtered_targets != c51->filtered_targets &&
           c51->filtered_targets != c52->filtered_targets);
    assert(&s3->snapshot != &c51->snapshot && &c51->snapshot != &c52->snapshot);
    assert(&s3->diagnostics_state != &c51->diagnostics_state &&
           &c51->diagnostics_state != &c52->diagnostics_state);
    assert(&s3->coordinate_config != &c51->coordinate_config &&
           &c51->coordinate_config != &c52->coordinate_config);

    radar_source_context_reset(s3, 3000U);
    radar_frame_t s3_frame = {0};
    s3_frame.frame_seq = 1U;
    s3_frame.received_at_ms = 3000U;
    s3_frame.target_count = 1U;
    s3_frame.targets[0] = (radar_target_t){
        .valid = true, .x_mm = 100, .y_mm = 1000, .resolution_mm = 320U,
        .distance_mm = 1005U, .confidence = 80U,
    };
    radar_spatial_state_on_frame(s3->spatial_state, &s3_frame, true, 3000U);
    radar_spatial_snapshot_t s3_snapshot = {0};
    radar_spatial_state_get_snapshot(s3->spatial_state, &s3_snapshot);
    radar_source_context_publish(s3, &s3_snapshot, NULL, true, 1U, 3000U);

    radar_gateway_output_t output = {0};
    const radar_gateway_sample_t c51_sample = sample(1U, 4U, 3U, 200);
    const radar_gateway_sample_t c52_sample = sample(2U, 11U, 235U, 300);
    assert(radar_gateway_ingest_admit(&c51_sample, 1U, 3010U, &output) ==
           RADAR_GATEWAY_INGEST_ACCEPTED);
    assert(radar_gateway_ingest_admit(&c52_sample, 1U, 3020U, &output) ==
           RADAR_GATEWAY_INGEST_ACCEPTED);

    c51 = radar_source_context_get(RADAR_SOURCE_C51);
    c52 = radar_source_context_get(RADAR_SOURCE_C52);
    assert(s3->raw_targets[0].x_mm == 100);
    assert(c51->raw_targets[0].x_mm == 200);
    assert(c52->raw_targets[0].x_mm == 300);
    assert(s3->sequence == 1U && c51->sequence == 4U && c52->sequence == 11U);
    assert(s3->person_state->source_id == RADAR_SOURCE_S3_LOCAL);
    assert(c51->person_state->source_id == RADAR_SOURCE_C51);
    assert(c52->person_state->source_id == RADAR_SOURCE_C52);
}

int main(void)
{
    assert(RADAR_SOURCE_S3_LOCAL == 0);
    assert(RADAR_SOURCE_C51 == 1);
    assert(RADAR_SOURCE_C52 == 2);
    assert(radar_gateway_ingest_start() == ESP_OK);
    test_schema_and_identity_boundary();
    test_sequence_and_source_isolation();
    test_radar_source_context_boundaries();
    puts("S3 radar v2 ingest and source isolation tests: PASS");
    return 0;
}

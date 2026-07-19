#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "radar_gateway_ingest.h"
#include "radar_ingest.h"

static const char *valid_c52_json =
    "{\"p\":3,\"id\":2,\"t\":\"radar\",\"u\":123456,\"q\":10,\"v\":"
    "{\"device_id\":\"sensair_shuttle_02\",\"link_state\":5,\"sample_valid\":1,"
    "\"frame_seq\":234,\"frame_uptime_ms\":123450,\"target_count\":1,\"targets\":["
    "{\"target_id\":1,\"x_mm\":1200,\"y_mm\":800,\"velocity_cm_s\":15,"
    "\"confidence\":80,\"resolution_mm\":320,\"distance_mm\":1442}]}}";

static void test_v3_adapter_and_latest_only_worker(void)
{
    assert(radar_ingest_start() == ESP_OK);
    assert(radar_ingest_process_json(valid_c52_json, strlen(valid_c52_json), 1000U) ==
           RADAR_INGEST_ACCEPTED);
    radar_ingest_process_pending(1000U);

    radar_gateway_output_t output = {0};
    assert(radar_gateway_ingest_get_output(2U, &output));
    assert(output.local_id == 2U && output.radar_online && output.target_count == 1U);
    assert(output.targets[0].x_mm == 1200 && output.targets[0].speed_cm_s == 15);
    assert(output.targets[0].confidence > 0U);
    radar_ingest_history_stats_t history = {0};
    assert(radar_ingest_history_get_stats(&history));
    assert(history.count == 1U && history.capacity == RADAR_INGEST_HISTORY_DEPTH);
}

static void test_schema_and_identity_rejection(void)
{
    const char wrong_identity[] =
        "{\"p\":3,\"id\":1,\"t\":\"radar\",\"u\":1,\"q\":1,\"v\":"
        "{\"device_id\":\"sensair_shuttle_02\",\"link_state\":5,\"sample_valid\":0,"
        "\"frame_seq\":0,\"frame_uptime_ms\":0,\"target_count\":0,\"targets\":[]}}";
    assert(radar_ingest_process_json(wrong_identity, strlen(wrong_identity), 1010U) ==
           RADAR_INGEST_IDENTITY_MISMATCH);

    const char invalid_confidence[] =
        "{\"p\":3,\"id\":1,\"t\":\"radar\",\"u\":1,\"q\":1,\"v\":"
        "{\"device_id\":\"sensair_shuttle_01\",\"link_state\":5,\"sample_valid\":1,"
        "\"frame_seq\":1,\"frame_uptime_ms\":1,\"target_count\":1,\"targets\":["
        "{\"target_id\":1,\"x_mm\":1,\"y_mm\":1,\"velocity_cm_s\":0,"
        "\"confidence\":101,\"resolution_mm\":1,\"distance_mm\":1}]}}";
    assert(radar_ingest_process_json(invalid_confidence, strlen(invalid_confidence), 1020U) ==
           RADAR_INGEST_INVALID_TARGETS);
}

int main(void)
{
    test_v3_adapter_and_latest_only_worker();
    test_schema_and_identity_rejection();
    puts("S3 radar v3 adapter and latest-frame worker tests: PASS");
    return 0;
}

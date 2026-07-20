#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "radar_protocol.h"
#include "radar_registry.h"
#include "radar_remote_ingest.h"

static const char *valid_c51_json =
    "{\"schema_version\":1,\"local_id\":1,\"sequence\":1,\"uptime_ms\":1000,"
    "\"state\":\"motion\",\"target_count\":1,\"uart_online\":true,"
    "\"frame_fresh\":true,\"last_motion_age_ms\":0,\"targets\":["
    "{\"x_mm\":-782,\"y_mm\":1713,\"speed_cm_s\":-16,\"resolution_mm\":320}]}";

static radar_remote_identity_t identity(uint8_t local_id,
                                        uint32_t generation,
                                        const char *expected_peer,
                                        const char *request_peer)
{
    radar_remote_identity_t result = {
        .registered = true,
        .online = true,
        .session_live = true,
        .expected_local_id = local_id,
        .session_generation = generation,
        .expected_peer_ip = expected_peer,
        .request_peer_ip = request_peer,
    };
    return result;
}

static radar_protocol_payload_t payload(uint8_t local_id,
                                        uint32_t sequence,
                                        uint64_t uptime_ms,
                                        radar_presence_state_t state)
{
    radar_protocol_payload_t result = {
        .schema_version = RADAR_PROTOCOL_SCHEMA_VERSION,
        .local_id = local_id,
        .sequence = sequence,
        .uptime_ms = uptime_ms,
        .state = state,
        .target_count = state == RADAR_STATE_MOTION ? 1U : 0U,
        .uart_online = true,
        .frame_fresh = true,
        .last_motion_age_ms = state == RADAR_STATE_MOTION ? 0U : 100U,
    };
    if (result.target_count == 1U) {
        result.targets[0].valid = true;
        result.targets[0].x_mm = 100;
        result.targets[0].y_mm = 200;
        result.targets[0].speed_cm_s = 5;
        result.targets[0].resolution_mm = 300U;
    }
    return result;
}

static void test_protocol(void)
{
    radar_protocol_payload_t parsed;
    assert(radar_protocol_parse_json(valid_c51_json, strlen(valid_c51_json), &parsed) ==
           RADAR_PROTOCOL_OK);
    assert(parsed.local_id == 1U);
    assert(parsed.sequence == 1U);
    assert(parsed.state == RADAR_STATE_MOTION);
    assert(parsed.target_count == 1U);
    assert(parsed.targets[0].valid);
    assert(parsed.targets[0].x_mm == -782);

    const char *mismatch =
        "{\"schema_version\":1,\"local_id\":1,\"sequence\":1,\"uptime_ms\":1,"
        "\"state\":\"unknown\",\"target_count\":1,\"uart_online\":false,"
        "\"frame_fresh\":false,\"last_motion_age_ms\":4294967295,\"targets\":[]}";
    assert(radar_protocol_parse_json(mismatch, strlen(mismatch), &parsed) ==
           RADAR_PROTOCOL_INVALID_TARGETS);

    const char *legacy_field =
        "{\"schema_version\":1,\"local_id\":1,\"sequence\":1,\"uptime_ms\":1,"
        "\"state\":\"unknown\",\"target_count\":0,\"uart_online\":false,"
        "\"frame_fresh\":false,\"last_motion_age_ms\":4294967295,\"targets\":[],"
        "\"raw_csi\":[]}";
    assert(radar_protocol_parse_json(legacy_field, strlen(legacy_field), &parsed) ==
           RADAR_PROTOCOL_INVALID_SCHEMA);

    char oversized[RADAR_PROTOCOL_MAX_BODY_BYTES + 2U];
    memset(oversized, ' ', sizeof(oversized));
    oversized[sizeof(oversized) - 1U] = '\0';
    assert(radar_protocol_parse_json(oversized, sizeof(oversized) - 1U, &parsed) ==
           RADAR_PROTOCOL_TOO_LARGE);
}

static void test_remote_sequence_and_identity(void)
{
    assert(radar_registry_init());
    radar_protocol_payload_t c51 = payload(1U, 1U, 1000U, RADAR_STATE_MOTION);
    radar_remote_identity_t c51_identity = identity(1U, 1U, "192.168.4.2", "192.168.4.2");
    bool changed = false;
    assert(radar_remote_ingest_admit(&c51, &c51_identity, 1000U, &changed) ==
           RADAR_REMOTE_INGEST_ACCEPTED);
    assert(changed);

    radar_registry_entry_t c51_entry;
    radar_registry_entry_t c52_entry;
    assert(radar_registry_get(RADAR_SOURCE_C51, &c51_entry));
    assert(radar_registry_get(RADAR_SOURCE_C52, &c52_entry));
    assert(c51_entry.snapshot.state == RADAR_STATE_MOTION);
    assert(c52_entry.snapshot.state == RADAR_STATE_UNKNOWN);

    assert(radar_remote_ingest_admit(&c51, &c51_identity, 1100U, &changed) ==
           RADAR_REMOTE_INGEST_DUPLICATE);
    radar_protocol_payload_t conflict = c51;
    conflict.state = RADAR_STATE_HOLD;
    conflict.target_count = 0U;
    memset(conflict.targets, 0, sizeof(conflict.targets));
    assert(radar_remote_ingest_admit(&conflict, &c51_identity, 1200U, &changed) ==
           RADAR_REMOTE_INGEST_SEQUENCE_CONFLICT);

    c51 = payload(1U, 2U, 2000U, RADAR_STATE_HOLD);
    assert(radar_remote_ingest_admit(&c51, &c51_identity, 2000U, &changed) ==
           RADAR_REMOTE_INGEST_ACCEPTED);
    c51 = payload(1U, 1U, 1500U, RADAR_STATE_MOTION);
    assert(radar_remote_ingest_admit(&c51, &c51_identity, 2100U, &changed) ==
           RADAR_REMOTE_INGEST_SEQUENCE_BACKWARD);
    c51.uptime_ms = 100U;
    assert(radar_remote_ingest_admit(&c51, &c51_identity, 5000U, &changed) ==
           RADAR_REMOTE_INGEST_ACCEPTED);

    radar_remote_identity_t forged = c51_identity;
    forged.expected_local_id = 2U;
    assert(radar_remote_ingest_admit(&c51, &forged, 5100U, &changed) ==
           RADAR_REMOTE_INGEST_IDENTITY_MISMATCH);
    forged = c51_identity;
    forged.request_peer_ip = "192.168.4.9";
    assert(radar_remote_ingest_admit(&c51, &forged, 5200U, &changed) ==
           RADAR_REMOTE_INGEST_IDENTITY_MISMATCH);
}

static void test_source_isolation_and_freshness(void)
{
    radar_protocol_payload_t c52 = payload(2U, 1U, 8000U, RADAR_STATE_MOTION);
    radar_remote_identity_t c52_identity = identity(2U, 2U, "192.168.4.3", "192.168.4.3");
    bool changed = false;
    assert(radar_remote_ingest_admit(&c52, &c52_identity, 8000U, &changed) ==
           RADAR_REMOTE_INGEST_ACCEPTED);

    radar_remote_identity_t unregistered = c52_identity;
    unregistered.registered = false;
    assert(radar_remote_ingest_admit(&c52, &unregistered, 8050U, &changed) ==
           RADAR_REMOTE_INGEST_IDENTITY_MISMATCH);

    radar_snapshot_t local = {
        .state = RADAR_STATE_HOLD,
        .state_seq = 1U,
        .state_since_ms = 8000U,
        .last_valid_frame_ms = 8000U,
        .last_motion_ms = 7900U,
        .uart_online = true,
        .frame_fresh = true,
    };
    const radar_count_summary_t local_counts = {
        .raw_target_count = 1U,
        .accepted_target_count = 1U,
        .visible_track_count = 0U,
        .confirmed_active_track_count = 1U,
        .history_target_count = 7U,
        .visible_person_count = 0U,
        .retained_person_count = 1U,
        .business_person_count = 1U,
        .count_state = RADAR_PERSON_COUNT_ESTIMATED,
    };
    assert(radar_registry_update_local(&local, &local_counts, NULL, 8000U, &changed));

    radar_registry_note_parse_error(RADAR_SOURCE_C51);
    radar_registry_refresh(8001U);

    radar_registry_entry_t c51_entry;
    radar_registry_entry_t c52_entry;
    radar_registry_entry_t local_entry;
    assert(radar_registry_get(RADAR_SOURCE_C51, &c51_entry));
    assert(radar_registry_get(RADAR_SOURCE_C52, &c52_entry));
    assert(radar_registry_get(RADAR_SOURCE_S3_LOCAL, &local_entry));
    assert(c51_entry.snapshot.state == RADAR_STATE_UNKNOWN);
    assert(!c51_entry.source_online);
    assert(c52_entry.snapshot.state == RADAR_STATE_MOTION);
    assert(c52_entry.source_online);
    assert(local_entry.snapshot.state == RADAR_STATE_HOLD);
    assert(local_entry.source_online);
    assert(local_entry.count_summary.business_person_count == 1U);
    assert(local_entry.count_summary.retained_person_count == 1U);
    assert(local_entry.count_summary.history_target_count == 7U);
    assert(local_entry.count_summary.count_state == RADAR_PERSON_COUNT_ESTIMATED);
    assert(c51_entry.diagnostics.parse_error_count == 1U);
}

int main(void)
{
    test_protocol();
    test_remote_sequence_and_identity();
    test_source_isolation_and_freshness();
    puts("radar protocol/registry/ingest host tests: PASS");
    return 0;
}

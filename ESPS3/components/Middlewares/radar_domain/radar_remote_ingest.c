#include "radar_remote_ingest.h"

#include <string.h>

#ifndef RADAR_DOMAIN_HOST_TEST
#include "child_registry.h"
#include "protocol_adapter.h"
#include "radar_diagnostics.h"
#include "resource_manager.h"
#endif

/*
 * 远端接入先校验设备登记、在线会话、本地编号和对端 IP，再写入注册表。
 * 身份校验与序列校验分层，避免合法设备的乱序包掩盖来源伪造问题。
 */

static bool text_equal_nonempty(const char *a, const char *b)
{
    return a != NULL && b != NULL && a[0] != '\0' && b[0] != '\0' && strcmp(a, b) == 0;
}

radar_remote_ingest_result_t radar_remote_ingest_admit(
    const radar_protocol_payload_t *payload,
    const radar_remote_identity_t *identity,
    uint64_t received_at_ms,
    bool *out_state_changed)
{
    if (out_state_changed != NULL) {
        *out_state_changed = false;
    }
    if (payload == NULL || identity == NULL || received_at_ms == 0U) {
        return RADAR_REMOTE_INGEST_INVALID_ARGUMENT;
    }

    radar_source_id_t source = radar_registry_source_for_local_id(payload->local_id);
    if (source == RADAR_SOURCE_COUNT) {
        return RADAR_REMOTE_INGEST_INVALID_ARGUMENT;
    }
    if (!identity->registered || !identity->online || !identity->session_live ||
        identity->session_generation == 0U ||
        identity->expected_local_id != payload->local_id ||
        !text_equal_nonempty(identity->expected_peer_ip, identity->request_peer_ip)) {
        radar_registry_note_identity_mismatch(source);
        return RADAR_REMOTE_INGEST_IDENTITY_MISMATCH;
    }

    radar_registry_update_result_t update =
        radar_registry_update_remote(source,
                                     payload,
                                     NULL,
                                     identity->session_generation,
                                     received_at_ms,
                                     out_state_changed);
    switch (update) {
    case RADAR_REGISTRY_UPDATE_ACCEPTED:
        return RADAR_REMOTE_INGEST_ACCEPTED;
    case RADAR_REGISTRY_UPDATE_DUPLICATE:
        return RADAR_REMOTE_INGEST_DUPLICATE;
    case RADAR_REGISTRY_UPDATE_SEQUENCE_CONFLICT:
        return RADAR_REMOTE_INGEST_SEQUENCE_CONFLICT;
    case RADAR_REGISTRY_UPDATE_SEQUENCE_BACKWARD:
        return RADAR_REMOTE_INGEST_SEQUENCE_BACKWARD;
    case RADAR_REGISTRY_UPDATE_INVALID_ARGUMENT:
    default:
        return RADAR_REMOTE_INGEST_UNAVAILABLE;
    }
}

#ifndef RADAR_DOMAIN_HOST_TEST
radar_remote_ingest_result_t radar_remote_ingest_process(
    const radar_protocol_payload_t *payload,
    const char *peer_ip,
    uint64_t received_at_ms,
    bool *out_state_changed)
{
    if (payload == NULL || peer_ip == NULL || peer_ip[0] == '\0') {
        return RADAR_REMOTE_INGEST_INVALID_ARGUMENT;
    }

    radar_source_id_t source = radar_registry_source_for_local_id(payload->local_id);
    const char *expected_device_id = radar_registry_device_id(source);
    const char *mapped_device_id =
        protocol_adapter_local_device_id_to_device_id(payload->local_id);
    if (source == RADAR_SOURCE_COUNT || expected_device_id == NULL || mapped_device_id == NULL ||
        strcmp(expected_device_id, mapped_device_id) != 0) {
        if (source != RADAR_SOURCE_COUNT) {
            radar_registry_note_identity_mismatch(source);
        }
        return RADAR_REMOTE_INGEST_IDENTITY_MISMATCH;
    }

    child_registry_status_view_t child = {0};
    char registered_peer_ip[16] = {0};
    resource_manager_session_view_t session = {0};
    const bool child_found = child_registry_get_status_view(expected_device_id, &child);
    const bool peer_found = child_registry_get_peer_ip(expected_device_id,
                                                       registered_peer_ip,
                                                       sizeof(registered_peer_ip));
    const bool session_found = resource_manager_get_session(expected_device_id, &session);
    radar_remote_identity_t identity = {
        .registered = child_found && child.registered,
        .online = child_found && child.online,
        .session_live = session_found && resource_manager_is_live(expected_device_id),
        .expected_local_id = protocol_adapter_device_id_to_local_id(expected_device_id),
        .session_generation = session_found ? session.generation : 0U,
        .expected_peer_ip = peer_found ? registered_peer_ip : NULL,
        .request_peer_ip = peer_ip,
    };

    radar_remote_ingest_result_t result =
        radar_remote_ingest_admit(payload, &identity, received_at_ms, out_state_changed);
    if (result == RADAR_REMOTE_INGEST_ACCEPTED && out_state_changed != NULL &&
        *out_state_changed) {
        radar_diagnostics_log_transition(source, RADAR_DIAGNOSTICS_TRANSITION_REMOTE_UPDATE);
    }
    return result;
}
#endif

const char *radar_remote_ingest_result_name(radar_remote_ingest_result_t result)
{
    switch (result) {
    case RADAR_REMOTE_INGEST_ACCEPTED:
        return "accepted";
    case RADAR_REMOTE_INGEST_DUPLICATE:
        return "duplicate";
    case RADAR_REMOTE_INGEST_IDENTITY_MISMATCH:
        return "identity_mismatch";
    case RADAR_REMOTE_INGEST_SEQUENCE_CONFLICT:
        return "sequence_conflict";
    case RADAR_REMOTE_INGEST_SEQUENCE_BACKWARD:
        return "sequence_backward";
    case RADAR_REMOTE_INGEST_UNAVAILABLE:
        return "unavailable";
    case RADAR_REMOTE_INGEST_INVALID_ARGUMENT:
        return "invalid_argument";
    default:
        return "unknown";
    }
}

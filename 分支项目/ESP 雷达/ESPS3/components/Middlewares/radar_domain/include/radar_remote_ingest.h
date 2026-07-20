#ifndef RADAR_REMOTE_INGEST_H
#define RADAR_REMOTE_INGEST_H

#include <stdbool.h>
#include <stdint.h>

#include "radar_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* 以下字段均来自当前连接会话，不能由请求体自行声明。 */
    bool registered;
    bool online;
    bool session_live;
    uint8_t expected_local_id;
    uint32_t session_generation;
    const char *expected_peer_ip;
    const char *request_peer_ip;
} radar_remote_identity_t;

typedef enum {
    RADAR_REMOTE_INGEST_ACCEPTED = 0,
    RADAR_REMOTE_INGEST_DUPLICATE,
    RADAR_REMOTE_INGEST_IDENTITY_MISMATCH,
    RADAR_REMOTE_INGEST_SEQUENCE_CONFLICT,
    RADAR_REMOTE_INGEST_SEQUENCE_BACKWARD,
    RADAR_REMOTE_INGEST_UNAVAILABLE,
    RADAR_REMOTE_INGEST_INVALID_ARGUMENT,
} radar_remote_ingest_result_t;

radar_remote_ingest_result_t radar_remote_ingest_admit(
    const radar_protocol_payload_t *payload,
    const radar_remote_identity_t *identity,
    uint64_t received_at_ms,
    bool *out_state_changed);

#ifndef RADAR_DOMAIN_HOST_TEST
radar_remote_ingest_result_t radar_remote_ingest_process(
    const radar_protocol_payload_t *payload,
    const char *peer_ip,
    uint64_t received_at_ms,
    bool *out_state_changed);
#endif

const char *radar_remote_ingest_result_name(radar_remote_ingest_result_t result);

#ifdef __cplusplus
}
#endif

#endif

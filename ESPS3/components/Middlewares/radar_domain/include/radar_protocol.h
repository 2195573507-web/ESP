#ifndef RADAR_PROTOCOL_H
#define RADAR_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ld2450_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RADAR_PROTOCOL_SCHEMA_VERSION 1U
#define RADAR_PROTOCOL_MAX_BODY_BYTES 768U

/* C51/C52 到 S3 的雷达状态协议版本和单包上限。 */

typedef struct {
    uint8_t schema_version;
    uint8_t local_id;
    uint32_t sequence;
    uint64_t uptime_ms;
    radar_presence_state_t state;
    uint8_t target_count;
    bool uart_online;
    bool frame_fresh;
    uint32_t last_motion_age_ms;
    radar_target_t targets[LD2450_MAX_TARGETS];
} radar_protocol_payload_t;

typedef enum {
    RADAR_PROTOCOL_OK = 0,
    RADAR_PROTOCOL_INVALID_ARGUMENT,
    RADAR_PROTOCOL_TOO_LARGE,
    RADAR_PROTOCOL_INVALID_JSON,
    RADAR_PROTOCOL_INVALID_SCHEMA,
    RADAR_PROTOCOL_INVALID_LOCAL_ID,
    RADAR_PROTOCOL_INVALID_SEQUENCE,
    RADAR_PROTOCOL_INVALID_STATE,
    RADAR_PROTOCOL_INVALID_TARGETS,
} radar_protocol_result_t;

radar_protocol_result_t radar_protocol_parse_json(const char *json,
                                                  size_t json_len,
                                                  radar_protocol_payload_t *out);
bool radar_protocol_payload_equal(const radar_protocol_payload_t *a,
                                  const radar_protocol_payload_t *b);
const char *radar_protocol_result_name(radar_protocol_result_t result);

#ifdef __cplusplus
}
#endif

#endif

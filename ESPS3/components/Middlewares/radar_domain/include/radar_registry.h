#ifndef RADAR_REGISTRY_H
#define RADAR_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radar_protocol.h"
#include "radar_source_context.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RADAR_REGISTRY_DEVICE_ID_LEN RADAR_SOURCE_CONTEXT_DEVICE_ID_LEN
#define RADAR_REGISTRY_ROOM_ID_LEN RADAR_SOURCE_CONTEXT_ROOM_ID_LEN
#define RADAR_REGISTRY_FRESHNESS_TIMEOUT_MS 3000U

/* 接入层统一维护的逻辑雷达源；S3_LOCAL 为本地 UART，C51/C52 为远端。 */

typedef struct {
    uint32_t accepted_count;
    uint32_t duplicate_count;
    uint32_t sequence_reject_count;
    uint32_t identity_mismatch_count;
    uint32_t parse_error_count;
    uint32_t reboot_reset_count;
    uint32_t freshness_expiry_count;
} radar_registry_diagnostics_t;

typedef struct {
    uint32_t parser_bad_header;
    uint32_t parser_bad_length;
    uint32_t parser_bad_tail;
    uint32_t parser_skipped_bytes;
    uint32_t uart_read_driver_error;
} radar_registry_local_diagnostics_t;

typedef struct {
    radar_source_id_t source;
    char device_id[RADAR_REGISTRY_DEVICE_ID_LEN];
    char room_id[RADAR_REGISTRY_ROOM_ID_LEN];
    bool source_online;
    radar_snapshot_t snapshot;
    /* A target array remains a motion-track transport value.  Business person
     * counts live here so retained continuity never needs a fake target. */
    radar_count_summary_t count_summary;
    uint32_t sequence;
    uint64_t source_uptime_ms;
    uint32_t session_generation;
    uint64_t last_report_ms;
    uint64_t last_state_change_ms;
    radar_registry_diagnostics_t diagnostics;
} radar_registry_entry_t;

/* Room state is derived from one source snapshot.  It never owns tracker or
 * person state and therefore cannot overwrite another source. */
typedef struct {
    radar_source_id_t source_id;
    char source[RADAR_SOURCE_CONTEXT_NAME_LEN];
    char device_id[RADAR_REGISTRY_DEVICE_ID_LEN];
    char room_id[RADAR_REGISTRY_ROOM_ID_LEN];
    bool occupied;
    radar_presence_state_t presence;
    radar_motion_state_t motion;
    uint8_t person_count;
    uint64_t last_update_ms;
} RadarRoomState;

typedef struct {
    uint8_t occupied_room_count;
    RadarRoomState occupied_rooms[RADAR_SOURCE_COUNT];
    uint8_t home_person_count;
} RadarHomeState;

typedef RadarHomeState radar_home_presence_t;

typedef enum {
    RADAR_REGISTRY_UPDATE_ACCEPTED = 0,
    RADAR_REGISTRY_UPDATE_DUPLICATE,
    RADAR_REGISTRY_UPDATE_SEQUENCE_CONFLICT,
    RADAR_REGISTRY_UPDATE_SEQUENCE_BACKWARD,
    RADAR_REGISTRY_UPDATE_INVALID_ARGUMENT,
} radar_registry_update_result_t;

bool radar_registry_init(void);
radar_source_id_t radar_registry_source_for_local_id(uint8_t local_id);
const char *radar_registry_source_name(radar_source_id_t source);
const char *radar_registry_device_id(radar_source_id_t source);
const char *radar_registry_room_id(radar_source_id_t source);
bool radar_registry_get(radar_source_id_t source, radar_registry_entry_t *out);
size_t radar_registry_snapshot(radar_registry_entry_t *out, size_t capacity);
void radar_registry_get_home_presence(radar_home_presence_t *out);
void radar_registry_get_home_state(RadarHomeState *out);
radar_registry_update_result_t radar_registry_update_remote(
    radar_source_id_t source,
    const radar_protocol_payload_t *payload,
    const radar_count_summary_t *count_summary,
    uint32_t session_generation,
    uint64_t received_at_ms,
    bool *out_state_changed);
bool radar_registry_update_local(const radar_snapshot_t *snapshot,
                                 const radar_count_summary_t *count_summary,
                                 const radar_registry_local_diagnostics_t *service_diagnostics,
                                 uint64_t received_at_ms,
                                 bool *out_state_changed);
void radar_registry_note_parse_error(radar_source_id_t source);
void radar_registry_note_identity_mismatch(radar_source_id_t source);
void radar_registry_refresh(uint64_t now_ms);
uint32_t radar_registry_unattributed_parse_errors(void);

#ifdef __cplusplus
}
#endif

#endif

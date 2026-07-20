#ifndef RADAR_DIAGNOSTICS_H
#define RADAR_DIAGNOSTICS_H

#include "esp_err.h"
#include "radar_registry.h"
#include "radar_spatial_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RADAR_DIAGNOSTICS_TRANSITION_LOCAL_UART = 0,
    RADAR_DIAGNOSTICS_TRANSITION_REMOTE_UPDATE,
    RADAR_DIAGNOSTICS_TRANSITION_FRESHNESS_OR_UPDATE,
} radar_diagnostics_transition_reason_t;

#define RADAR_DIAG_SOURCE_TEXT_LEN 16U
#define RADAR_DIAG_STATE_TEXT_LEN 16U
#define RADAR_DIAG_REASON_TEXT_LEN 32U

typedef struct {
    radar_source_id_t source;
    char source_name[RADAR_DIAG_SOURCE_TEXT_LEN];
    char device_id[RADAR_REGISTRY_DEVICE_ID_LEN];
    char room_id[RADAR_REGISTRY_ROOM_ID_LEN];
    char state[RADAR_DIAG_STATE_TEXT_LEN];
    bool source_online;
    radar_snapshot_t snapshot;
    uint32_t sequence;
    uint32_t session_generation;
    radar_registry_diagnostics_t diagnostics;
} radar_diag_registry_snapshot_t;

/* Text is always owned by this value snapshot; it contains no string pointers. */
typedef struct {
    radar_diag_registry_snapshot_t registry[RADAR_SOURCE_COUNT];
    size_t registry_count;
    uint32_t unattributed_parse_errors;
    bool has_local_spatial;
    radar_spatial_snapshot_t local_spatial;
    char sensor_state[RADAR_DIAG_STATE_TEXT_LEN];
    char occupancy_state[RADAR_DIAG_STATE_TEXT_LEN];
    char motion_state[RADAR_DIAG_STATE_TEXT_LEN];
    char recovery_state[RADAR_DIAG_STATE_TEXT_LEN];
    char transition_reason[RADAR_DIAG_REASON_TEXT_LEN];
} radar_diag_snapshot_t;

esp_err_t radar_diagnostics_start(void);
esp_err_t radar_diagnostics_stop(void);
bool radar_diag_snapshot_copy(radar_diag_snapshot_t *out);
void radar_diagnostics_log_transition(radar_source_id_t source,
                                      radar_diagnostics_transition_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif

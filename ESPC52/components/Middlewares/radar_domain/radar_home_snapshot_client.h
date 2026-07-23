#ifndef RADAR_HOME_SNAPSHOT_CLIENT_H
#define RADAR_HOME_SNAPSHOT_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "radar_board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RADAR_HOME_SNAPSHOT_MAX_SOURCES 3U
#define RADAR_HOME_SNAPSHOT_SOURCE_NAME_MAX 16U
#define RADAR_HOME_SNAPSHOT_ROOM_MAX 32U
#define RADAR_HOME_SNAPSHOT_LOCAL_SOURCE_ID RADAR_BOARD_LOCAL_ID

typedef enum {
    RADAR_HOME_MOTION_UNKNOWN = 0,
    RADAR_HOME_MOTION_NONE,
    RADAR_HOME_MOTION_STILL_CANDIDATE,
    RADAR_HOME_MOTION_MOVING,
} radar_home_motion_t;

typedef struct {
    uint8_t source_id;
    char source[RADAR_HOME_SNAPSHOT_SOURCE_NAME_MAX];
    char room[RADAR_HOME_SNAPSHOT_ROOM_MAX];
    bool online;
    bool occupied;
    radar_home_motion_t motion;
    uint8_t person_count;
} radar_home_snapshot_source_t;

typedef struct {
    bool valid;
    uint64_t received_at_ms;
    uint32_t generation;
    uint8_t source_count;
    radar_home_snapshot_source_t sources[RADAR_HOME_SNAPSHOT_MAX_SOURCES];
    bool home_known;
    bool home_occupied;
    uint8_t home_person_count;
    uint8_t home_room_count;
} radar_home_snapshot_t;

typedef struct {
    uint32_t request_count;
    uint32_t success_count;
    uint32_t transport_error_count;
    uint32_t parse_error_count;
} radar_home_snapshot_client_stats_t;

/* Polls the S3-owned UI snapshot at a bounded rate. It never reads raw radar data. */
void radar_home_snapshot_client_poll(uint64_t now_ms);
/* Copies the last valid S3 snapshot; false means LCD must render snapshot-unavailable. */
bool radar_home_snapshot_client_get(radar_home_snapshot_t *out);
void radar_home_snapshot_client_get_stats(radar_home_snapshot_client_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* RADAR_HOME_SNAPSHOT_CLIENT_H */

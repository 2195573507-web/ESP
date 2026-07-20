#ifndef RADAR_HOME_SNAPSHOT_H
#define RADAR_HOME_SNAPSHOT_H

#include <stdbool.h>
#include <stdint.h>

#include "radar_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RADAR_HOME_SNAPSHOT_MAX_ROOMS RADAR_SOURCE_COUNT

typedef struct {
    bool known;
    char room_id[RADAR_REGISTRY_ROOM_ID_LEN];
    bool occupied;
    uint8_t person_count;
    char dominant_source[RADAR_SOURCE_CONTEXT_NAME_LEN];
    uint64_t timestamp_ms;
} radar_home_snapshot_room_t;

typedef struct {
    bool occupancy_known;
    bool occupied;
    uint8_t person_count;
    uint8_t room_count;
    uint64_t timestamp_ms;
    radar_home_snapshot_room_t rooms[RADAR_HOME_SNAPSHOT_MAX_ROOMS];
} radar_home_snapshot_t;

/* Read-only HOME view derived from the source-room-HOME aggregation layer. */
bool radar_home_snapshot_get(radar_home_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif

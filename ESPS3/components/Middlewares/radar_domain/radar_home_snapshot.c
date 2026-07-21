#include "radar_home_snapshot.h"

#include <limits.h>
#include <string.h>

static void copy_text(char *out, size_t out_size, const char *value)
{
    if (out == NULL || out_size == 0U) return;
    if (value == NULL) {
        out[0] = '\0';
        return;
    }
    size_t length = strlen(value);
    if (length >= out_size) length = out_size - 1U;
    memcpy(out, value, length);
    out[length] = '\0';
}

static int find_room(const radar_home_snapshot_t *snapshot, const char *room_id)
{
    const uint8_t room_count = snapshot->room_count > RADAR_HOME_SNAPSHOT_MAX_ROOMS
        ? RADAR_HOME_SNAPSHOT_MAX_ROOMS : snapshot->room_count;
    for (uint8_t index = 0U; index < room_count; ++index) {
        if (strcmp(snapshot->rooms[index].room_id, room_id) == 0) return (int)index;
    }
    return -1;
}

static const RadarRoomState *find_occupied_source(const RadarHomeState *home,
                                                   radar_source_id_t source)
{
    const uint8_t room_count = home->occupied_room_count > RADAR_SOURCE_COUNT
        ? RADAR_SOURCE_COUNT : home->occupied_room_count;
    for (uint8_t index = 0U; index < room_count; ++index) {
        if (home->occupied_rooms[index].source_id == source) return &home->occupied_rooms[index];
    }
    return NULL;
}

bool radar_home_snapshot_get(radar_home_snapshot_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));

    RadarHomeState home = {0};
    radar_registry_get_home_state(&home);
    radar_registry_entry_t sources[RADAR_SOURCE_COUNT] = {0};
    const size_t source_count = radar_registry_snapshot(sources, RADAR_SOURCE_COUNT);
    if (source_count == 0U) return false;

    for (size_t index = 0U; index < source_count; ++index) {
        const radar_registry_entry_t *source = &sources[index];
        if (!source->source_online || source->last_report_ms == 0U ||
            source->room_id[0] == '\0') {
            continue;
        }
        int room_index = find_room(out, source->room_id);
        if (room_index < 0) {
            if (out->room_count >= RADAR_HOME_SNAPSHOT_MAX_ROOMS) continue;
            room_index = (int)out->room_count++;
            copy_text(out->rooms[room_index].room_id, sizeof(out->rooms[room_index].room_id),
                      source->room_id);
        }

        radar_home_snapshot_room_t *room = &out->rooms[room_index];
        const RadarRoomState *occupied = find_occupied_source(&home, source->source);
        room->known = true;
        if (occupied != NULL) {
            room->occupied = true;
            const uint16_t count = (uint16_t)room->person_count + occupied->person_count;
            room->person_count = count > UINT8_MAX ? UINT8_MAX : (uint8_t)count;
            if (occupied->last_update_ms >= room->timestamp_ms) {
                copy_text(room->dominant_source, sizeof(room->dominant_source), occupied->source);
                room->timestamp_ms = occupied->last_update_ms;
            }
        } else if (room->timestamp_ms == 0U || source->last_report_ms >= room->timestamp_ms) {
            copy_text(room->dominant_source, sizeof(room->dominant_source),
                      radar_registry_source_name(source->source));
            room->timestamp_ms = source->last_report_ms;
        }
    }

    const uint8_t room_count = out->room_count > RADAR_HOME_SNAPSHOT_MAX_ROOMS
        ? RADAR_HOME_SNAPSHOT_MAX_ROOMS : out->room_count;
    for (uint8_t index = 0U; index < room_count; ++index) {
        const radar_home_snapshot_room_t *room = &out->rooms[index];
        out->occupied = out->occupied || room->occupied;
        const uint16_t count = (uint16_t)out->person_count + room->person_count;
        out->person_count = count > UINT8_MAX ? UINT8_MAX : (uint8_t)count;
        if (room->timestamp_ms > out->timestamp_ms) out->timestamp_ms = room->timestamp_ms;
    }
    out->occupancy_known = out->room_count > 0U;
    return true;
}

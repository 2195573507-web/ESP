#include "habit_rule_adapter.h"

#include <stdio.h>
#include <string.h>

#include "habit_rule_engine.h"
#include "radar_home_snapshot.h"

static bool habit_rule_snapshot(void *context, habit_room_snapshot_t *out)
{
    static size_t next_room;
    (void)context;
    radar_home_snapshot_t snapshot;
    if (out == NULL || !radar_home_snapshot_get(&snapshot) || !snapshot.occupancy_known ||
        snapshot.room_count == 0U) return false;
    const size_t room_count = snapshot.room_count > RADAR_HOME_SNAPSHOT_MAX_ROOMS
        ? RADAR_HOME_SNAPSHOT_MAX_ROOMS : snapshot.room_count;
    if (room_count == 0U) return false;
    const radar_home_snapshot_room_t *room = &snapshot.rooms[next_room % room_count];
    ++next_room;
    if (!room->known || room->room_id[0] == '\0' || room->timestamp_ms == 0U) return false;
    memset(out, 0, sizeof(*out));
    out->occupied_known = true;
    out->occupied = room->occupied;
    out->person_count = room->person_count;
    out->room = room->room_id;
    out->source = room->dominant_source;
    out->monotonic_ms = room->timestamp_ms;
    return true;
}

esp_err_t habit_rule_adapter_start(void)
{
    return habit_rule_engine_start(habit_rule_snapshot, NULL);
}

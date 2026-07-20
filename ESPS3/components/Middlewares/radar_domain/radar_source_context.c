#include "radar_source_context.h"

#include <string.h>

#include <limits.h>

static RadarSourceContext s_contexts[RADAR_SOURCE_COUNT];
static bool s_initialized;

static const char *const s_source_names[RADAR_SOURCE_COUNT] = {
    "S3_LOCAL",
    "C51",
    "C52",
};

static const char *const s_device_ids[RADAR_SOURCE_COUNT] = {
    "sensair_s3_gateway_01",
    "sensair_shuttle_01",
    "sensair_shuttle_02",
};

static const char *const s_room_ids[RADAR_SOURCE_COUNT] = {
    "s3_local",
    "living_room",
    "bedroom",
};

static const radar_transport_type_t s_transports[RADAR_SOURCE_COUNT] = {
    RADAR_TRANSPORT_S3_LOCAL_UART,
    RADAR_TRANSPORT_C5_BLE_HTTP,
    RADAR_TRANSPORT_C5_BLE_HTTP,
};

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

static radar_presence_state_t presence_from_snapshot(const radar_spatial_snapshot_t *snapshot)
{
    if (snapshot == NULL || snapshot->sensor_state != RADAR_SENSOR_VALID) {
        return RADAR_STATE_UNKNOWN;
    }
    switch (snapshot->occupancy_state) {
    case RADAR_OCCUPANCY_PRESENT:
        return snapshot->motion_state == RADAR_MOTION_MOVING
            ? RADAR_STATE_MOTION : RADAR_STATE_PRESENT;
    case RADAR_OCCUPANCY_HOLD:
        return RADAR_STATE_HOLD;
    case RADAR_OCCUPANCY_VACANT_INFERRED:
        return RADAR_STATE_VACANT_INFERRED;
    case RADAR_OCCUPANCY_UNKNOWN:
    default:
        return RADAR_STATE_UNKNOWN;
    }
}

bool radar_source_context_is_valid(radar_source_id_t source)
{
    return source >= RADAR_SOURCE_S3_LOCAL && source < RADAR_SOURCE_COUNT;
}

static void wire_state_aliases(RadarSourceContext *context)
{
    context->spatial_state = &context->spatial_storage;
    context->tracker_state = &context->spatial_storage.tracker;
    context->person_state = &context->spatial_storage.person_continuity;
}

static radar_motion_state_t motion_from_presence(radar_presence_state_t presence)
{
    if (presence == RADAR_STATE_MOTION) return RADAR_MOTION_MOVING;
    if (presence == RADAR_STATE_PRESENT || presence == RADAR_STATE_HOLD ||
        presence == RADAR_STATE_VACANT_INFERRED) return RADAR_MOTION_NONE;
    return RADAR_MOTION_UNKNOWN;
}

static void publish_source_state(RadarSourceContext *context)
{
    RadarSourceState *state = &context->source_state;
    state->source_id = context->source_id;
    copy_text(state->device_id, sizeof(state->device_id), context->device_id);
    copy_text(state->room_id, sizeof(state->room_id), context->room_id);
    state->online = context->online_state;
    state->presence = context->presence_state;
    state->motion = context->snapshot.motion_state != RADAR_MOTION_UNKNOWN
        ? context->snapshot.motion_state : motion_from_presence(context->presence_state);
    memcpy(state->tracks, context->snapshot.tracks, sizeof(state->tracks));
    memcpy(state->persons, context->snapshot.persons, sizeof(state->persons));
    state->count_summary = context->count_summary;
    state->timestamp_ms = context->last_update_timestamp;
    state->sequence = context->sequence;
}

void radar_source_context_reset(RadarSourceContext *context, uint64_t now_ms)
{
    if (context == NULL || !radar_source_context_is_valid(context->source_id)) return;

    radar_spatial_config_t config = radar_spatial_default_config();
    config.installation = context->coordinate_config;
    radar_spatial_state_init(&context->spatial_storage, &config, now_ms);
    radar_spatial_state_set_source(&context->spatial_storage, (uint8_t)context->source_id);
    wire_state_aliases(context);
    memset(context->raw_targets, 0, sizeof(context->raw_targets));
    memset(context->filtered_targets, 0, sizeof(context->filtered_targets));
    memset(context->history, 0, sizeof(context->history));
    memset(&context->diagnostics_state, 0, sizeof(context->diagnostics_state));
    memset(&context->snapshot, 0, sizeof(context->snapshot));
    memset(&context->count_summary, 0, sizeof(context->count_summary));
    context->count_summary.count_state = RADAR_PERSON_COUNT_UNKNOWN;
    memset(&context->source_state, 0, sizeof(context->source_state));
    context->online_state = false;
    context->presence_state = RADAR_STATE_UNKNOWN;
    context->state_version = 0U;
    context->last_update_timestamp = 0U;
    context->last_frame_time = 0U;
    context->sequence = 0U;
    context->frame_sequence = 0U;
    context->history_count = 0U;
    publish_source_state(context);
}

bool radar_source_context_init(uint64_t now_ms)
{
    if (s_initialized) return true;
    memset(s_contexts, 0, sizeof(s_contexts));
    for (radar_source_id_t source = RADAR_SOURCE_S3_LOCAL;
         source < RADAR_SOURCE_COUNT;
         source = (radar_source_id_t)(source + 1)) {
        RadarSourceContext *context = &s_contexts[source];
        context->source_id = source;
        copy_text(context->source_name, sizeof(context->source_name), s_source_names[source]);
        copy_text(context->device_id, sizeof(context->device_id), s_device_ids[source]);
        copy_text(context->room_id, sizeof(context->room_id), s_room_ids[source]);
        context->transport_type = s_transports[source];
        const radar_spatial_config_t defaults = radar_spatial_default_config();
        context->coordinate_config = defaults.installation;
        context->mount_position = (radar_point_mm_t){0, 0};
        context->offset = (radar_point_mm_t){
            defaults.installation.origin_offset_x_mm,
            defaults.installation.origin_offset_y_mm,
        };
        context->rotation = defaults.installation.rotation_deg;
        radar_source_context_reset(context, now_ms);
    }
    s_initialized = true;
    return true;
}

RadarSourceContext *radar_source_context_mutable(radar_source_id_t source)
{
    if (!s_initialized || !radar_source_context_is_valid(source)) return NULL;
    return &s_contexts[source];
}

const RadarSourceContext *radar_source_context_get(radar_source_id_t source)
{
    if (!s_initialized || !radar_source_context_is_valid(source)) return NULL;
    return &s_contexts[source];
}

const char *radar_source_context_source_name(radar_source_id_t source)
{
    const RadarSourceContext *context = radar_source_context_get(source);
    return context != NULL ? context->source_name : "UNKNOWN";
}

const char *radar_source_context_device_id(radar_source_id_t source)
{
    const RadarSourceContext *context = radar_source_context_get(source);
    return context != NULL ? context->device_id : NULL;
}

const char *radar_source_context_room_id(radar_source_id_t source)
{
    const RadarSourceContext *context = radar_source_context_get(source);
    return context != NULL ? context->room_id : NULL;
}

radar_transport_type_t radar_source_context_transport(radar_source_id_t source)
{
    const RadarSourceContext *context = radar_source_context_get(source);
    return context != NULL ? context->transport_type : RADAR_TRANSPORT_S3_LOCAL_UART;
}

const char *radar_source_context_transport_name(radar_transport_type_t transport)
{
    switch (transport) {
    case RADAR_TRANSPORT_C5_BLE_HTTP: return "C5_BLE_HTTP";
    case RADAR_TRANSPORT_S3_LOCAL_UART:
    default: return "S3_LOCAL_UART";
    }
}

bool radar_source_context_get_state(radar_source_id_t source, RadarSourceState *out)
{
    const RadarSourceContext *context = radar_source_context_get(source);
    if (context == NULL || out == NULL) return false;
    *out = context->source_state;
    return true;
}

void radar_source_context_publish(RadarSourceContext *context,
                                  const radar_spatial_snapshot_t *snapshot,
                                  const radar_count_summary_t *count_summary,
                                  bool online,
                                  uint32_t sequence,
                                  uint64_t frame_time_ms)
{
    if (context == NULL || snapshot == NULL) return;
    context->snapshot = *snapshot;
    context->diagnostics_state = snapshot->diagnostics;
    context->online_state = online;
    context->presence_state = presence_from_snapshot(snapshot);
    context->sequence = sequence;
    context->frame_sequence = context->spatial_state != NULL
        ? context->spatial_state->last_frame_seq : 0U;
    context->last_frame_time = frame_time_ms != 0U
        ? frame_time_ms : snapshot->latest_frame_ms;
    context->last_update_timestamp = context->last_frame_time;
    if (context->state_version < UINT32_MAX) {
        ++context->state_version;
    }
    context->count_summary = count_summary != NULL ? *count_summary : (radar_count_summary_t){0};
    if (count_summary == NULL) {
        context->count_summary.raw_target_count = snapshot->raw_target_count;
        context->count_summary.accepted_target_count = snapshot->accepted_target_count;
        context->count_summary.visible_track_count = snapshot->visible_track_count;
        context->count_summary.confirmed_active_track_count = snapshot->confirmed_active_track_count;
        context->count_summary.history_target_count = snapshot->history_target_count;
        context->count_summary.visible_person_count = snapshot->visible_person_count;
        context->count_summary.retained_person_count = snapshot->retained_person_count;
        context->count_summary.source_person_count = snapshot->source_person_count;
        context->count_summary.count_state = snapshot->count_state;
    }
    memcpy(context->raw_targets, snapshot->raw_targets, sizeof(context->raw_targets));
    memcpy(context->filtered_targets, snapshot->accepted_targets, sizeof(context->filtered_targets));
    memcpy(context->history, snapshot->history_targets, sizeof(context->history));
    context->history_count = snapshot->history_target_count > RADAR_TRACKER_HISTORY_MAX_TARGETS
        ? RADAR_TRACKER_HISTORY_MAX_TARGETS : snapshot->history_target_count;
    publish_source_state(context);
}

void radar_source_context_commit_state(RadarSourceContext *context,
                                       radar_presence_state_t presence_state,
                                       bool online,
                                       uint32_t sequence,
                                       uint64_t update_timestamp_ms,
                                       const radar_count_summary_t *count_summary)
{
    if (context == NULL) return;
    context->presence_state = presence_state;
    context->online_state = online;
    context->sequence = sequence;
    if (update_timestamp_ms != 0U) {
        context->last_update_timestamp = update_timestamp_ms;
    }
    if (count_summary != NULL) {
        context->count_summary = *count_summary;
    }
    if (context->state_version < UINT32_MAX) {
        ++context->state_version;
    }
    publish_source_state(context);
}

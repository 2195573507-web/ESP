#include "radar_local_handler.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "radar_ingest.h"
#include "radar_gateway_ingest.h"
#include "radar_home_snapshot.h"
#include "radar_local_adapter.h"
#include "radar_person_continuity.h"
#include "radar_registry.h"
#include "radar_service.h"

static esp_err_t respond(httpd_req_t *req, const char *status, const char *body)
{
    if (req == NULL) return ESP_ERR_INVALID_ARG;
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static bool append_text(char *out, size_t out_size, size_t *used, const char *format, ...)
{
    if (out == NULL || used == NULL || *used >= out_size) return false;
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(out + *used, out_size - *used, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= out_size - *used) return false;
    *used += (size_t)written;
    return true;
}

static const char *debug_sensor_state_name(radar_sensor_state_t state)
{
    switch (state) {
    case RADAR_SENSOR_VALID: return "online";
    case RADAR_SENSOR_STALE: return "stale";
    case RADAR_SENSOR_OFFLINE:
    default: return "offline";
    }
}

static const char *debug_occupancy_name(radar_occupancy_state_t state)
{
    switch (state) {
    case RADAR_OCCUPANCY_PRESENT:
    case RADAR_OCCUPANCY_HOLD: return "occupied";
    case RADAR_OCCUPANCY_VACANT_INFERRED: return "vacant";
    case RADAR_OCCUPANCY_UNKNOWN:
    default: return "unknown";
    }
}

static const char *debug_presence_occupancy_name(radar_presence_state_t state)
{
    switch (state) {
    case RADAR_STATE_MOTION:
    case RADAR_STATE_PRESENT:
    case RADAR_STATE_HOLD: return "occupied";
    case RADAR_STATE_VACANT_INFERRED: return "vacant";
    case RADAR_STATE_UNKNOWN:
    default: return "unknown";
    }
}

static const char *debug_recovery_state_name(radar_uart_recovery_state_t state)
{
    switch (state) {
    case RADAR_UART_RECOVERY_VALID: return "RUNNING";
    case RADAR_UART_RECOVERY_WAITING_VALID: return "WAITING_VALID";
    case RADAR_UART_RECOVERY_BACKOFF: return "BACKOFF";
    case RADAR_UART_RECOVERY_OFFLINE: return "DISABLED_OR_OFFLINE";
    default: return "UNKNOWN";
    }
}

static const RadarRoomState *home_source_state(const RadarHomeState *home,
                                                radar_source_id_t source)
{
    if (home == NULL) return NULL;
    const uint8_t room_count = home->occupied_room_count > RADAR_SOURCE_COUNT
        ? RADAR_SOURCE_COUNT : home->occupied_room_count;
    for (uint8_t index = 0U; index < room_count; ++index) {
        if (home->occupied_rooms[index].source_id == source) {
            return &home->occupied_rooms[index];
        }
    }
    return NULL;
}

static const char *home_motion_name(radar_motion_state_t state)
{
    switch (state) {
    case RADAR_MOTION_MOVING: return "moving";
    case RADAR_MOTION_STILL_CANDIDATE: return "still_candidate";
    case RADAR_MOTION_NONE: return "none";
    case RADAR_MOTION_UNKNOWN:
    default: return "unknown";
    }
}

static const char *inactive_source_motion_name(radar_presence_state_t state)
{
    switch (state) {
    case RADAR_STATE_MOTION: return "moving";
    case RADAR_STATE_VACANT_INFERRED:
    case RADAR_STATE_HOLD:
    case RADAR_STATE_PRESENT: return "none";
    case RADAR_STATE_UNKNOWN:
    default: return "unknown";
    }
}

static bool append_json_string(char *out, size_t out_size, size_t *used, const char *value)
{
    if (!append_text(out, out_size, used, "\"")) return false;
    const unsigned char *cursor = (const unsigned char *)(value != NULL ? value : "");
    while (*cursor != '\0') {
        if (*cursor == '\"' || *cursor == '\\') {
            if (!append_text(out, out_size, used, "\\%c", *cursor)) return false;
        } else if (*cursor < 0x20U) {
            if (!append_text(out, out_size, used, "\\u%04x", (unsigned int)*cursor)) return false;
        } else if (!append_text(out, out_size, used, "%c", *cursor)) {
            return false;
        }
        ++cursor;
    }
    return append_text(out, out_size, used, "\"");
}

esp_err_t radar_local_handler(httpd_req_t *req)
{
    if (req == NULL || req->content_len <= 0) {
        return respond(req, "400 Bad Request", "{\"ok\":0,\"error\":\"body_required\"}");
    }
    if ((size_t)req->content_len > RADAR_INGEST_MAX_BODY_BYTES) {
        return respond(req, "413 Payload Too Large", "{\"ok\":0,\"error\":\"payload_too_large\"}");
    }
    char content_type[32] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strcasecmp(content_type, "application/json") != 0) {
        return respond(req, "415 Unsupported Media Type", "{\"ok\":0,\"error\":\"content_type\"}");
    }

    const size_t capacity = (size_t)req->content_len + 1U;
    char *body = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) {
        return respond(req, "503 Service Unavailable", "{\"ok\":0,\"error\":\"no_memory\"}");
    }
    size_t received = 0U;
    while (received < (size_t)req->content_len) {
        const int read_len = httpd_req_recv(req, body + received,
                                            (size_t)req->content_len - received);
        if (read_len <= 0) {
            heap_caps_free(body);
            return respond(req, "400 Bad Request", "{\"ok\":0,\"error\":\"body_read\"}");
        }
        received += (size_t)read_len;
    }
    body[received] = '\0';
    const int64_t now_us = esp_timer_get_time();
    const uint64_t received_at_ms = now_us > 0 ? (uint64_t)(now_us / 1000) : 1U;
    const radar_ingest_result_t result = radar_ingest_process_json(body, received, received_at_ms);
    heap_caps_free(body);

    switch (result) {
    case RADAR_INGEST_ACCEPTED:
        return respond(req, "200 OK", "{\"ok\":1,\"accepted\":1}");
    case RADAR_INGEST_IDENTITY_MISMATCH:
        return respond(req, "403 Forbidden", "{\"ok\":0,\"error\":\"identity_mismatch\"}");
    case RADAR_INGEST_UNAVAILABLE:
        return respond(req, "503 Service Unavailable", "{\"ok\":0,\"error\":\"unavailable\"}");
    default: {
        char response[96];
        const int written = snprintf(response, sizeof(response),
                                     "{\"ok\":0,\"error\":\"%s\"}",
                                     radar_ingest_result_name(result));
        return written > 0 && (size_t)written < sizeof(response)
            ? respond(req, "400 Bad Request", response)
            : respond(req, "400 Bad Request", "{\"ok\":0,\"error\":\"invalid_request\"}");
    }
    }
}

esp_err_t radar_home_snapshot_handler(httpd_req_t *req)
{
    if (req == NULL) return ESP_ERR_INVALID_ARG;

    enum { RADAR_HOME_SNAPSHOT_BODY_BYTES = 1536U };
    char *body = heap_caps_calloc(1U, RADAR_HOME_SNAPSHOT_BODY_BYTES,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) {
        return respond(req, "503 Service Unavailable", "{\"ok\":0,\"error\":\"home_snapshot_memory\"}");
    }

    radar_home_snapshot_t snapshot = {0};
    radar_registry_entry_t sources[RADAR_SOURCE_COUNT] = {0};
    const bool has_snapshot = radar_home_snapshot_get(&snapshot);
    const size_t source_count = radar_registry_snapshot(sources, RADAR_SOURCE_COUNT);
    if (!has_snapshot || source_count == 0U) {
        heap_caps_free(body);
        return respond(req, "503 Service Unavailable", "{\"ok\":0,\"error\":\"home_snapshot_unavailable\"}");
    }

    RadarHomeState home = {0};
    radar_registry_get_home_state(&home);
    size_t used = 0U;
    if (!append_text(body, RADAR_HOME_SNAPSHOT_BODY_BYTES, &used, "{\"ok\":1,\"sources\":[")) {
        goto home_snapshot_memory;
    }

    for (size_t index = 0U; index < source_count; ++index) {
        const radar_registry_entry_t *source = &sources[index];
        const RadarRoomState *source_state = home_source_state(&home, source->source);
        if (!append_text(body, RADAR_HOME_SNAPSHOT_BODY_BYTES, &used,
                         "%s{\"source_id\":%u,\"source\":",
                         index == 0U ? "" : ",", (unsigned int)source->source) ||
            !append_json_string(body, RADAR_HOME_SNAPSHOT_BODY_BYTES, &used,
                                radar_registry_source_name(source->source)) ||
            !append_text(body, RADAR_HOME_SNAPSHOT_BODY_BYTES, &used, ",\"room\":") ||
            !append_json_string(body, RADAR_HOME_SNAPSHOT_BODY_BYTES, &used, source->room_id) ||
            !append_text(body, RADAR_HOME_SNAPSHOT_BODY_BYTES, &used,
                         ",\"online\":%s,\"occupied\":%s,\"motion\":\"%s\",\"person_count\":%u}",
                         source->source_online ? "true" : "false",
                         source_state != NULL ? "true" : "false",
                         source_state != NULL ? home_motion_name(source_state->motion) :
                                                inactive_source_motion_name(source->snapshot.state),
                         (unsigned int)(source_state != NULL ? source_state->person_count : 0U))) {
            goto home_snapshot_memory;
        }
    }

    if (!append_text(body, RADAR_HOME_SNAPSHOT_BODY_BYTES, &used,
                     "],\"home\":{\"known\":%s,\"occupied\":%s,\"person_count\":%u,\"room_count\":%u}}",
                     snapshot.occupancy_known ? "true" : "false",
                     snapshot.occupied ? "true" : "false",
                     (unsigned int)snapshot.person_count,
                     (unsigned int)snapshot.room_count)) {
        goto home_snapshot_memory;
    }

    {
        const esp_err_t ret = respond(req, "200 OK", body);
        heap_caps_free(body);
        return ret;
    }

home_snapshot_memory:
    heap_caps_free(body);
    return respond(req, "503 Service Unavailable", "{\"ok\":0,\"error\":\"home_snapshot_memory\"}");
}

esp_err_t radar_debug_handler(httpd_req_t *req)
{
    if (req == NULL) return ESP_ERR_INVALID_ARG;
    enum { RADAR_DEBUG_BODY_BYTES = 4096U };
    char *body = heap_caps_calloc(1U,
                                  RADAR_DEBUG_BODY_BYTES,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) {
        return respond(req, "503 Service Unavailable", "{\"ok\":0,\"error\":\"debug_memory\"}");
    }
    size_t used = 0U;
    if (!append_text(body, RADAR_DEBUG_BODY_BYTES, &used, "{\"sources\":[")) {
        goto debug_memory;
    }

    radar_readonly_snapshot_t local_snapshot = {0};
    const bool has_local_snapshot = radar_local_adapter_get_readonly_snapshot(&local_snapshot);
    radar_service_diagnostics_t local_service = {0};
    radar_service_get_diagnostics(&local_service);
    for (radar_source_id_t source = RADAR_SOURCE_S3_LOCAL;
         source < RADAR_SOURCE_COUNT;
         source = (radar_source_id_t)(source + 1)) {
        const RadarSourceContext *context = radar_source_context_get(source);
        radar_registry_entry_t entry = {0};
        const bool has_entry = radar_registry_get(source, &entry);
        const char *source_name = context != NULL ? context->source_name : "UNKNOWN";
        const char *device_id = context != NULL ? context->device_id : "";
        const char *room_id = context != NULL ? context->room_id : "";
        bool online = has_entry && entry.source_online;
        const char *sensor_state = online ? "online" : "offline";
        const char *occupancy = has_entry
            ? debug_presence_occupancy_name(entry.snapshot.state) : "unknown";
        const char *recovery_state = online ? "RUNNING" : "NOT_CONNECTED";
        uint32_t targets = has_entry ? entry.snapshot.current_target_count : 0U;
        uint32_t tracks = 0U;
        uint32_t raw_targets = 0U;
        uint32_t accepted_targets = 0U;
        uint32_t confirmed_active_tracks = 0U;
        uint32_t history_tracks = 0U;
        uint32_t visible_persons = 0U;
        uint32_t retained_persons = 0U;
        uint32_t business_persons = 0U;
        radar_person_count_state_t count_state = RADAR_PERSON_COUNT_UNKNOWN;
        uint32_t accepted = has_entry ? entry.diagnostics.accepted_count : 0U;
        uint32_t bad_header = 0U;
        uint32_t bad_tail = 0U;
        uint32_t resync = 0U;
        uint64_t last_update = has_entry ? entry.last_report_ms : 0U;

        if (source == RADAR_SOURCE_S3_LOCAL) {
            online = has_local_snapshot && local_snapshot.sensor_state != RADAR_SENSOR_OFFLINE;
            sensor_state = has_local_snapshot
                ? debug_sensor_state_name(local_snapshot.sensor_state) : "offline";
            occupancy = has_local_snapshot
                ? debug_occupancy_name(local_snapshot.occupancy_state) : "unknown";
            recovery_state = debug_recovery_state_name(local_service.recovery.state);
            tracks = has_local_snapshot ? local_snapshot.track_count : 0U;
            if (has_local_snapshot) {
                raw_targets = local_snapshot.count_summary.raw_target_count;
                accepted_targets = local_snapshot.count_summary.accepted_target_count;
                tracks = local_snapshot.count_summary.visible_track_count;
                confirmed_active_tracks = local_snapshot.count_summary.confirmed_active_track_count;
                history_tracks = local_snapshot.count_summary.history_target_count;
                visible_persons = local_snapshot.count_summary.visible_person_count;
                retained_persons = local_snapshot.count_summary.retained_person_count;
                business_persons = local_snapshot.count_summary.source_person_count;
                count_state = local_snapshot.count_summary.count_state;
            }
            accepted = local_service.parser.valid_frames;
            bad_header = local_service.parser.bad_header;
            bad_tail = local_service.parser.bad_tail;
            resync = local_service.parser.resync_count;
            if (has_local_snapshot && local_snapshot.latest_frame_ms != 0U) {
                last_update = local_snapshot.latest_frame_ms;
            }
        } else {
            const uint8_t local_id = source == RADAR_SOURCE_C51 ? 1U : 2U;
            radar_gateway_output_t output = {0};
            if (radar_gateway_ingest_get_output(local_id, &output)) {
                online = output.radar_online;
                sensor_state = online ? "online" :
                    (output.radar_stale ? "stale" : "offline");
                recovery_state = online ? "RUNNING" :
                    (output.radar_stale ? "STALE" : "NOT_CONNECTED");
                occupancy = debug_occupancy_name(output.occupancy);
                targets = output.target_count;
                raw_targets = output.count_summary.raw_target_count;
                accepted_targets = output.count_summary.accepted_target_count;
                tracks = output.count_summary.visible_track_count;
                confirmed_active_tracks = output.count_summary.confirmed_active_track_count;
                history_tracks = output.count_summary.history_target_count;
                visible_persons = output.count_summary.visible_person_count;
                retained_persons = output.count_summary.retained_person_count;
                business_persons = output.count_summary.source_person_count;
                count_state = output.count_summary.count_state;
                last_update = output.updated_at_ms;
            }
        }

        if (!append_text(body, RADAR_DEBUG_BODY_BYTES, &used,
                         "%s{\"source_id\":%u,\"source\":\"%s\",\"device_id\":\"%s\",\"room\":\"%s\",\"online\":%s,"
                         "\"transport\":\"%s\",\"sequence\":%lu,\"sensor_state\":\"%s\",\"occupancy\":\"%s\",\"targets\":[",
                         source == RADAR_SOURCE_S3_LOCAL ? "" : ",",
                         (unsigned int)source,
                         source_name != NULL ? source_name : "UNKNOWN",
                         device_id != NULL ? device_id : "",
                         room_id != NULL ? room_id : "",
                         online ? "true" : "false",
                         context != NULL ? radar_source_context_transport_name(context->transport_type) : "unknown",
                         (unsigned long)(context != NULL ? context->sequence : 0U),
                         sensor_state,
                         occupancy)) {
            goto debug_memory;
        }

        bool first_source_target = true;
        if (source == RADAR_SOURCE_S3_LOCAL && has_local_snapshot) {
            const uint8_t track_count = local_snapshot.track_count > RADAR_TRACKER_MAX_TRACKS
                ? RADAR_TRACKER_MAX_TRACKS : local_snapshot.track_count;
            for (uint8_t index = 0U; index < track_count; ++index) {
                const radar_readonly_track_t *track = &local_snapshot.tracks[index];
                if (!append_text(body, RADAR_DEBUG_BODY_BYTES, &used,
                                 "%s{\"source_id\":%u,\"source\":\"%s\",\"device_id\":\"%s\",\"room\":\"%s\",\"id\":%lu,\"x_mm\":%ld,\"y_mm\":%ld,\"visible\":%s}",
                                 first_source_target ? "" : ",", (unsigned int)source, source_name, device_id, room_id,
                                 (unsigned long)track->track_id, (long)track->filtered_x_mm,
                                 (long)track->filtered_y_mm, track->visible ? "true" : "false")) {
                    goto debug_memory;
                }
                first_source_target = false;
            }
        } else if (source != RADAR_SOURCE_S3_LOCAL) {
            const uint8_t local_id = source == RADAR_SOURCE_C51 ? 1U : 2U;
            radar_gateway_output_t output = {0};
            if (radar_gateway_ingest_get_output(local_id, &output)) {
                const uint8_t target_count = output.target_count > LD2450_MAX_TARGETS
                    ? LD2450_MAX_TARGETS : output.target_count;
                for (uint8_t index = 0U; index < target_count; ++index) {
                    const radar_gateway_target_output_t *target = &output.targets[index];
                    if (!append_text(body, RADAR_DEBUG_BODY_BYTES, &used,
                                     "%s{\"source_id\":%u,\"source\":\"%s\",\"device_id\":\"%s\",\"room\":\"%s\",\"id\":%lu,\"x_mm\":%ld,\"y_mm\":%ld,\"visible\":%s}",
                                     first_source_target ? "" : ",", (unsigned int)source, source_name, device_id, room_id,
                                     (unsigned long)target->track_id, (long)target->x_mm,
                                     (long)target->y_mm, target->visible ? "true" : "false")) {
                        goto debug_memory;
                    }
                    first_source_target = false;
                }
            }
        }

        if (!append_text(body, RADAR_DEBUG_BODY_BYTES, &used,
                         "],\"target_count\":%lu,\"tracks\":%lu,\"raw_target_count\":%lu,\"accepted_target_count\":%lu,"
                         "\"visible_track_count\":%lu,\"confirmed_active_track_count\":%lu,"
                         "\"history_target_count\":%lu,\"visible_person_count\":%lu,"
                         "\"retained_person_count\":%lu,\"source_person_count\":%lu,"
                         "\"count_state\":\"%s\",\"accepted\":%lu,\"bad_header\":%lu,"
                         "\"bad_tail\":%lu,\"resync\":%lu,\"recovery_state\":\"%s\","
                         "\"last_update\":%llu}",
                         (unsigned long)targets,
                         (unsigned long)tracks,
                         (unsigned long)raw_targets,
                         (unsigned long)accepted_targets,
                         (unsigned long)tracks,
                         (unsigned long)confirmed_active_tracks,
                         (unsigned long)history_tracks,
                         (unsigned long)visible_persons,
                         (unsigned long)retained_persons,
                         (unsigned long)business_persons,
                         radar_person_count_state_name(count_state),
                         (unsigned long)accepted,
                         (unsigned long)bad_header,
                         (unsigned long)bad_tail,
                         (unsigned long)resync,
                         recovery_state,
                         (unsigned long long)last_update)) {
            goto debug_memory;
        }
    }

    radar_ingest_history_stats_t history = {0};
    const bool has_history = radar_ingest_history_get_stats(&history);
    if (!append_text(body, RADAR_DEBUG_BODY_BYTES, &used,
                     "],\"source_count\":%u,\"history_count\":%u,\"history_capacity\":%u}",
                     (unsigned int)RADAR_SOURCE_COUNT,
                     has_history ? (unsigned int)history.count : 0U,
                     has_history ? (unsigned int)history.capacity : 0U)) {
        goto debug_memory;
    }
    const esp_err_t response_ret = respond(req, "200 OK", body);
    heap_caps_free(body);
    return response_ret;

debug_memory:
    heap_caps_free(body);
    return respond(req, "503 Service Unavailable", "{\"ok\":0,\"error\":\"debug_memory\"}");
}

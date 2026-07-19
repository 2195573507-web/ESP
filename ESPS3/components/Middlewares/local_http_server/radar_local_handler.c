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
#include "radar_local_adapter.h"
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
    case RADAR_UART_RECOVERY_OFFLINE:
    default: return "FAILED";
    }
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
    char *body = heap_caps_malloc(capacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
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

esp_err_t radar_debug_handler(httpd_req_t *req)
{
    if (req == NULL) return ESP_ERR_INVALID_ARG;
    char body[2048];
    size_t used = 0U;
    bool first_target = true;
    if (!append_text(body, sizeof(body), &used, "{\"targets\":[")) {
        return respond(req, "503 Service Unavailable", "{\"ok\":0,\"error\":\"debug_memory\"}");
    }

    radar_readonly_snapshot_t local_snapshot = {0};
    const bool has_local_snapshot = radar_local_adapter_get_readonly_snapshot(&local_snapshot);
    for (uint8_t index = 0U; has_local_snapshot && index < local_snapshot.track_count; ++index) {
        const radar_readonly_track_t *track = &local_snapshot.tracks[index];
        if (!append_text(body, sizeof(body), &used,
                         "%s{\"source_id\":%u,\"id\":%lu,\"x_mm\":%ld,\"y_mm\":%ld,"
                         "\"speed_cm_s\":%d,\"confidence\":%lu,\"visible\":%s}",
                         first_target ? "" : ",",
                         (unsigned int)RADAR_SOURCE_S3_LOCAL,
                         (unsigned long)track->track_id,
                         (long)track->filtered_x_mm,
                         (long)track->filtered_y_mm,
                         (int)track->speed_cm_s,
                         (unsigned long)track->confidence,
                         track->visible ? "true" : "false")) {
            return respond(req, "503 Service Unavailable", "{\"ok\":0,\"error\":\"debug_memory\"}");
        }
        first_target = false;
    }

    for (uint8_t local_id = 1U; local_id <= 2U; ++local_id) {
        radar_gateway_output_t output = {0};
        if (!radar_gateway_ingest_get_output(local_id, &output)) continue;
        for (uint8_t index = 0U; index < output.target_count; ++index) {
            const radar_gateway_target_output_t *target = &output.targets[index];
            if (!append_text(body, sizeof(body), &used,
                             "%s{\"source_id\":%u,\"id\":%lu,\"x_mm\":%ld,\"y_mm\":%ld,"
                             "\"speed_cm_s\":%d,\"confidence\":%u,\"visible\":%s}",
                             first_target ? "" : ",",
                             (unsigned int)local_id,
                             (unsigned long)target->track_id,
                             (long)target->x_mm,
                             (long)target->y_mm,
                             (int)target->speed_cm_s,
                             (unsigned int)target->confidence,
                             target->visible ? "true" : "false")) {
                return respond(req, "503 Service Unavailable", "{\"ok\":0,\"error\":\"debug_memory\"}");
            }
            first_target = false;
        }
    }

    if (!append_text(body, sizeof(body), &used, "],\"sources\":[")) {
        return respond(req, "503 Service Unavailable", "{\"ok\":0,\"error\":\"debug_memory\"}");
    }

    radar_service_diagnostics_t local_service = {0};
    radar_service_get_diagnostics(&local_service);
    for (radar_source_id_t source = RADAR_SOURCE_S3_LOCAL;
         source < RADAR_SOURCE_COUNT;
         source = (radar_source_id_t)(source + 1)) {
        radar_registry_entry_t entry = {0};
        const bool has_entry = radar_registry_get(source, &entry);
        const char *source_name = radar_registry_source_name(source);
        const char *device_id = has_entry && entry.device_id[0] != '\0'
            ? entry.device_id : radar_registry_device_id(source);
        bool online = has_entry && entry.source_online;
        const char *sensor_state = online ? "online" : "offline";
        const char *occupancy = has_entry
            ? debug_presence_occupancy_name(entry.snapshot.state) : "unknown";
        const char *recovery_state = online ? "RUNNING" : "FAILED";
        uint32_t targets = has_entry ? entry.snapshot.current_target_count : 0U;
        uint32_t tracks = targets;
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
                sensor_state = online ? "online" : "offline";
                occupancy = debug_occupancy_name(output.occupancy);
                targets = output.target_count;
                tracks = output.target_count;
                last_update = output.updated_at_ms;
            }
        }

        if (!append_text(body, sizeof(body), &used,
                         "%s{\"source\":\"%s\",\"device_id\":\"%s\",\"online\":%s,"
                         "\"sensor_state\":\"%s\",\"occupancy\":\"%s\",\"targets\":%lu,"
                         "\"tracks\":%lu,\"accepted\":%lu,\"bad_header\":%lu,"
                         "\"bad_tail\":%lu,\"resync\":%lu,\"recovery_state\":\"%s\","
                         "\"last_update\":%llu}",
                         source == RADAR_SOURCE_S3_LOCAL ? "" : ",",
                         source_name != NULL ? source_name : "UNKNOWN",
                         device_id != NULL ? device_id : "",
                         online ? "true" : "false",
                         sensor_state,
                         occupancy,
                         (unsigned long)targets,
                         (unsigned long)tracks,
                         (unsigned long)accepted,
                         (unsigned long)bad_header,
                         (unsigned long)bad_tail,
                         (unsigned long)resync,
                         recovery_state,
                         (unsigned long long)last_update)) {
            return respond(req, "503 Service Unavailable", "{\"ok\":0,\"error\":\"debug_memory\"}");
        }
    }

    radar_ingest_history_stats_t history = {0};
    const bool has_history = radar_ingest_history_get_stats(&history);
    if (!append_text(body, sizeof(body), &used,
                     "],\"source_count\":%u,\"history_count\":%u,\"history_capacity\":%u}",
                     (unsigned int)RADAR_SOURCE_COUNT,
                     has_history ? (unsigned int)history.count : 0U,
                     has_history ? (unsigned int)history.capacity : 0U)) {
        return respond(req, "503 Service Unavailable", "{\"ok\":0,\"error\":\"debug_memory\"}");
    }
    return respond(req, "200 OK", body);
}

#include "radar_diagnostics.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radar_config.h"
#include "radar_local_adapter.h"

/*
 * Diagnostics only log local value snapshots.  Registry and spatial state own
 * their locks; this module asks each owner for a copy, then formats after both
 * locks have been released.
 */

static const char TAG[] = "radar_diag";
static TaskHandle_t s_task;
/* The full value snapshot is larger than the diagnostics task's safe stack budget. */
static radar_diag_snapshot_t s_task_snapshot;
static radar_registry_entry_t s_registry_entries[RADAR_SOURCE_COUNT];

static uint64_t now_ms(void)
{
    int64_t now_us = esp_timer_get_time();
    return now_us > 0 ? (uint64_t)(now_us / 1000) : 0U;
}

static uint64_t age_ms(uint64_t timestamp_ms, uint64_t current_ms)
{
    return timestamp_ms > 0U && current_ms >= timestamp_ms ? current_ms - timestamp_ms : 0U;
}

static void write_source_name(char out[RADAR_DIAG_SOURCE_TEXT_LEN], radar_source_id_t source)
{
    switch (source) {
    case RADAR_SOURCE_C51:
        (void)snprintf(out, RADAR_DIAG_SOURCE_TEXT_LEN, "C51");
        break;
    case RADAR_SOURCE_C52:
        (void)snprintf(out, RADAR_DIAG_SOURCE_TEXT_LEN, "C52");
        break;
    case RADAR_SOURCE_S3_LOCAL:
        (void)snprintf(out, RADAR_DIAG_SOURCE_TEXT_LEN, "S3_LOCAL");
        break;
    default:
        (void)snprintf(out, RADAR_DIAG_SOURCE_TEXT_LEN, "UNKNOWN");
        break;
    }
}

static void write_presence_state(char out[RADAR_DIAG_STATE_TEXT_LEN], radar_presence_state_t state)
{
    switch (state) {
    case RADAR_STATE_VACANT_INFERRED:
        (void)snprintf(out, RADAR_DIAG_STATE_TEXT_LEN, "vacant_inferred");
        break;
    case RADAR_STATE_HOLD:
        (void)snprintf(out, RADAR_DIAG_STATE_TEXT_LEN, "hold");
        break;
    case RADAR_STATE_MOTION:
        (void)snprintf(out, RADAR_DIAG_STATE_TEXT_LEN, "moving");
        break;
    case RADAR_STATE_PRESENT:
        (void)snprintf(out, RADAR_DIAG_STATE_TEXT_LEN, "present");
        break;
    case RADAR_STATE_UNKNOWN:
    default:
        (void)snprintf(out, RADAR_DIAG_STATE_TEXT_LEN, "unknown");
        break;
    }
}

static void write_sensor_state(char out[RADAR_DIAG_STATE_TEXT_LEN], radar_sensor_state_t state)
{
    switch (state) {
    case RADAR_SENSOR_VALID:
        (void)snprintf(out, RADAR_DIAG_STATE_TEXT_LEN, "valid");
        break;
    case RADAR_SENSOR_STALE:
        (void)snprintf(out, RADAR_DIAG_STATE_TEXT_LEN, "stale");
        break;
    case RADAR_SENSOR_OFFLINE:
    default:
        (void)snprintf(out, RADAR_DIAG_STATE_TEXT_LEN, "offline");
        break;
    }
}

static void write_occupancy_state(char out[RADAR_DIAG_STATE_TEXT_LEN],
                                  radar_occupancy_state_t state)
{
    switch (state) {
    case RADAR_OCCUPANCY_PRESENT:
        (void)snprintf(out, RADAR_DIAG_STATE_TEXT_LEN, "present");
        break;
    case RADAR_OCCUPANCY_HOLD:
        (void)snprintf(out, RADAR_DIAG_STATE_TEXT_LEN, "hold");
        break;
    case RADAR_OCCUPANCY_VACANT_INFERRED:
        (void)snprintf(out, RADAR_DIAG_STATE_TEXT_LEN, "vacant_inferred");
        break;
    case RADAR_OCCUPANCY_UNKNOWN:
    default:
        (void)snprintf(out, RADAR_DIAG_STATE_TEXT_LEN, "unknown");
        break;
    }
}

static void write_motion_state(char out[RADAR_DIAG_STATE_TEXT_LEN], radar_motion_state_t state)
{
    switch (state) {
    case RADAR_MOTION_STILL_CANDIDATE:
        (void)snprintf(out, RADAR_DIAG_STATE_TEXT_LEN, "still_candidate");
        break;
    case RADAR_MOTION_MOVING:
        (void)snprintf(out, RADAR_DIAG_STATE_TEXT_LEN, "moving");
        break;
    case RADAR_MOTION_NONE:
        (void)snprintf(out, RADAR_DIAG_STATE_TEXT_LEN, "none");
        break;
    case RADAR_MOTION_UNKNOWN:
    default:
        (void)snprintf(out, RADAR_DIAG_STATE_TEXT_LEN, "unknown");
        break;
    }
}

static void write_recovery_state(char out[RADAR_DIAG_STATE_TEXT_LEN],
                                 radar_uart_recovery_state_t state)
{
    switch (state) {
    case RADAR_UART_RECOVERY_BACKOFF:
        (void)snprintf(out, RADAR_DIAG_STATE_TEXT_LEN, "backoff");
        break;
    case RADAR_UART_RECOVERY_WAITING_VALID:
        (void)snprintf(out, RADAR_DIAG_STATE_TEXT_LEN, "waiting_valid");
        break;
    case RADAR_UART_RECOVERY_VALID:
        (void)snprintf(out, RADAR_DIAG_STATE_TEXT_LEN, "valid");
        break;
    case RADAR_UART_RECOVERY_OFFLINE:
    default:
        (void)snprintf(out, RADAR_DIAG_STATE_TEXT_LEN, "offline");
        break;
    }
}

static void write_transition_reason(char out[RADAR_DIAG_REASON_TEXT_LEN],
                                    radar_diagnostics_transition_reason_t reason)
{
    switch (reason) {
    case RADAR_DIAGNOSTICS_TRANSITION_LOCAL_UART:
        (void)snprintf(out, RADAR_DIAG_REASON_TEXT_LEN, "local_uart");
        break;
    case RADAR_DIAGNOSTICS_TRANSITION_REMOTE_UPDATE:
        (void)snprintf(out, RADAR_DIAG_REASON_TEXT_LEN, "remote_update");
        break;
    case RADAR_DIAGNOSTICS_TRANSITION_FRESHNESS_OR_UPDATE:
        (void)snprintf(out, RADAR_DIAG_REASON_TEXT_LEN, "freshness_or_update");
        break;
    default:
        (void)snprintf(out, RADAR_DIAG_REASON_TEXT_LEN, "unknown");
        break;
    }
}

static void copy_registry_entry(radar_diag_registry_snapshot_t *out,
                                const radar_registry_entry_t *entry)
{
    out->source = entry->source;
    write_source_name(out->source_name, entry->source);
    (void)snprintf(out->device_id,
                   sizeof(out->device_id),
                   "%.*s",
                   (int)sizeof(entry->device_id),
                   entry->device_id);
    (void)snprintf(out->room_id,
                   sizeof(out->room_id),
                   "%.*s",
                   (int)sizeof(entry->room_id),
                   entry->room_id);
    write_presence_state(out->state, entry->snapshot.state);
    out->source_online = entry->source_online;
    out->snapshot = entry->snapshot;
    out->sequence = entry->sequence;
    out->session_generation = entry->session_generation;
    out->diagnostics = entry->diagnostics;
}

bool radar_diag_snapshot_copy(radar_diag_snapshot_t *out)
{
    if (out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    const size_t count = radar_registry_snapshot(s_registry_entries, RADAR_SOURCE_COUNT);
    out->registry_count = count;
    for (size_t i = 0U; i < count; ++i) {
        copy_registry_entry(&out->registry[i], &s_registry_entries[i]);
    }
    out->unattributed_parse_errors = radar_registry_unattributed_parse_errors();

    out->has_local_spatial = radar_local_adapter_get_spatial_snapshot(&out->local_spatial);
    if (out->has_local_spatial) {
        write_sensor_state(out->sensor_state, out->local_spatial.sensor_state);
        write_occupancy_state(out->occupancy_state, out->local_spatial.occupancy_state);
        write_motion_state(out->motion_state, out->local_spatial.motion_state);
        write_recovery_state(out->recovery_state,
                             out->local_spatial.diagnostics.recovery.state);
    }

    return out->registry_count > 0U || out->has_local_spatial;
}

void radar_diagnostics_log_transition(radar_source_id_t source,
                                      radar_diagnostics_transition_reason_t reason)
{
    radar_registry_entry_t registry_entry;
    if (!radar_registry_get(source, &registry_entry)) {
        return;
    }
    radar_diag_registry_snapshot_t entry;
    memset(&entry, 0, sizeof(entry));
    copy_registry_entry(&entry, &registry_entry);
    char transition_reason[RADAR_DIAG_REASON_TEXT_LEN];
    write_transition_reason(transition_reason, reason);
    ESP_LOGI(TAG,
             "source transition source=%s room=%s state=%s targets=%u online=%d uart_online=%d frame_fresh=%d sequence=%lu session=%lu reason=%s",
             entry.source_name,
             entry.room_id,
             entry.state,
             (unsigned int)entry.snapshot.current_target_count,
             entry.source_online ? 1 : 0,
             entry.snapshot.uart_online ? 1 : 0,
             entry.snapshot.frame_fresh ? 1 : 0,
             (unsigned long)entry.sequence,
             (unsigned long)entry.session_generation,
             transition_reason);
}

static void log_summary(const radar_diag_snapshot_t *snapshot, uint64_t current_ms)
{
    for (size_t i = 0U; i < snapshot->registry_count; ++i) {
        const radar_diag_registry_snapshot_t *entry = &snapshot->registry[i];
        ESP_LOGI(TAG,
                 "source=%s device_id=%s room=%s state=%s targets=%u online=%d uart_online=%d valid_age_ms=%" PRIu64 " motion_age_ms=%" PRIu64 " parse_errors=%lu sequence_rejects=%lu identity_mismatches=%lu freshness_expiry=%lu",
                 entry->source_name,
                 entry->device_id,
                 entry->room_id,
                 entry->state,
                 (unsigned int)entry->snapshot.current_target_count,
                 entry->source_online ? 1 : 0,
                 entry->snapshot.uart_online ? 1 : 0,
                 entry->snapshot.frame_fresh ? 1 : 0,
                 age_ms(entry->snapshot.last_valid_frame_ms, current_ms),
                 age_ms(entry->snapshot.last_motion_ms, current_ms),
                 (unsigned long)(entry->diagnostics.parse_error_count == UINT32_MAX ? 0U :
                                 entry->diagnostics.parse_error_count),
                 (unsigned long)entry->diagnostics.sequence_reject_count,
                 (unsigned long)entry->diagnostics.identity_mismatch_count,
                 (unsigned long)entry->diagnostics.freshness_expiry_count);
    }
    ESP_LOGI(TAG,
             "unattributed_parse_errors=%lu",
             (unsigned long)snapshot->unattributed_parse_errors);

    if (!snapshot->has_local_spatial) {
        return;
    }

    const radar_spatial_snapshot_t *spatial = &snapshot->local_spatial;
    ESP_LOGI(TAG,
             "local sensor=%s occupancy=%s motion=%s raw=%u accepted=%u tracks=%u frame_age_ms=%lu confidence=%lu parser[bytes=%lu valid=%lu bad_header=%lu bad_length=%lu bad_tail=%lu skipped=%lu invalid_slots=%lu coord_outliers=%lu resync=%lu] tracker[coord_outliers=%lu jump_outliers=%lu] uart[read_timeout=%lu read_zero=%lu read_driver_error=%lu fifo_overflow=%lu queue_full=%lu] recovery[state=%s count=%lu init_fail=%lu errors=%lu no_valid=%lu no_rx_timeout=%lu]",
             snapshot->sensor_state,
             snapshot->occupancy_state,
             snapshot->motion_state,
             (unsigned int)spatial->raw_target_count,
             (unsigned int)spatial->accepted_target_count,
             (unsigned int)spatial->active_track_count,
             (unsigned long)spatial->frame_age_ms,
             (unsigned long)spatial->occupancy_confidence,
             (unsigned long)spatial->diagnostics.parser.bytes_received,
             (unsigned long)spatial->diagnostics.parser.valid_frames,
             (unsigned long)spatial->diagnostics.parser.bad_header,
             (unsigned long)spatial->diagnostics.parser.bad_length,
             (unsigned long)spatial->diagnostics.parser.bad_tail,
             (unsigned long)spatial->diagnostics.parser.skipped_bytes,
             (unsigned long)spatial->diagnostics.parser.invalid_target_slots,
             (unsigned long)spatial->diagnostics.parser.coordinate_outliers,
             (unsigned long)spatial->diagnostics.parser.resync_count,
             (unsigned long)spatial->diagnostics.tracker.coordinate_outliers,
             (unsigned long)spatial->diagnostics.tracker.jump_outliers,
             (unsigned long)spatial->diagnostics.uart.read_timeout,
             (unsigned long)spatial->diagnostics.uart.read_zero,
             (unsigned long)spatial->diagnostics.uart.read_driver_error,
             (unsigned long)spatial->diagnostics.uart.fifo_overflow,
             (unsigned long)spatial->diagnostics.uart.queue_full,
             snapshot->recovery_state,
             (unsigned long)spatial->diagnostics.recovery.recovery_count,
             (unsigned long)spatial->diagnostics.recovery.init_failure_count,
             (unsigned long)spatial->diagnostics.recovery.consecutive_error_count,
             (unsigned long)spatial->diagnostics.recovery.consecutive_no_valid_count,
             (unsigned long)spatial->diagnostics.recovery.consecutive_no_rx_timeout_count);
    ESP_LOGI(TAG,
             "radar_tracker: active=%lu created=%lu matched=%lu deleted=%lu stale=%lu velocity_outliers=%lu dropped=%lu",
             (unsigned long)spatial->diagnostics.tracker.active_track_count,
             (unsigned long)spatial->diagnostics.tracker.new_track_count,
             (unsigned long)spatial->diagnostics.tracker.association_count,
             (unsigned long)spatial->diagnostics.tracker.deleted_track_count,
             (unsigned long)spatial->diagnostics.tracker.stale_track_count,
             (unsigned long)spatial->diagnostics.tracker.velocity_outliers,
             (unsigned long)spatial->diagnostics.tracker.dropped_target_count);

    for (size_t i = 0U; i < LD2450_MAX_TARGETS; ++i) {
        const radar_target_t *raw = &spatial->raw_targets[i];
        if (!raw->valid) {
            continue;
        }
        ESP_LOGI(TAG,
                 "local raw slot=%u x=%d y=%d speed=%d resolution=%u distance=%lu",
                 (unsigned int)i,
                 (int)raw->x_mm,
                 (int)raw->y_mm,
                 (int)raw->speed_cm_s,
                 (unsigned int)raw->resolution_mm,
                 (unsigned long)raw->distance_mm);
    }
    for (size_t i = 0U; i < spatial->accepted_target_count; ++i) {
        const radar_spatial_target_t *target = &spatial->accepted_targets[i];
        ESP_LOGI(TAG,
                 "local accepted index=%u x=%ld y=%ld distance=%lu angle=%d speed=%d",
                 (unsigned int)i,
                 (long)target->x_mm,
                 (long)target->y_mm,
                 (unsigned long)target->distance_mm,
                 (int)target->angle_deg,
                 (int)target->speed_cm_s);
    }
    for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS; ++i) {
        const radar_track_snapshot_t *track = &spatial->tracks[i];
        if (!track->active) {
            continue;
        }
        ESP_LOGI(TAG,
                 "local track=%lu visible=%d raw_x=%ld raw_y=%ld filtered_x=%ld filtered_y=%ld distance=%lu angle=%d speed=%d direction=%d confidence=%lu seen=%lu missed=%lu",
                 (unsigned long)track->track_id,
                 track->visible ? 1 : 0,
                 (long)track->raw_x_mm,
                 (long)track->raw_y_mm,
                 (long)track->filtered_x_mm,
                 (long)track->filtered_y_mm,
                 (unsigned long)track->distance_mm,
                 (int)track->angle_deg,
                 (int)track->speed_cm_s,
                 (int)track->direction_deg,
                 (unsigned long)track->confidence,
                 (unsigned long)track->consecutive_seen,
                 (unsigned long)track->missed_frames);
    }
}

static void diagnostics_task(void *arg)
{
    (void)arg;
    radar_presence_state_t previous_state[RADAR_SOURCE_COUNT] = {0};
    bool previous_online[RADAR_SOURCE_COUNT] = {0};
    radar_sensor_state_t previous_sensor = RADAR_SENSOR_OFFLINE;
    radar_occupancy_state_t previous_occupancy = RADAR_OCCUPANCY_UNKNOWN;
    radar_motion_state_t previous_motion = RADAR_MOTION_UNKNOWN;
    radar_uart_recovery_state_t previous_recovery = RADAR_UART_RECOVERY_OFFLINE;
    bool have_previous = false;
    uint64_t last_log_ms = 0U;

    while (true) {
        const uint64_t current_ms = now_ms();
        radar_registry_refresh(current_ms);

        if (radar_diag_snapshot_copy(&s_task_snapshot)) {
            for (size_t i = 0U; i < s_task_snapshot.registry_count; ++i) {
                const radar_diag_registry_snapshot_t *entry = &s_task_snapshot.registry[i];
                if (have_previous &&
                    (entry->snapshot.state != previous_state[i] ||
                     entry->source_online != previous_online[i])) {
                    radar_diagnostics_log_transition(
                        entry->source, RADAR_DIAGNOSTICS_TRANSITION_FRESHNESS_OR_UPDATE);
                }
                previous_state[i] = entry->snapshot.state;
                previous_online[i] = entry->source_online;
            }
            have_previous = s_task_snapshot.registry_count == RADAR_SOURCE_COUNT;

            if (s_task_snapshot.has_local_spatial) {
                const radar_spatial_snapshot_t *spatial = &s_task_snapshot.local_spatial;
                if (have_previous &&
                    (spatial->sensor_state != previous_sensor ||
                     spatial->occupancy_state != previous_occupancy ||
                     spatial->motion_state != previous_motion ||
                     spatial->diagnostics.recovery.state != previous_recovery)) {
                    ESP_LOGI(TAG,
                             "local transition sensor=%s occupancy=%s motion=%s recovery=%s frame_age_ms=%lu",
                             s_task_snapshot.sensor_state,
                             s_task_snapshot.occupancy_state,
                             s_task_snapshot.motion_state,
                             s_task_snapshot.recovery_state,
                             (unsigned long)spatial->frame_age_ms);
                }
                previous_sensor = spatial->sensor_state;
                previous_occupancy = spatial->occupancy_state;
                previous_motion = spatial->motion_state;
                previous_recovery = spatial->diagnostics.recovery.state;
            }

            /* Adaptive state/track logs are owned by radar_log_manager.  Keep
             * this legacy snapshot formatter compiled for diagnostics builds,
             * but never run it on the radar hot path. */
            if (false && (last_log_ms == 0U ||
                          current_ms - last_log_ms >= RADAR_CONFIG_DIAGNOSTICS_LOG_MS)) {
                last_log_ms = current_ms;
                log_summary(&s_task_snapshot, current_ms);
                ESP_LOGI(TAG,
                         "RADAR_DIAG_STACK_MONITOR free_words=%u",
                         (unsigned int)uxTaskGetStackHighWaterMark(NULL));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(RADAR_CONFIG_DIAGNOSTICS_POLL_MS));
    }
}

esp_err_t radar_diagnostics_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    if (!radar_registry_init()) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t created = xTaskCreate(diagnostics_task,
                                     "radar_diag",
                                     RADAR_CONFIG_DIAGNOSTICS_TASK_STACK,
                                     NULL,
                                     RADAR_CONFIG_DIAGNOSTICS_TASK_PRIORITY,
                                     &s_task);
    if (created != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "diagnostics task started summary_interval_ms=%u",
             (unsigned int)RADAR_CONFIG_DIAGNOSTICS_LOG_MS);
    return ESP_OK;
}

#include "radar_diagnostics.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "app_stack_monitor.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
static volatile bool s_stop_requested;
static StaticSemaphore_t s_exit_storage;
static SemaphoreHandle_t s_exit;
/* Keep the static task control block and its internal-RAM stack alive across
 * every radar_diag start/stop cycle.  ESP32-S3 StackType_t is byte-sized, but
 * derive the FreeRTOS depth from the byte contract to keep that explicit. */
#define RADAR_DIAG_TASK_STACK_BYTES ((size_t)RADAR_CONFIG_DIAGNOSTICS_TASK_STACK)
#define RADAR_DIAG_TASK_STACK_DEPTH \
    ((configSTACK_DEPTH_TYPE)(RADAR_DIAG_TASK_STACK_BYTES / sizeof(StackType_t)))
_Static_assert((RADAR_DIAG_TASK_STACK_BYTES % sizeof(StackType_t)) == 0U,
               "radar_diag stack bytes must align to StackType_t");
static StaticTask_t s_task_storage;
static StackType_t s_task_stack[RADAR_DIAG_TASK_STACK_DEPTH];
_Static_assert(sizeof(s_task_stack) == RADAR_DIAG_TASK_STACK_BYTES,
               "radar_diag static stack must match configured bytes");
/* The full value snapshot is larger than the diagnostics task's safe stack budget. */
static radar_diag_snapshot_t *s_task_snapshot;
static radar_registry_entry_t s_registry_entries[RADAR_SOURCE_COUNT];

static void log_static_stack_admission(void)
{
    ESP_LOGI(TAG,
             "RADAR_MEMORY_ADMISSION stage=radar_diag_stack requested=%u memory=internal storage=static admitted=1",
             (unsigned int)sizeof(s_task_stack));
}

static bool psram_workspace_admitted(size_t workspace_bytes)
{
    const size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t largest_bytes =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t required = workspace_bytes + RADAR_CONFIG_PSRAM_ALLOCATION_HEADROOM_BYTES;
    const size_t largest_required = workspace_bytes;
    const bool admitted = free_bytes >= required && largest_bytes >= largest_required;
    ESP_LOGI(TAG,
             "RADAR_MEMORY_ADMISSION stage=radar_diag_workspace requested=%u memory=psram psram_free=%u psram_largest=%u admitted=%u",
             (unsigned int)workspace_bytes,
             (unsigned int)free_bytes,
             (unsigned int)largest_bytes,
             admitted ? 1U : 0U);
    return admitted;
}

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
    out->registry_count = count > RADAR_SOURCE_COUNT ? RADAR_SOURCE_COUNT : count;
    for (size_t i = 0U; i < out->registry_count; ++i) {
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
             "RADAR_SOURCE_STATE event=transition source_id=%u source=%s device_id=%s room=%s sequence=%lu state=%s targets=%u online=%d uart_online=%d frame_fresh=%d session=%lu reason=%s",
             (unsigned int)entry.source,
             entry.source_name,
             entry.device_id,
             entry.room_id,
             (unsigned long)entry.sequence,
             entry.state,
             (unsigned int)entry.snapshot.current_target_count,
             entry.source_online ? 1 : 0,
             entry.snapshot.uart_online ? 1 : 0,
             entry.snapshot.frame_fresh ? 1 : 0,
             (unsigned long)entry.session_generation,
             transition_reason);
}

static void log_summary(const radar_diag_snapshot_t *snapshot, uint64_t current_ms)
{
    const size_t registry_count = snapshot->registry_count > RADAR_SOURCE_COUNT
        ? RADAR_SOURCE_COUNT : snapshot->registry_count;
    for (size_t i = 0U; i < registry_count; ++i) {
        const radar_diag_registry_snapshot_t *entry = &snapshot->registry[i];
        ESP_LOGI(TAG,
                 "RADAR_SOURCE_STATE event=diagnostic source_id=%u source=%s device_id=%s room=%s sequence=%lu state=%s targets=%u online=%d uart_online=%d valid_age_ms=%" PRIu64 " motion_age_ms=%" PRIu64 " parse_errors=%lu sequence_rejects=%lu identity_mismatches=%lu freshness_expiry=%lu",
                 (unsigned int)entry->source,
                 entry->source_name,
                 entry->device_id,
                 entry->room_id,
                 (unsigned long)entry->sequence,
                 entry->state,
                 (unsigned int)entry->snapshot.current_target_count,
                 entry->source_online ? 1 : 0,
                 entry->snapshot.uart_online ? 1 : 0,
                 age_ms(entry->snapshot.last_valid_frame_ms, current_ms),
                 age_ms(entry->snapshot.last_motion_ms, current_ms),
                 (unsigned long)(entry->diagnostics.parse_error_count == UINT32_MAX ? 0U :
                                 entry->diagnostics.parse_error_count),
                 (unsigned long)entry->diagnostics.sequence_reject_count,
                 (unsigned long)entry->diagnostics.identity_mismatch_count,
                 (unsigned long)entry->diagnostics.freshness_expiry_count);
    }
    ESP_LOGI(TAG,
             "RADAR_SOURCE_STATE event=unattributed source_id=255 source=UNKNOWN device_id=unknown room=unknown sequence=0 unattributed_parse_errors=%lu",
             (unsigned long)snapshot->unattributed_parse_errors);

    if (!snapshot->has_local_spatial) {
        return;
    }

    const radar_spatial_snapshot_t *spatial = &snapshot->local_spatial;
    const radar_source_id_t local_source = RADAR_SOURCE_S3_LOCAL;
    const char *local_source_name = radar_registry_source_name(local_source);
    const char *local_device_id = radar_registry_device_id(local_source);
    const char *local_room_id = radar_registry_room_id(local_source);
    radar_source_context_log_view_t local_view = {0};
    const unsigned long local_sequence = (unsigned long)(
        radar_source_context_get_log_view(local_source, &local_view) ? local_view.sequence : 0U);
    ESP_LOGI(TAG,
             "RADAR_SOURCE_STATE event=local_sensor sensor=%s occupancy=%s motion=%s raw=%u accepted=%u tracks=%u frame_age_ms=%lu confidence=%lu source_id=%u source=%s device_id=%s room=%s sequence=%lu parser[bytes=%lu valid=%lu bad_header=%lu bad_length=%lu bad_tail=%lu skipped=%lu invalid_slots=%lu coord_outliers=%lu resync=%lu] tracker[coord_outliers=%lu jump_outliers=%lu] uart[read_timeout=%lu read_zero=%lu read_driver_error=%lu fifo_overflow=%lu queue_full=%lu] recovery[state=%s count=%lu init_fail=%lu errors=%lu no_valid=%lu no_rx_timeout=%lu]",
             snapshot->sensor_state,
             snapshot->occupancy_state,
             snapshot->motion_state,
             (unsigned int)spatial->raw_target_count,
             (unsigned int)spatial->accepted_target_count,
             (unsigned int)spatial->active_track_count,
             (unsigned long)spatial->frame_age_ms,
             (unsigned long)spatial->occupancy_confidence,
             (unsigned int)local_source,
             local_source_name,
             local_device_id != NULL ? local_device_id : "unknown",
             local_room_id != NULL ? local_room_id : "unknown",
             local_sequence,
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
             "RADAR_TRACK_UPDATE event=diagnostic active=%lu created=%lu matched=%lu deleted=%lu stale=%lu velocity_outliers=%lu dropped=%lu source_id=%u source=%s device_id=%s room=%s sequence=%lu",
             (unsigned long)spatial->diagnostics.tracker.active_track_count,
             (unsigned long)spatial->diagnostics.tracker.new_track_count,
             (unsigned long)spatial->diagnostics.tracker.association_count,
             (unsigned long)spatial->diagnostics.tracker.deleted_track_count,
             (unsigned long)spatial->diagnostics.tracker.stale_track_count,
             (unsigned long)spatial->diagnostics.tracker.velocity_outliers,
             (unsigned long)spatial->diagnostics.tracker.dropped_target_count,
             (unsigned int)local_source,
             local_source_name,
             local_device_id != NULL ? local_device_id : "unknown",
             local_room_id != NULL ? local_room_id : "unknown",
             local_sequence);

    for (size_t i = 0U; i < LD2450_MAX_TARGETS; ++i) {
        const radar_target_t *raw = &spatial->raw_targets[i];
        if (!raw->valid) {
            continue;
        }
        ESP_LOGI(TAG,
                 "RADAR_RX_FRAME event=raw_slot slot=%u x=%d y=%d speed=%d resolution=%u distance=%lu source_id=%u source=%s device_id=%s room=%s sequence=%lu",
                 (unsigned int)i,
                 (int)raw->x_mm,
                 (int)raw->y_mm,
                 (int)raw->speed_cm_s,
                 (unsigned int)raw->resolution_mm,
                 (unsigned long)raw->distance_mm,
                 (unsigned int)local_source,
                 local_source_name,
                 local_device_id != NULL ? local_device_id : "unknown",
                 local_room_id != NULL ? local_room_id : "unknown",
                 local_sequence);
    }
    const size_t accepted_count = spatial->accepted_target_count > LD2450_MAX_TARGETS
        ? LD2450_MAX_TARGETS : spatial->accepted_target_count;
    for (size_t i = 0U; i < accepted_count; ++i) {
        const radar_spatial_target_t *target = &spatial->accepted_targets[i];
        ESP_LOGI(TAG,
                 "RADAR_TRACK_UPDATE event=accepted index=%u x=%ld y=%ld distance=%lu angle=%d speed=%d source_id=%u source=%s device_id=%s room=%s sequence=%lu",
                 (unsigned int)i,
                 (long)target->x_mm,
                 (long)target->y_mm,
                 (unsigned long)target->distance_mm,
                 (int)target->angle_deg,
                 (int)target->speed_cm_s,
                 (unsigned int)local_source,
                 local_source_name,
                 local_device_id != NULL ? local_device_id : "unknown",
                 local_room_id != NULL ? local_room_id : "unknown",
                 local_sequence);
    }
    for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS; ++i) {
        const radar_track_snapshot_t *track = &spatial->tracks[i];
        if (!track->active) {
            continue;
        }
        ESP_LOGI(TAG,
                 "RADAR_TRACK_UPDATE_COMPAT local track=%lu visible=%d raw_x=%ld raw_y=%ld filtered_x=%ld filtered_y=%ld distance=%lu angle=%d speed=%d direction=%d confidence=%lu seen=%lu missed=%lu source_id=%u source=%s device_id=%s room=%s sequence=%lu",
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
                 (unsigned long)track->missed_frames,
                 (unsigned int)local_source,
                 local_source_name,
                 local_device_id != NULL ? local_device_id : "unknown",
                 local_room_id != NULL ? local_room_id : "unknown",
                 local_sequence);
    }
}

static void diagnostics_task(void *arg)
{
    (void)arg;
    app_stack_monitor_report(TAG,
                             "radar_diag",
                             sizeof(s_task_stack),
                             "entry");
    radar_presence_state_t previous_state[RADAR_SOURCE_COUNT] = {0};
    bool previous_online[RADAR_SOURCE_COUNT] = {0};
    radar_sensor_state_t previous_sensor = RADAR_SENSOR_OFFLINE;
    radar_occupancy_state_t previous_occupancy = RADAR_OCCUPANCY_UNKNOWN;
    radar_motion_state_t previous_motion = RADAR_MOTION_UNKNOWN;
    radar_uart_recovery_state_t previous_recovery = RADAR_UART_RECOVERY_OFFLINE;
    bool have_previous = false;
    uint64_t last_log_ms = 0U;

    while (!s_stop_requested) {
        const uint64_t current_ms = now_ms();
        radar_registry_refresh(current_ms);

        if (s_task_snapshot != NULL && radar_diag_snapshot_copy(s_task_snapshot)) {
            const size_t registry_count = s_task_snapshot->registry_count > RADAR_SOURCE_COUNT
                ? RADAR_SOURCE_COUNT : s_task_snapshot->registry_count;
            for (size_t i = 0U; i < registry_count; ++i) {
                const radar_diag_registry_snapshot_t *entry = &s_task_snapshot->registry[i];
                if (have_previous &&
                    (entry->snapshot.state != previous_state[i] ||
                     entry->source_online != previous_online[i])) {
                    radar_diagnostics_log_transition(
                        entry->source, RADAR_DIAGNOSTICS_TRANSITION_FRESHNESS_OR_UPDATE);
                }
                previous_state[i] = entry->snapshot.state;
                previous_online[i] = entry->source_online;
            }
            have_previous = registry_count == RADAR_SOURCE_COUNT;

            if (s_task_snapshot->has_local_spatial) {
                const radar_spatial_snapshot_t *spatial = &s_task_snapshot->local_spatial;
                if (have_previous &&
                    (spatial->sensor_state != previous_sensor ||
                     spatial->occupancy_state != previous_occupancy ||
                     spatial->motion_state != previous_motion ||
                     spatial->diagnostics.recovery.state != previous_recovery)) {
                    radar_source_context_log_view_t local_view = {0};
                    const bool have_local = radar_source_context_get_log_view(
                        RADAR_SOURCE_S3_LOCAL, &local_view);
                    ESP_LOGI(TAG,
                             "RADAR_SOURCE_STATE event=local_transition source_id=0 source=S3_LOCAL device_id=sensair_s3_gateway_01 room=s3_local sequence=%lu sensor=%s occupancy=%s motion=%s recovery=%s frame_age_ms=%lu",
                             (unsigned long)(have_local ? local_view.sequence : 0U),
                             s_task_snapshot->sensor_state,
                             s_task_snapshot->occupancy_state,
                             s_task_snapshot->motion_state,
                             s_task_snapshot->recovery_state,
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
                log_summary(s_task_snapshot, current_ms);
                radar_source_context_log_view_t local_view = {0};
                const bool have_local = radar_source_context_get_log_view(
                    RADAR_SOURCE_S3_LOCAL, &local_view);
                ESP_LOGI(TAG,
                         "RADAR_STACK task=radar_diag free_bytes=%u source_id=0 source=S3_LOCAL device_id=sensair_s3_gateway_01 room=s3_local sequence=%lu",
                         (unsigned int)app_stack_monitor_high_water(),
                         (unsigned long)(have_local ? local_view.sequence : 0U));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(RADAR_CONFIG_DIAGNOSTICS_POLL_MS));
    }
    if (s_exit != NULL) {
        xSemaphoreGive(s_exit);
    }
    for (;;) {
        vTaskSuspend(NULL);
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
    if (s_exit == NULL) {
        s_exit = xSemaphoreCreateBinaryStatic(&s_exit_storage);
        if (s_exit == NULL) return ESP_ERR_NO_MEM;
    }
    log_static_stack_admission();
    if (!psram_workspace_admitted(sizeof(*s_task_snapshot))) {
        return ESP_ERR_NO_MEM;
    }
    bool allocated_here = false;
    if (s_task_snapshot == NULL) {
        s_task_snapshot = heap_caps_calloc(1U,
                                           sizeof(*s_task_snapshot),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_task_snapshot == NULL) {
            return ESP_ERR_NO_MEM;
        }
        allocated_here = true;
    }
    while (xSemaphoreTake(s_exit, 0) == pdTRUE) {
    }
    s_stop_requested = false;
    s_task = xTaskCreateStatic(diagnostics_task,
                               "radar_diag",
                               RADAR_DIAG_TASK_STACK_DEPTH,
                               NULL,
                               RADAR_CONFIG_DIAGNOSTICS_TASK_PRIORITY,
                               s_task_stack,
                               &s_task_storage);
    if (s_task == NULL) {
        s_task = NULL;
        s_stop_requested = false;
        if (allocated_here) {
            heap_caps_free(s_task_snapshot);
            s_task_snapshot = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    app_stack_monitor_log_task_created(TAG,
                                       "radar_diag",
                                       s_task,
                                       sizeof(s_task_stack));
    ESP_LOGI(TAG,
             "RADAR_SOURCE_STATE event=diagnostics_started source_id=255 source=SYSTEM device_id=system room=system sequence=0 summary_interval_ms=%u snapshot=psram bytes=%u",
             (unsigned int)RADAR_CONFIG_DIAGNOSTICS_LOG_MS,
             (unsigned int)sizeof(*s_task_snapshot));
    return ESP_OK;
}

esp_err_t radar_diagnostics_stop(void)
{
    TaskHandle_t task = s_task;
    if (task != NULL) {
        if (task == xTaskGetCurrentTaskHandle()) return ESP_ERR_INVALID_STATE;
        s_stop_requested = true;
        if (s_exit == NULL ||
            xSemaphoreTake(s_exit, pdMS_TO_TICKS(RADAR_CONFIG_TASK_STOP_TIMEOUT_MS)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelete(task);
        s_task = NULL;
    }
    s_stop_requested = false;
    if (s_task_snapshot != NULL) {
        heap_caps_free(s_task_snapshot);
        s_task_snapshot = NULL;
    }
    return ESP_OK;
}

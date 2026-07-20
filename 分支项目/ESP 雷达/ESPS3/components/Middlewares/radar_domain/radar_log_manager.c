#include "radar_log_manager.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radar_config.h"
#include "radar_person_continuity.h"
#include "radar_presence.h"
#include "app_stack_monitor.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"

static const char TAG[] = "radar_log";
static TaskHandle_t s_task;
typedef struct {
    radar_source_id_t source_id;
    char source_name[RADAR_SOURCE_CONTEXT_NAME_LEN];
    char device_id[RADAR_SOURCE_CONTEXT_DEVICE_ID_LEN];
    char room_id[RADAR_SOURCE_CONTEXT_ROOM_ID_LEN];
    uint32_t sequence;
    uint32_t frame_sequence;
} radar_log_identity_t;

typedef struct {
    bool pending;
    radar_log_identity_t identity;
    radar_spatial_snapshot_t snapshot;
    radar_rate_manager_t rate_manager;
    uint32_t drop_count;
} radar_log_pending_context_t;

static radar_log_pending_context_t s_pending_contexts[RADAR_SOURCE_COUNT];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_stack_free_words;
static bool s_stack_pending;

static uint64_t now_ms(void)
{
    const int64_t now_us = esp_timer_get_time();
    return now_us > 0 ? (uint64_t)(now_us / 1000) : 0U;
}

static bool elapsed(uint64_t now, uint64_t previous, uint32_t interval_ms)
{
    return previous == 0U || now < previous || now - previous >= interval_ms;
}

static bool snapshot_occupies_room(const radar_spatial_snapshot_t *snapshot)
{
    return snapshot != NULL && (snapshot->occupancy_state == RADAR_OCCUPANCY_PRESENT ||
        snapshot->occupancy_state == RADAR_OCCUPANCY_HOLD);
}

static const char *motion_name(radar_motion_state_t motion)
{
    switch (motion) {
    case RADAR_MOTION_MOVING: return "moving";
    case RADAR_MOTION_STILL_CANDIDATE: return "still_candidate";
    case RADAR_MOTION_NONE: return "none";
    case RADAR_MOTION_UNKNOWN:
    default: return "unknown";
    }
}

static radar_presence_state_t snapshot_presence(const radar_spatial_snapshot_t *snapshot)
{
    if (snapshot == NULL || snapshot->sensor_state != RADAR_SENSOR_VALID) return RADAR_STATE_UNKNOWN;
    if (snapshot->occupancy_state == RADAR_OCCUPANCY_PRESENT) {
        return snapshot->motion_state == RADAR_MOTION_MOVING ? RADAR_STATE_MOTION : RADAR_STATE_PRESENT;
    }
    if (snapshot->occupancy_state == RADAR_OCCUPANCY_HOLD) return RADAR_STATE_HOLD;
    if (snapshot->occupancy_state == RADAR_OCCUPANCY_VACANT_INFERRED) return RADAR_STATE_VACANT_INFERRED;
    return RADAR_STATE_UNKNOWN;
}

static uint32_t velocity_mm_s(const radar_track_snapshot_t *track)
{
    const double x = (double)track->velocity_x_mm_s;
    const double y = (double)track->velocity_y_mm_s;
    const double velocity = sqrt(x * x + y * y);
    return velocity > UINT32_MAX ? UINT32_MAX : (uint32_t)velocity;
}

static void format_meters(int32_t millimeters, char *out, size_t out_size)
{
    const int64_t absolute_mm = millimeters < 0 ? -(int64_t)millimeters : millimeters;
    (void)snprintf(out, out_size, "%s%lld.%03lld", millimeters < 0 ? "-" : "",
                   (long long)(absolute_mm / 1000),
                   (long long)(absolute_mm % 1000));
}

static bool state_changed(const radar_spatial_snapshot_t *snapshot,
                          const radar_rate_manager_t *rate_manager,
                          const radar_spatial_snapshot_t *previous_snapshot,
                          radar_rate_mode_t previous_mode,
                          bool have_previous)
{
    return !have_previous || previous_mode != rate_manager->mode ||
           previous_snapshot->sensor_state != snapshot->sensor_state ||
           previous_snapshot->occupancy_state != snapshot->occupancy_state ||
           previous_snapshot->motion_state != snapshot->motion_state ||
           previous_snapshot->active_track_count != snapshot->active_track_count ||
           previous_snapshot->visible_person_count != snapshot->visible_person_count ||
           previous_snapshot->retained_person_count != snapshot->retained_person_count ||
           previous_snapshot->source_person_count != snapshot->source_person_count ||
           previous_snapshot->count_state != snapshot->count_state;
}

static void format_occupied_rooms(const RadarHomeState *home,
                                  char *out,
                                  size_t out_size)
{
    if (out == NULL || out_size == 0U) return;
    out[0] = '\0';
    if (home == NULL || home->occupied_room_count == 0U) {
        (void)snprintf(out, out_size, "[]");
        return;
    }
    size_t used = 0U;
    const int prefix_written = snprintf(out, out_size, "[");
    if (prefix_written < 0 || (size_t)prefix_written >= out_size) return;
    used = (size_t)prefix_written;
    for (uint8_t i = 0U; i < home->occupied_room_count; ++i) {
        const RadarRoomState *room = &home->occupied_rooms[i];
        const int written = snprintf(out + used, out_size - used, "%s%s:%s",
                                     i == 0U ? "" : "|", room->source, room->room_id);
        if (written < 0 || (size_t)written >= out_size - used) break;
        used += (size_t)written;
    }
    if (used + 1U < out_size) {
        out[used++] = ']';
        out[used] = '\0';
    }
}

static void emit_source_logs(const radar_log_identity_t *context,
                             const radar_spatial_snapshot_t *snapshot,
                             const radar_rate_manager_t *rate_manager,
                             const radar_spatial_snapshot_t *previous_snapshot,
                             radar_rate_mode_t previous_mode,
                             bool have_previous,
                             uint64_t current_ms,
                             uint64_t *last_state_log_ms,
                             uint64_t *last_track_log_ms,
                             uint64_t *last_compat_track_log_ms,
                             uint64_t *last_summary_log_ms,
                             uint64_t *last_drop_log_ms,
                             uint32_t drops)
{
    if (context == NULL || snapshot == NULL || rate_manager == NULL) return;
    const radar_source_id_t source = context->source_id;
    const radar_rate_policy_t *policy = &rate_manager->policy;
    const char *source_name = context->source_name;
    const char *device_id = context->device_id;
    const char *room = context->room_id;
    const uint32_t sequence = context->sequence;
    const radar_track_snapshot_t *target = snapshot->active_track_count > 0U ?
        &snapshot->current_targets[0] : NULL;
    const bool transition = state_changed(snapshot, rate_manager, previous_snapshot,
                                          previous_mode, have_previous);
    if (transition || elapsed(current_ms, *last_state_log_ms, policy->state_log_period_ms)) {
        char valid_age[24];
        if (snapshot->latest_frame_ms == 0U || current_ms < snapshot->latest_frame_ms) {
            (void)snprintf(valid_age, sizeof(valid_age), "unknown");
        } else {
            (void)snprintf(valid_age, sizeof(valid_age), "%llu",
                           (unsigned long long)(current_ms - snapshot->latest_frame_ms));
        }
        ESP_LOGI(TAG,
                 "RADAR_SOURCE_STATE event=summary mode=%s active=%u target=%lu x=%ld y=%ld velocity=%lu valid_age_ms=%s timestamp_ms=%llu source_id=%u source=%s device_id=%s room=%s sequence=%lu source_person_count=%u count_state=%s track_log_hz=%lu",
                 radar_rate_manager_mode_name(rate_manager->mode),
                 (unsigned int)snapshot->active_track_count,
                 target == NULL ? 0UL : (unsigned long)target->track_id,
                 target == NULL ? 0L : (long)target->filtered_x_mm,
                 target == NULL ? 0L : (long)target->filtered_y_mm,
                 target == NULL ? 0UL : (unsigned long)velocity_mm_s(target), valid_age,
                 (unsigned long long)current_ms, (unsigned int)source, source_name,
                 device_id != NULL ? device_id : "unknown",
                 room != NULL ? room : "unknown",
                 (unsigned long)sequence,
                 (unsigned int)snapshot->source_person_count,
                 radar_person_count_state_name(snapshot->count_state),
                 policy->track_log_period_ms == 0U ? 0UL :
                     (unsigned long)(1000U / policy->track_log_period_ms));
        *last_state_log_ms = current_ms;
    }
    if (target != NULL && policy->track_log_period_ms > 0U &&
        (transition || elapsed(current_ms, *last_track_log_ms, policy->track_log_period_ms))) {
        ESP_LOGI(TAG,
                 "RADAR_TRACK_UPDATE track_id=T%03lu raw_x=%ld raw_y=%ld filtered_x=%ld filtered_y=%ld velocity=%lu match_distance=%lu confidence=%lu timestamp_ms=%llu source_id=%u source=%s device_id=%s room=%s sequence=%lu",
                 (unsigned long)target->track_id, (long)target->raw_x_mm,
                 (long)target->raw_y_mm, (long)target->filtered_x_mm,
                 (long)target->filtered_y_mm, (unsigned long)velocity_mm_s(target),
                 (unsigned long)target->last_match_distance_mm,
                 (unsigned long)target->confidence, (unsigned long long)current_ms,
                 (unsigned int)source, source_name,
                 device_id != NULL ? device_id : "unknown", room != NULL ? room : "unknown",
                 (unsigned long)sequence);
        if (elapsed(current_ms, *last_compat_track_log_ms, 1000U)) {
            char x_meters[16];
            char y_meters[16];
            format_meters(target->filtered_x_mm, x_meters, sizeof(x_meters));
            format_meters(target->filtered_y_mm, y_meters, sizeof(y_meters));
            ESP_LOGI(TAG,
                     "RADAR_TRACK_UPDATE_COMPAT local track=%lu visible=1 raw_x=%ld raw_y=%ld filtered_x=%ld filtered_y=%ld distance=%lu angle=%d speed=%d direction=%d confidence=%lu seen=%lu missed=%lu timestamp_ms=%llu source_id=%u source=%s device_id=%s room=%s sequence=%lu",
                     (unsigned long)target->track_id, (long)target->raw_x_mm,
                     (long)target->raw_y_mm, (long)target->filtered_x_mm,
                     (long)target->filtered_y_mm, (unsigned long)target->distance_mm,
                     (int)target->angle_deg, (int)target->speed_cm_s, (int)target->direction_deg,
                     (unsigned long)target->confidence, (unsigned long)target->consecutive_seen,
                     (unsigned long)target->missed_frames, (unsigned long long)current_ms,
                     (unsigned int)source, source_name,
                     device_id != NULL ? device_id : "unknown", room != NULL ? room : "unknown",
                     (unsigned long)sequence);
            ESP_LOGI(TAG,
                     "RADAR_TRACK_UPDATE_METERS local track id=T%03lu x=%s y=%s source_id=%u source=%s device_id=%s room=%s sequence=%lu",
                     (unsigned long)target->track_id, x_meters, y_meters,
                     (unsigned int)source, source_name, device_id != NULL ? device_id : "unknown",
                     room != NULL ? room : "unknown", (unsigned long)sequence);
            *last_compat_track_log_ms = current_ms;
        }
        *last_track_log_ms = current_ms;
    }
    const bool summary_due = transition || elapsed(current_ms, *last_summary_log_ms, 1000U);
    if (summary_due) {
        const radar_tracker_diagnostics_t *diagnostics = &snapshot->diagnostics.tracker;
        ESP_LOGI(TAG,
                 "RADAR_TRACK_UPDATE event=summary active=%u created=%lu matched=%lu deleted=%lu stale=%lu tentative=%u velocity_outliers=%lu source_id=%u source=%s device_id=%s room=%s sequence=%lu",
                 (unsigned int)snapshot->active_track_count,
                 (unsigned long)diagnostics->new_track_count,
                 (unsigned long)diagnostics->association_count,
                 (unsigned long)diagnostics->deleted_track_count,
                 (unsigned long)diagnostics->stale_track_count,
                 (unsigned int)(snapshot->accepted_target_count > snapshot->active_track_count),
                 (unsigned long)diagnostics->velocity_outliers, (unsigned int)source, source_name,
                 device_id != NULL ? device_id : "unknown", room != NULL ? room : "unknown",
                 (unsigned long)sequence);
        *last_summary_log_ms = current_ms;
    }
    if (summary_due) {
        ESP_LOGI(TAG,
                 "RADAR_SOURCE_STATE event=counts source_id=%u source=%s device_id=%s room=%s sequence=%lu timestamp_ms=%llu raw_target_count=%u accepted_target_count=%u visible_track_count=%u confirmed_active_track_count=%u history_target_count=%u visible_person_count=%u retained_person_count=%u source_person_count=%u count_state=%s",
                 (unsigned int)source, source_name, device_id != NULL ? device_id : "unknown",
                 room != NULL ? room : "unknown",
                 (unsigned long)sequence,
                 (unsigned long long)current_ms,
                 (unsigned int)snapshot->raw_target_count,
                 (unsigned int)snapshot->accepted_target_count,
                 (unsigned int)snapshot->visible_track_count,
                 (unsigned int)snapshot->confirmed_active_track_count,
                 (unsigned int)snapshot->history_target_count,
                 (unsigned int)snapshot->visible_person_count,
                 (unsigned int)snapshot->retained_person_count,
                 (unsigned int)snapshot->source_person_count,
                 radar_person_count_state_name(snapshot->count_state));
        ESP_LOGI(TAG,
                 "RADAR_RX_FRAME source_id=%u source=%s device_id=%s room=%s sequence=%lu frame_sequence=%lu timestamp_ms=%llu raw_target_count=%u filtered_target_count=%u",
                 (unsigned int)source, source_name,
                 device_id != NULL ? device_id : "unknown", room != NULL ? room : "unknown",
                 (unsigned long)sequence, (unsigned long)context->frame_sequence,
                 (unsigned long long)current_ms, (unsigned int)snapshot->raw_target_count,
                 (unsigned int)snapshot->accepted_target_count);
    }
    if (drops > 0U && elapsed(current_ms, *last_drop_log_ms, 1000U)) {
        ESP_LOGW(TAG, "RADAR_LOG_DROP count=%lu source_id=%u source=%s device_id=%s room=%s sequence=%lu",
                 (unsigned long)drops, (unsigned int)source, source_name,
                 device_id != NULL ? device_id : "unknown", room != NULL ? room : "unknown",
                 (unsigned long)sequence);
        *last_drop_log_ms = current_ms;
    }
    if (transition) {
        RadarHomeState home;
        radar_registry_get_home_state(&home);
        char occupied_rooms[128];
        format_occupied_rooms(&home, occupied_rooms, sizeof(occupied_rooms));
        ESP_LOGI(TAG,
                 "RADAR_ROOM_STATE source_id=%u source=%s device_id=%s room=%s occupied=%u presence=%s motion=%s last_update_ms=%llu",
                 (unsigned int)source, source_name, device_id != NULL ? device_id : "unknown",
                 room != NULL ? room : "unknown",
                 snapshot_occupies_room(snapshot) ? 1U : 0U,
                 radar_presence_state_name(snapshot_presence(snapshot)),
                 motion_name(snapshot->motion_state),
                 (unsigned long long)current_ms);
        ESP_LOGI(TAG,
                 "RADAR_HOME_STATE occupied_room_count=%u occupied_rooms=%s home_person_count=%u timestamp_ms=%llu",
                 (unsigned int)home.occupied_room_count, occupied_rooms,
                 (unsigned int)home.home_person_count, (unsigned long long)current_ms);
    }
}

static void log_task(void *arg)
{
    (void)arg;
    app_stack_monitor_report(TAG, "radar_log", 6144U, "entry");
    static radar_log_identity_t identities[RADAR_SOURCE_COUNT];
    static radar_spatial_snapshot_t snapshots[RADAR_SOURCE_COUNT];
    static radar_spatial_snapshot_t previous_snapshots[RADAR_SOURCE_COUNT];
    static radar_rate_manager_t rate_managers[RADAR_SOURCE_COUNT];
    static radar_rate_mode_t previous_modes[RADAR_SOURCE_COUNT];
    static bool have_previous[RADAR_SOURCE_COUNT];
    uint64_t last_state_log_ms[RADAR_SOURCE_COUNT] = {0};
    uint64_t last_track_log_ms[RADAR_SOURCE_COUNT] = {0};
    uint64_t last_compat_track_log_ms[RADAR_SOURCE_COUNT] = {0};
    uint64_t last_summary_log_ms[RADAR_SOURCE_COUNT] = {0};
    uint64_t last_drop_log_ms[RADAR_SOURCE_COUNT] = {0};

    ESP_LOGI(TAG,
             "RADAR_TOOL_COMPAT_VERSION=3 source_id=255 source=SYSTEM device_id=system room=system sequence=0");
    while (true) {
        bool pending[RADAR_SOURCE_COUNT] = {false};
        uint32_t drops[RADAR_SOURCE_COUNT] = {0};
        bool stack_pending = false;
        uint32_t stack_free_words = 0U;
        portENTER_CRITICAL(&s_lock);
        for (size_t i = 0U; i < RADAR_SOURCE_COUNT; ++i) {
            if (s_pending_contexts[i].pending) {
                identities[i] = s_pending_contexts[i].identity;
                snapshots[i] = s_pending_contexts[i].snapshot;
                rate_managers[i] = s_pending_contexts[i].rate_manager;
                s_pending_contexts[i].pending = false;
                pending[i] = true;
            }
            drops[i] = s_pending_contexts[i].drop_count;
        }
        stack_pending = s_stack_pending;
        stack_free_words = s_stack_free_words;
        s_stack_pending = false;
        portEXIT_CRITICAL(&s_lock);

        bool any_pending = stack_pending;
        for (size_t i = 0U; i < RADAR_SOURCE_COUNT; ++i) any_pending = any_pending || pending[i];
        if (!any_pending) {
            vTaskDelay(pdMS_TO_TICKS(20U));
            continue;
        }
        const uint64_t current_ms = now_ms();
        if (stack_pending) {
            const RadarSourceContext *local = radar_source_context_get(RADAR_SOURCE_S3_LOCAL);
            ESP_LOGI(TAG,
                     "RADAR_STACK task=radar_local free_words=%lu source_id=0 source=%s device_id=%s room=%s sequence=%lu",
                     (unsigned long)stack_free_words,
                     local != NULL ? local->source_name : "S3_LOCAL",
                     local != NULL ? local->device_id : "unknown",
                     local != NULL ? local->room_id : "unknown",
                     (unsigned long)(local != NULL ? local->sequence : 0U));
        }
        for (radar_source_id_t source = RADAR_SOURCE_S3_LOCAL;
             source < RADAR_SOURCE_COUNT;
             source = (radar_source_id_t)(source + 1)) {
#if !CONFIG_S3_RADAR_PER_SOURCE_LOG_SCHEDULER
            if (source != RADAR_SOURCE_S3_LOCAL) continue;
#endif
            if (!pending[source]) continue;
            emit_source_logs(&identities[source], &snapshots[source], &rate_managers[source],
                             &previous_snapshots[source], previous_modes[source],
                             have_previous[source], current_ms, &last_state_log_ms[source],
                             &last_track_log_ms[source], &last_compat_track_log_ms[source],
                             &last_summary_log_ms[source],
                             &last_drop_log_ms[source], drops[source]);
            previous_snapshots[source] = snapshots[source];
            previous_modes[source] = rate_managers[source].mode;
            have_previous[source] = true;
        }
    }
}

esp_err_t radar_log_manager_start(void)
{
    if (s_task != NULL) return ESP_OK;
    const BaseType_t created = xTaskCreateWithCaps(log_task,
                                                   "radar_log",
                                                   6144U,
                                                   NULL,
                                                   1U,
                                                   &s_task,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void radar_log_manager_publish(const RadarSourceContext *context,
                               const radar_rate_manager_t *rate_manager)
{
    if (context == NULL || context->source_id >= RADAR_SOURCE_COUNT || rate_manager == NULL) return;
    const radar_source_id_t source = context->source_id;
#if !CONFIG_S3_RADAR_PER_SOURCE_LOG_SCHEDULER
    if (source != RADAR_SOURCE_S3_LOCAL) return;
#endif
    portENTER_CRITICAL(&s_lock);
    if (s_pending_contexts[source].pending &&
        s_pending_contexts[source].drop_count < UINT32_MAX) {
        ++s_pending_contexts[source].drop_count;
    }
    s_pending_contexts[source].identity = (radar_log_identity_t){
        .source_id = context->source_id,
        .sequence = context->sequence,
        .frame_sequence = context->frame_sequence,
    };
    memcpy(s_pending_contexts[source].identity.source_name,
           context->source_name,
           sizeof(s_pending_contexts[source].identity.source_name));
    memcpy(s_pending_contexts[source].identity.device_id,
           context->device_id,
           sizeof(s_pending_contexts[source].identity.device_id));
    memcpy(s_pending_contexts[source].identity.room_id,
           context->room_id,
           sizeof(s_pending_contexts[source].identity.room_id));
    s_pending_contexts[source].snapshot = context->snapshot;
    s_pending_contexts[source].rate_manager = *rate_manager;
    s_pending_contexts[source].pending = true;
    portEXIT_CRITICAL(&s_lock);
}

void radar_log_manager_publish_stack_words(uint32_t free_words)
{
    portENTER_CRITICAL(&s_lock);
    s_stack_free_words = free_words;
    s_stack_pending = true;
    portEXIT_CRITICAL(&s_lock);
}

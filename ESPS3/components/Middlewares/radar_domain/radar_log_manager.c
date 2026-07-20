#include "radar_log_manager.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "radar_config.h"
#include "radar_person_continuity.h"

static const char TAG[] = "radar_log";
static TaskHandle_t s_task;
static bool s_task_with_caps;

typedef struct {
    radar_spatial_snapshot_t published_snapshots[RADAR_SOURCE_COUNT];
    radar_spatial_snapshot_t task_snapshots[RADAR_SOURCE_COUNT];
    radar_spatial_snapshot_t previous_snapshots[RADAR_SOURCE_COUNT];
    radar_rate_manager_t published_rate_managers[RADAR_SOURCE_COUNT];
    radar_rate_manager_t task_rate_managers[RADAR_SOURCE_COUNT];
    radar_rate_mode_t previous_modes[RADAR_SOURCE_COUNT];
    bool have_previous[RADAR_SOURCE_COUNT];
} radar_log_storage_t;

static radar_log_storage_t *s_storage;
static bool s_pending[RADAR_SOURCE_COUNT];
static uint32_t s_log_drop_count[RADAR_SOURCE_COUNT];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_stack_free_words;
static bool s_stack_pending;

#define RADAR_LOG_TASK_STACK 6144U

static radar_log_storage_t *allocate_log_storage(void)
{
    radar_log_storage_t *storage = heap_caps_calloc(1U,
                                                    sizeof(*storage),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storage == NULL) {
        ESP_LOGW(TAG, "PSRAM storage allocation failed; falling back to internal RAM");
        storage = heap_caps_calloc(1U,
                                   sizeof(*storage),
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return storage;
}

static uint64_t now_ms(void)
{
    const int64_t now_us = esp_timer_get_time();
    return now_us > 0 ? (uint64_t)(now_us / 1000) : 0U;
}

static bool elapsed(uint64_t now, uint64_t previous, uint32_t interval_ms)
{
    return previous == 0U || now < previous || now - previous >= interval_ms;
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
           previous_snapshot->business_person_count != snapshot->business_person_count ||
           previous_snapshot->count_state != snapshot->count_state;
}

static void emit_source_logs(radar_source_id_t source,
                             const radar_spatial_snapshot_t *snapshot,
                             const radar_rate_manager_t *rate_manager,
                             const radar_spatial_snapshot_t *previous_snapshot,
                             radar_rate_mode_t previous_mode,
                             bool have_previous,
                             uint64_t current_ms,
                             uint64_t *last_state_log_ms,
                             uint64_t *last_track_log_ms,
                             uint64_t *last_summary_log_ms,
                             uint64_t *last_drop_log_ms,
                             uint32_t drops)
{
    const radar_rate_policy_t *policy = &rate_manager->policy;
    const char *source_name = radar_registry_source_name(source);
    const char *room = radar_registry_room_id(source);
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
                 "RADAR_STATE: mode=%s active=%u target=%lu x=%ld y=%ld velocity=%lu valid_age_ms=%s source=%s room=%s track_log_hz=%lu",
                 radar_rate_manager_mode_name(rate_manager->mode),
                 (unsigned int)snapshot->active_track_count,
                 target == NULL ? 0UL : (unsigned long)target->track_id,
                 target == NULL ? 0L : (long)target->filtered_x_mm,
                 target == NULL ? 0L : (long)target->filtered_y_mm,
                 target == NULL ? 0UL : (unsigned long)velocity_mm_s(target), valid_age,
                 source_name, room != NULL ? room : "unknown",
                 policy->track_log_period_ms == 0U ? 0UL :
                     (unsigned long)(1000U / policy->track_log_period_ms));
        *last_state_log_ms = current_ms;
    }
    if (target != NULL && policy->track_log_period_ms > 0U &&
        (transition || elapsed(current_ms, *last_track_log_ms, policy->track_log_period_ms))) {
        ESP_LOGI(TAG,
                 "RADAR_TRACK: id=T%03lu raw_x=%ld raw_y=%ld filtered_x=%ld filtered_y=%ld velocity=%lu match_distance=%lu confidence=%lu source=%s room=%s",
                 (unsigned long)target->track_id, (long)target->raw_x_mm,
                 (long)target->raw_y_mm, (long)target->filtered_x_mm,
                 (long)target->filtered_y_mm, (unsigned long)velocity_mm_s(target),
                 (unsigned long)target->last_match_distance_mm,
                 (unsigned long)target->confidence, source_name, room != NULL ? room : "unknown");
        char x_meters[16];
        char y_meters[16];
        format_meters(target->filtered_x_mm, x_meters, sizeof(x_meters));
        format_meters(target->filtered_y_mm, y_meters, sizeof(y_meters));
        ESP_LOGI(TAG,
                 "RADAR_TRACK_COMPAT: local track=%lu visible=1 raw_x=%ld raw_y=%ld filtered_x=%ld filtered_y=%ld distance=%lu angle=%d speed=%d direction=%d confidence=%lu seen=%lu missed=%lu source=%s room=%s",
                 (unsigned long)target->track_id, (long)target->raw_x_mm,
                 (long)target->raw_y_mm, (long)target->filtered_x_mm,
                 (long)target->filtered_y_mm, (unsigned long)target->distance_mm,
                 (int)target->angle_deg, (int)target->speed_cm_s, (int)target->direction_deg,
                 (unsigned long)target->confidence, (unsigned long)target->consecutive_seen,
                 (unsigned long)target->missed_frames, source_name, room != NULL ? room : "unknown");
        ESP_LOGI(TAG,
                 "RADAR_TRACK_COMPAT_METERS: local track id=T%03lu x=%s y=%s source=%s room=%s",
                 (unsigned long)target->track_id, x_meters, y_meters, source_name,
                 room != NULL ? room : "unknown");
        *last_track_log_ms = current_ms;
    }
    const bool summary_due = transition || elapsed(current_ms, *last_summary_log_ms, 1000U);
    if (summary_due) {
        const radar_tracker_diagnostics_t *diagnostics = &snapshot->diagnostics.tracker;
        ESP_LOGI(TAG,
                 "RADAR_TRACKER: active=%u created=%lu matched=%lu deleted=%lu stale=%lu tentative=%u velocity_outliers=%lu source=%s room=%s",
                 (unsigned int)snapshot->active_track_count,
                 (unsigned long)diagnostics->new_track_count,
                 (unsigned long)diagnostics->association_count,
                 (unsigned long)diagnostics->deleted_track_count,
                 (unsigned long)diagnostics->stale_track_count,
                 (unsigned int)(snapshot->accepted_target_count > snapshot->active_track_count),
                 (unsigned long)diagnostics->velocity_outliers, source_name,
                 room != NULL ? room : "unknown");
        *last_summary_log_ms = current_ms;
    }
    if (summary_due) {
        ESP_LOGI(TAG,
                 "RADAR_COUNTS: source=%s raw_target_count=%u accepted_target_count=%u visible_track_count=%u confirmed_active_track_count=%u history_target_count=%u visible_person_count=%u retained_person_count=%u business_person_count=%u count_state=%s",
                 source_name,
                 (unsigned int)snapshot->raw_target_count,
                 (unsigned int)snapshot->accepted_target_count,
                 (unsigned int)snapshot->visible_track_count,
                 (unsigned int)snapshot->confirmed_active_track_count,
                 (unsigned int)snapshot->history_target_count,
                 (unsigned int)snapshot->visible_person_count,
                 (unsigned int)snapshot->retained_person_count,
                 (unsigned int)snapshot->business_person_count,
                 radar_person_count_state_name(snapshot->count_state));
    }
    if (drops > 0U && elapsed(current_ms, *last_drop_log_ms, 1000U)) {
        ESP_LOGW(TAG, "RADAR_LOG_DROP count=%lu source=%s room=%s", (unsigned long)drops,
                 source_name, room != NULL ? room : "unknown");
        *last_drop_log_ms = current_ms;
    }
    if (transition) {
        radar_home_presence_t home;
        radar_registry_get_home_presence(&home);
        ESP_LOGI(TAG,
                 "RADAR_HOME occupied_room_count=%u active_source=%s active_room=%s last_transition=%s",
                 (unsigned int)home.occupied_room_count,
                 home.active_source < RADAR_SOURCE_COUNT ?
                     radar_registry_source_name(home.active_source) : "NONE",
                 home.active_room[0] != '\0' ? home.active_room : "none",
                 home.last_transition[0] != '\0' ? home.last_transition : "none");
    }
}

static void log_task(void *arg)
{
    radar_log_storage_t *storage = (radar_log_storage_t *)arg;
    uint64_t last_state_log_ms[RADAR_SOURCE_COUNT] = {0};
    uint64_t last_track_log_ms[RADAR_SOURCE_COUNT] = {0};
    uint64_t last_summary_log_ms[RADAR_SOURCE_COUNT] = {0};
    uint64_t last_drop_log_ms[RADAR_SOURCE_COUNT] = {0};

    ESP_LOGI(TAG, "RADAR_TOOL_COMPAT_VERSION=2");
    while (true) {
        bool pending[RADAR_SOURCE_COUNT] = {false};
        uint32_t drops[RADAR_SOURCE_COUNT] = {0};
        bool stack_pending = false;
        uint32_t stack_free_words = 0U;
        portENTER_CRITICAL(&s_lock);
        for (size_t i = 0U; i < RADAR_SOURCE_COUNT; ++i) {
            if (s_pending[i]) {
                storage->task_snapshots[i] = storage->published_snapshots[i];
                storage->task_rate_managers[i] = storage->published_rate_managers[i];
                s_pending[i] = false;
                pending[i] = true;
            }
            drops[i] = s_log_drop_count[i];
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
            ESP_LOGI(TAG, "RADAR_STACK: task=radar_local free_words=%lu",
                     (unsigned long)stack_free_words);
        }
        for (radar_source_id_t source = RADAR_SOURCE_S3_LOCAL;
             source < RADAR_SOURCE_COUNT;
             source = (radar_source_id_t)(source + 1)) {
#if !CONFIG_S3_RADAR_PER_SOURCE_LOG_SCHEDULER
            if (source != RADAR_SOURCE_S3_LOCAL) continue;
#endif
            if (!pending[source]) continue;
            emit_source_logs(source, &storage->task_snapshots[source],
                             &storage->task_rate_managers[source],
                             &storage->previous_snapshots[source],
                             storage->previous_modes[source],
                             storage->have_previous[source], current_ms,
                             &last_state_log_ms[source],
                             &last_track_log_ms[source], &last_summary_log_ms[source],
                             &last_drop_log_ms[source], drops[source]);
            storage->previous_snapshots[source] = storage->task_snapshots[source];
            storage->previous_modes[source] = storage->task_rate_managers[source].mode;
            storage->have_previous[source] = true;
        }
    }
}

esp_err_t radar_log_manager_start(void)
{
    if (s_task != NULL) return ESP_OK;
    if (s_storage == NULL) {
        s_storage = allocate_log_storage();
        if (s_storage == NULL) return ESP_ERR_NO_MEM;
    }
    s_task_with_caps = true;
    BaseType_t created = xTaskCreateWithCaps(log_task,
                                             "radar_log",
                                             RADAR_LOG_TASK_STACK,
                                             s_storage,
                                             1U,
                                             &s_task,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGW(TAG, "PSRAM task stack allocation failed; falling back to internal RAM");
        s_task_with_caps = false;
        created = xTaskCreate(log_task,
                              "radar_log",
                              RADAR_LOG_TASK_STACK,
                              s_storage,
                              1U,
                              &s_task);
    }
    if (created != pdPASS) {
        s_task = NULL;
        s_task_with_caps = false;
        heap_caps_free(s_storage);
        s_storage = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t radar_log_manager_stop(void)
{
    TaskHandle_t task = s_task;
    const bool task_with_caps = s_task_with_caps;
    s_task = NULL;
    s_task_with_caps = false;
    if (task != NULL) {
        if (task_with_caps) {
            vTaskDeleteWithCaps(task);
        } else {
            vTaskDelete(task);
        }
    }

    portENTER_CRITICAL(&s_lock);
    radar_log_storage_t *storage = s_storage;
    s_storage = NULL;
    memset(s_pending, 0, sizeof(s_pending));
    memset(s_log_drop_count, 0, sizeof(s_log_drop_count));
    s_stack_free_words = 0U;
    s_stack_pending = false;
    portEXIT_CRITICAL(&s_lock);
    if (storage != NULL) {
        heap_caps_free(storage);
    }
    return ESP_OK;
}

bool radar_log_manager_is_started(void)
{
    return s_task != NULL;
}

void radar_log_manager_publish(radar_source_id_t source,
                               const radar_spatial_snapshot_t *snapshot,
                               const radar_rate_manager_t *rate_manager)
{
    if (source >= RADAR_SOURCE_COUNT || snapshot == NULL || rate_manager == NULL) return;
#if !CONFIG_S3_RADAR_PER_SOURCE_LOG_SCHEDULER
    if (source != RADAR_SOURCE_S3_LOCAL) return;
#endif
    portENTER_CRITICAL(&s_lock);
    if (s_storage == NULL) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    if (s_pending[source] && s_log_drop_count[source] < UINT32_MAX) {
        ++s_log_drop_count[source];
    }
    s_storage->published_snapshots[source] = *snapshot;
    s_storage->published_rate_managers[source] = *rate_manager;
    s_pending[source] = true;
    portEXIT_CRITICAL(&s_lock);
}

void radar_log_manager_publish_stack_words(uint32_t free_words)
{
    portENTER_CRITICAL(&s_lock);
    s_stack_free_words = free_words;
    s_stack_pending = true;
    portEXIT_CRITICAL(&s_lock);
}

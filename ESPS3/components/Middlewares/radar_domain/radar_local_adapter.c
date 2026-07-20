#include "radar_local_adapter.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "app_stack_monitor.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "radar_diagnostics.h"
#include "radar_log_manager.h"
#include "radar_config.h"
#include "radar_rate_manager.h"
#include "radar_registry.h"
#include "radar_service.h"
#include "radar_spatial_state.h"

/*
 * 本地适配层把 S3 串口服务产生的原始帧转换为空间快照，并写入统一注册表。
 * 该层是本地传感器与 C51/C52 远端上报汇合前的唯一适配边界。
 */

static const char *TAG = "radar_local";
static TaskHandle_t s_task;
static radar_local_adapter_diagnostics_t s_diagnostics;
static portMUX_TYPE s_diagnostics_lock = portMUX_INITIALIZER_UNLOCKED;
static RadarSourceContext *s_context;
static radar_rate_manager_t s_rate_manager;
static portMUX_TYPE s_spatial_lock = portMUX_INITIALIZER_UNLOCKED;

#define RADAR_LOCAL_STACK_LOG_INTERVAL_MS 30000U

/* The adapter is the only writer, so its per-poll buffers can stay off its task stack. */
typedef struct {
    radar_service_diagnostics_t service_diagnostics;
    radar_frame_t latest_frame;
    radar_spatial_snapshot_t spatial_snapshot;
    radar_snapshot_t registry_snapshot;
    radar_registry_local_diagnostics_t registry_diagnostics;
} radar_local_task_workspace_t;

static radar_local_task_workspace_t *s_task_workspace;

static uint64_t now_ms(void)
{
    int64_t now_us = esp_timer_get_time();
    return now_us > 0 ? (uint64_t)(now_us / 1000) : 0U;
}

static void sat_inc_u32(uint32_t *value)
{
    if (value != NULL && *value < UINT32_MAX) {
        ++(*value);
    }
}

static void registry_snapshot_from_spatial(const radar_spatial_snapshot_t *spatial,
                                           radar_snapshot_t *snapshot)
{
    if (spatial == NULL || snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->uart_online = spatial->sensor_state != RADAR_SENSOR_OFFLINE;
    snapshot->frame_fresh = spatial->sensor_state == RADAR_SENSOR_VALID;
    snapshot->last_valid_frame_ms = spatial->latest_frame_ms;
    snapshot->current_target_count = spatial->visible_track_count;
    for (size_t i = 0U; i < spatial->visible_track_count; ++i) {
        const radar_track_snapshot_t *target = &spatial->current_targets[i];
        snapshot->targets[i] = (radar_target_t){
            .valid = true,
            .x_mm = target->filtered_x_mm > INT16_MAX ? INT16_MAX :
                    (target->filtered_x_mm < INT16_MIN ? INT16_MIN :
                     (int16_t)target->filtered_x_mm),
            .y_mm = target->filtered_y_mm > INT16_MAX ? INT16_MAX :
                    (target->filtered_y_mm < INT16_MIN ? INT16_MIN :
                     (int16_t)target->filtered_y_mm),
            .speed_cm_s = target->speed_cm_s,
            .resolution_mm = target->resolution_mm,
            .distance_mm = target->distance_mm,
            .confidence = target->confidence,
        };
    }
    switch (spatial->occupancy_state) {
    case RADAR_OCCUPANCY_PRESENT:
        snapshot->state = spatial->motion_state == RADAR_MOTION_MOVING ?
            RADAR_STATE_MOTION : RADAR_STATE_PRESENT;
        if (spatial->motion_state == RADAR_MOTION_MOVING) {
            snapshot->last_motion_ms = spatial->latest_frame_ms;
        }
        break;
    case RADAR_OCCUPANCY_HOLD:
        snapshot->state = RADAR_STATE_HOLD;
        break;
    case RADAR_OCCUPANCY_VACANT_INFERRED:
        snapshot->state = RADAR_STATE_VACANT_INFERRED;
        break;
    case RADAR_OCCUPANCY_UNKNOWN:
    default:
        snapshot->state = RADAR_STATE_UNKNOWN;
        break;
    }
}

static radar_count_summary_t count_summary_from_spatial(const radar_spatial_snapshot_t *spatial)
{
    radar_count_summary_t summary = {0};
    if (spatial == NULL) {
        summary.count_state = RADAR_PERSON_COUNT_UNKNOWN;
        return summary;
    }
    summary.raw_target_count = spatial->raw_target_count;
    summary.accepted_target_count = spatial->accepted_target_count;
    summary.visible_track_count = spatial->visible_track_count;
    summary.confirmed_active_track_count = spatial->confirmed_active_track_count;
    summary.history_target_count = spatial->history_target_count;
    summary.visible_person_count = spatial->visible_person_count;
    summary.retained_person_count = spatial->retained_person_count;
    summary.source_person_count = spatial->source_person_count;
    summary.count_state = spatial->count_state;
    return summary;
}

static void adapter_task(void *arg)
{
    radar_local_task_workspace_t *workspace = (radar_local_task_workspace_t *)arg;
    app_stack_monitor_report(TAG,
                             "radar_local",
                             RADAR_CONFIG_LOCAL_ADAPTER_TASK_STACK,
                             "entry");
    uint64_t last_parser_ms = 0U;
    uint64_t last_tracker_ms = 0U;
    uint64_t last_snapshot_ms = 0U;
    uint64_t last_stack_log_ms = 0U;

    if (workspace == NULL) {
        ESP_LOGE(TAG,
                 "RADAR_SOURCE_STATE event=workspace_unavailable source_id=0 source=S3_LOCAL device_id=sensair_s3_gateway_01 room=s3_local sequence=0");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        const uint64_t current_ms = now_ms();
        const radar_rate_policy_t *policy = &s_rate_manager.policy;
        if (last_parser_ms == 0U || current_ms < last_parser_ms ||
            current_ms - last_parser_ms >= policy->parser_period_ms) {
            (void)radar_service_process_pending(current_ms);
            last_parser_ms = current_ms;
        }
        radar_service_get_diagnostics(&workspace->service_diagnostics);
        policy = &s_rate_manager.policy;
        if (last_tracker_ms == 0U || current_ms < last_tracker_ms ||
            current_ms - last_tracker_ms >= policy->tracker_period_ms) {
            if (radar_service_get_latest_frame(&workspace->latest_frame)) {
                radar_spatial_state_on_frame(s_context->spatial_state, &workspace->latest_frame,
                                             workspace->service_diagnostics.recovery.state ==
                                                 RADAR_UART_RECOVERY_VALID, current_ms);
            }
            radar_spatial_state_poll(s_context->spatial_state,
                                     workspace->service_diagnostics.recovery.state, current_ms);
            radar_spatial_state_set_diagnostics(s_context->spatial_state,
                                                &workspace->service_diagnostics.parser,
                                                &workspace->service_diagnostics.uart,
                                                &workspace->service_diagnostics.recovery);
            radar_spatial_state_get_snapshot(s_context->spatial_state, &workspace->spatial_snapshot);
            uint8_t retained_track_count = 0U;
            for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS; ++i) {
                const radar_track_snapshot_t *track = &workspace->spatial_snapshot.tracks[i];
                if (!track->active) continue;
                if (track->track_id != 0U && retained_track_count < UINT8_MAX) {
                    ++retained_track_count;
                }
            }
            (void)radar_rate_manager_update(&s_rate_manager,
                                            workspace->spatial_snapshot.accepted_target_count,
                                            workspace->spatial_snapshot.active_track_count,
                                            retained_track_count,
                                            current_ms);
            last_tracker_ms = current_ms;
        }

        policy = &s_rate_manager.policy;
        if (last_snapshot_ms == 0U || current_ms < last_snapshot_ms ||
            current_ms - last_snapshot_ms >= policy->snapshot_period_ms) {
            radar_spatial_state_get_snapshot(s_context->spatial_state, &workspace->spatial_snapshot);
            registry_snapshot_from_spatial(&workspace->spatial_snapshot, &workspace->registry_snapshot);
            workspace->registry_diagnostics = (radar_registry_local_diagnostics_t){
                .parser_bad_header = workspace->service_diagnostics.parser.bad_header,
                .parser_bad_length = workspace->service_diagnostics.parser.bad_length,
                .parser_bad_tail = workspace->service_diagnostics.parser.bad_tail,
                .parser_skipped_bytes = workspace->service_diagnostics.parser.skipped_bytes,
                .uart_read_driver_error = workspace->service_diagnostics.uart.read_driver_error,
            };
            bool state_changed = false;
            const radar_count_summary_t count_summary =
                count_summary_from_spatial(&workspace->spatial_snapshot);
            portENTER_CRITICAL(&s_spatial_lock);
            radar_source_context_publish(s_context,
                                         &workspace->spatial_snapshot,
                                         &count_summary,
                                         workspace->registry_snapshot.uart_online &&
                                             workspace->registry_snapshot.frame_fresh,
                                         s_context->spatial_state->last_frame_seq,
                                         workspace->spatial_snapshot.latest_frame_ms);
            portEXIT_CRITICAL(&s_spatial_lock);
            radar_log_manager_publish(s_context, &s_rate_manager);
            if (radar_registry_update_local(&workspace->registry_snapshot,
                                            &count_summary,
                                            &workspace->registry_diagnostics,
                                            current_ms, &state_changed)) {
                portENTER_CRITICAL(&s_diagnostics_lock);
                sat_inc_u32(&s_diagnostics.registry_update_count);
                if (state_changed) sat_inc_u32(&s_diagnostics.state_change_count);
                portEXIT_CRITICAL(&s_diagnostics_lock);
                if (state_changed) {
                    radar_diagnostics_log_transition(RADAR_SOURCE_S3_LOCAL,
                                                     RADAR_DIAGNOSTICS_TRANSITION_LOCAL_UART);
                }
            }
            last_snapshot_ms = current_ms;
        }
        if (last_stack_log_ms == 0U || current_ms < last_stack_log_ms ||
            current_ms - last_stack_log_ms >= RADAR_LOCAL_STACK_LOG_INTERVAL_MS) {
            last_stack_log_ms = current_ms;
            const UBaseType_t high_water_words = uxTaskGetStackHighWaterMark(NULL);
            app_stack_monitor_report(TAG,
                                     "radar_local",
                                     RADAR_CONFIG_LOCAL_ADAPTER_TASK_STACK,
                                     "periodic");
            radar_log_manager_publish_stack_words(high_water_words);
        }
        vTaskDelay(pdMS_TO_TICKS(RADAR_CONFIG_LOCAL_ADAPTER_POLL_MS));
    }
}

esp_err_t radar_local_adapter_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    ESP_LOGI(TAG,
             "RADAR_SOURCE_STATE event=adapter_start source_id=0 source=S3_LOCAL device_id=sensair_s3_gateway_01 room=s3_local sequence=0");
    if (!radar_registry_init()) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = radar_service_init(NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    if (!radar_source_context_init(now_ms())) {
        return ESP_ERR_NO_MEM;
    }
    s_context = radar_source_context_mutable(RADAR_SOURCE_S3_LOCAL);
    if (s_context == NULL) {
        return ESP_ERR_NO_MEM;
    }
    radar_source_context_reset(s_context, now_ms());
    radar_rate_manager_init(&s_rate_manager, now_ms());
    ret = radar_log_manager_start();
    if (ret != ESP_OK) {
        return ret;
    }

    esp_err_t uart_start_result = radar_service_start();
    portENTER_CRITICAL(&s_diagnostics_lock);
    s_diagnostics.uart_start_result = uart_start_result;
    portEXIT_CRITICAL(&s_diagnostics_lock);
    if (uart_start_result != ESP_OK) {
        ESP_LOGW(TAG,
                 "RADAR_SOURCE_STATE event=uart_offline source_id=0 source=S3_LOCAL device_id=sensair_s3_gateway_01 room=s3_local sequence=%lu recovery=backoff ret=%d",
                 (unsigned long)s_context->sequence, (int)uart_start_result);
    } else {
        ESP_LOGI(TAG,
                 "RADAR_SOURCE_STATE event=uart_started source_id=0 source=S3_LOCAL device_id=sensair_s3_gateway_01 room=s3_local sequence=%lu",
                 (unsigned long)s_context->sequence);
    }

    s_task_workspace = heap_caps_calloc(1U,
                                        sizeof(*s_task_workspace),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_task_workspace == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreateWithCaps(adapter_task,
                                             "radar_local",
                                             RADAR_CONFIG_LOCAL_ADAPTER_TASK_STACK,
                                             s_task_workspace,
                                             RADAR_CONFIG_LOCAL_ADAPTER_TASK_PRIORITY,
                                             &s_task,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        s_task = NULL;
        heap_caps_free(s_task_workspace);
        s_task_workspace = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "RADAR_SOURCE_STATE event=adapter_ready source_id=0 source=S3_LOCAL device_id=sensair_s3_gateway_01 room=s3_local sequence=%lu poll_ms=%u",
             (unsigned long)s_context->sequence,
             (unsigned int)RADAR_CONFIG_LOCAL_ADAPTER_POLL_MS);
    return ESP_OK;
}

void radar_local_adapter_get_diagnostics(radar_local_adapter_diagnostics_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_diagnostics_lock);
    *out = s_diagnostics;
    portEXIT_CRITICAL(&s_diagnostics_lock);
}

bool radar_local_adapter_get_spatial_snapshot(radar_spatial_snapshot_t *out)
{
    if (out == NULL) {
        return false;
    }
    if (s_context == NULL) {
        return false;
    }
    portENTER_CRITICAL(&s_spatial_lock);
    *out = s_context->snapshot;
    const bool available = out->captured_at_ms != 0U;
    portEXIT_CRITICAL(&s_spatial_lock);
    return available;
}

bool radar_local_adapter_get_readonly_snapshot(radar_readonly_snapshot_t *out)
{
    if (out == NULL) {
        return false;
    }

    radar_spatial_snapshot_t spatial;
    if (!radar_local_adapter_get_spatial_snapshot(&spatial)) {
        memset(out, 0, sizeof(*out));
        out->sensor_state = RADAR_SENSOR_OFFLINE;
        out->occupancy_state = RADAR_OCCUPANCY_UNKNOWN;
        out->motion_state = RADAR_MOTION_UNKNOWN;
        out->count_summary.count_state = RADAR_PERSON_COUNT_UNKNOWN;
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->timestamp_ms = spatial.captured_at_ms;
    out->latest_frame_ms = spatial.latest_frame_ms;
    out->frame_age_ms = spatial.frame_age_ms;
    out->sensor_state = spatial.sensor_state;
    out->occupancy_state = spatial.occupancy_state;
    out->motion_state = spatial.motion_state;
    out->count_summary = count_summary_from_spatial(&spatial);
    for (size_t i = 0U; i < spatial.visible_track_count; ++i) {
        const radar_track_snapshot_t *track = &spatial.current_targets[i];
        out->tracks[out->track_count++] = (radar_readonly_track_t){
            .track_id = track->track_id,
            .raw_x_mm = track->raw_x_mm,
            .raw_y_mm = track->raw_y_mm,
            .filtered_x_mm = track->filtered_x_mm,
            .filtered_y_mm = track->filtered_y_mm,
            .distance_mm = track->distance_mm,
            .angle_deg = track->angle_deg,
            .speed_cm_s = track->speed_cm_s,
            .confidence = track->confidence,
            .visible = track->visible,
            .timestamp_ms = track->last_seen_ms,
        };
    }
    return true;
}

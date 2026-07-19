#include "radar_state_client.h"

#include <string.h>

#include "app_runtime.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp111_protocol_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_link.h"
#include "radar_board_config.h"
#include "radar_edge_filter.h"
#include "radar_service.h"
#include "radar_state_codec.h"
#include "server_comm_http.h"

#define RADAR_CLIENT_TASK_STACK 4096U
#define RADAR_CLIENT_TASK_PRIORITY 3U
#define RADAR_CLIENT_POLL_MS 100U
#define RADAR_CLIENT_RETRY_MS 1000U
#define RADAR_CLIENT_HTTP_TIMEOUT_MS 3000U
#define RADAR_RESULT_HEARTBEAT_MS 1000U
#define RADAR_RESULT_MIN_INTERVAL_MS 100U

static const char *TAG = "radar_client";
static TaskHandle_t s_task;
static radar_state_client_stats_t s_stats;
static radar_edge_filter_t s_filter;

static uint32_t now_ms(void)
{
    const int64_t now_us = esp_timer_get_time();
    return now_us > 0 ? (uint32_t)(now_us / 1000) : 0U;
}

static bool target_changed(const radar_target_t *a, const radar_target_t *b)
{
    return a->valid != b->valid ||
           (a->valid &&
            (a->slot != b->slot || a->x_mm != b->x_mm || a->y_mm != b->y_mm ||
             a->speed_cm_s != b->speed_cm_s || a->resolution_mm != b->resolution_mm ||
             a->distance_mm != b->distance_mm));
}

static bool sample_changed(const radar_target_sample_t *a,
                           const radar_target_sample_t *b)
{
    if (a->link_state != b->link_state || a->sample_valid != b->sample_valid ||
        a->frame_seq != b->frame_seq || a->frame_uptime_ms != b->frame_uptime_ms ||
        a->target_count != b->target_count) {
        return true;
    }
    for (size_t i = 0U; i < LD2450_MAX_TARGETS; ++i) {
        if (target_changed(&a->targets[i], &b->targets[i])) {
            return true;
        }
    }
    return false;
}

static bool report_due(const radar_target_sample_t *current,
                       const radar_target_sample_t *last_sent,
                       bool has_last_sent,
                       uint32_t timestamp_ms,
                       uint32_t last_sent_ms)
{
    return !has_last_sent || sample_changed(current, last_sent) ||
           (uint32_t)(timestamp_ms - last_sent_ms) >= RADAR_RESULT_HEARTBEAT_MS;
}

static uint32_t next_sequence(uint32_t value)
{
    ++value;
    return value == 0U ? 1U : value;
}

static void make_status_only(radar_target_sample_t *sample)
{
    if (sample == NULL) {
        return;
    }
    sample->sample_valid = false;
    sample->target_count = 0U;
    memset(sample->targets, 0, sizeof(sample->targets));
}

static void radar_client_task(void *arg)
{
    (void)arg;
    radar_target_sample_t last_sent = {0};
    radar_target_sample_t pending = {0};
    bool has_last_sent = false;
    bool has_pending = false;
    uint32_t last_sent_ms = 0U;
    uint32_t next_attempt_ms = 0U;
    uint32_t next_request_sequence = 1U;
    uint32_t pending_request_sequence = 0U;
    char json_body[RADAR_RESULT_JSON_MAX_BYTES];

    while (true) {
        const uint32_t timestamp_ms = now_ms();
        radar_target_sample_t current;
        radar_service_get_target_sample(&current);
        radar_edge_filter_apply(&s_filter, &current);

        if (!has_pending && report_due(&current, &last_sent, has_last_sent,
                                       timestamp_ms, last_sent_ms)) {
            pending = current;
            if (has_last_sent && pending.sample_valid && last_sent.sample_valid &&
                pending.frame_seq == last_sent.frame_seq) {
                make_status_only(&pending);
            }
            pending_request_sequence = next_request_sequence;
            next_request_sequence = next_sequence(next_request_sequence);
            has_pending = true;
        } else if (has_pending && sample_changed(&current, &pending)) {
            pending = current;
            pending_request_sequence = next_request_sequence;
            next_request_sequence = next_sequence(next_request_sequence);
            next_attempt_ms = 0U;
            ++s_stats.coalesced_count;
        }

        if (!has_pending ||
            (next_attempt_ms != 0U && (int32_t)(timestamp_ms - next_attempt_ms) < 0) ||
            app_runtime_non_voice_is_paused() || !gateway_link_is_ready()) {
            vTaskDelay(pdMS_TO_TICKS(RADAR_CLIENT_POLL_MS));
            continue;
        }

        const int body_len = radar_result_encode_json(&pending,
                                                      timestamp_ms,
                                                      pending_request_sequence,
                                                      json_body,
                                                      sizeof(json_body));
        if (body_len <= 0 || (size_t)body_len >= sizeof(json_body)) {
            ++s_stats.encode_error_count;
            next_attempt_ms = timestamp_ms + RADAR_CLIENT_RETRY_MS;
            vTaskDelay(pdMS_TO_TICKS(RADAR_CLIENT_POLL_MS));
            continue;
        }
        s_stats.last_body_bytes = (uint32_t)body_len;

        server_comm_http_response_t response = {0};
        const esp_err_t ret = server_comm_http_post_json(ESP111_PROTOCOL_ROUTE_RADAR_RESULT,
                                                          json_body,
                                                          RADAR_CLIENT_HTTP_TIMEOUT_MS,
                                                          NULL,
                                                          0U,
                                                          &response);
        if (ret == ESP_OK) {
            last_sent = pending;
            has_last_sent = true;
            last_sent_ms = timestamp_ms;
            has_pending = false;
            next_attempt_ms = timestamp_ms + RADAR_RESULT_MIN_INTERVAL_MS;
            ++s_stats.accepted_count;
            ESP_LOGI(TAG,
                     "radar_upload_ok local_id=%u q=%lu frame_seq=%lu valid=%u targets=%u",
                     (unsigned int)pending.local_id,
                     (unsigned long)pending_request_sequence,
                     (unsigned long)pending.frame_seq,
                     pending.sample_valid ? 1U : 0U,
                     (unsigned int)pending.target_count);
        } else {
            ++s_stats.failed_count;
            next_attempt_ms = timestamp_ms + RADAR_CLIENT_RETRY_MS;
            ESP_LOGW(TAG,
                     "radar_upload_deferred local_id=%u q=%lu ret=%s status=%d",
                     (unsigned int)pending.local_id,
                     (unsigned long)pending_request_sequence,
                     esp_err_to_name(ret),
                     response.status_code);
        }
        vTaskDelay(pdMS_TO_TICKS(RADAR_CLIENT_POLL_MS));
    }
}

esp_err_t radar_state_client_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    if (RADAR_BOARD_LOCAL_ID < 1 || RADAR_BOARD_LOCAL_ID > 2) {
        return ESP_ERR_INVALID_STATE;
    }
    radar_edge_filter_init(&s_filter);
    if (xTaskCreate(radar_client_task,
                    "radar_report",
                    RADAR_CLIENT_TASK_STACK,
                    NULL,
                    RADAR_CLIENT_TASK_PRIORITY,
                    &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void radar_state_client_get_stats(radar_state_client_stats_t *out)
{
    if (out != NULL) {
        *out = s_stats;
    }
}

#include "radar_worker.h"

#include <limits.h>
#include <string.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp111_protocol_common.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "gateway_link.h"
#include "radar_ble_binding_config.h"
#include "radar_buffer.h"
#include "radar_edge_filter.h"
#include "radar_memory_manager.h"
#include "radar_resource_adapter.h"
#include "radar_service.h"
#include "radar_state_codec.h"
#include "radar_upload_queue.h"
#include "server_comm_http.h"

#ifndef RADAR_DEBUG_RAW_FRAME
#define RADAR_DEBUG_RAW_FRAME 0
#endif

#define RADAR_DIAGNOSTIC_WINDOW_MS 1000U
#define RADAR_LOG_IDENTITY_FORMAT \
    " source_id=%u source=%s device_id=%s room=%s sequence=%lu"
#define RADAR_LOG_IDENTITY_ARGS(sequence_value) \
    (unsigned int)RADAR_BLE_BINDING_LOCAL_ID, \
    (RADAR_BLE_BINDING_LOCAL_ID == 1 ? "C51" : "C52"), \
    RADAR_BLE_BINDING_DEVICE_ID, \
    RADAR_BLE_BINDING_ROOM_ID, \
    (unsigned long)(sequence_value)

static const char *TAG = "radar_domain";
static radar_buffer_t s_raw;
#if !CONFIG_C5_RADAR_ADAPTIVE_UPLOAD
static radar_upload_queue_t s_upload;
#endif
static radar_target_sample_t *s_history;
static radar_edge_filter_t s_filter;
static TaskHandle_t s_worker_task;
static TaskHandle_t s_upload_task;
static uint32_t s_history_index;
#if !CONFIG_C5_RADAR_ADAPTIVE_UPLOAD
static uint32_t s_request_sequence = 1U;
#endif
static char *s_json_body;
static bool s_started;

typedef struct {
    uint64_t window_start_ms;
    uint32_t baseline_notify_count;
    uint32_t baseline_valid_frame_count;
    uint32_t baseline_invalid_frame_count;
    uint32_t baseline_drop_count;
    uint32_t filtered_frame_count;
    uint32_t filtered_frames_in_window;
    uint32_t interval_total_ms;
    uint32_t interval_count;
    uint32_t last_interval_ms;
    uint32_t last_filtered_frame_ms;
    bool has_last_filtered_frame;
    bool has_window_sample;
    radar_target_sample_t latest_sample;
} radar_runtime_rate_diagnostics_t;

static radar_runtime_rate_diagnostics_t s_rate_diagnostics;

static uint64_t now_ms(void)
{
    const int64_t us = esp_timer_get_time();
    return us > 0 ? (uint64_t)(us / 1000) : 0U;
}

static uint32_t counter_delta(uint32_t current, uint32_t baseline)
{
    return current - baseline;
}

static void saturating_increment(uint32_t *value)
{
    if (value != NULL && *value < UINT32_MAX) {
        ++(*value);
    }
}

static void radar_runtime_rate_diagnostics_init(void)
{
    radar_buffer_stats_t raw_stats = {0};
    radar_service_diagnostics_t service_stats = {0};
    radar_buffer_get_stats(&s_raw, &raw_stats);
    radar_service_get_diagnostics(&service_stats);
    memset(&s_rate_diagnostics, 0, sizeof(s_rate_diagnostics));
    s_rate_diagnostics.window_start_ms = now_ms();
    s_rate_diagnostics.baseline_notify_count = raw_stats.notify_count;
    s_rate_diagnostics.baseline_valid_frame_count = service_stats.parser.valid_frames;
    s_rate_diagnostics.baseline_invalid_frame_count = service_stats.parser.invalid_tail_frames;
    s_rate_diagnostics.baseline_drop_count = raw_stats.drop_count;
}

static void radar_runtime_rate_diagnostics_note_filtered(const radar_target_sample_t *sample)
{
    if (sample == NULL) return;
    const uint32_t frame_ms = sample->frame_uptime_ms;
    if (s_rate_diagnostics.has_last_filtered_frame) {
        const uint32_t interval_ms = frame_ms - s_rate_diagnostics.last_filtered_frame_ms;
        s_rate_diagnostics.last_interval_ms = interval_ms;
        if (interval_ms > 0U) {
            s_rate_diagnostics.interval_total_ms += interval_ms;
            saturating_increment(&s_rate_diagnostics.interval_count);
        }
    }
    s_rate_diagnostics.last_filtered_frame_ms = frame_ms;
    s_rate_diagnostics.has_last_filtered_frame = true;
    saturating_increment(&s_rate_diagnostics.filtered_frame_count);
    saturating_increment(&s_rate_diagnostics.filtered_frames_in_window);
    s_rate_diagnostics.latest_sample = *sample;
    s_rate_diagnostics.has_window_sample = true;
}

static void radar_runtime_rate_diagnostics_emit_if_due(uint64_t timestamp_ms)
{
    if (timestamp_ms < s_rate_diagnostics.window_start_ms ||
        timestamp_ms - s_rate_diagnostics.window_start_ms < RADAR_DIAGNOSTIC_WINDOW_MS) {
        return;
    }

    radar_buffer_stats_t raw_stats = {0};
    radar_service_diagnostics_t service_stats = {0};
    radar_buffer_get_stats(&s_raw, &raw_stats);
    radar_service_get_diagnostics(&service_stats);

    const uint32_t notify_count = counter_delta(raw_stats.notify_count,
                                                 s_rate_diagnostics.baseline_notify_count);
    const uint32_t valid_frame_count = counter_delta(service_stats.parser.valid_frames,
                                                      s_rate_diagnostics.baseline_valid_frame_count);
    const uint32_t invalid_frame_count = counter_delta(service_stats.parser.invalid_tail_frames,
                                                        s_rate_diagnostics.baseline_invalid_frame_count);
    const uint32_t drop_count = counter_delta(raw_stats.drop_count,
                                              s_rate_diagnostics.baseline_drop_count);

    if (s_rate_diagnostics.has_window_sample) {
        const radar_target_sample_t *sample = &s_rate_diagnostics.latest_sample;
        const radar_target_t *target = sample->target_count > 0U ? &sample->targets[0] : NULL;
        const uint32_t hz_tenths = s_rate_diagnostics.last_interval_ms > 0U
            ? 10000U / s_rate_diagnostics.last_interval_ms : 0U;
        const uint32_t average_interval_ms = s_rate_diagnostics.interval_count > 0U
            ? s_rate_diagnostics.interval_total_ms / s_rate_diagnostics.interval_count : 0U;
        ESP_LOGI(TAG,
                 "RADAR_LOCAL_PROCESS local_id=%u frame_count=%lu targets=%u target0_id=%u "
                 "x_mm=%d y_mm=%d speed_cm_s=%d distance_mm=%lu confidence=%u "
                 "interval_ms=%lu hz=%lu.%lu" RADAR_LOG_IDENTITY_FORMAT,
                 (unsigned int)sample->local_id,
                 (unsigned long)s_rate_diagnostics.filtered_frame_count,
                 (unsigned int)sample->target_count,
                 target == NULL ? 0U : (unsigned int)target->slot + 1U,
                 target == NULL ? 0 : (int)target->x_mm,
                 target == NULL ? 0 : (int)target->y_mm,
                 target == NULL ? 0 : (int)target->speed_cm_s,
                 (unsigned long)(target == NULL ? 0U : target->distance_mm),
                 target == NULL ? 0U : (unsigned int)target->confidence,
                 (unsigned long)s_rate_diagnostics.last_interval_ms,
                 (unsigned long)(hz_tenths / 10U),
                 (unsigned long)(hz_tenths % 10U),
                 RADAR_LOG_IDENTITY_ARGS(sample->frame_seq));
        radar_resource_adapter_stats_t resource_stats = {0};
        radar_resource_adapter_get_stats(&resource_stats);
        ESP_LOGI(TAG,
                 "RADAR_LOCAL_SUMMARY local_id=%u frames_last_sec=%lu avg_interval_ms=%lu "
                 "targets=%u queue_depth=%lu drop_count=%lu mode=%s upload_attempts=%lu "
                 "upload_success=%lu coalesce_count=%lu voice_active=%u" RADAR_LOG_IDENTITY_FORMAT,
                 (unsigned int)sample->local_id,
                 (unsigned long)s_rate_diagnostics.filtered_frames_in_window,
                 (unsigned long)average_interval_ms,
                 (unsigned int)sample->target_count,
                 (unsigned long)raw_stats.queue_depth,
                 (unsigned long)drop_count,
                 radar_resource_adapter_mode_name(resource_stats.mode),
                 (unsigned long)resource_stats.radar_upload_attempt_count,
                 (unsigned long)resource_stats.radar_upload_success_count,
                 (unsigned long)resource_stats.radar_upload_coalesce_count,
                 resource_stats.voice_active ? 1U : 0U,
                 RADAR_LOG_IDENTITY_ARGS(sample->frame_seq));
    }
    if (notify_count > 0U || valid_frame_count > 0U || invalid_frame_count > 0U) {
        ESP_LOGI(TAG,
                 "RADAR_BLE_RX_SUMMARY local_id=%u notify_count=%lu valid_frame_count=%lu "
                 "invalid_frame_count=%lu" RADAR_LOG_IDENTITY_FORMAT,
                 (unsigned int)RADAR_BLE_BINDING_LOCAL_ID,
                 (unsigned long)notify_count,
                 (unsigned long)valid_frame_count,
                 (unsigned long)invalid_frame_count,
                 RADAR_LOG_IDENTITY_ARGS(s_rate_diagnostics.has_window_sample
                     ? s_rate_diagnostics.latest_sample.frame_seq : 0U));
    }

    s_rate_diagnostics.window_start_ms = timestamp_ms;
    s_rate_diagnostics.baseline_notify_count = raw_stats.notify_count;
    s_rate_diagnostics.baseline_valid_frame_count = service_stats.parser.valid_frames;
    s_rate_diagnostics.baseline_invalid_frame_count = service_stats.parser.invalid_tail_frames;
    s_rate_diagnostics.baseline_drop_count = raw_stats.drop_count;
    s_rate_diagnostics.filtered_frames_in_window = 0U;
    s_rate_diagnostics.interval_total_ms = 0U;
    s_rate_diagnostics.interval_count = 0U;
    s_rate_diagnostics.has_window_sample = false;
}

static void radar_worker_task(void *arg)
{
    (void)arg;
    radar_raw_frame_t raw;
    uint32_t previous_frames = 0U;
    radar_runtime_rate_diagnostics_init();
    while (true) {
        if (!radar_buffer_pop(&s_raw, &raw, pdMS_TO_TICKS(1000))) {
            radar_runtime_rate_diagnostics_emit_if_due(now_ms());
            continue;
        }
#if RADAR_DEBUG_RAW_FRAME
        ESP_LOG_BUFFER_HEXDUMP(TAG, raw.raw_data, raw.length, ESP_LOG_INFO);
#endif
        radar_service_ingest_ble_bytes(raw.raw_data, raw.length, raw.timestamp_ms);
        radar_service_diagnostics_t diag = {0};
        radar_service_get_diagnostics(&diag);
        if (diag.frame_received_count != previous_frames) {
            previous_frames = diag.frame_received_count;
            radar_target_sample_t sample;
            radar_service_get_target_sample(&sample);
            radar_edge_filter_apply(&s_filter, &sample);
            radar_runtime_rate_diagnostics_note_filtered(&sample);
            s_history[s_history_index++ % 128U] = sample;
#if CONFIG_C5_RADAR_ADAPTIVE_UPLOAD
            radar_resource_adapter_update_sample(&sample, now_ms());
#else
            radar_upload_packet_t packet = {
                .sample = sample,
                .request_sequence = s_request_sequence++,
                .attempts = 0U,
            };
            if (s_request_sequence == 0U) s_request_sequence = 1U;
            (void)radar_upload_queue_push(&s_upload, &packet);
#endif
        }
        radar_runtime_rate_diagnostics_emit_if_due(now_ms());
    }
}

static void radar_upload_task(void *arg)
{
    (void)arg;
#if CONFIG_C5_RADAR_ADAPTIVE_UPLOAD
    while (true) {
        const uint64_t timestamp_ms = now_ms();
        radar_resource_adapter_tick(timestamp_ms);
        if (!gateway_link_is_ready()) {
            vTaskDelay(pdMS_TO_TICKS(100U));
            continue;
        }

        radar_target_sample_t sample = {0};
        uint32_t request_sequence = 0U;
        if (!radar_resource_adapter_take_radar_upload(timestamp_ms, &sample, &request_sequence)) {
            vTaskDelay(pdMS_TO_TICKS(50U));
            continue;
        }

        ESP_LOGI(TAG,
                 "RADAR_RESULT_ENCODE local_id=%u targets=%u seq=%lu" RADAR_LOG_IDENTITY_FORMAT,
                 (unsigned int)sample.local_id,
                 (unsigned int)sample.target_count,
                 (unsigned long)request_sequence,
                 RADAR_LOG_IDENTITY_ARGS(request_sequence));
        const int body_len = radar_result_encode_json(&sample,
                                                      (uint32_t)timestamp_ms,
                                                      request_sequence,
                                                      s_json_body,
                                                      RADAR_RESULT_JSON_MAX_BYTES);
        if (body_len <= 0) {
            radar_resource_adapter_complete_radar_upload(false, now_ms());
            continue;
        }
        ESP_LOGI(TAG,
                 "RADAR_UPLOAD_BEGIN local_id=%u seq=%lu targets=%u payload_size=%u" RADAR_LOG_IDENTITY_FORMAT,
                 (unsigned int)sample.local_id,
                 (unsigned long)request_sequence,
                 (unsigned int)sample.target_count,
                 (unsigned int)body_len,
                 RADAR_LOG_IDENTITY_ARGS(request_sequence));
        ESP_LOGI(TAG,
                 "RADAR_RESULT_UPLOAD local_id=%u endpoint=%s payload_size=%u" RADAR_LOG_IDENTITY_FORMAT,
                 (unsigned int)sample.local_id,
                 ESP111_PROTOCOL_ROUTE_RADAR_RESULT,
                 (unsigned int)body_len,
                 RADAR_LOG_IDENTITY_ARGS(request_sequence));
        server_comm_http_response_t response = {0};
        const esp_err_t ret = server_comm_http_post_json(ESP111_PROTOCOL_ROUTE_RADAR_RESULT,
                                                          s_json_body,
                                                          1000U,
                                                          NULL,
                                                          0U,
                                                          &response);
        radar_resource_adapter_complete_radar_upload(ret == ESP_OK, now_ms());
        ESP_LOGI(TAG,
                 "RADAR_UPLOAD_RESULT status=%d retry_count=%u" RADAR_LOG_IDENTITY_FORMAT,
                 response.status_code,
                 ret == ESP_OK ? 0U : 1U,
                 RADAR_LOG_IDENTITY_ARGS(request_sequence));
        ESP_LOGI(TAG,
                 "RADAR_RESULT_UPLOAD_DONE status=%s response_code=%d" RADAR_LOG_IDENTITY_FORMAT,
                 esp_err_to_name(ret),
                 response.status_code,
                 RADAR_LOG_IDENTITY_ARGS(request_sequence));
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "RADAR_UPLOAD_OK local_id=%u targets=%u" RADAR_LOG_IDENTITY_FORMAT,
                     (unsigned int)sample.local_id,
                     (unsigned int)sample.target_count,
                     RADAR_LOG_IDENTITY_ARGS(request_sequence));
        } else {
            ESP_LOGW(TAG, "RADAR_UPLOAD_RETRY ret=%s status=%d latest_only=1" RADAR_LOG_IDENTITY_FORMAT,
                     esp_err_to_name(ret), response.status_code,
                     RADAR_LOG_IDENTITY_ARGS(request_sequence));
        }
    }
#else
    const bool upload_retriable = true;
    radar_upload_packet_t packet;
    while (true) {
        if (!radar_upload_queue_pop(&s_upload, &packet, portMAX_DELAY)) continue;
        if (!gateway_link_is_ready()) {
            (void)radar_upload_queue_push(&s_upload, &packet);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG,
                 "RADAR_RESULT_ENCODE local_id=%u targets=%u seq=%lu" RADAR_LOG_IDENTITY_FORMAT,
                 (unsigned int)packet.sample.local_id,
                 (unsigned int)packet.sample.target_count,
                 (unsigned long)packet.request_sequence,
                 RADAR_LOG_IDENTITY_ARGS(packet.request_sequence));
        const int body_len = radar_result_encode_json(&packet.sample,
                                                      (uint32_t)now_ms(),
                                                      packet.request_sequence,
                                                      s_json_body,
                                                      RADAR_RESULT_JSON_MAX_BYTES);
        if (body_len <= 0) continue;
        ESP_LOGI(TAG,
                 "RADAR_UPLOAD_BEGIN local_id=%u seq=%lu targets=%u payload_size=%u" RADAR_LOG_IDENTITY_FORMAT,
                 (unsigned int)packet.sample.local_id,
                 (unsigned long)packet.request_sequence,
                 (unsigned int)packet.sample.target_count,
                 (unsigned int)body_len,
                 RADAR_LOG_IDENTITY_ARGS(packet.request_sequence));
        ESP_LOGI(TAG,
                 "RADAR_RESULT_UPLOAD local_id=%u endpoint=%s payload_size=%u" RADAR_LOG_IDENTITY_FORMAT,
                 (unsigned int)packet.sample.local_id,
                 ESP111_PROTOCOL_ROUTE_RADAR_RESULT,
                 (unsigned int)body_len,
                 RADAR_LOG_IDENTITY_ARGS(packet.request_sequence));
        server_comm_http_response_t response = {0};
        const esp_err_t ret = server_comm_http_post_json(ESP111_PROTOCOL_ROUTE_RADAR_RESULT,
                                                          s_json_body,
                                                          3000U,
                                                          NULL,
                                                          0U,
                                                          &response);
        const uint8_t retry_count = ret != ESP_OK && packet.attempts < 4U
            ? (uint8_t)(packet.attempts + 1U) : packet.attempts;
        ESP_LOGI(TAG,
                 "RADAR_UPLOAD_RESULT status=%d retry_count=%u" RADAR_LOG_IDENTITY_FORMAT,
                 response.status_code,
                 (unsigned int)retry_count,
                 RADAR_LOG_IDENTITY_ARGS(packet.request_sequence));
        ESP_LOGI(TAG,
                 "RADAR_RESULT_UPLOAD_DONE status=%s response_code=%d" RADAR_LOG_IDENTITY_FORMAT,
                 esp_err_to_name(ret),
                 response.status_code,
                 RADAR_LOG_IDENTITY_ARGS(packet.request_sequence));
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "RADAR_UPLOAD_OK local_id=%u targets=%u" RADAR_LOG_IDENTITY_FORMAT,
                     (unsigned int)packet.sample.local_id,
                     (unsigned int)packet.sample.target_count,
                     RADAR_LOG_IDENTITY_ARGS(packet.request_sequence));
        } else if (upload_retriable && packet.attempts < 4U) {
            ++packet.attempts;
            (void)radar_upload_queue_push(&s_upload, &packet);
            vTaskDelay(pdMS_TO_TICKS(250U * packet.attempts));
            ESP_LOGW(TAG, "RADAR_UPLOAD_RETRY ret=%s status=%d attempt=%u" RADAR_LOG_IDENTITY_FORMAT,
                     esp_err_to_name(ret), response.status_code, (unsigned int)packet.attempts,
                     RADAR_LOG_IDENTITY_ARGS(packet.request_sequence));
        }
    }
#endif
}

esp_err_t radar_domain_start(void)
{
    if (s_started) return ESP_OK;
    esp_err_t ret = radar_service_start();
    if (ret != ESP_OK) return ret;
    radar_memory_log("before_init");
    ret = radar_buffer_init(&s_raw);
    if (ret != ESP_OK) return ret;
#if CONFIG_C5_RADAR_ADAPTIVE_UPLOAD
    radar_resource_adapter_init(now_ms());
#else
    ret = radar_upload_queue_init(&s_upload);
    if (ret != ESP_OK) return ret;
#endif
    s_history = radar_memory_alloc_psram(sizeof(*s_history) * 128U, "target_history");
    s_json_body = radar_memory_alloc_psram(RADAR_RESULT_JSON_MAX_BYTES, "upload_json");
    if (s_history == NULL || s_json_body == NULL) return ESP_ERR_NO_MEM;
    memset(s_history, 0, sizeof(*s_history) * 128U);
    radar_edge_filter_init(&s_filter);
    if (xTaskCreateWithCaps(radar_worker_task, "radar_worker", 4096, NULL, 3,
                            &s_worker_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS ||
        xTaskCreateWithCaps(radar_upload_task, "radar_upload", 3072, NULL, 2,
                            &s_upload_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    radar_memory_log("after_init");
    return ESP_OK;
}

void radar_domain_notify(const uint8_t *data, size_t length, uint64_t timestamp_ms)
{
    (void)radar_buffer_push_notify(&s_raw, data, length, timestamp_ms);
}

void radar_domain_set_link_state(uint8_t link_state, bool online)
{
    radar_service_set_link_state(link_state, online);
#if CONFIG_C5_RADAR_ADAPTIVE_UPLOAD
    radar_resource_adapter_set_link_state(link_state, online, now_ms());
#endif
}

void radar_domain_mark_timeout(uint64_t timestamp_ms)
{
    radar_service_mark_ble_timeout(timestamp_ms);
#if CONFIG_C5_RADAR_ADAPTIVE_UPLOAD
    radar_resource_adapter_tick(timestamp_ms);
#endif
}

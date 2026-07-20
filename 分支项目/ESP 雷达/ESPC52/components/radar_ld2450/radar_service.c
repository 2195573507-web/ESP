#include "radar_service.h"

#include <limits.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "radar_board_config.h"

static const char *TAG = "radar_ld2450";

static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static ld2450_parser_t s_parser;
static radar_target_sample_t s_sample;
static bool s_initialized;
static uint8_t s_link_state;
static uint32_t s_snapshot_updates;
static uint32_t s_frame_received_count;
static uint32_t s_last_target_count;
static uint64_t s_last_frame_log_ms;

static void sat_inc_u32(uint32_t *value)
{
    if (value != NULL && *value < UINT32_MAX) {
        ++(*value);
    }
}

static void handle_frame(const radar_frame_t *frame, void *ctx)
{
    (void)ctx;
    if (frame == NULL || s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }

    const uint8_t link_state = s_link_state;
    memset(&s_sample, 0, sizeof(s_sample));
    s_sample.local_id = RADAR_BOARD_LOCAL_ID;
    s_sample.link_state = link_state;
    s_sample.sample_valid = true;
    s_sample.frame_seq = frame->frame_seq;
    s_sample.frame_uptime_ms = (uint32_t)frame->received_at_ms;
    for (size_t i = 0U; i < LD2450_MAX_TARGETS; ++i) {
        if (frame->targets[i].valid) {
            s_sample.targets[s_sample.target_count++] = frame->targets[i];
        }
    }
    sat_inc_u32(&s_snapshot_updates);
    sat_inc_u32(&s_frame_received_count);
    s_last_target_count = s_sample.target_count;
    xSemaphoreGive(s_lock);

    if (s_last_frame_log_ms == 0U || frame->received_at_ms < s_last_frame_log_ms ||
        frame->received_at_ms - s_last_frame_log_ms >= 1000U) {
        s_last_frame_log_ms = frame->received_at_ms;
        ESP_LOGI(TAG,
                 "radar_frame_received local_id=%u frame_seq=%lu targets=%u",
                 (unsigned int)RADAR_BOARD_LOCAL_ID,
                 (unsigned long)frame->frame_seq,
                 (unsigned int)s_last_target_count);
    }
}

esp_err_t radar_service_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ld2450_parser_init(&s_parser);
    memset(&s_sample, 0, sizeof(s_sample));
    s_sample.local_id = RADAR_BOARD_LOCAL_ID;
    s_link_state = 0U;
    s_snapshot_updates = 0U;
    s_frame_received_count = 0U;
    s_last_target_count = 0U;
    s_last_frame_log_ms = 0U;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t radar_service_start(void)
{
    const esp_err_t ret = radar_service_init();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "LD2450 BLE parser ready local_id=%u device_id=%s",
                 (unsigned int)RADAR_BOARD_LOCAL_ID,
                 RADAR_BOARD_DEVICE_ID);
    }
    return ret;
}

bool radar_service_is_started(void)
{
    return s_initialized;
}

void radar_service_get_target_sample(radar_target_sample_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        *out = s_sample;
        xSemaphoreGive(s_lock);
    }
}

void radar_service_get_diagnostics(radar_service_diagnostics_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    ld2450_parser_get_diagnostics(&s_parser, &out->parser);
    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        out->snapshot_updates = s_snapshot_updates;
        out->frame_received_count = s_frame_received_count;
        out->last_target_count = s_last_target_count;
        out->link_state = s_link_state;
        xSemaphoreGive(s_lock);
    }
}

void radar_service_ingest_ble_bytes(const uint8_t *data, size_t data_len, uint64_t received_at_ms)
{
    if (s_initialized && data != NULL && data_len > 0U) {
        (void)ld2450_parser_feed(&s_parser, data, data_len, received_at_ms, handle_frame, NULL);
    }
}

void radar_service_mark_ble_timeout(uint64_t timestamp_ms)
{
    if (!s_initialized || s_lock == NULL) {
        return;
    }
    ld2450_parser_note_timeout(&s_parser, timestamp_ms);
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        const uint32_t now = (uint32_t)timestamp_ms;
        if (s_sample.sample_valid && (uint32_t)(now - s_sample.frame_uptime_ms) > 1000U) {
            s_sample.sample_valid = false;
            s_sample.target_count = 0U;
            memset(s_sample.targets, 0, sizeof(s_sample.targets));
        }
        xSemaphoreGive(s_lock);
    }
}

void radar_service_set_link_state(uint8_t link_state, bool link_online)
{
    if (!s_initialized || s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_link_state = link_state;
        s_sample.link_state = link_state;
        if (!link_online) {
            s_sample.sample_valid = false;
            s_sample.target_count = 0U;
            memset(s_sample.targets, 0, sizeof(s_sample.targets));
        }
        xSemaphoreGive(s_lock);
    }
}

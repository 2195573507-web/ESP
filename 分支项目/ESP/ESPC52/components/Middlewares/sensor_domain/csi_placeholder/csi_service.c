/**
 * @file csi_service.c
 * @brief C5 终端 CSI 运行服务。
 *
 * 本服务只在 C5 本地完成校准、低维 feature 提取和本地 IDLE/MOTION hint，
 * 再把 feature frame 发给 ESPS3。C5 不上传 raw CSI、I/Q buffer 或子载波矩阵。
 */

#include "csi_service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_main_config.h"
#include "c5_backpressure_controller.h"
#include "c5_event_bus.h"
#include "csi_capture.h"
#include "csi_edge_detector.h"
#include "csi_feature.h"
#include "csi_server_client.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "csi_service";

typedef struct {
    uint64_t timestamp_ms;
    int8_t rssi;
    uint8_t iq_count;
    csi_iq_sample_t iq_samples[CSI_PHASE_A_MAX_RAW_SUBCARRIERS];
} csi_pending_sample_t;

typedef struct {
    uint64_t timestamp_ms;
    float motion_score;
    float confidence;
    float variance;
    float cv;
    float quality;
    int rssi;
    envelope_state_hint_t state_hint;
    csi_feature_frame_t feature;
    bool valid;
} csi_latest_feature_slot_t;

static bool s_csi_started;
static bool s_csi_paused;
static bool s_csi_initialized;
static bool s_pending_sample_valid;
static csi_feature_config_t s_feature_config;
static csi_feature_processor_t s_processor;
static csi_edge_detector_t s_edge_detector;
static csi_pending_sample_t s_pending_sample;
static csi_latest_feature_slot_t s_latest_feature_slots[2];
static uint32_t s_pending_sample_overwrites;
static uint8_t s_latest_feature_read_index;
static uint8_t s_latest_feature_write_index;
static uint32_t s_worker_active_count;
static portMUX_TYPE s_feature_lock = portMUX_INITIALIZER_UNLOCKED;

static void csi_service_clear_pending_locked(void)
{
    memset(&s_pending_sample, 0, sizeof(s_pending_sample));
    memset(s_latest_feature_slots, 0, sizeof(s_latest_feature_slots));
    s_pending_sample_valid = false;
    s_pending_sample_overwrites = 0U;
    s_latest_feature_read_index = 0U;
    s_latest_feature_write_index = 0U;
}

static bool csi_service_worker_begin(void)
{
    bool active = false;
    portENTER_CRITICAL(&s_feature_lock);
    if (s_csi_started && !s_csi_paused) {
        s_worker_active_count++;
        active = true;
    }
    portEXIT_CRITICAL(&s_feature_lock);
    return active;
}

static void csi_service_worker_end(void)
{
    portENTER_CRITICAL(&s_feature_lock);
    if (s_worker_active_count > 0U) {
        s_worker_active_count--;
    }
    portEXIT_CRITICAL(&s_feature_lock);
}

static envelope_state_hint_t csi_service_edge_state_to_local_hint(csi_edge_state_t state)
{
    return state == CSI_EDGE_STATE_MOTION ? ENVELOPE_STATE_HINT_MOTION
                                          : ENVELOPE_STATE_HINT_IDLE;
}

static bool csi_service_store_pending_sample(const wifi_csi_info_t *data)
{
    if (data == NULL || data->buf == NULL || data->len < 2U) {
        return false;
    }

    size_t pair_count = (size_t)data->len / 2U;
    size_t start_pair = data->first_word_invalid ? 2U : 0U;
    if (pair_count <= start_pair) {
        return false;
    }

    size_t copied = 0;
    portENTER_CRITICAL(&s_feature_lock);
    if (!s_csi_started || s_csi_paused) {
        portEXIT_CRITICAL(&s_feature_lock);
        return false;
    }
    if (s_pending_sample_valid && s_pending_sample_overwrites < UINT32_MAX) {
        s_pending_sample_overwrites++;
    }
    s_pending_sample.timestamp_ms = (uint64_t)(esp_timer_get_time() / 1000);
    s_pending_sample.rssi = data->rx_ctrl.rssi;
    for (size_t pair = start_pair;
         pair < pair_count && copied < CSI_PHASE_A_MAX_RAW_SUBCARRIERS;
         ++pair) {
        s_pending_sample.iq_samples[copied].i = data->buf[pair * 2U];
        s_pending_sample.iq_samples[copied].q = data->buf[(pair * 2U) + 1U];
        ++copied;
    }
    s_pending_sample.iq_count = (uint8_t)copied;
    s_pending_sample_valid = copied > 0U;
    portEXIT_CRITICAL(&s_feature_lock);

    return copied > 0U;
}

static bool csi_service_take_pending_frame(csi_frame_sample_t *out_frame)
{
    if (out_frame == NULL) {
        return false;
    }

    csi_iq_sample_t iq_samples[CSI_PHASE_A_MAX_RAW_SUBCARRIERS] = {0};
    uint8_t iq_count = 0;
    int8_t rssi = 0;
    uint64_t timestamp_ms = 0;

    portENTER_CRITICAL(&s_feature_lock);
    if (s_pending_sample_valid) {
        iq_count = s_pending_sample.iq_count;
        rssi = s_pending_sample.rssi;
        timestamp_ms = s_pending_sample.timestamp_ms;
        for (uint8_t i = 0; i < iq_count && i < CSI_PHASE_A_MAX_RAW_SUBCARRIERS; ++i) {
            iq_samples[i] = s_pending_sample.iq_samples[i];
        }
        s_pending_sample_valid = false;
    }
    portEXIT_CRITICAL(&s_feature_lock);

    return iq_count > 0U &&
           csi_capture_build_frame_from_iq(iq_samples, iq_count, rssi, timestamp_ms, out_frame);
}

static void csi_service_process_frame(const csi_frame_sample_t *frame)
{
    if (frame == NULL) {
        return;
    }

    csi_feature_frame_t feature = {0};
    bool ready = csi_feature_processor_push(&s_processor, frame, &feature);
    if (ready) {
        strlcpy(feature.link_id,
                csi_server_client_local_link_id(),
                sizeof(feature.link_id));
        csi_edge_detection_t edge_detection = {0};
        if (csi_edge_detector_push(&s_edge_detector, &feature, &edge_detection)) {
            feature.state_hint =
                csi_service_edge_state_to_local_hint(edge_detection.local_state_hint);
            feature.motion_score = edge_detection.motion_score;
            feature.confidence = edge_detection.confidence;
        }
        uint8_t slot = (uint8_t)(s_latest_feature_write_index ^ 1U);
        portENTER_CRITICAL(&s_feature_lock);
        s_latest_feature_slots[slot].timestamp_ms = feature.timestamp_ms;
        s_latest_feature_slots[slot].motion_score = feature.motion_score;
        s_latest_feature_slots[slot].confidence = feature.confidence;
        s_latest_feature_slots[slot].variance = feature.metrics.variance;
        s_latest_feature_slots[slot].cv = feature.metrics.cv;
        s_latest_feature_slots[slot].quality = feature.metrics.quality;
        s_latest_feature_slots[slot].rssi = feature.metrics.rssi;
        s_latest_feature_slots[slot].state_hint = feature.state_hint;
        s_latest_feature_slots[slot].feature = feature;
        s_latest_feature_slots[slot].valid = true;
        s_latest_feature_slots[s_latest_feature_read_index].valid = false;
        s_latest_feature_read_index = slot;
        s_latest_feature_write_index = slot;
        portEXIT_CRITICAL(&s_feature_lock);
    }
}

static void csi_service_rx_cb(void *ctx, wifi_csi_info_t *data)
{
    (void)ctx;
    if (data == NULL) {
        return;
    }

    if (csi_service_store_pending_sample(data)) {
        (void)c5_event_bus_enqueue(C5_EVENT_CSI_READY, C5_EVENT_SOURCE_CALLBACK);
    }
}

static esp_err_t csi_service_configure_wifi_csi(void)
{
    wifi_csi_config_t config = {0};
#if CONFIG_SOC_WIFI_HE_SUPPORT
    config.enable = 1;
    config.acquire_csi_legacy = 1;
    config.acquire_csi_ht20 = 1;
    config.acquire_csi_su = 1;
    config.val_scale_cfg = 1;
    config.dump_ack_en = 0;
#else
    config.lltf_en = true;
    config.htltf_en = true;
    config.stbc_htltf2_en = false;
    config.ltf_merge_en = true;
    config.channel_filter_en = true;
    config.manu_scale = false;
    config.shift = 0;
    config.dump_ack_en = false;
#endif

    esp_err_t ret = esp_wifi_set_csi_rx_cb(csi_service_rx_cb, NULL);
    ESP_LOGI(TAG, "esp_wifi_set_csi_rx_cb ret=%d (%s)", (int)ret, esp_err_to_name(ret));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_wifi_set_csi_config(&config);
    ESP_LOGI(TAG, "esp_wifi_set_csi_config ret=%d (%s)", (int)ret, esp_err_to_name(ret));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_wifi_set_csi(true);
    ESP_LOGI(TAG, "esp_wifi_set_csi ret=%d (%s)", (int)ret, esp_err_to_name(ret));
    if (ret != ESP_OK) {
        return ret;
    }
    ESP_LOGI(TAG, "wifi promiscuous mode unchanged by CSI service");
    return ESP_OK;
}

esp_err_t csi_service_process_tick(void)
{
    if (!c5_should_run(C5_TASK_TYPE_CSI_PROCESS)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!csi_service_worker_begin()) {
        return ESP_ERR_INVALID_STATE;
    }

    csi_frame_sample_t frame = {0};
    if (!csi_service_take_pending_frame(&frame)) {
        csi_service_worker_end();
        return ESP_ERR_NOT_FOUND;
    }

    csi_service_process_frame(&frame);
    csi_service_worker_end();
    return ESP_OK;
}

esp_err_t csi_service_report_tick(void)
{
    if (!c5_should_run(C5_TASK_TYPE_CSI_REPORT)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!csi_service_worker_begin()) {
        return ESP_ERR_INVALID_STATE;
    }

    csi_feature_frame_t feature = {0};
    bool has_feature = false;
    portENTER_CRITICAL(&s_feature_lock);
    csi_latest_feature_slot_t *slot = &s_latest_feature_slots[s_latest_feature_read_index];
    if (slot->valid) {
        feature = slot->feature;
        slot->valid = false;
        has_feature = true;
    }
    portEXIT_CRITICAL(&s_feature_lock);

    if (!has_feature) {
        if (!csi_feature_processor_ready(&s_processor)) {
            ESP_LOGD(TAG, "CSI calibration in progress");
        }
        csi_service_worker_end();
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = csi_server_client_publish_feature_result(&feature,
                                                             CSI_OUTPUT_ENABLE_LOG != 0,
                                                             CSI_OUTPUT_ENABLE_HTTP != 0);
    csi_service_worker_end();
    return ret;
}

esp_err_t csi_service_tick(void)
{
    esp_err_t process_ret = csi_service_process_tick();
    esp_err_t report_ret = csi_service_report_tick();
    return report_ret != ESP_ERR_NOT_FOUND ? report_ret : process_ret;
}

esp_err_t csi_service_init(void)
{
    csi_feature_default_config(&s_feature_config);
    csi_feature_processor_init(&s_processor, &s_feature_config);
    csi_edge_detector_init(&s_edge_detector, NULL);
    memset(s_latest_feature_slots, 0, sizeof(s_latest_feature_slots));
    s_latest_feature_read_index = 0;
    s_latest_feature_write_index = 0;
    s_pending_sample_valid = false;
    s_pending_sample_overwrites = 0;
    s_worker_active_count = 0U;
    s_csi_initialized = true;

    if (!MAIN_ENABLE_CSI_SERVICE) {
        ESP_LOGI(TAG, "CSI service reserved; MAIN_ENABLE_CSI_SERVICE=0");
        return ESP_OK;
    }

    esp_err_t ret = csi_server_client_init();
    if (ret != ESP_OK) {
        return ret;
    }
    ESP_LOGI(TAG,
             "CSI service initialized calibration_ms=%u ewma_alpha=%.2f edge_window=%u log=%d http=%d",
             (unsigned int)s_feature_config.calibration_duration_ms,
             (double)s_feature_config.ewma_alpha,
             (unsigned int)CSI_EDGE_DETECTOR_DEFAULT_WINDOW_SIZE,
             CSI_OUTPUT_ENABLE_LOG,
             CSI_OUTPUT_ENABLE_HTTP);
    return ESP_OK;
}

esp_err_t csi_service_start(void)
{
    if (!MAIN_ENABLE_CSI_SERVICE) {
        ESP_LOGI(TAG, "CSI service start skipped; MAIN_ENABLE_CSI_SERVICE=0");
        return ESP_OK;
    }
    if (!s_csi_initialized) {
        esp_err_t init_ret = csi_service_init();
        if (init_ret != ESP_OK) {
            return init_ret;
        }
    }
    if (s_csi_started) {
        return ESP_OK;
    }

    esp_err_t bus_ret = c5_event_bus_init();
    if (bus_ret != ESP_OK) {
        ESP_LOGW(TAG, "CSI event bus init failed: %s", esp_err_to_name(bus_ret));
        return bus_ret;
    }

    csi_feature_processor_init(&s_processor, &s_feature_config);
    csi_edge_detector_init(&s_edge_detector, NULL);
    memset(s_latest_feature_slots, 0, sizeof(s_latest_feature_slots));
    s_latest_feature_read_index = 0;
    s_latest_feature_write_index = 0;
    s_pending_sample_valid = false;
    s_pending_sample_overwrites = 0;
    s_worker_active_count = 0U;

    esp_err_t ret = csi_service_configure_wifi_csi();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi CSI configure failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_csi_started = true;
    s_csi_paused = false;
    ESP_LOGI(TAG,
             "CSI service started: event-worker calibration first, edge-state output interval_ms=%u log=%d http=%d feature_version=%s",
             (unsigned int)CSI_SERVICE_REPORT_INTERVAL_MS,
             CSI_OUTPUT_ENABLE_LOG,
             CSI_OUTPUT_ENABLE_HTTP,
             CSI_ALGORITHM_VERSION);
    return ESP_OK;
}

void csi_service_pause(void)
{
    portENTER_CRITICAL(&s_feature_lock);
    if (s_csi_started) {
        s_csi_paused = true;
        csi_service_clear_pending_locked();
    }
    portEXIT_CRITICAL(&s_feature_lock);
}

esp_err_t csi_service_pause_and_wait(uint32_t timeout_ms)
{
    csi_service_pause();
    const int64_t deadline_ms = (esp_timer_get_time() / 1000) + (int64_t)timeout_ms;
    while (true) {
        uint32_t active_count;
        portENTER_CRITICAL(&s_feature_lock);
        active_count = s_worker_active_count;
        portEXIT_CRITICAL(&s_feature_lock);
        if (active_count == 0U) {
            return ESP_OK;
        }
        if ((esp_timer_get_time() / 1000) >= deadline_ms) {
            ESP_LOGW(TAG,
                     "CSI pause timeout active_workers=%lu timeout_ms=%u",
                     (unsigned long)active_count,
                     (unsigned int)timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void csi_service_resume(void)
{
    portENTER_CRITICAL(&s_feature_lock);
    if (s_csi_started) {
        s_csi_paused = false;
    }
    portEXIT_CRITICAL(&s_feature_lock);
}

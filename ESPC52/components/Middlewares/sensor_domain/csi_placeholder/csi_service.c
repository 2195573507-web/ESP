/**
 * @file csi_service.c
 * @brief C5 CSI runtime service.
 *
 * The runtime performs local calibration and feature extraction, then publishes
 * only low-dimensional feature frames to ESPS3. C5 does not decide IDLE/MOTION/HOLD
 * and never uploads raw CSI or subcarrier arrays.
 */

#include "csi_service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_main_config.h"
#include "csi_capture.h"
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

#ifndef CSI_SERVICE_PROCESS_WAIT_MS
#define CSI_SERVICE_PROCESS_WAIT_MS 50U
#endif

typedef struct {
    uint64_t timestamp_ms;
    int8_t rssi;
    uint8_t iq_count;
    csi_iq_sample_t iq_samples[CSI_PHASE_A_MAX_RAW_SUBCARRIERS];
} csi_pending_sample_t;

static bool s_csi_started;
static bool s_csi_paused;
static bool s_csi_initialized;
static bool s_latest_feature_valid;
static bool s_pending_sample_valid;
static TaskHandle_t s_csi_task;
static csi_feature_config_t s_feature_config;
static csi_feature_processor_t s_processor;
static csi_feature_frame_t s_latest_feature;
static csi_pending_sample_t s_pending_sample;
static uint32_t s_pending_sample_overwrites;
static portMUX_TYPE s_feature_lock = portMUX_INITIALIZER_UNLOCKED;

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
        portENTER_CRITICAL(&s_feature_lock);
        s_latest_feature = feature;
        s_latest_feature_valid = true;
        portEXIT_CRITICAL(&s_feature_lock);
    }
}

static void csi_service_rx_cb(void *ctx, wifi_csi_info_t *data)
{
    (void)ctx;
    if (!s_csi_started || s_csi_paused || data == NULL || s_csi_task == NULL) {
        return;
    }

    if (csi_service_store_pending_sample(data)) {
        xTaskNotifyGive(s_csi_task);
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

static void csi_service_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG,
             "CSI feature task started interval_ms=%u log=%d http=%d feature_version=%s",
             (unsigned int)CSI_SERVICE_REPORT_INTERVAL_MS,
             CSI_OUTPUT_ENABLE_LOG,
             CSI_OUTPUT_ENABLE_HTTP,
             CSI_ALGORITHM_VERSION);

    TickType_t last_report_tick = xTaskGetTickCount();

    while (1) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CSI_SERVICE_PROCESS_WAIT_MS));
        if (!s_csi_started || s_csi_paused) {
            continue;
        }

        csi_frame_sample_t frame = {0};
        if (csi_service_take_pending_frame(&frame)) {
            csi_service_process_frame(&frame);
        }

        TickType_t now_tick = xTaskGetTickCount();
        if (now_tick - last_report_tick < pdMS_TO_TICKS(CSI_SERVICE_REPORT_INTERVAL_MS)) {
            continue;
        }
        last_report_tick = now_tick;

        csi_feature_frame_t feature = {0};
        bool has_feature = false;
        portENTER_CRITICAL(&s_feature_lock);
        if (s_latest_feature_valid) {
            feature = s_latest_feature;
            s_latest_feature_valid = false;
            has_feature = true;
        }
        portEXIT_CRITICAL(&s_feature_lock);

        if (!has_feature) {
            if (!csi_feature_processor_ready(&s_processor)) {
                ESP_LOGD(TAG, "CSI calibration in progress");
            }
            continue;
        }

        esp_err_t ret = csi_server_client_publish_feature_result(&feature,
                                                                 CSI_OUTPUT_ENABLE_LOG != 0,
                                                                 CSI_OUTPUT_ENABLE_HTTP != 0);
        (void)ret;
    }
}

esp_err_t csi_service_init(void)
{
    csi_feature_default_config(&s_feature_config);
    csi_feature_processor_init(&s_processor, &s_feature_config);
    s_latest_feature_valid = false;
    s_pending_sample_valid = false;
    s_pending_sample_overwrites = 0;
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
             "CSI service initialized calibration_ms=%u ewma_alpha=%.2f feature_only=1 log=%d http=%d",
             (unsigned int)s_feature_config.calibration_duration_ms,
             (double)s_feature_config.ewma_alpha,
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

    csi_feature_processor_init(&s_processor, &s_feature_config);
    s_latest_feature_valid = false;
    s_pending_sample_valid = false;
    s_pending_sample_overwrites = 0;

    esp_err_t ret = csi_service_configure_wifi_csi();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi CSI configure failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_csi_started = true;
    s_csi_paused = false;
    if (s_csi_task == NULL) {
        BaseType_t created = xTaskCreate(csi_service_task,
                                         "csi_feature",
                                         CSI_SERVICE_TASK_STACK,
                                         NULL,
                                         CSI_SERVICE_TASK_PRIORITY,
                                         &s_csi_task);
        if (created != pdPASS) {
            s_csi_started = false;
            s_csi_task = NULL;
            (void)esp_wifi_set_csi(false);
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "CSI service started: calibration first, feature-only output");
    return ESP_OK;
}

void csi_service_pause(void)
{
    if (s_csi_started) {
        s_csi_paused = true;
    }
}

void csi_service_resume(void)
{
    if (s_csi_started) {
        s_csi_paused = false;
    }
}

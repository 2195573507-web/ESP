/**
 * @file csi_service.c
 * @brief C5 终端 CSI runtime 服务。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），在 MAIN_ENABLE_CSI_SERVICE
 * 打开后配置 WiFi CSI callback，并用阶段 A 纯函数生成 occupancy 摘要。callback
 * 只抽取少量子载波振幅并写入固定窗口；HTTP 上传在低优先级任务中执行。
 *
 * 关闭总开关时，init/start 仅记录日志并返回 ESP_OK，不启动任务、不上传结果。
 */

#include "csi_service.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "app_main_config.h"
#include "csi_capture.h"
#include "csi_feature.h"
#include "csi_presence.h"
#include "csi_server_client.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "csi_service";

static bool s_csi_started;
static bool s_csi_paused;
static bool s_csi_initialized;
static TaskHandle_t s_csi_task;
static csi_feature_config_t s_feature_config;
static csi_presence_config_t s_presence_config;
static csi_presence_state_machine_t s_presence_machine;
static csi_feature_window_t s_window;
static portMUX_TYPE s_window_lock = portMUX_INITIALIZER_UNLOCKED;

static const uint8_t s_selected_subcarriers[] = {4U, 8U, 12U, 16U, 20U, 24U, 28U, 32U};

static bool csi_service_build_frame_from_wifi(const wifi_csi_info_t *data,
                                              csi_frame_sample_t *out_frame)
{
    if (data == NULL || data->buf == NULL || data->len < 2U || out_frame == NULL) {
        return false;
    }

    const size_t pair_count = (size_t)data->len / 2U;
    csi_iq_sample_t selected_iq[CSI_PHASE_A_MAX_SELECTED_SUBCARRIERS] = {0};
    uint8_t local_indices[CSI_PHASE_A_MAX_SELECTED_SUBCARRIERS] = {0};
    size_t selected_count = 0;

    for (size_t i = 0; i < sizeof(s_selected_subcarriers) && selected_count < sizeof(selected_iq) / sizeof(selected_iq[0]); ++i) {
        size_t source_pair = s_selected_subcarriers[i];
        if (data->first_word_invalid) {
            source_pair += 2U;
        }
        if (source_pair >= pair_count) {
            continue;
        }

        selected_iq[selected_count].i = data->buf[source_pair * 2U];
        selected_iq[selected_count].q = data->buf[(source_pair * 2U) + 1U];
        local_indices[selected_count] = (uint8_t)selected_count;
        selected_count++;
    }

    return csi_capture_build_frame_from_iq(selected_iq,
                                           selected_count,
                                           local_indices,
                                           selected_count,
                                           data->rx_ctrl.rssi,
                                           (uint64_t)(esp_timer_get_time() / 1000),
                                           out_frame);
}

static void csi_service_rx_cb(void *ctx, wifi_csi_info_t *data)
{
    (void)ctx;
    if (!s_csi_started || s_csi_paused || data == NULL) {
        return;
    }

    csi_frame_sample_t frame = {0};
    if (!csi_service_build_frame_from_wifi(data, &frame)) {
        return;
    }
    if (!csi_feature_hampel_filter_frame(&frame, &s_feature_config)) {
        return;
    }

    portENTER_CRITICAL(&s_window_lock);
    (void)csi_feature_window_push(&s_window, &frame);
    portEXIT_CRITICAL(&s_window_lock);
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

    esp_err_t ret = esp_wifi_set_csi_config(&config);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_wifi_set_csi_rx_cb(csi_service_rx_cb, NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    return esp_wifi_set_csi(true);
}

static void csi_service_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "CSI summary task started interval_ms=%u", (unsigned int)CSI_SERVICE_REPORT_INTERVAL_MS);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CSI_SERVICE_REPORT_INTERVAL_MS));
        if (!s_csi_started || s_csi_paused) {
            continue;
        }

        csi_feature_window_t window_snapshot;
        portENTER_CRITICAL(&s_window_lock);
        window_snapshot = s_window;
        portEXIT_CRITICAL(&s_window_lock);

        csi_window_stats_t stats = {0};
        if (!csi_feature_window_compute_stats(&window_snapshot, &s_feature_config, &stats)) {
            continue;
        }

        csi_presence_result_t result = {0};
        if (!csi_presence_update(&s_presence_machine, &s_presence_config, &stats, &result)) {
            continue;
        }

        esp_err_t ret = csi_server_client_upload_presence_result(&result);
        if (ret != ESP_OK) {
            ESP_LOGD(TAG, "CSI result upload deferred: %s", esp_err_to_name(ret));
        }
    }
}

esp_err_t csi_service_init(void)
{
    csi_feature_default_config(&s_feature_config);
    s_feature_config.min_samples_for_good_quality = CSI_SERVICE_WINDOW_SAMPLES >= 8U ? 8U : CSI_SERVICE_WINDOW_SAMPLES;
    csi_presence_default_config(&s_presence_config);
    s_presence_config.min_samples = s_feature_config.min_samples_for_good_quality;
    csi_presence_state_machine_init(&s_presence_machine);
    csi_feature_window_init(&s_window, CSI_SERVICE_WINDOW_SAMPLES);
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
             "CSI service initialized window=%u selected_subcarriers=%u",
             (unsigned int)CSI_SERVICE_WINDOW_SAMPLES,
             (unsigned int)(sizeof(s_selected_subcarriers) / sizeof(s_selected_subcarriers[0])));
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

    esp_err_t ret = csi_service_configure_wifi_csi();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi CSI configure failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_csi_started = true;
    s_csi_paused = false;
    if (s_csi_task == NULL) {
        BaseType_t created = xTaskCreate(csi_service_task,
                                         "csi_summary",
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
    ESP_LOGI(TAG, "CSI service started: raw CSI stays local, only summary is uploaded");
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

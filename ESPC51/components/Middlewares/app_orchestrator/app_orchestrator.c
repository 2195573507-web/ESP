/**
 * @file app_orchestrator.c
 * @brief C5 终端启动编排实现。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），负责把启动链路串起来：
 * app_main -> app_orchestrator_start -> WiFi -> system/register/command -> BME -> voice。
 * 本文件不实现 WiFi 状态机、BME 驱动、Mic/VAD、语音代理、命令协议或 LCD/CSI 逻辑；
 * 这些职责分别由 wifi_manager、system_service、bme_sensor_service、voice_chain、
 * display_placeholder 和 csi_placeholder 等模块承担。
 */

#include "app_orchestrator.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "app_debug_config.h"
#include "app_main_config.h"
#include "app_stack_monitor.h"
#include "bme_sensor_service.h"
#include "gateway_link.h"
#if MAIN_ENABLE_CSI_SERVICE
#include "csi_service.h"
#endif
#include "speaker_player.h"
#include "system_service.h"
#include "voice_chain.h"
#include "wifi_manager.h"

/* 日志标签：只在本文件使用，不作为调试参数。 */
static const char *TAG = "APP_MAIN";

void app_orchestrator_start(void)
{
    char connected_ssid[33] = {0};

    app_stack_monitor_log(TAG, "app_startup_task", "orchestrator_enter");

    /*
     * 启动流程边界：
     * 1. WiFi 只连接 S3 SoftAP；
     * 2. system_service 通过 /local/v1/register/heartbeat/status/commands 与 S3 交互；
     * 3. BME 通过轻量 local JSON 上报；
     * 4. voice_chain 通过 /local/v1/voice/turn 发送 PCM，S3 再代理完整 Server 协议。
     */

    // 初始化 WiFi 管理器：内部完成 NVS、网络接口、事件循环和 STA 模式初始化。
    ESP_ERROR_CHECK(wifi_manager_init());
    app_stack_monitor_log(TAG, "app_startup_task", "after_wifi_manager_init");

    ESP_ERROR_CHECK(gateway_link_start());
    app_stack_monitor_log(TAG, "app_startup_task", "after_gateway_link_start");

    // C5 terminal 正式模式只连接 ESPS3 SoftAP，不连接家庭 WiFi。
    if (wifi_connect_to_ap() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (wifi_get_connected_ssid(connected_ssid, sizeof(connected_ssid))) {
        ESP_LOGI(TAG, "WiFi connected: %s", connected_ssid);
    } else {
        ESP_LOGI(TAG, "WiFi connected");
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_wifi_connect");

    // 等待 WiFi 连续稳定后再探测 S3 local HTTP，避免刚拿到 IP 时就发起业务请求。
    while (!wifi_is_stable()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_wifi_stable");

    /*
     * C5 与 S3 断联时，恢复 S3 连接是最高优先级。只有 health probe 和 child register
     * 成功进入 LINK_READY 后，才允许 heartbeat/status/command、BME 上传和 server voice。
     */
    ESP_ERROR_CHECK(gateway_link_wait_ready(GATEWAY_LINK_WAIT_FOREVER_MS));
    app_stack_monitor_log(TAG, "app_startup_task", "after_gateway_link_ready");
    ESP_LOGI(TAG,
             "heap summary: free=%u min_free=%u largest_8bit_block=%u",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    esp_err_t system_ret = system_service_init();
    if (system_ret != ESP_OK) {
        ESP_LOGW(TAG, "Local gateway system service init failed: %s", esp_err_to_name(system_ret));
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_system_service_init");

#if MAIN_ENABLE_CSI_SERVICE
    /*
     * CSI 运行链路必须显式打开。默认构建不会编译到这里的 init/start 调用，
     * 避免 C5 在主启动链路中注册 WiFi CSI callback 或上传 CSI 结果。
     */
    esp_err_t csi_ret = csi_service_init();
    if (csi_ret == ESP_OK) {
        csi_ret = csi_service_start();
    }
    if (csi_ret != ESP_OK) {
        ESP_LOGW(TAG, "CSI service start skipped: %s", esp_err_to_name(csi_ret));
    }
#else
    ESP_LOGI(TAG, "CSI service disabled by MAIN_ENABLE_CSI_SERVICE");
#endif
    app_stack_monitor_log(TAG, "app_startup_task", "after_csi_service_gate");

    if (MAIN_ENABLE_SPEAKER_SELF_TEST) {
        /*
         * Speaker 自检直接走 speaker_player/IIS，不经过 server voice。
         * 用于区分硬件/IIS/功放问题和服务器 PCM/播放路径问题。
         */
        esp_err_t speaker_test_ret =
            audio_player_self_test_1khz(MAIN_SPEAKER_SELF_TEST_DURATION_MS);
        if (speaker_test_ret != ESP_OK) {
            ESP_LOGE(TAG, "Speaker self-test failed: %s", esp_err_to_name(speaker_test_ret));
        } else {
            ESP_LOGI(TAG, "Speaker self-test done");
        }
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_speaker_self_test_gate");

    if (MAIN_ENABLE_BME_SERVICE) {
        /*
         * WiFi 稳定后先启动 BME690 后台服务。语音链路进入 local gateway voice turn 时，
         * voice_chain 会暂停本服务，S3 回传 PCM 播放完成并恢复 Mic 监听后再恢复。
         */
        esp_err_t bme_ret = bme_sensor_service_start();
        if (bme_ret != ESP_OK) {
            ESP_LOGE(TAG, "BME service start failed: %s", esp_err_to_name(bme_ret));
        }
    } else {
        ESP_LOGI(TAG, "BME service disabled by MAIN_ENABLE_BME_SERVICE");
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_bme_service_start");

    if (MAIN_ENABLE_MIC_CHAIN) {
        /*
         * WiFi 稳定后启动完整本地网关半双工语音链路：
         * Mic/VAD -> ESPS3 /local/v1/voice/turn -> speaker 播放 S3 回传 PCM -> 恢复 Mic。
         */
        esp_err_t voice_ret = voice_chain_start();
        if (voice_ret != ESP_OK) {
            ESP_LOGE(TAG, "Voice chain start failed: %s", esp_err_to_name(voice_ret));
        }
    } else {
        ESP_LOGI(TAG, "Mic chain disabled by MAIN_ENABLE_MIC_CHAIN");
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_voice_chain_start");

    // WiFi 重连、Mic ADC/VAD、本地 voice turn、speaker PCM 播放和 BME 服务都在后台任务中运行。
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(MAIN_IDLE_DELAY_MS));
    }
}

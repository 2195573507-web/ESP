#include "app_orchestrator.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "app_debug_config.h"
#include "app_main_config.h"
#include "app_stack_monitor.h"
#include "app_time_sync.h"
#include "bme_sensor_service.h"
#include "speaker_player.h"
#include "system_service.h"
#include "voice_chain.h"
#include "wake_prompt_cache.h"
#include "wifi_manager.h"

/* 日志标签：只在本文件使用，不作为调试参数。 */
static const char *TAG = "APP_MAIN";

void app_orchestrator_start(void)
{
    char connected_ssid[33] = {0};

    app_stack_monitor_log(TAG, "app_startup_task", "orchestrator_enter");

    // 初始化 WiFi 管理器：内部完成 NVS、网络接口、事件循环和 STA 模式初始化。
    ESP_ERROR_CHECK(wifi_manager_init());
    app_stack_monitor_log(TAG, "app_startup_task", "after_wifi_manager_init");

    // 持续扫描并连接已保存列表中当前可用且信号最强的 WiFi。
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

    // 等待 WiFi 连续稳定后再启动 Mic/server voice，避免刚拿到 IP 时就发起语音 turn。
    while (!wifi_is_stable()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_wifi_stable");
    esp_err_t time_sync_ret = app_time_sync_once(APP_TIME_SYNC_SERVER_URL);
    if (time_sync_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "time sync failed, uploads continue time_synced=false: %s",
                 esp_err_to_name(time_sync_ret));
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_time_sync_once");
    ESP_LOGI(TAG,
             "heap summary: free=%u min_free=%u largest_8bit_block=%u",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    esp_err_t wake_prompt_ret = wake_prompt_cache_start_async();
    if (wake_prompt_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "wake prompt cache task start failed, continue with builtin prompt: %s",
                 esp_err_to_name(wake_prompt_ret));
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_wake_prompt_cache_start");

    esp_err_t system_ret = system_service_init();
    if (system_ret != ESP_OK) {
        ESP_LOGW(TAG, "System command service init failed: %s", esp_err_to_name(system_ret));
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_system_service_init");

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
         * WiFi 稳定后先启动 BME690 后台服务。语音链路进入 server voice turn 时，
         * voice_chain 会暂停本服务，服务器 PCM 播放完成并恢复 Mic 监听后再恢复。
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
         * WiFi 稳定后启动完整 server-only 半双工语音链路：
         * Mic/VAD -> server_voice_client POST /api/voice/turn -> speaker 播放服务器裸 PCM -> 恢复 Mic。
         */
        esp_err_t voice_ret = voice_chain_start();
        if (voice_ret != ESP_OK) {
            ESP_LOGE(TAG, "Voice chain start failed: %s", esp_err_to_name(voice_ret));
        }
    } else {
        ESP_LOGI(TAG, "Mic chain disabled by MAIN_ENABLE_MIC_CHAIN");
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_voice_chain_start");

    // WiFi 重连、Mic ADC/VAD、server voice turn、speaker PCM 播放和 BME 服务都在后台任务中运行。
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(MAIN_IDLE_DELAY_MS));
    }
}

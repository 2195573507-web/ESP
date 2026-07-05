/**
 * @file gateway_orchestrator.c
 * @brief S3 网关启动编排实现。
 *
 * 本文件属于 ESPS3 网关，负责串起 offline_policy、child_registry、command_router、
 * sensor_aggregator、voice_proxy、CSI placeholder、SoftAP 和 local HTTP server。
 * 本文件不解析 C5 JSON、不直接访问 ESP-server 业务路径、不实现真实 CSI，也不实现
 * ASR/LLM/TTS；这些边界分别在 protocol_adapter、server_client、csi_placeholder_gateway
 * 和 voice_proxy 中处理。
 */

#include "gateway_orchestrator.h"

#include "app_main_config.h"
#include "app_stack_monitor.h"
#include "child_registry.h"
#include "command_router.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_config.h"
#include "gateway_event_reporter.h"
#if GATEWAY_CONFIG_ENABLE_CSI_TRIGGER
#include "csi_placeholder_gateway.h"
#endif
#include "gateway_wifi.h"
#include "local_http_server.h"
#include "offline_policy.h"
#include "sensor_aggregator.h"
#include "smart_home_gateway.h"
#include "voice_proxy.h"
#include "wake_prompt_cache_gateway.h"

static const char *TAG = "gateway_main";

static TaskHandle_t s_gateway_periodic_task;

static void gateway_periodic_task(void *arg)
{
    (void)arg;
    while (1) {
        sensor_aggregator_upload_snapshot();
        if (!voice_proxy_should_skip_non_voice_task("gateway periodic task")) {
            smart_home_gateway_poll_once();
        }
        vTaskDelay(pdMS_TO_TICKS(gateway_config_get()->sensor_forward_period_ms));
    }
}

void gateway_orchestrator_start(void)
{
    app_stack_monitor_log(TAG, "gateway_startup_task", "orchestrator_enter");
    gateway_config_log_boot_profile();

    /*
     * S3 启动流程边界：
     * 1. 先初始化本地状态和代理模块；
     * 2. gateway_wifi_start() 创建 SoftAP，并在配置存在时启动 STA 上云；
     * 3. local_http_server_start() 暴露 /local/v1，接收 C5 轻量协议；
     * 4. server_client 只在各 handler 被调用时访问完整 /api/... Server 协议。
     */

    offline_policy_init();
    gateway_event_reporter_init();
    ESP_ERROR_CHECK(child_registry_init());
    ESP_ERROR_CHECK(command_router_init());
    sensor_aggregator_init();
    ESP_ERROR_CHECK(smart_home_gateway_init());
    ESP_ERROR_CHECK(voice_proxy_init());
    ESP_ERROR_CHECK(wake_prompt_cache_gateway_init());
#if GATEWAY_CONFIG_ENABLE_CSI_TRIGGER
    csi_placeholder_gateway_init();
#else
    ESP_LOGI(TAG, "CSI trigger disabled by GATEWAY_CONFIG_ENABLE_CSI_TRIGGER");
#endif

    ESP_ERROR_CHECK(gateway_wifi_start());
    app_stack_monitor_log(TAG, "gateway_startup_task", "after_gateway_wifi_start");

    ESP_ERROR_CHECK(local_http_server_start());
    app_stack_monitor_log(TAG, "gateway_startup_task", "after_local_http_start");

    if (s_gateway_periodic_task == NULL) {
        BaseType_t created = xTaskCreate(gateway_periodic_task,
                                         "gateway_periodic",
                                         8192U,
                                         NULL,
                                         3U,
                                         &s_gateway_periodic_task);
        if (created != pdPASS) {
            s_gateway_periodic_task = NULL;
            ESP_LOGW(TAG, "gateway periodic task create failed");
        } else {
            ESP_LOGI(TAG, "gateway periodic task started");
        }
    }

    while (1) {
        ESP_LOGI(TAG,
                 "gateway heartbeat softap=%d sta=%d server=%d voice_busy=%d free_heap=%u psram_heap=%u last_error=%s",
                 gateway_wifi_is_softap_ready() ? 1 : 0,
                 gateway_wifi_is_sta_connected() ? 1 : 0,
                 offline_policy_server_available() ? 1 : 0,
                 voice_proxy_is_busy() ? 1 : 0,
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 offline_policy_last_error_code());
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

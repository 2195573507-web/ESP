/**
 * @file gateway_orchestrator.c
 * @brief S3 网关启动编排实现。
 *
 * orchestrator 只负责编排模块生命周期。runtime 事件分发、周期 cadence 和
 * backpressure 都由 s3_scheduler 负责，避免启动层混入业务调度逻辑。
 */

#include "gateway_orchestrator.h"

#include "app_stack_monitor.h"
#include "bme_cache_manager.h"
#include "child_registry.h"
#include "command_router.h"
#include "csi_placeholder_gateway.h"
#include "device_stream_gateway.h"
#include "esp_check.h"
#include "esp_log.h"
#include "gateway_config.h"
#include "gateway_event_reporter.h"
#include "gateway_wifi.h"
#include "network_replay_worker.h"
#include "network_worker.h"
#include "offline_policy.h"
#include "resource_manager.h"
#include "s3_scheduler.h"
#include "sensor_aggregator.h"
#include "smart_home_gateway.h"
#include "voice_proxy.h"
#include "wake_prompt_cache_gateway.h"

static const char *TAG = "gateway_main";

void gateway_orchestrator_start(void)
{
    app_stack_monitor_log(TAG, "gateway_startup_task", "orchestrator_enter");
    app_stack_monitor_log(TAG, "gateway_orchestrator", "orchestrator_enter");
    gateway_config_log_boot_profile();

    /*
     * 启动顺序：
     * 1. 先初始化本地状态和 Server-facing helper；
     * 2. 在任何 ingress 入队前初始化 scheduler；
     * 3. 打开本地服务前先启动 Wi-Fi 和 network state worker；
     * 4. HTTP/UDP ingress 交付 work 前先启动 scheduler worker。
     */
    offline_policy_init();
    gateway_event_reporter_init();
    ESP_ERROR_CHECK(bme_cache_manager_init());
    ESP_ERROR_CHECK(child_registry_init());
    ESP_ERROR_CHECK(command_router_init());
    sensor_aggregator_init();
    ESP_ERROR_CHECK(smart_home_gateway_init());
    ESP_ERROR_CHECK(voice_proxy_init());
    ESP_ERROR_CHECK(wake_prompt_cache_gateway_init());
    ESP_ERROR_CHECK(device_stream_gateway_init());
    csi_placeholder_gateway_init();
    ESP_ERROR_CHECK(resource_manager_init());
    ESP_ERROR_CHECK(s3_scheduler_init());
    ESP_ERROR_CHECK(network_worker_init());
    ESP_ERROR_CHECK(network_replay_worker_init());

    ESP_ERROR_CHECK(gateway_wifi_start());
    app_stack_monitor_log(TAG, "gateway_startup_task", "after_gateway_wifi_start");
    app_stack_monitor_log(TAG, "gateway_orchestrator", "after_gateway_wifi_start");

    ESP_ERROR_CHECK(s3_scheduler_start());
    /* local HTTP is owned by network_worker after it has observed SoftAP ready. */
    ESP_ERROR_CHECK(network_worker_enable_local_http_server());
    ESP_ERROR_CHECK(device_stream_gateway_start());
    ESP_ERROR_CHECK(csi_placeholder_gateway_start());
    app_stack_monitor_log(TAG, "gateway_startup_task", "scheduler_started");
    app_stack_monitor_log(TAG, "gateway_orchestrator", "services_started");

    ESP_LOGI(TAG, "gateway orchestrator startup complete; scheduler owns runtime");
}

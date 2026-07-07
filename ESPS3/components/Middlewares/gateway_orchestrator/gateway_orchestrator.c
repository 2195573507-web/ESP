/**
 * @file gateway_orchestrator.c
 * @brief S3 gateway startup orchestration.
 *
 * The orchestrator only wires module lifecycle. Runtime event dispatch,
 * periodic cadence, and backpressure live in s3_scheduler.
 */

#include "gateway_orchestrator.h"

#include "app_stack_monitor.h"
#include "child_registry.h"
#include "command_router.h"
#include "csi_placeholder_gateway.h"
#include "device_stream_gateway.h"
#include "esp_check.h"
#include "esp_log.h"
#include "gateway_config.h"
#include "gateway_event_reporter.h"
#include "gateway_wifi.h"
#include "local_http_server.h"
#include "network_worker.h"
#include "offline_policy.h"
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
     * Startup order:
     * 1. initialize local state and Server-facing helpers;
     * 2. initialize the scheduler before any ingress can enqueue events;
     * 3. start Wi-Fi and network state worker before opening local services;
     * 4. start scheduler before HTTP/UDP ingress can deliver work.
     */
    offline_policy_init();
    gateway_event_reporter_init();
    ESP_ERROR_CHECK(child_registry_init());
    ESP_ERROR_CHECK(command_router_init());
    sensor_aggregator_init();
    ESP_ERROR_CHECK(smart_home_gateway_init());
    ESP_ERROR_CHECK(voice_proxy_init());
    ESP_ERROR_CHECK(wake_prompt_cache_gateway_init());
    ESP_ERROR_CHECK(device_stream_gateway_init());
    csi_placeholder_gateway_init();
    ESP_ERROR_CHECK(s3_scheduler_init());
    ESP_ERROR_CHECK(network_worker_init());

    ESP_ERROR_CHECK(gateway_wifi_start());
    app_stack_monitor_log(TAG, "gateway_startup_task", "after_gateway_wifi_start");
    app_stack_monitor_log(TAG, "gateway_orchestrator", "after_gateway_wifi_start");

    ESP_ERROR_CHECK(s3_scheduler_start());
    ESP_ERROR_CHECK(local_http_server_start());
    ESP_ERROR_CHECK(device_stream_gateway_start());
    ESP_ERROR_CHECK(csi_placeholder_gateway_start());
    app_stack_monitor_log(TAG, "gateway_startup_task", "scheduler_started");
    app_stack_monitor_log(TAG, "gateway_orchestrator", "services_started");

    ESP_LOGI(TAG, "gateway orchestrator startup complete; scheduler owns runtime");
}

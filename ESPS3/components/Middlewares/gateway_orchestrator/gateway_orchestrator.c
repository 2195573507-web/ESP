/**
 * @file gateway_orchestrator.c
 * @brief S3 网关启动编排实现。
 *
 * orchestrator 只负责编排模块生命周期。runtime 事件分发、周期 cadence 和
 * backpressure 都由 s3_scheduler 负责，避免启动层混入业务调度逻辑。
 */

#include "gateway_orchestrator.h"

#include "app_stack_monitor.h"
#include "audio_wake_gateway.h"
#include "bme_cache_manager.h"
#include "child_registry.h"
#include "command_router.h"
#include "device_stream_gateway.h"
#include "environment_alarm_adapter.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "gateway_config.h"
#include "gateway_event_reporter.h"
#include "gateway_wifi.h"
#include "habit_rule_adapter.h"
#include "habit_event_reporter.h"
#include "network_replay_worker.h"
#include "network_worker.h"
#include "offline_policy.h"
#include "radar_diagnostics.h"
#include "radar_gateway_ingest.h"
#include "radar_ingest.h"
#include "radar_local_adapter.h"
#include "radar_registry.h"
#include "resource_manager.h"
#include "s3_scheduler.h"
#include "sensor_aggregator.h"
#include "smart_home_gateway.h"
#include "voice_proxy.h"
#include "wake_prompt_cache_gateway.h"

static const char *TAG = "gateway_main";

static void startup_memory_check(const char *module)
{
    ESP_LOGI(TAG,
             "STARTUP_MEMORY_CHECK module=%s internal_free=%u internal_min=%u internal_largest=%u dma_free=%u dma_largest=%u psram_free=%u psram_largest=%u",
             module != NULL ? module : "unknown",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void startup_module_result(const char *module, esp_err_t ret)
{
    startup_memory_check(module);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "module disabled module=%s ret=%s", module, esp_err_to_name(ret));
    }
}

void gateway_orchestrator_start(void)
{
    (void)app_heap_integrity_check(TAG, "gateway_enter");
    app_stack_monitor_log(TAG, "gateway_startup_task", "orchestrator_enter");
    app_stack_monitor_log(TAG, "gateway_orchestrator", "orchestrator_enter");
    gateway_config_log_boot_profile();
    (void)app_heap_integrity_check(TAG, "after_gateway");

    /* Phase 1: prepare queues and workers before WiFi callbacks can publish state. */
    const esp_err_t resource_ret = resource_manager_init();
    const esp_err_t scheduler_init_ret = resource_ret == ESP_OK ? s3_scheduler_init() : resource_ret;

    /* The worker retains its own HTTP retry policy.  Starting it first prevents
     * the AP_START/STA_START callbacks from racing an uninitialised queue. */
    startup_memory_check("network_worker.before");
    const esp_err_t network_worker_ret = network_worker_init();
    startup_module_result("network_worker.after", network_worker_ret);
    (void)app_heap_integrity_check(TAG, "after_network_worker");

    startup_memory_check("wifi.before");
    startup_memory_check("event_loop.before");
    const esp_err_t wifi_ret = gateway_wifi_start();
    startup_module_result("event_loop.after", wifi_ret);
    startup_module_result("wifi.after", wifi_ret);
    (void)app_heap_integrity_check(TAG, "after_wifi");
    app_stack_monitor_log(TAG, "gateway_startup_task", "after_gateway_wifi_start");
    app_stack_monitor_log(TAG, "gateway_orchestrator", "after_gateway_wifi_start");

    /* Enable local HTTP only after both its worker and the WiFi state machine exist. */
    startup_memory_check("local_http.before");
    const esp_err_t local_http_ret = network_worker_ret == ESP_OK
        ? network_worker_enable_local_http_server() : ESP_ERR_INVALID_STATE;
    startup_module_result("local_http.after", local_http_ret);
    (void)app_heap_integrity_check(TAG, "after_local_http");

    /* Phase 2: sensors.  A failed sensor is isolated from the gateway core. */
    startup_memory_check("radar.before");
    const esp_err_t radar_registry_ret = radar_registry_init() ? ESP_OK : ESP_ERR_NO_MEM;
    const esp_err_t radar_ingest_ret = radar_registry_ret == ESP_OK
        ? radar_ingest_start() : radar_registry_ret;
    const esp_err_t radar_local_ret = radar_ingest_ret == ESP_OK
        ? radar_local_adapter_start() : radar_ingest_ret;
    startup_module_result("radar.after", radar_local_ret);
    (void)app_heap_integrity_check(TAG, "after_radar");
    const esp_err_t habit_rule_ret = radar_local_ret == ESP_OK
        ? habit_rule_adapter_start() : radar_local_ret;
    startup_module_result("habit_rule.after", habit_rule_ret);
    const esp_err_t habit_reporter_ret = habit_rule_ret == ESP_OK
        ? habit_event_reporter_start() : habit_rule_ret;
    startup_module_result("habit_event_reporter.after", habit_reporter_ret);

    startup_memory_check("BME.before");
    const esp_err_t bme_ret = bme_cache_manager_init();
    startup_module_result("BME.after", bme_ret);
    (void)app_heap_integrity_check(TAG, "after_bme");

    /* Lightweight state-only services do not allocate task stacks. */
    offline_policy_init();
    gateway_event_reporter_init();
    startup_memory_check("command.before");
    const esp_err_t child_registry_ret = child_registry_init();
    const esp_err_t command_ret = child_registry_ret == ESP_OK
        ? command_router_init() : child_registry_ret;
    startup_module_result("command.after", command_ret);
    sensor_aggregator_init();

    /* Phase 3: analysis and dispatch. */
    startup_memory_check("radar_diagnostics.before");
    const esp_err_t radar_diagnostics_ret = radar_local_ret == ESP_OK
        ? radar_diagnostics_start() : radar_local_ret;
    startup_module_result("radar_diagnostics.after", radar_diagnostics_ret);

    startup_memory_check("environment_alarm.before");
    const esp_err_t environment_alarm_ret = environment_alarm_adapter_init();
    startup_module_result("environment_alarm.after", environment_alarm_ret);

    startup_memory_check("scheduler.before");
    const esp_err_t scheduler_ret = scheduler_init_ret == ESP_OK ? s3_scheduler_start() : scheduler_init_ret;
    startup_module_result("scheduler.after", scheduler_ret);

    startup_memory_check("reporter.before");
    const esp_err_t smart_home_ret = smart_home_gateway_init();
    const esp_err_t wake_prompt_ret = smart_home_ret == ESP_OK
        ? wake_prompt_cache_gateway_init() : smart_home_ret;
    const esp_err_t replay_ret = wake_prompt_ret == ESP_OK
        ? network_replay_worker_init() : wake_prompt_ret;
    const esp_err_t device_stream_init_ret = replay_ret == ESP_OK
        ? device_stream_gateway_init() : replay_ret;
    const esp_err_t device_stream_ret = device_stream_init_ret == ESP_OK && scheduler_ret == ESP_OK
        ? device_stream_gateway_start() : (device_stream_init_ret != ESP_OK ? device_stream_init_ret : scheduler_ret);
    startup_module_result("reporter.after", device_stream_ret);

    /* Phase 4: voice is intentionally last because it owns the largest request stack. */
    startup_memory_check("voice.before");
    const esp_err_t wake_audio_init_ret = audio_wake_gateway_init();
    startup_module_result("wake_audio.init", wake_audio_init_ret);
    const esp_err_t voice_ret = wake_audio_init_ret == ESP_OK ? voice_proxy_init() : wake_audio_init_ret;
    startup_module_result("voice.after", voice_ret);
    app_stack_monitor_log(TAG, "gateway_startup_task", "scheduler_started");
    app_stack_monitor_log(TAG, "gateway_orchestrator", "services_started");
    app_s3_mem_log(TAG, "boot_complete");

    ESP_LOGI(TAG, "gateway orchestrator startup complete; scheduler owns runtime");
}

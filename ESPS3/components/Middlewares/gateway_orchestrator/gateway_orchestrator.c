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
#include "device_stream_gateway.h"
#include "environment_alarm_adapter.h"
#include "esp_check.h"
#include "esp_log.h"
#include "gateway_config.h"
#include "gateway_event_reporter.h"
#include "gateway_wifi.h"
#include "home_ai_room_state.h"
#include "home_ai_history_store.h"
#include "home_ai_event_reporter.h"
#include "home_ai_emergency_coordinator.h"
#include "home_ai_rule_engine.h"
#include "home_ai_runtime.h"
#include "home_ai_user_override.h"
#include "home_ai_virtual_device.h"
#include "home_ai_voice_session.h"
#include "home_ai_voice_router.h"
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

static esp_err_t startup_bool_result(bool initialized)
{
    return initialized ? ESP_OK : ESP_ERR_NO_MEM;
}

static void startup_module_begin(const char *module)
{
    app_startup_memory_check(TAG, module, "before");
}

static esp_err_t startup_module_result(const char *module, esp_err_t ret)
{
    app_startup_memory_check(TAG, module, "after");
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "STARTUP_MODULE_RESULT module=%s state=ready ret=%s",
                 module != NULL ? module : "unknown",
                 esp_err_to_name(ret));
    } else {
        ESP_LOGE(TAG,
                 "STARTUP_MODULE_RESULT module=%s state=degraded ret=%s action=continue",
                 module != NULL ? module : "unknown",
                 esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t startup_module_skipped(const char *module,
                                        const char *dependency,
                                        esp_err_t dependency_ret)
{
    app_startup_memory_check(TAG, module, "after");
    ESP_LOGW(TAG,
             "STARTUP_MODULE_RESULT module=%s state=skipped dependency=%s dependency_ret=%s action=continue",
             module != NULL ? module : "unknown",
             dependency != NULL ? dependency : "unknown",
             esp_err_to_name(dependency_ret));
    return dependency_ret != ESP_OK ? dependency_ret : ESP_ERR_INVALID_STATE;
}

void gateway_orchestrator_start(void)
{
    app_startup_memory_check(TAG, "gateway_orchestrator", "entry");
    app_stack_monitor_log(TAG, "gateway_startup_task", "orchestrator_enter");
    app_stack_monitor_log(TAG, "gateway_orchestrator", "orchestrator_enter");
    gateway_config_log_boot_profile();

    /* Bootstrap state and all no-task services.  Each failure is isolated. */
    startup_module_begin("offline_policy");
    offline_policy_init();
    startup_module_result("offline_policy", ESP_OK);

    startup_module_begin("gateway_event_reporter");
    gateway_event_reporter_init();
    startup_module_result("gateway_event_reporter", ESP_OK);

    startup_module_begin("resource_manager");
    const esp_err_t resource_ret = resource_manager_init();
    startup_module_result("resource_manager", resource_ret);

    /* Initialize scheduler queues before workers can publish runtime events. */
    startup_module_begin("s3_scheduler_init");
    const esp_err_t scheduler_init_ret = s3_scheduler_init();
    startup_module_result("s3_scheduler_init", scheduler_init_ret);

    startup_module_begin("bme_cache_manager");
    const esp_err_t bme_ret = bme_cache_manager_init();
    startup_module_result("bme_cache_manager", bme_ret);

    startup_module_begin("child_registry");
    const esp_err_t child_registry_ret = child_registry_init();
    startup_module_result("child_registry", child_registry_ret);

    startup_module_begin("command_router");
    const esp_err_t command_router_ret = command_router_init();
    startup_module_result("command_router", command_router_ret);

    startup_module_begin("sensor_aggregator");
    sensor_aggregator_init();
    startup_module_result("sensor_aggregator", ESP_OK);

    startup_module_begin("environment_alarm_adapter");
    const esp_err_t environment_alarm_ret = environment_alarm_adapter_init();
    startup_module_result("environment_alarm_adapter", environment_alarm_ret);
    if (environment_alarm_ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "ENV_ALARM_INIT engine_count=0 reporter_ready=0 queue_capacity=0 rules_enabled=0 result=%s",
                 esp_err_to_name(environment_alarm_ret));
    }

    startup_module_begin("smart_home_gateway");
    const esp_err_t smart_home_ret = smart_home_gateway_init();
    startup_module_result("smart_home_gateway", smart_home_ret);

    startup_module_begin("wake_prompt_cache_gateway");
    const esp_err_t wake_prompt_ret = wake_prompt_cache_gateway_init();
    startup_module_result("wake_prompt_cache_gateway", wake_prompt_ret);

    startup_module_begin("device_stream_gateway_init");
    const esp_err_t device_stream_init_ret = device_stream_gateway_init();
    startup_module_result("device_stream_gateway_init", device_stream_init_ret);

    startup_module_begin("radar_registry");
    const esp_err_t radar_registry_ret = startup_bool_result(radar_registry_init());
    startup_module_result("radar_registry", radar_registry_ret);

    /* Home AI remains a complete feature group; failed optional stores degrade locally. */
    startup_module_begin("home_ai_voice_session");
    const esp_err_t home_ai_voice_session_ret = home_ai_voice_session_manager_init();
    startup_module_result("home_ai_voice_session", home_ai_voice_session_ret);

    startup_module_begin("home_ai_voice_router");
    const esp_err_t home_ai_voice_router_ret = startup_bool_result(home_ai_voice_router_init());
    startup_module_result("home_ai_voice_router", home_ai_voice_router_ret);

    startup_module_begin("home_ai_room_state");
    const esp_err_t home_ai_room_state_ret = startup_bool_result(home_ai_room_state_init());
    startup_module_result("home_ai_room_state", home_ai_room_state_ret);

    startup_module_begin("home_ai_user_override");
    const esp_err_t home_ai_user_override_ret =
        startup_bool_result(home_ai_user_override_manager_init());
    startup_module_result("home_ai_user_override", home_ai_user_override_ret);

    startup_module_begin("home_ai_rule_engine");
    const esp_err_t home_ai_rule_engine_ret = startup_bool_result(home_ai_rule_engine_init());
    startup_module_result("home_ai_rule_engine", home_ai_rule_engine_ret);

    startup_module_begin("home_ai_virtual_device");
    const esp_err_t home_ai_virtual_device_ret =
        startup_bool_result(home_ai_virtual_device_executor_init());
    startup_module_result("home_ai_virtual_device", home_ai_virtual_device_ret);

    startup_module_begin("home_ai_history_store");
    const esp_err_t home_ai_history_ret = startup_bool_result(home_ai_history_store_init());
    startup_module_result("home_ai_history_store", home_ai_history_ret);

    startup_module_begin("home_ai_event_reporter");
    const esp_err_t home_ai_event_reporter_ret =
        startup_bool_result(home_ai_event_reporter_init());
    startup_module_result("home_ai_event_reporter", home_ai_event_reporter_ret);

    startup_module_begin("home_ai_emergency_coordinator");
    esp_err_t home_ai_emergency_ret = ESP_ERR_INVALID_STATE;
    if (environment_alarm_ret == ESP_OK) {
        home_ai_emergency_ret =
            startup_bool_result(home_ai_emergency_coordinator_init());
        startup_module_result("home_ai_emergency_coordinator", home_ai_emergency_ret);
    } else {
        home_ai_emergency_ret = startup_module_skipped("home_ai_emergency_coordinator",
                                                       "environment_alarm_adapter",
                                                       environment_alarm_ret);
    }

    startup_module_begin("home_ai_runtime");
    const esp_err_t home_ai_runtime_ret = startup_bool_result(home_ai_runtime_init());
    startup_module_result("home_ai_runtime", home_ai_runtime_ret);

    /* Sensor workers are independent of one another once the registry exists. */
    startup_module_begin("radar_ingest_start");
    esp_err_t radar_ingest_ret = ESP_ERR_INVALID_STATE;
    if (radar_registry_ret == ESP_OK) {
        radar_ingest_ret = radar_ingest_start();
        startup_module_result("radar_ingest_start", radar_ingest_ret);
    } else {
        radar_ingest_ret = startup_module_skipped("radar_ingest_start",
                                                  "radar_registry",
                                                  radar_registry_ret);
    }

    /* Network worker owns retries and remains useful even if another subsystem degraded. */
    startup_module_begin("network_worker_init");
    const esp_err_t network_worker_ret = network_worker_init();
    startup_module_result("network_worker_init", network_worker_ret);

    startup_module_begin("network_replay_worker_init");
    esp_err_t network_replay_ret = ESP_ERR_INVALID_STATE;
    if (network_worker_ret == ESP_OK) {
        network_replay_ret = network_replay_worker_init();
        startup_module_result("network_replay_worker_init", network_replay_ret);
    } else {
        network_replay_ret = startup_module_skipped("network_replay_worker_init",
                                                    "network_worker_init",
                                                    network_worker_ret);
    }

    /* Network queues exist before Wi-Fi callbacks can publish link or station events. */
    startup_module_begin("gateway_wifi_start");
    const esp_err_t wifi_ret = gateway_wifi_start();
    startup_module_result("gateway_wifi_start", wifi_ret);
    app_stack_monitor_log(TAG, "gateway_startup_task", "after_gateway_wifi_start");
    app_stack_monitor_log(TAG, "gateway_orchestrator", "after_gateway_wifi_start");

    startup_module_begin("s3_scheduler_start");
    esp_err_t scheduler_ret = ESP_ERR_INVALID_STATE;
    if (scheduler_init_ret == ESP_OK) {
        scheduler_ret = s3_scheduler_start();
        startup_module_result("s3_scheduler_start", scheduler_ret);
    } else {
        scheduler_ret = startup_module_skipped("s3_scheduler_start",
                                               "s3_scheduler_init",
                                               scheduler_init_ret);
    }

    startup_module_begin("radar_local_adapter_start");
    esp_err_t radar_local_ret = ESP_ERR_INVALID_STATE;
    if (radar_registry_ret == ESP_OK) {
        radar_local_ret = radar_local_adapter_start();
        startup_module_result("radar_local_adapter_start", radar_local_ret);
    } else {
        radar_local_ret = startup_module_skipped("radar_local_adapter_start",
                                                 "radar_registry",
                                                 radar_registry_ret);
    }

    startup_module_begin("radar_diagnostics_start");
    esp_err_t radar_diagnostics_ret = ESP_ERR_INVALID_STATE;
    if (radar_registry_ret == ESP_OK) {
        radar_diagnostics_ret = radar_diagnostics_start();
        startup_module_result("radar_diagnostics_start", radar_diagnostics_ret);
    } else {
        radar_diagnostics_ret = startup_module_skipped("radar_diagnostics_start",
                                                        "radar_registry",
                                                        radar_registry_ret);
    }

    startup_module_begin("device_stream_gateway_start");
    esp_err_t device_stream_ret = ESP_ERR_INVALID_STATE;
    if (device_stream_init_ret == ESP_OK && scheduler_ret == ESP_OK) {
        device_stream_ret = device_stream_gateway_start();
        startup_module_result("device_stream_gateway_start", device_stream_ret);
    } else {
        const esp_err_t dependency_ret = device_stream_init_ret != ESP_OK
            ? device_stream_init_ret : scheduler_ret;
        const char *dependency = device_stream_init_ret != ESP_OK
            ? "device_stream_gateway_init" : "s3_scheduler_start";
        device_stream_ret = startup_module_skipped("device_stream_gateway_start",
                                                   dependency,
                                                   dependency_ret);
    }

    /* Voice owns the largest turn-local allocation, so start it after core workers. */
    startup_module_begin("voice_proxy");
    const esp_err_t voice_proxy_ret = voice_proxy_init();
    startup_module_result("voice_proxy", voice_proxy_ret);

    /* local HTTP is owned by network_worker and opens only after voice init was attempted. */
    startup_module_begin("network_worker_enable_local_http_server");
    esp_err_t local_http_ret = ESP_ERR_INVALID_STATE;
    if (network_worker_ret == ESP_OK) {
        local_http_ret = network_worker_enable_local_http_server();
        startup_module_result("network_worker_enable_local_http_server", local_http_ret);
    } else {
        local_http_ret = startup_module_skipped("network_worker_enable_local_http_server",
                                                "network_worker_init",
                                                network_worker_ret);
    }

    app_startup_memory_check(TAG, "gateway_orchestrator", "complete");
    app_stack_monitor_log(TAG, "gateway_startup_task", "startup_complete");
    app_stack_monitor_log(TAG, "gateway_orchestrator", "startup_complete");

    ESP_LOGI(TAG,
             "gateway orchestrator startup complete wifi=%s scheduler=%s network=%s replay=%s radar_ingest=%s radar_local=%s http=%s device_stream=%s",
             esp_err_to_name(wifi_ret),
             esp_err_to_name(scheduler_ret),
             esp_err_to_name(network_worker_ret),
             esp_err_to_name(network_replay_ret),
             esp_err_to_name(radar_ingest_ret),
             esp_err_to_name(radar_local_ret),
             esp_err_to_name(local_http_ret),
             esp_err_to_name(device_stream_ret));
}

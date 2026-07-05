/**
 * @file system_service.c
 * @brief C5 终端 register/heartbeat/status/command 后台服务。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），由 app_orchestrator_start()
 * 启动，负责初始化 display placeholder 和 system_server_client，并创建后台任务
 * 周期性 register/heartbeat/status/commands。它不执行 WiFi 连接、不读取 BME、不处理
 * voice PCM，也不改变 S3 下发命令的协议字段。
 */

#include "system_service.h"

#include "app_runtime.h"
#include "app_stack_monitor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_link.h"
#include "screen_service.h"
#include "server_comm_config.h"
#include "system_server_client.h"

static const char *TAG = "system_service";

static TaskHandle_t s_command_task;

static bool system_service_should_log_periodic(uint32_t *counter)
{
    if (counter == NULL) {
        return true;
    }

    *counter += 1U;
    return *counter == 1U || (*counter % 12U) == 0U;
}

static void system_service_command_task(void *arg)
{
    (void)arg;
    uint32_t poll_error_count = 0;
    uint32_t heartbeat_error_count = 0;
    uint32_t status_error_count = 0;
    bool first_success_logged = false;
    bool low_water_logged = false;
    TickType_t last_heartbeat_tick = 0;
    TickType_t last_status_tick = 0;

    app_stack_monitor_log(TAG, "system_cmd_poll", "entry");

    while (1) {
        TickType_t now = xTaskGetTickCount();
        const char *device_id = server_comm_get_device_id();

        if (app_runtime_should_skip_non_voice_task("system heartbeat/status/command poll") ||
            !gateway_link_can_run_non_voice_task("system heartbeat/status/command poll")) {
            vTaskDelay(pdMS_TO_TICKS(SYSTEM_SERVICE_COMMAND_POLL_INTERVAL_MS));
            continue;
        }

        if (last_heartbeat_tick == 0 ||
            now - last_heartbeat_tick >= pdMS_TO_TICKS(SYSTEM_SERVICE_HEARTBEAT_INTERVAL_MS)) {
            esp_err_t heartbeat_ret = system_server_client_send_heartbeat(device_id);
            if (heartbeat_ret == ESP_OK) {
                heartbeat_error_count = 0;
                last_heartbeat_tick = now;
            } else if (system_service_should_log_periodic(&heartbeat_error_count)) {
                ESP_LOGW(TAG, "heartbeat failed: %s", esp_err_to_name(heartbeat_ret));
            }
        }

        if (last_status_tick == 0 ||
            now - last_status_tick >= pdMS_TO_TICKS(SYSTEM_SERVICE_STATUS_INTERVAL_MS)) {
            esp_err_t status_ret = system_server_client_send_status(device_id);
            if (status_ret == ESP_OK) {
                status_error_count = 0;
                last_status_tick = now;
            } else if (system_service_should_log_periodic(&status_error_count)) {
                ESP_LOGW(TAG, "status upload failed: %s", esp_err_to_name(status_ret));
            }
        }

        esp_err_t ret = system_server_client_poll_commands(server_comm_get_device_id());
        if (ret == ESP_OK || ret == ESP_ERR_NOT_FOUND) {
            poll_error_count = 0;
            if (!first_success_logged) {
                first_success_logged = true;
                app_stack_monitor_log(TAG,
                                      "system_cmd_poll",
                                      ret == ESP_OK ? "first_success" : "first_no_command");
            }
        } else if (system_service_should_log_periodic(&poll_error_count)) {
            ESP_LOGW(TAG, "command poll failed: %s", esp_err_to_name(ret));
            app_stack_monitor_log(TAG, "system_cmd_poll", "poll_error");
        }

        UBaseType_t high_water = app_stack_monitor_high_water();
        if (!low_water_logged &&
            high_water > 0 &&
            high_water < APP_STACK_LOW_WATER_WARNING_BYTES) {
            low_water_logged = true;
            app_stack_monitor_log(TAG, "system_cmd_poll", "low_water");
        }
        vTaskDelay(pdMS_TO_TICKS(SYSTEM_SERVICE_COMMAND_POLL_INTERVAL_MS));
    }
}

esp_err_t system_service_init(void)
{
    esp_err_t screen_ret = screen_service_init();
    if (screen_ret != ESP_OK) {
        ESP_LOGW(TAG, "screen service init failed: %s", esp_err_to_name(screen_ret));
    }

    esp_err_t ret = system_server_client_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "system server client init failed: %s", esp_err_to_name(ret));
    }

    if (s_command_task == NULL) {
        BaseType_t created = xTaskCreate(system_service_command_task,
                                         "system_cmd_poll",
                                         SYSTEM_SERVICE_COMMAND_TASK_STACK,
                                         NULL,
                                         SYSTEM_SERVICE_COMMAND_TASK_PRIORITY,
                                         &s_command_task);
        if (created != pdPASS) {
            s_command_task = NULL;
            ESP_LOGE(TAG, "create command polling task failed");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG,
                 "system command polling task started interval_ms=%u",
                 (unsigned int)SYSTEM_SERVICE_COMMAND_POLL_INTERVAL_MS);
    }

    return ESP_OK;
}

esp_err_t system_service_tick(void)
{
    if (app_runtime_should_skip_non_voice_task("system tick")) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!gateway_link_can_run_non_voice_task("system tick")) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *device_id = server_comm_get_device_id();
    (void)system_server_client_send_heartbeat(device_id);
    return system_server_client_poll_commands(device_id);
}

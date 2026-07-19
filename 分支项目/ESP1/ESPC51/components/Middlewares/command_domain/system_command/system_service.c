/**
 * @file system_service.c
 * @brief C5 终端 register/heartbeat/status/command 后台服务。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），由 app_orchestrator_start()
 * 启动，负责初始化 display placeholder 和 system_server_client，并向 C5 system
 * worker 暴露 heartbeat/status/commands tick。它不执行 WiFi 连接、不读取 BME、不处理
 * voice PCM，也不改变 S3 下发命令的协议字段。
 */

#include "system_service.h"

#include <stdbool.h>

#include "app_stack_monitor.h"
#include "c5_backpressure_controller.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "screen_service.h"
#include "server_comm_config.h"
#include "system_server_client.h"

static const char *TAG = "system_service";

typedef struct {
    uint32_t poll_error_count;
    uint32_t heartbeat_error_count;
    uint32_t status_error_count;
    bool first_success_logged;
    bool low_water_logged;
} system_service_context_t;

static system_service_context_t s_system_service;

static bool system_service_should_log_periodic(uint32_t *counter)
{
    if (counter == NULL) {
        return true;
    }

    *counter += 1U;
    return *counter == 1U || (*counter % 12U) == 0U;
}

esp_err_t system_service_tick_heartbeat(void)
{
    if (!c5_should_run(C5_TASK_TYPE_SYSTEM_HEARTBEAT)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = system_server_client_send_heartbeat(server_comm_get_device_id());
    if (ret == ESP_OK) {
        s_system_service.heartbeat_error_count = 0;
    } else if (system_service_should_log_periodic(&s_system_service.heartbeat_error_count)) {
        ESP_LOGW(TAG, "heartbeat failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t system_service_tick_status(void)
{
    if (!c5_should_run(C5_TASK_TYPE_SYSTEM_STATUS)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = system_server_client_send_status(server_comm_get_device_id());
    if (ret == ESP_OK) {
        s_system_service.status_error_count = 0;
    } else if (system_service_should_log_periodic(&s_system_service.status_error_count)) {
        ESP_LOGW(TAG, "status upload failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t system_service_tick_command_poll(void)
{
    if (!c5_should_run(C5_TASK_TYPE_SYSTEM_COMMAND_POLL)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = system_server_client_poll_commands(server_comm_get_device_id());
    if (ret == ESP_OK || ret == ESP_ERR_NOT_FOUND) {
        s_system_service.poll_error_count = 0;
        if (!s_system_service.first_success_logged) {
            s_system_service.first_success_logged = true;
            app_stack_monitor_log(TAG,
                                  "system_cmd_poll",
                                  ret == ESP_OK ? "first_success" : "first_no_command");
        }
    } else if (system_service_should_log_periodic(&s_system_service.poll_error_count)) {
        ESP_LOGW(TAG, "command poll failed: %s", esp_err_to_name(ret));
        app_stack_monitor_log(TAG, "system_cmd_poll", "poll_error");
    }

    UBaseType_t high_water = app_stack_monitor_high_water();
    if (!s_system_service.low_water_logged &&
        high_water > 0 &&
        high_water < APP_STACK_LOW_WATER_WARNING_BYTES) {
        s_system_service.low_water_logged = true;
        app_stack_monitor_log(TAG, "system_cmd_poll", "low_water");
    }
    return ret;
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

    ESP_LOGI(TAG,
             "system service registered with C5 event worker heartbeat_ms=%u status_ms=%u command_poll_ms=%u",
             (unsigned int)SYSTEM_SERVICE_HEARTBEAT_INTERVAL_MS,
             (unsigned int)SYSTEM_SERVICE_STATUS_INTERVAL_MS,
             (unsigned int)SYSTEM_SERVICE_COMMAND_POLL_INTERVAL_MS);

    return ESP_OK;
}

esp_err_t system_service_tick(void)
{
    esp_err_t heartbeat_ret = system_service_tick_heartbeat();
    esp_err_t command_ret = system_service_tick_command_poll();
    return command_ret != ESP_ERR_INVALID_STATE ? command_ret : heartbeat_ret;
}

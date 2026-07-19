#include "system_service.h"

#include "app_stack_monitor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
    bool first_success_logged = false;
    bool low_water_logged = false;

    app_stack_monitor_log(TAG, "system_cmd_poll", "entry");

    while (1) {
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
    return system_server_client_poll_commands(server_comm_get_device_id());
}

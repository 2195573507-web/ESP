/**
 * @file main.c
 * @brief ESP32-C5 终端固件入口。
 *
 * 本文件属于 C5 终端（ESPC51/ESPC52 共用），只负责 ESP-IDF app_main 入口、
 * 日志等级初始化和启动任务创建；不直接初始化 WiFi、BME690、语音、命令轮询或
 * 与 S3/Server 的协议细节。启动任务进入 app_orchestrator_start() 后，才按
 * C5 app_main -> app_orchestrator_start -> LCD -> local sensors -> local runtime
 * -> WiFi/Gateway -> register/heartbeat/command -> network voice 的顺序交给各业务模块。
 */

#include "app_debug_config.h"
#include "app_main_config.h"
#include "app_orchestrator.h"
#include "app_stack_monitor.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

static const char *TAG = "APP_ENTRY";

static TaskHandle_t s_app_startup_task;

static esp_err_t app_startup_init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG,
                 "NVS_INIT recovery required ret=%s; erasing default NVS partition",
                 esp_err_to_name(ret));
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "NVS_INIT erase failed ret=%s", esp_err_to_name(ret));
            return ret;
        }
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS_INIT failed ret=%s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "NVS_INIT ready before orchestrator startup");
    return ESP_OK;
}

static void app_startup_task(void *arg)
{
    (void)arg;

    app_stack_monitor_log(TAG, "app_startup_task", "entry");
    const esp_err_t nvs_ret = app_startup_init_nvs();
    if (nvs_ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "BOOT_STAGE stage=nvs state=failed ret=%s; orchestrator startup blocked",
                 esp_err_to_name(nvs_ret));
        TaskHandle_t task = xTaskGetCurrentTaskHandle();
        s_app_startup_task = NULL;
        if (task != NULL) {
            vTaskDeleteWithCaps(task);
        }
        return;
    }
    /*
     * C5 启动主链路从这里交给 orchestrator：
     * LCD 完成后先启动 BME/Radar 与 Mic/ADC/VAD，再创建 EventBus/Queue/Dispatcher；
     * WiFi/Gateway/system service 与 PCM 网络语音留在后续阶段，断网时进入离线模式。
     */
    app_stack_monitor_log_system_state(TAG, "before_app_orchestrator_start");
    app_orchestrator_start();
    app_stack_monitor_log(TAG, "app_startup_task", "orchestrator_returned");

    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    s_app_startup_task = NULL;
    if (task != NULL) {
        vTaskDeleteWithCaps(task);
    }
}

/**
 * @brief C5 终端应用入口函数。
 *
 * 调用位置：ESP-IDF 启动流程自动调用，应用代码不要手动调用。
 * 调用时机：芯片启动、系统组件初始化完成后进入。
 * 输入参数：无。
 * 返回值：无；本函数创建 app_startup 任务后返回给 ESP-IDF。
 * 失败处理：启动任务创建失败时记录明确的资源错误并返回，不触发重启循环；WiFi/BME/
 * voice/command 等后续失败由 app_orchestrator_start() 及各模块记录和处理。
 */
void app_main(void)
{
    app_debug_apply_log_levels();
    app_stack_monitor_log(TAG, "app_main", "entry");

    ESP_LOGI(TAG,
             "MEM_ALLOC_PLAN owner=app_startup_task caps=0x%08lx size=%u region=internal_control",
             (unsigned long)(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)APP_STARTUP_TASK_STACK);
    const BaseType_t created = xTaskCreateWithCaps(app_startup_task,
                                                   "app_startup",
                                                   APP_STARTUP_TASK_STACK,
                                                   NULL,
                                                   APP_STARTUP_TASK_PRIORITY,
                                                   &s_app_startup_task,
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    app_stack_monitor_log(TAG, "app_main", "after_startup_task_create");
    if (created != pdPASS || s_app_startup_task == NULL) {
        s_app_startup_task = NULL;
        ESP_LOGE(TAG,
                 "create app_startup task failed stack=%u priority=%u",
                 (unsigned int)APP_STARTUP_TASK_STACK,
                 (unsigned int)APP_STARTUP_TASK_PRIORITY);
        ESP_LOGE(TAG, "app_startup disabled ret=%s", esp_err_to_name(ESP_ERR_NO_MEM));
        return;
    }

    ESP_LOGI(TAG, "TASK_CREATE task=app_startup stack=%u source=internal_dynamic_reclaimable",
             (unsigned int)APP_STARTUP_TASK_STACK);
    app_stack_monitor_log(TAG, "app_main", "return");
}

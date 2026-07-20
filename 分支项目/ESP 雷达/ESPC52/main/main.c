/**
 * @file main.c
 * @brief ESP32-C5 终端固件入口。
 *
 * 本文件属于 C5 终端（ESPC51/ESPC52 共用），只负责 ESP-IDF app_main 入口、
 * 日志等级初始化和启动任务创建；不直接初始化 WiFi、BME690、语音、命令轮询或
 * 与 S3/Server 的协议细节。启动任务进入 app_orchestrator_start() 后，才按
 * C5 app_main -> app_orchestrator_start -> WiFi -> register -> BME -> voice -> command
 * 的顺序交给各业务模块。
 */

#include "app_debug_config.h"
#include "app_main_config.h"
#include "app_orchestrator.h"
#include "app_stack_monitor.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "APP_ENTRY";
static TaskHandle_t s_app_startup_task;

static void app_startup_task(void *arg)
{
    (void)arg;

    app_stack_monitor_log(TAG, "app_startup_task", "entry");
    /*
     * C5 启动主链路从这里交给 orchestrator：
     * WiFi 连接 S3 SoftAP 后，system_service 完成 register/heartbeat/status/command，
     * BME 后台服务开始读数上传，voice_chain 最后启动 Mic/VAD/voice turn。
     */
    app_orchestrator_start();
    app_stack_monitor_log(TAG, "app_startup_task", "orchestrator_returned");

    s_app_startup_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief C5 终端应用入口函数。
 *
 * 调用位置：ESP-IDF 启动流程自动调用，应用代码不要手动调用。
 * 调用时机：芯片启动、系统组件初始化完成后进入。
 * 输入参数：无。
 * 返回值：无；本函数创建 app_startup 任务后返回给 ESP-IDF。
 * 失败处理：启动任务创建失败时通过 ESP_ERROR_CHECK(ESP_ERR_NO_MEM) 进入
 * ESP-IDF 错误处理；WiFi/BME/voice/command 等后续失败由 app_orchestrator_start()
 * 及各模块记录和处理。
 */
void app_main(void)
{
    app_debug_apply_log_levels();
    app_stack_monitor_log(TAG, "app_main", "entry");

    BaseType_t created = xTaskCreate(app_startup_task,
                                     "app_startup",
                                     APP_STARTUP_TASK_STACK,
                                     NULL,
                                     APP_STARTUP_TASK_PRIORITY,
                                     &s_app_startup_task);
    app_stack_monitor_log(TAG, "app_main", "after_startup_task_create");
    if (created != pdPASS) {
        s_app_startup_task = NULL;
        ESP_LOGE(TAG,
                 "create app_startup task failed stack=%u priority=%u",
                 (unsigned int)APP_STARTUP_TASK_STACK,
                 (unsigned int)APP_STARTUP_TASK_PRIORITY);
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    app_stack_monitor_log(TAG, "app_main", "return");
}

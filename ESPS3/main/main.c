/**
 * @file main.c
 * @brief ESP32-S3 本地网关固件入口。
 *
 * 本文件属于 S3 网关（ESPS3），只负责 ESP-IDF app_main 入口、日志等级初始化和
 * gateway_startup 任务创建；不直接启动 SoftAP、HTTP handler、子设备注册表或 Server
 * 转发。启动任务进入 gateway_orchestrator_start() 后，才按
 * S3 app_main -> gateway_orchestrator_start -> SoftAP -> local_http_server ->
 * child_registry -> server_client 的边界运行。
 */

#include "app_debug_config.h"
#include "app_main_config.h"
#include "app_stack_monitor.h"
#include "gateway_orchestrator.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

static const char *TAG = "S3_ENTRY";
static TaskHandle_t s_gateway_task;

static void gateway_startup_task(void *arg)
{
    (void)arg;

    app_stack_monitor_report(TAG,
                             "gateway_startup_task",
                             APP_STARTUP_TASK_STACK,
                             "entry");
    app_stack_monitor_log(TAG, "gateway_startup_task", "entry");
    /*
     * S3 启动主链路从这里交给 gateway_orchestrator：
     * 先初始化 registry/router/adapter/proxy 等本地状态，再启动 SoftAP，最后开放
     * /local/v1 HTTP。C5<->S3 的轻量协议在 local_http_server/protocol_adapter 内结束，
     * S3<->Server 的完整协议由 server_client 负责。
     */
    gateway_orchestrator_start();
    app_stack_monitor_log(TAG, "gateway_startup_task", "orchestrator_returned");

    s_gateway_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief S3 网关应用入口函数。
 *
 * 调用位置：ESP-IDF 启动流程自动调用。
 * 调用时机：芯片启动、系统组件初始化完成后进入。
 * 输入参数：无。
 * 返回值：无；创建 gateway_startup 任务后返回给 ESP-IDF。
 * 失败处理：任务创建失败只记录并返回，绝不因单个可选模块资源不足触发重启；
 * 后续 SoftAP/HTTP/Server 转发失败由 gateway_orchestrator 或各模块记录。
 */
void app_main(void)
{
    app_debug_apply_log_levels();
    app_stack_monitor_log(TAG, "app_main", "entry");

    BaseType_t created = xTaskCreateWithCaps(gateway_startup_task,
                                             "gateway_startup",
                                             APP_STARTUP_TASK_STACK,
                                             NULL,
                                             APP_STARTUP_TASK_PRIORITY,
                                             &s_gateway_task,
                                             /* Flash/NVS cache-off paths require an internal-RAM stack. */
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    app_stack_monitor_log(TAG, "app_main", "after_gateway_task_create");
    if (created != pdPASS) {
        s_gateway_task = NULL;
        ESP_LOGE(TAG,
                 "create gateway startup task failed stack=%u priority=%u",
                 (unsigned int)APP_STARTUP_TASK_STACK,
                 (unsigned int)APP_STARTUP_TASK_PRIORITY);
        ESP_LOGE(TAG, "gateway startup disabled ret=%s", esp_err_to_name(ESP_ERR_NO_MEM));
        return;
    }

    app_stack_monitor_log(TAG, "app_main", "return");
}

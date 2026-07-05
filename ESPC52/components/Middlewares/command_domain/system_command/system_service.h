#ifndef SYSTEM_SERVICE_H
#define SYSTEM_SERVICE_H

/**
 * @file system_service.h
 * @brief C5 终端系统类后台服务接口。
 *
 * system_service 负责 C5 向 S3 注册、心跳、状态上报和命令轮询；display 命令只进入
 * display_placeholder 验证上层接口，不接真实 LCD 底层。
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SYSTEM_SERVICE_COMMAND_TASK_STACK
#define SYSTEM_SERVICE_COMMAND_TASK_STACK 12288U // 命令轮询任务栈，单位字节；HTTP/JSON/日志链路较深。
#endif

#ifndef SYSTEM_SERVICE_COMMAND_TASK_PRIORITY
#define SYSTEM_SERVICE_COMMAND_TASK_PRIORITY 2U // 低优先级后台任务，不抢语音链路。
#endif

#ifndef SYSTEM_SERVICE_COMMAND_POLL_INTERVAL_MS
#define SYSTEM_SERVICE_COMMAND_POLL_INTERVAL_MS 5000U // 待执行命令轮询间隔。
#endif

#ifndef SYSTEM_SERVICE_HEARTBEAT_INTERVAL_MS
#define SYSTEM_SERVICE_HEARTBEAT_INTERVAL_MS 5000U
#endif

#ifndef SYSTEM_SERVICE_STATUS_INTERVAL_MS
#define SYSTEM_SERVICE_STATUS_INTERVAL_MS 15000U
#endif

/**
 * @brief 启动系统后台服务。
 *
 * 调用位置：app_orchestrator_start()。
 * 调用时机：WiFi 连接并稳定后、BME/voice 启动前。
 * 输入参数：无。
 * @return ESP_OK 表示初始化完成或任务已存在；任务创建失败返回 ESP_ERR_NO_MEM。
 * 失败处理：orchestrator 记录警告后继续尝试后续 BME/voice 服务，后台命令链路稍后由重启恢复。
 */
esp_err_t system_service_init(void);

/**
 * @brief 执行一次轻量 heartbeat 与 command poll。
 *
 * 调用位置：当前常驻后台任务之外的预留/调试入口。
 * 调用时机：需要手动触发一次系统 tick 时。
 * 输入参数：无。
 * @return command poll 的返回值；heartbeat 失败仅被忽略。
 * 失败处理：调用方按返回值记录日志或稍后重试。
 */
esp_err_t system_service_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_SERVICE_H */

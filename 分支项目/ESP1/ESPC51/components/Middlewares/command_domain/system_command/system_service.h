#ifndef SYSTEM_SERVICE_H
#define SYSTEM_SERVICE_H

/**
 * @file system_service.h
 * @brief C5 终端系统类后台服务接口。
 *
 * system_service 负责 C5 向 S3 注册、心跳、状态上报和命令轮询；周期事件由
 * C5 system worker 执行。display 命令只进入 display_placeholder 验证上层接口，
 * 不接真实 LCD 底层。
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
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
 * @brief 初始化系统后台服务，并注册为 event worker 驱动。
 *
 * 调用位置：app_orchestrator_start()。
 * 调用时机：WiFi 连接并稳定后、BME/voice 启动前。
 * 输入参数：无。
 * @return ESP_OK 表示初始化完成。
 * 失败处理：orchestrator 记录警告后继续尝试后续 BME/voice 服务，后台命令链路稍后由重启恢复。
 */
esp_err_t system_service_init(void);

/** @brief System worker 调用：执行一次 heartbeat。 */
esp_err_t system_service_tick_heartbeat(void);

/** @brief System worker 调用：执行一次 status upload。 */
esp_err_t system_service_tick_status(void);

/** @brief System worker 调用：执行一次 command poll。 */
esp_err_t system_service_tick_command_poll(void);

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

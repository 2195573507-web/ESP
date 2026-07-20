#ifndef SYSTEM_SERVICE_H
#define SYSTEM_SERVICE_H

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

/** 调用方法：app_main 初始化系统类后台服务时调用一次；当前仅预留接口。 */
esp_err_t system_service_init(void);

/** 调用方法：未来主循环或定时器周期调用；当前用于预留 heartbeat/命令轮询入口。 */
esp_err_t system_service_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_SERVICE_H */

#ifndef C5_RUNTIME_WORKERS_H
#define C5_RUNTIME_WORKERS_H

#include <stdint.h>

/**
 * @file c5_runtime_workers.h
 * @brief C5 runtime worker 任务启动入口。
 *
 * dispatcher 只负责事件路由，真正的 BME/system business ticks run in these workers.
 * 任务内执行。这样可以保持 dispatcher 优先级高、执行时间短。
 */

#include "esp_err.h"

#include "app_main_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef C5_WORKER_TASK_STACK
#define C5_WORKER_TASK_STACK 8192U
#endif

#ifndef C5_WORKER_TASK_PRIORITY
#define C5_WORKER_TASK_PRIORITY 3U
#endif

#ifndef C5_WORKER_QUEUE_LENGTH
#define C5_WORKER_QUEUE_LENGTH 6U
#endif

/** @brief Create queues, register event handlers, and start BME/system workers. */
esp_err_t c5_runtime_workers_start(void);

/** @brief Stop new worker dispatch, clear queued work, and wait for current work to finish. */
esp_err_t c5_runtime_workers_quiesce(uint32_t timeout_ms);

/** @brief Reopen worker dispatch after the voice lease has released audio resources. */
void c5_runtime_workers_resume(void);

#ifdef __cplusplus
}
#endif

#endif /* C5_RUNTIME_WORKERS_H */

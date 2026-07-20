#ifndef C5_BACKPRESSURE_CONTROLLER_H
#define C5_BACKPRESSURE_CONTROLLER_H

/**
 * @file c5_backpressure_controller.h
 * @brief C5 终端回压状态和自适应调度入口。
 *
 * 周期性 C5 工作按 task type 分类。本模块统一决定 timer 何时产生事件、下一次
 * 间隔如何放大；gateway_link 和 voice_chain 只作为只读输入。业务 payload 在
 * worker 内处理，本模块不让 C5 直接访问 Server，所有上云路径仍只到 ESPS3
 * local gateway。
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gateway_link.h"
#include "voice_chain.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef C5_SCHEDULER_TASK_STACK
#define C5_SCHEDULER_TASK_STACK 12288U
#endif

#ifndef C5_SCHEDULER_TASK_PRIORITY
#define C5_SCHEDULER_TASK_PRIORITY 4U
#endif

#ifndef C5_SCHEDULER_MIN_SLEEP_MS
#define C5_SCHEDULER_MIN_SLEEP_MS 20U
#endif

#ifndef C5_SCHEDULER_MAX_SLEEP_MS
#define C5_SCHEDULER_MAX_SLEEP_MS 1000U
#endif

#ifndef C5_EVENT_DISPATCHER_DIAGNOSTIC_INTERVAL_MS
#define C5_EVENT_DISPATCHER_DIAGNOSTIC_INTERVAL_MS 10000U
#endif

#ifndef C5_BACKPRESSURE_VOICE_BACKOFF_MS
#define C5_BACKPRESSURE_VOICE_BACKOFF_MS 1000U
#endif

typedef enum {
    C5_TASK_TYPE_VOICE_HIGH = 0,
    C5_TASK_TYPE_NORMAL,
    C5_TASK_TYPE_LOW,
    C5_TASK_TYPE_SYSTEM_HEARTBEAT,
    C5_TASK_TYPE_SYSTEM_STATUS,
    C5_TASK_TYPE_SYSTEM_COMMAND_POLL,
    C5_TASK_TYPE_BME_SENSOR,
} c5_task_type_t;

typedef enum {
    C5_TASK_PRIORITY_HIGH = 0,
    C5_TASK_PRIORITY_NORMAL,
    C5_TASK_PRIORITY_LOW,
} c5_task_priority_t;

typedef struct {
    uint8_t queue_load;
    gateway_link_state_t gateway_state;
    voice_chain_state_t voice_state;
    bool voice_active;
    uint8_t cpu_idle_estimate;
} c5_backpressure_state_t;

/** @brief 从 gateway_link/voice_chain 刷新回压输入状态；调度 tick 每轮调用。 */
void c5_backpressure_refresh(void);

/** @brief 更新本地待运行任务比例，取值会被限制在 0..100。 */
void c5_backpressure_set_queue_load(uint8_t queue_load);

/** @brief 更新调度器估算的空闲比例，取值会被限制在 0..100。 */
void c5_backpressure_set_cpu_idle_estimate(uint8_t cpu_idle_estimate);

/** @brief 读取当前回压快照；调用方拿到的是结构体副本。 */
c5_backpressure_state_t c5_backpressure_get_state(void);

/** @brief 判断指定周期任务本轮是否允许执行；语音独占、S3 未 ready 或低空闲时会跳过。 */
bool c5_should_run(c5_task_type_t task_type);

/** @brief 获取指定任务的下一轮调度间隔；高负载/低空闲时自动放大。 */
uint32_t c5_get_interval(c5_task_type_t task_type);

/** @brief 启动 event bus、handlers 和 dispatcher（启动阶段 1）；可重复调用。 */
esp_err_t c5_scheduler_start(void);

/** @brief 创建 BME/system workers（启动阶段 2）；必须在 startup task 释放后调用。 */
esp_err_t c5_scheduler_start_deferred_workers(void);

/** @brief 执行一次 timer 扫描并产生事件；测试或手动 tick 可直接调用。 */
void c5_scheduler_tick(void);

/** @brief 将 task type 转成稳定日志字符串。 */
const char *c5_task_type_name(c5_task_type_t task_type);

#ifdef __cplusplus
}
#endif

#endif /* C5_BACKPRESSURE_CONTROLLER_H */

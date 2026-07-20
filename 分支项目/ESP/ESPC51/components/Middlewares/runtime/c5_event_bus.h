#ifndef C5_EVENT_BUS_H
#define C5_EVENT_BUS_H

/**
 * @file c5_event_bus.h
 * @brief C5 运行时事件总线。
 *
 * timer 和轻量 callback 只把 runtime event 投递到这里。dispatcher 负责从队列
 * 取出事件并转发给 worker；业务函数只在 worker 内执行，不放在 scheduler 或
 * dispatcher 路径里，避免定时扫描线程被长业务阻塞。
 */

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef C5_EVENT_BUS_QUEUE_LENGTH
#define C5_EVENT_BUS_QUEUE_LENGTH 24U
#endif

typedef enum {
    C5_LIFECYCLE_STOPPED = 0,
    C5_LIFECYCLE_STARTING,
    C5_LIFECYCLE_RUNNING,
    C5_LIFECYCLE_STOPPING,
    C5_LIFECYCLE_FAULT,
} c5_lifecycle_state_t;

typedef enum {
    C5_EVENT_CSI_READY = 0,
    C5_EVENT_BME_SAMPLE,
    C5_EVENT_HEARTBEAT,
    C5_EVENT_STATUS,
    C5_EVENT_COMMAND,
    C5_EVENT_MAX,
} c5_event_type_t;

/** @brief 事件来源只用于诊断和日志，不改变事件处理语义。 */
typedef enum {
    C5_EVENT_SOURCE_TIMER = 0,
    C5_EVENT_SOURCE_CALLBACK,
    C5_EVENT_SOURCE_INTERNAL,
} c5_event_source_t;

/** @brief C5 runtime 事件快照；入队时固定时间戳和序号，worker 用它计算延迟。 */
typedef struct {
    c5_event_type_t type;
    c5_event_source_t source;
    uint64_t timestamp_ms;
    uint32_t sequence;
} c5_event_t;

typedef esp_err_t (*c5_event_dispatch_handler_t)(const c5_event_t *event, void *ctx);

/** @brief event bus 运行状态；诊断日志读取副本，不持有内部锁。 */
typedef struct {
    uint32_t event_queue_depth;
    uint32_t drop_count;
    uint32_t enqueue_count;
    uint32_t dispatch_count;
    uint32_t last_worker_latency_ms;
    uint32_t max_worker_latency_ms;
} c5_event_bus_stats_t;

/**
 * @brief 初始化事件队列；可重复调用，已有队列时直接返回 ESP_OK。
 *
 * init/deinit must be called serially by one control thread. Concurrent
 * init/deinit safety is not provided.
 */
esp_err_t c5_event_bus_init(void);

/** @brief 释放事件队列并清空 handler、统计与事件序号。调用方必须先停止 dispatcher。 */
esp_err_t c5_event_bus_deinit(void);

/** @brief 获取事件总线生命周期状态；状态查询不授予资源访问权限。 */
c5_lifecycle_state_t c5_event_bus_get_state(void);

/** @brief 注册某类事件的 dispatcher handler；handler 内只做快速路由。 */
esp_err_t c5_event_bus_register_handler(c5_event_type_t type,
                                         c5_event_dispatch_handler_t handler,
                                         void *ctx);

/** @brief 从 timer/callback 路径入队一个事件；队列满时记录 drop 并返回 ESP_ERR_TIMEOUT。 */
esp_err_t c5_event_bus_enqueue(c5_event_type_t type, c5_event_source_t source);

/** @brief dispatcher 执行一次出队和路由；timeout_ms 控制等待事件的最长时间。 */
esp_err_t c5_event_bus_dispatch(uint32_t timeout_ms);

/** @brief 记录一次丢弃；worker 队列满或 handler 缺失时也复用该计数。 */
void c5_event_bus_note_drop(void);

/** @brief 记录 worker 从事件入队到开始处理之间的延迟。 */
void c5_event_bus_record_worker_latency(uint32_t latency_ms);

/** @brief 返回当前 event bus 主队列深度。 */
uint32_t c5_event_bus_queue_depth(void);

/** @brief 读取 event bus 诊断快照。 */
void c5_event_bus_get_stats(c5_event_bus_stats_t *out_stats);

/** @brief 将事件类型转成稳定日志字符串。 */
const char *c5_event_type_name(c5_event_type_t type);

/** @brief 将事件来源转成稳定日志字符串。 */
const char *c5_event_source_name(c5_event_source_t source);

#ifdef __cplusplus
}
#endif

#endif /* C5_EVENT_BUS_H */

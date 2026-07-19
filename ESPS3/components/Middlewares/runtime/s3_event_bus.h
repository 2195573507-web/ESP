#ifndef S3_EVENT_BUS_H
#define S3_EVENT_BUS_H

/**
 * @file s3_event_bus.h
 * @brief ESPS3 运行时优先级事件总线。
 *
 * 本模块集中承载 S3 runtime 回压策略：
 * CRITICAL/REALTIME 事件进入 FIFO 队列，高压时由 scheduler 调用方重试；
 * STATE 事件按 key 合并，只保留最新状态；BACKGROUND 事件在高压时允许丢弃。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define S3_EVENT_BUS_DEVICE_ID_LEN 48U

typedef enum {
    S3_EVENT_BUS_LEVEL_CRITICAL = 0,
    S3_EVENT_BUS_LEVEL_REALTIME,
    S3_EVENT_BUS_LEVEL_STATE,
    S3_EVENT_BUS_LEVEL_BACKGROUND,
    S3_EVENT_BUS_LEVEL_COUNT,
} s3_event_bus_level_t;

/** @brief STATE 层事件的合并 key；同 key 的旧事件会被最新事件替换。 */
typedef enum {
    S3_EVENT_BUS_STATE_NONE = 0,
    S3_EVENT_BUS_STATE_BME_LATEST_C51,
    S3_EVENT_BUS_STATE_BME_LATEST_C52,
    S3_EVENT_BUS_STATE_DEVICE_STATUS_C51,
    S3_EVENT_BUS_STATE_DEVICE_STATUS_C52,
    S3_EVENT_BUS_STATE_COUNT,
} s3_event_bus_state_key_t;

/** @brief event bus 诊断快照；队列深度和 drop/coalesce 计数用于判断回压状态。 */
typedef struct {
    size_t queue_depth;
    size_t critical_depth;
    size_t realtime_depth;
    size_t state_depth;
    size_t background_depth;
    uint32_t drop_count;
    uint32_t background_drop_count;
    uint32_t coalesce_count;
} s3_event_bus_stats_t;

struct s3_scheduler_event;
typedef void (*s3_event_bus_release_fn_t)(struct s3_scheduler_event *event);

/** @brief 初始化锁、信号量和事件释放回调；scheduler init 阶段调用。 */
esp_err_t s3_event_bus_init(s3_event_bus_release_fn_t release_fn);

/** @brief 清空所有队列和 STATE 槽位；测试或 runtime reset 时调用。 */
void s3_event_bus_reset(void);

/**
 * @brief 推入一个已转移所有权的事件。
 *
 * 只要 bus 接收了事件所有权就返回 ESP_OK。BACKGROUND 被策略丢弃、STATE 被合并
 * 也返回 ESP_OK，因为事件已经被策略消费。非 ESP_OK 表示调用方仍持有事件，必须
 * 自己释放。
 */
esp_err_t s3_event_bus_push_owned(struct s3_scheduler_event *event);

/**
 * @brief Push one owned event while bounding only the event-bus mutex wait.
 *
 * A non-OK result leaves ownership with the caller. This is for local HTTP
 * admission paths that must fail promptly; normal scheduler callers retain
 * the existing unbounded/reliable behavior through s3_event_bus_push_owned().
 */
esp_err_t s3_event_bus_push_owned_timed(struct s3_scheduler_event *event,
                                        uint32_t lock_timeout_ms,
                                        uint32_t *out_lock_wait_ms,
                                        s3_event_bus_stats_t *out_stats);

/** @brief 等待 event bus 有可消费事件；timeout_ms 为最长等待时间。 */
bool s3_event_bus_wait(uint32_t timeout_ms);

/** @brief 按 CRITICAL > REALTIME > STATE > BACKGROUND 顺序取出一个事件。 */
bool s3_event_bus_dequeue(struct s3_scheduler_event **out_event);

/** @brief 读取 event bus 当前诊断快照。 */
s3_event_bus_stats_t s3_event_bus_get_stats(void);

/** @brief 输出一次队列深度/drop/coalesce 诊断日志。 */
void s3_event_bus_log_stats(const char *reason);

/** @brief 将优先级层级转为稳定日志字符串。 */
const char *s3_event_bus_level_name(s3_event_bus_level_t level);

/** @brief 将 STATE 合并 key 转为稳定日志字符串。 */
const char *s3_event_bus_state_key_name(s3_event_bus_state_key_t key);

#ifdef __cplusplus
}
#endif

#endif /* S3_EVENT_BUS_H */

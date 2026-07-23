/**
 * @file s3_event_bus.c
 * @brief ESPS3 优先级事件总线实现。
 *
 * s3_scheduler 负责把入口事件标记为 CRITICAL/REALTIME/STATE/BACKGROUND，
 * 本文件只执行队列策略和所有权释放策略，不解析 JSON、不访问 ESP-server。
 */

#include "s3_event_bus.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "s3_scheduler.h"

static const char *TAG = "S3_EVENT_BUS";

#ifndef S3_EVENT_BUS_CRITICAL_DEPTH
#define S3_EVENT_BUS_CRITICAL_DEPTH 12U
#endif

#ifndef S3_EVENT_BUS_REALTIME_DEPTH
#define S3_EVENT_BUS_REALTIME_DEPTH 12U
#endif

#ifndef S3_EVENT_BUS_BACKGROUND_DEPTH
#define S3_EVENT_BUS_BACKGROUND_DEPTH 6U
#endif

typedef struct {
    s3_scheduler_event_t *items[S3_EVENT_BUS_CRITICAL_DEPTH];
    size_t count;
} s3_event_bus_critical_queue_t;

typedef struct {
    s3_scheduler_event_t *items[S3_EVENT_BUS_REALTIME_DEPTH];
    size_t count;
} s3_event_bus_realtime_queue_t;

typedef struct {
    s3_scheduler_event_t *items[S3_EVENT_BUS_BACKGROUND_DEPTH];
    size_t count;
} s3_event_bus_background_queue_t;

static s3_event_bus_critical_queue_t s_critical_queue;
static s3_event_bus_realtime_queue_t s_realtime_queue;
static s3_event_bus_background_queue_t s_background_queue;
static s3_scheduler_event_t *s_state_slots[S3_EVENT_BUS_STATE_COUNT];
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_signal;
static s3_event_bus_release_fn_t s_release_fn;
static uint32_t s_drop_count;
static uint32_t s_background_drop_count;
static uint32_t s_coalesce_count;
static uint32_t s_drop_by_event_type[S3_EVENT_BUS_EVENT_TYPE_COUNT];
static uint32_t s_coalesce_by_state_key[S3_EVENT_BUS_STATE_COUNT];

const char *s3_event_bus_level_name(s3_event_bus_level_t level)
{
    switch (level) {
    case S3_EVENT_BUS_LEVEL_CRITICAL:
        return "CRITICAL";
    case S3_EVENT_BUS_LEVEL_REALTIME:
        return "REALTIME";
    case S3_EVENT_BUS_LEVEL_STATE:
        return "STATE";
    case S3_EVENT_BUS_LEVEL_BACKGROUND:
        return "BACKGROUND";
    case S3_EVENT_BUS_LEVEL_COUNT:
    default:
        return "UNKNOWN";
    }
}

const char *s3_event_bus_state_key_name(s3_event_bus_state_key_t key)
{
    switch (key) {
    case S3_EVENT_BUS_STATE_BME_LATEST_C51:
        return "BME_LATEST_C51";
    case S3_EVENT_BUS_STATE_BME_LATEST_C52:
        return "BME_LATEST_C52";
    case S3_EVENT_BUS_STATE_DEVICE_STATUS_C51:
        return "DEVICE_STATUS_C51";
    case S3_EVENT_BUS_STATE_DEVICE_STATUS_C52:
        return "DEVICE_STATUS_C52";
    case S3_EVENT_BUS_STATE_NONE:
        return "NONE";
    case S3_EVENT_BUS_STATE_COUNT:
    default:
        return "UNKNOWN";
    }
}

static size_t state_depth_locked(void)
{
    size_t count = 0U;
    for (size_t i = 1U; i < S3_EVENT_BUS_STATE_COUNT; ++i) {
        if (s_state_slots[i] != NULL) {
            ++count;
        }
    }
    return count;
}

static size_t queue_depth_locked(void)
{
    return s_critical_queue.count + s_realtime_queue.count +
           state_depth_locked() + s_background_queue.count;
}

static void snapshot_stats_locked(s3_event_bus_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return;
    }
    out_stats->queue_depth = queue_depth_locked();
    out_stats->critical_depth = s_critical_queue.count;
    out_stats->realtime_depth = s_realtime_queue.count;
    out_stats->state_depth = state_depth_locked();
    out_stats->background_depth = s_background_queue.count;
    out_stats->drop_count = s_drop_count;
    out_stats->background_drop_count = s_background_drop_count;
    out_stats->coalesce_count = s_coalesce_count;
    memcpy(out_stats->drop_by_event_type,
           s_drop_by_event_type,
           sizeof(out_stats->drop_by_event_type));
    memcpy(out_stats->coalesce_by_state_key,
           s_coalesce_by_state_key,
           sizeof(out_stats->coalesce_by_state_key));
}

static void release_event(s3_scheduler_event_t *event)
{
    if (event != NULL && s_release_fn != NULL) {
        s_release_fn(event);
    }
}

static void reset_locked(void)
{
    /* reset 会释放所有被 bus 接管的事件，避免重启/测试清理时泄漏 owned payload。 */
    for (size_t i = 0; i < s_critical_queue.count; ++i) {
        release_event(s_critical_queue.items[i]);
        s_critical_queue.items[i] = NULL;
    }
    s_critical_queue.count = 0U;

    for (size_t i = 0; i < s_realtime_queue.count; ++i) {
        release_event(s_realtime_queue.items[i]);
        s_realtime_queue.items[i] = NULL;
    }
    s_realtime_queue.count = 0U;

    for (size_t i = 0; i < s_background_queue.count; ++i) {
        release_event(s_background_queue.items[i]);
        s_background_queue.items[i] = NULL;
    }
    s_background_queue.count = 0U;

    for (size_t i = 0; i < S3_EVENT_BUS_STATE_COUNT; ++i) {
        release_event(s_state_slots[i]);
        s_state_slots[i] = NULL;
    }

    s_drop_count = 0U;
    s_background_drop_count = 0U;
    s_coalesce_count = 0U;
    memset(s_drop_by_event_type, 0, sizeof(s_drop_by_event_type));
    memset(s_coalesce_by_state_key, 0, sizeof(s_coalesce_by_state_key));
}

esp_err_t s3_event_bus_init(s3_event_bus_release_fn_t release_fn)
{
    if (release_fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_signal == NULL) {
        s_signal = xSemaphoreCreateCounting(S3_EVENT_BUS_CRITICAL_DEPTH +
                                                S3_EVENT_BUS_REALTIME_DEPTH +
                                                S3_EVENT_BUS_STATE_COUNT +
                                                S3_EVENT_BUS_BACKGROUND_DEPTH,
                                            0U);
        if (s_signal == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_release_fn = release_fn;
    return ESP_OK;
}

void s3_event_bus_reset(void)
{
    if (s_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    reset_locked();
    xSemaphoreGive(s_lock);

    if (s_signal != NULL) {
        while (xSemaphoreTake(s_signal, 0) == pdTRUE) {
        }
    }
}

static esp_err_t push_fifo_locked(s3_scheduler_event_t **items,
                                  size_t *count,
                                  size_t capacity,
                                  s3_scheduler_event_t *event)
{
    if (items == NULL || count == NULL || event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* CRITICAL/REALTIME 队列满时不丢弃，返回 timeout 让 scheduler 侧决定是否重试。 */
    if (*count >= capacity) {
        return ESP_ERR_TIMEOUT;
    }
    items[*count] = event;
    ++(*count);
    return ESP_OK;
}

static bool pop_fifo_locked(s3_scheduler_event_t **items,
                            size_t *count,
                            s3_scheduler_event_t **out_event)
{
    if (items == NULL || count == NULL || out_event == NULL || *count == 0U) {
        return false;
    }
    *out_event = items[0];
    for (size_t i = 1U; i < *count; ++i) {
        items[i - 1U] = items[i];
    }
    --(*count);
    items[*count] = NULL;
    return true;
}

static bool event_is_valid_for_bus(const s3_scheduler_event_t *event)
{
    if (event == NULL || event->type == S3_SCHEDULER_EVENT_NONE ||
        event->bus_level >= S3_EVENT_BUS_LEVEL_COUNT) {
        return false;
    }
    if (event->bus_level == S3_EVENT_BUS_LEVEL_STATE &&
        (event->state_key == S3_EVENT_BUS_STATE_NONE ||
         event->state_key >= S3_EVENT_BUS_STATE_COUNT)) {
        return false;
    }
    return true;
}

static esp_err_t push_owned_with_timeout(s3_scheduler_event_t *event,
                                         TickType_t lock_timeout_ticks,
                                         uint32_t *out_lock_wait_ms,
                                         s3_event_bus_stats_t *out_stats)
{
    if (out_lock_wait_ms != NULL) {
        *out_lock_wait_ms = 0U;
    }
    if (out_stats != NULL) {
        memset(out_stats, 0, sizeof(*out_stats));
    }
    if (!event_is_valid_for_bus(event)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL || s_signal == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const s3_event_bus_level_t level = event->bus_level;
    const s3_event_bus_state_key_t state_key = event->state_key;
    const s3_scheduler_event_type_t type = event->type;
    bool accepted = false;
    bool signal_needed = false;
    esp_err_t ret = ESP_OK;

    /* 所有队列结构都在同一把 mutex 下更新；信号量只表示“有新事件可消费”。 */
    const int64_t lock_wait_started_us = esp_timer_get_time();
    if (xSemaphoreTake(s_lock, lock_timeout_ticks) != pdTRUE) {
        if (out_lock_wait_ms != NULL) {
            *out_lock_wait_ms = (uint32_t)((esp_timer_get_time() - lock_wait_started_us) / 1000);
        }
        return ESP_ERR_TIMEOUT;
    }
    if (out_lock_wait_ms != NULL) {
        *out_lock_wait_ms = (uint32_t)((esp_timer_get_time() - lock_wait_started_us) / 1000);
    }
    switch (level) {
    case S3_EVENT_BUS_LEVEL_CRITICAL:
        ret = push_fifo_locked(s_critical_queue.items,
                               &s_critical_queue.count,
                               S3_EVENT_BUS_CRITICAL_DEPTH,
                               event);
        accepted = ret == ESP_OK;
        signal_needed = accepted;
        break;
    case S3_EVENT_BUS_LEVEL_REALTIME:
        ret = push_fifo_locked(s_realtime_queue.items,
                               &s_realtime_queue.count,
                               S3_EVENT_BUS_REALTIME_DEPTH,
                               event);
        accepted = ret == ESP_OK;
        signal_needed = accepted;
        break;
    case S3_EVENT_BUS_LEVEL_STATE:
        if (s_state_slots[state_key] != NULL) {
            /* STATE 只保留最新值，旧事件已经过期，立即释放旧 owned event。 */
            release_event(s_state_slots[state_key]);
            ++s_coalesce_count;
            if ((size_t)state_key < S3_EVENT_BUS_STATE_COUNT) {
                ++s_coalesce_by_state_key[state_key];
            }
        } else {
            signal_needed = true;
        }
        s_state_slots[state_key] = event;
        accepted = true;
        ret = ESP_OK;
        break;
    case S3_EVENT_BUS_LEVEL_BACKGROUND:
        ret = push_fifo_locked(s_background_queue.items,
                               &s_background_queue.count,
                               S3_EVENT_BUS_BACKGROUND_DEPTH,
                               event);
        if (ret == ESP_OK) {
            accepted = true;
        } else {
            /* BACKGROUND 是唯一允许在 bus 内直接丢弃的层级。 */
            ++s_drop_count;
            ++s_background_drop_count;
            if ((size_t)type < S3_EVENT_BUS_EVENT_TYPE_COUNT) {
                ++s_drop_by_event_type[type];
            }
            ESP_LOGD(TAG,
                     "background drop type=%d drop_count=%lu depth=%u",
                     (int)type,
                     (unsigned long)s_drop_count,
                     (unsigned int)queue_depth_locked());
            release_event(event);
            accepted = true;
            signal_needed = false;
            ret = ESP_OK;
        }
        break;
    case S3_EVENT_BUS_LEVEL_COUNT:
    default:
        ret = ESP_ERR_INVALID_ARG;
        break;
    }

    const size_t depth = queue_depth_locked();
    const size_t critical_depth = s_critical_queue.count;
    const size_t realtime_depth = s_realtime_queue.count;
    const size_t state_depth = state_depth_locked();
    const size_t background_depth = s_background_queue.count;
    const uint32_t drop_count = s_drop_count;
    const uint32_t coalesce_count = s_coalesce_count;
    snapshot_stats_locked(out_stats);
    xSemaphoreGive(s_lock);

    if (accepted && signal_needed) {
        xSemaphoreGive(s_signal);
    }
    if (accepted) {
        ESP_LOGD(TAG,
                 "enqueue level=%s state=%s queue_depth=%u c=%u rt=%u st=%u bg=%u drop_count=%lu coalesce_count=%lu",
                 s3_event_bus_level_name(level),
                 s3_event_bus_state_key_name(state_key),
                 (unsigned int)depth,
                 (unsigned int)critical_depth,
                 (unsigned int)realtime_depth,
                 (unsigned int)state_depth,
                 (unsigned int)background_depth,
                 (unsigned long)drop_count,
                 (unsigned long)coalesce_count);
    }
    return ret;
}

esp_err_t s3_event_bus_push_owned(s3_scheduler_event_t *event)
{
    return push_owned_with_timeout(event, portMAX_DELAY, NULL, NULL);
}

esp_err_t s3_event_bus_push_owned_timed(s3_scheduler_event_t *event,
                                        uint32_t lock_timeout_ms,
                                        uint32_t *out_lock_wait_ms,
                                        s3_event_bus_stats_t *out_stats)
{
    TickType_t timeout_ticks = pdMS_TO_TICKS(lock_timeout_ms);
    if (lock_timeout_ms > 0U && timeout_ticks == 0U) {
        timeout_ticks = 1U;
    }
    return push_owned_with_timeout(event, timeout_ticks, out_lock_wait_ms, out_stats);
}

bool s3_event_bus_wait(uint32_t timeout_ms)
{
    if (s_signal == NULL) {
        return false;
    }
    return xSemaphoreTake(s_signal, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool s3_event_bus_dequeue(s3_scheduler_event_t **out_event)
{
    if (out_event == NULL || s_lock == NULL) {
        return false;
    }
    *out_event = NULL;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* 出队顺序是固定优先级：关键控制 > 实时数据 > 最新状态 > 后台任务。 */
    bool ok = pop_fifo_locked(s_critical_queue.items,
                              &s_critical_queue.count,
                              out_event);
    if (!ok) {
        ok = pop_fifo_locked(s_realtime_queue.items,
                             &s_realtime_queue.count,
                             out_event);
    }
    if (!ok) {
        for (size_t i = 1U; i < S3_EVENT_BUS_STATE_COUNT; ++i) {
            if (s_state_slots[i] != NULL) {
                *out_event = s_state_slots[i];
                s_state_slots[i] = NULL;
                ok = true;
                break;
            }
        }
    }
    if (!ok) {
        ok = pop_fifo_locked(s_background_queue.items,
                             &s_background_queue.count,
                             out_event);
    }
    xSemaphoreGive(s_lock);
    return ok;
}

s3_event_bus_stats_t s3_event_bus_get_stats(void)
{
    s3_event_bus_stats_t stats = {0};
    if (s_lock == NULL) {
        return stats;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    snapshot_stats_locked(&stats);
    xSemaphoreGive(s_lock);
    return stats;
}

void s3_event_bus_log_stats(const char *reason)
{
    s3_event_bus_stats_t stats = s3_event_bus_get_stats();
    ESP_LOGI(TAG,
             "queue_depth=%u critical=%u realtime=%u state=%u background=%u drop_count=%lu background_drop_count=%lu coalesce_count=%lu drop_by_type=[none:%lu ingress:%lu stream_frame:%lu stream_send:%lu network:%lu voice:%lu command:%lu stats:%lu] coalesce_state=[bme51:%lu bme52:%lu status51:%lu status52:%lu] reason=%s",
             (unsigned int)stats.queue_depth,
             (unsigned int)stats.critical_depth,
             (unsigned int)stats.realtime_depth,
             (unsigned int)stats.state_depth,
             (unsigned int)stats.background_depth,
             (unsigned long)stats.drop_count,
             (unsigned long)stats.background_drop_count,
             (unsigned long)stats.coalesce_count,
             (unsigned long)stats.drop_by_event_type[S3_SCHEDULER_EVENT_NONE],
             (unsigned long)stats.drop_by_event_type[S3_SCHEDULER_EVENT_INGRESS],
             (unsigned long)stats.drop_by_event_type[S3_SCHEDULER_EVENT_STREAM_FRAME],
             (unsigned long)stats.drop_by_event_type[S3_SCHEDULER_EVENT_STREAM_SEND],
             (unsigned long)stats.drop_by_event_type[S3_SCHEDULER_EVENT_NETWORK_STATE],
             (unsigned long)stats.drop_by_event_type[S3_SCHEDULER_EVENT_VOICE_STATE],
             (unsigned long)stats.drop_by_event_type[S3_SCHEDULER_EVENT_COMMAND_PULL],
             (unsigned long)stats.drop_by_event_type[S3_SCHEDULER_EVENT_BACKGROUND_STATS],
             (unsigned long)stats.coalesce_by_state_key[S3_EVENT_BUS_STATE_BME_LATEST_C51],
             (unsigned long)stats.coalesce_by_state_key[S3_EVENT_BUS_STATE_BME_LATEST_C52],
             (unsigned long)stats.coalesce_by_state_key[S3_EVENT_BUS_STATE_DEVICE_STATUS_C51],
             (unsigned long)stats.coalesce_by_state_key[S3_EVENT_BUS_STATE_DEVICE_STATUS_C52],
             reason != NULL ? reason : "periodic");
}

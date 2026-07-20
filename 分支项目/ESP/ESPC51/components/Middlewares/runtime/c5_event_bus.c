/**
 * @file c5_event_bus.c
 * @brief C5 运行时事件队列和 dispatcher 辅助实现。
 *
 * 本文件只管理事件所有权、队列顺序和诊断计数。具体业务处理由
 * c5_runtime_workers.c 中的 CSI/BME/system worker 完成。
 */

#include "c5_event_bus.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"

typedef struct {
    c5_event_dispatch_handler_t handler;
    void *ctx;
} c5_event_handler_slot_t;

/* 单一主队列承接 timer/callback 事件；handler 表把事件快速路由到各 worker queue。 */
static QueueHandle_t s_event_queue;
static QueueHandle_t s_retired_event_queue;
static c5_event_handler_slot_t s_handlers[C5_EVENT_MAX];
static c5_event_bus_stats_t s_stats;
static uint32_t s_next_sequence;
static uint32_t s_event_queue_users;
static c5_lifecycle_state_t s_event_bus_state = C5_LIFECYCLE_STOPPED;
static portMUX_TYPE s_event_bus_lock = portMUX_INITIALIZER_UNLOCKED;

static uint64_t c5_event_bus_now_ms(void)
{
    int64_t now_us = esp_timer_get_time();
    return now_us > 0 ? (uint64_t)(now_us / 1000) : 0U;
}

static bool c5_event_type_is_valid(c5_event_type_t type)
{
    return type >= C5_EVENT_CSI_READY && type < C5_EVENT_MAX;
}

static QueueHandle_t c5_event_bus_acquire_queue(void)
{
    QueueHandle_t queue = NULL;

    portENTER_CRITICAL(&s_event_bus_lock);
    if (s_event_bus_state == C5_LIFECYCLE_RUNNING &&
        s_event_queue != NULL &&
        s_event_queue_users < UINT32_MAX) {
        queue = s_event_queue;
        s_event_queue_users++;
    }
    portEXIT_CRITICAL(&s_event_bus_lock);

    return queue;
}

static void c5_event_bus_release_queue(void)
{
    portENTER_CRITICAL(&s_event_bus_lock);
    if (s_event_queue_users > 0U) {
        s_event_queue_users--;
    }
    portEXIT_CRITICAL(&s_event_bus_lock);
}

esp_err_t c5_event_bus_init(void)
{
    portENTER_CRITICAL(&s_event_bus_lock);
    if (s_event_bus_state == C5_LIFECYCLE_RUNNING) {
        portEXIT_CRITICAL(&s_event_bus_lock);
        return ESP_OK;
    }
    if (s_event_bus_state != C5_LIFECYCLE_STOPPED || s_retired_event_queue != NULL) {
        portEXIT_CRITICAL(&s_event_bus_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_event_bus_state = C5_LIFECYCLE_STARTING;
    portEXIT_CRITICAL(&s_event_bus_lock);

    /* 先在临时变量中创建，再在临界区内发布，避免重复 init 造成句柄覆盖。 */
    QueueHandle_t queue = xQueueCreate((UBaseType_t)C5_EVENT_BUS_QUEUE_LENGTH,
                                       sizeof(c5_event_t));
    if (queue == NULL) {
        portENTER_CRITICAL(&s_event_bus_lock);
        s_event_bus_state = C5_LIFECYCLE_STOPPED;
        portEXIT_CRITICAL(&s_event_bus_lock);
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_event_bus_lock);
    if (s_event_bus_state == C5_LIFECYCLE_STARTING && s_event_queue == NULL) {
        s_event_queue = queue;
        s_event_queue_users = 0U;
        memset(s_handlers, 0, sizeof(s_handlers));
        memset(&s_stats, 0, sizeof(s_stats));
        s_next_sequence = 0U;
        s_event_bus_state = C5_LIFECYCLE_RUNNING;
        queue = NULL;
    }
    portEXIT_CRITICAL(&s_event_bus_lock);

    if (queue != NULL) {
        vQueueDelete(queue);
        portENTER_CRITICAL(&s_event_bus_lock);
        if (s_event_bus_state == C5_LIFECYCLE_STARTING) {
            s_event_bus_state = C5_LIFECYCLE_FAULT;
        }
        portEXIT_CRITICAL(&s_event_bus_lock);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t c5_event_bus_deinit(void)
{
    QueueHandle_t queue = NULL;

    portENTER_CRITICAL(&s_event_bus_lock);
    if (s_event_bus_state == C5_LIFECYCLE_STOPPED) {
        portEXIT_CRITICAL(&s_event_bus_lock);
        return ESP_OK;
    }
    if (s_event_bus_state == C5_LIFECYCLE_STARTING ||
        s_event_bus_state == C5_LIFECYCLE_STOPPING) {
        portEXIT_CRITICAL(&s_event_bus_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_event_bus_state = C5_LIFECYCLE_STOPPING;
    if (s_retired_event_queue == NULL) {
        s_retired_event_queue = s_event_queue;
        s_event_queue = NULL;
        memset(s_handlers, 0, sizeof(s_handlers));
        memset(&s_stats, 0, sizeof(s_stats));
        s_next_sequence = 0U;
    }
    queue = s_retired_event_queue;
    portEXIT_CRITICAL(&s_event_bus_lock);

    if (queue == NULL) {
        portENTER_CRITICAL(&s_event_bus_lock);
        s_event_bus_state = C5_LIFECYCLE_STOPPED;
        portEXIT_CRITICAL(&s_event_bus_lock);
        return ESP_OK;
    }

    /* Existing enqueue/dispatch calls retain a reference until their queue access finishes. */
    const uint64_t deadline_ms = c5_event_bus_now_ms() + 1000U;
    while (true) {
        uint32_t users;

        portENTER_CRITICAL(&s_event_bus_lock);
        users = s_event_queue_users;
        portEXIT_CRITICAL(&s_event_bus_lock);
        if (users == 0U) {
            break;
        }
        if (c5_event_bus_now_ms() >= deadline_ms) {
            portENTER_CRITICAL(&s_event_bus_lock);
            s_event_bus_state = C5_LIFECYCLE_FAULT;
            portEXIT_CRITICAL(&s_event_bus_lock);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }

    vQueueDelete(queue);

    portENTER_CRITICAL(&s_event_bus_lock);
    s_retired_event_queue = NULL;
    memset(s_handlers, 0, sizeof(s_handlers));
    memset(&s_stats, 0, sizeof(s_stats));
    s_next_sequence = 0U;
    s_event_bus_state = C5_LIFECYCLE_STOPPED;
    portEXIT_CRITICAL(&s_event_bus_lock);
    return ESP_OK;
}

c5_lifecycle_state_t c5_event_bus_get_state(void)
{
    c5_lifecycle_state_t state;

    portENTER_CRITICAL(&s_event_bus_lock);
    state = s_event_bus_state;
    portEXIT_CRITICAL(&s_event_bus_lock);
    return state;
}

esp_err_t c5_event_bus_register_handler(c5_event_type_t type,
                                         c5_event_dispatch_handler_t handler,
                                         void *ctx)
{
    if (!c5_event_type_is_valid(type) || handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_event_bus_lock);
    if (s_event_bus_state != C5_LIFECYCLE_RUNNING) {
        portEXIT_CRITICAL(&s_event_bus_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_handlers[type].handler = handler;
    s_handlers[type].ctx = ctx;
    portEXIT_CRITICAL(&s_event_bus_lock);
    return ESP_OK;
}

void c5_event_bus_note_drop(void)
{
    portENTER_CRITICAL(&s_event_bus_lock);
    if (s_event_bus_state == C5_LIFECYCLE_RUNNING &&
        s_event_queue != NULL &&
        s_stats.drop_count < UINT32_MAX) {
        s_stats.drop_count++;
    }
    portEXIT_CRITICAL(&s_event_bus_lock);
}

esp_err_t c5_event_bus_enqueue(c5_event_type_t type, c5_event_source_t source)
{
    if (!c5_event_type_is_valid(type)) {
        return ESP_ERR_INVALID_ARG;
    }
    QueueHandle_t queue = c5_event_bus_acquire_queue();
    if (queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    c5_event_t event = {
        .type = type,
        .source = source,
        .timestamp_ms = c5_event_bus_now_ms(),
        .sequence = 0,
    };

    /* sequence 只用于日志和延迟追踪，不参与业务排序；真正顺序由 FreeRTOS 队列保证。 */
    portENTER_CRITICAL(&s_event_bus_lock);
    event.sequence = ++s_next_sequence;
    portEXIT_CRITICAL(&s_event_bus_lock);

    if (xQueueSend(queue, &event, 0) != pdTRUE) {
        c5_event_bus_release_queue();
        c5_event_bus_note_drop();
        return ESP_ERR_TIMEOUT;
    }

    c5_event_bus_release_queue();

    portENTER_CRITICAL(&s_event_bus_lock);
    if (s_event_bus_state == C5_LIFECYCLE_RUNNING &&
        s_event_queue != NULL &&
        s_stats.enqueue_count < UINT32_MAX) {
        s_stats.enqueue_count++;
    }
    portEXIT_CRITICAL(&s_event_bus_lock);
    return ESP_OK;
}

esp_err_t c5_event_bus_dispatch(uint32_t timeout_ms)
{
    QueueHandle_t queue = c5_event_bus_acquire_queue();
    if (queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    c5_event_t event = {0};
    if (xQueueReceive(queue, &event, timeout_ticks) != pdTRUE) {
        c5_event_bus_release_queue();
        return ESP_ERR_TIMEOUT;
    }

    /* handler/ctx 拷贝出临界区后执行，避免业务路径持有 event bus 锁。 */
    c5_event_dispatch_handler_t handler = NULL;
    void *ctx = NULL;
    portENTER_CRITICAL(&s_event_bus_lock);
    if (c5_event_type_is_valid(event.type)) {
        handler = s_handlers[event.type].handler;
        ctx = s_handlers[event.type].ctx;
    }
    if (s_event_bus_state == C5_LIFECYCLE_RUNNING &&
        s_stats.dispatch_count < UINT32_MAX) {
        s_stats.dispatch_count++;
    }
    portEXIT_CRITICAL(&s_event_bus_lock);

    if (handler == NULL) {
        c5_event_bus_release_queue();
        c5_event_bus_note_drop();
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = handler(&event, ctx);
    c5_event_bus_release_queue();
    return ret;
}

void c5_event_bus_record_worker_latency(uint32_t latency_ms)
{
    portENTER_CRITICAL(&s_event_bus_lock);
    if (s_event_bus_state != C5_LIFECYCLE_RUNNING) {
        portEXIT_CRITICAL(&s_event_bus_lock);
        return;
    }
    s_stats.last_worker_latency_ms = latency_ms;
    if (latency_ms > s_stats.max_worker_latency_ms) {
        s_stats.max_worker_latency_ms = latency_ms;
    }
    portEXIT_CRITICAL(&s_event_bus_lock);
}

uint32_t c5_event_bus_queue_depth(void)
{
    QueueHandle_t queue = c5_event_bus_acquire_queue();
    if (queue == NULL) {
        return 0U;
    }
    uint32_t depth = (uint32_t)uxQueueMessagesWaiting(queue);
    c5_event_bus_release_queue();
    return depth;
}

void c5_event_bus_get_stats(c5_event_bus_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_event_bus_lock);
    *out_stats = s_stats;
    portEXIT_CRITICAL(&s_event_bus_lock);
    out_stats->event_queue_depth = c5_event_bus_queue_depth();
}

const char *c5_event_type_name(c5_event_type_t type)
{
    switch (type) {
    case C5_EVENT_CSI_READY:
        return "C5_EVENT_CSI_READY";
    case C5_EVENT_BME_SAMPLE:
        return "C5_EVENT_BME_SAMPLE";
    case C5_EVENT_HEARTBEAT:
        return "C5_EVENT_HEARTBEAT";
    case C5_EVENT_STATUS:
        return "C5_EVENT_STATUS";
    case C5_EVENT_COMMAND:
        return "C5_EVENT_COMMAND";
    default:
        return "C5_EVENT_UNKNOWN";
    }
}

const char *c5_event_source_name(c5_event_source_t source)
{
    switch (source) {
    case C5_EVENT_SOURCE_TIMER:
        return "timer";
    case C5_EVENT_SOURCE_CALLBACK:
        return "callback";
    case C5_EVENT_SOURCE_INTERNAL:
        return "internal";
    default:
        return "unknown";
    }
}

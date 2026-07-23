/**
 * @file c5_event_bus.c
 * @brief C5 运行时事件队列和 dispatcher 辅助实现。
 *
 * 本文件只管理事件所有权、队列顺序和诊断计数。具体业务处理由
 * c5_runtime_workers.c 中的 BME/system workers 完成。
 */

#include "c5_event_bus.h"

#include <stdbool.h>
#include <stddef.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"

typedef struct {
    c5_event_dispatch_handler_t handler;
    void *ctx;
} c5_event_handler_slot_t;

/* 单一主队列承接 timer/callback 事件；handler 表把事件快速路由到各 worker queue。 */
static QueueHandle_t s_event_queue;
static StaticQueue_t s_event_queue_storage;
static uint8_t s_event_queue_buffer[C5_EVENT_BUS_QUEUE_LENGTH * sizeof(c5_event_t)]
    __attribute__((aligned(portBYTE_ALIGNMENT)));
static c5_event_handler_slot_t s_handlers[C5_EVENT_MAX];
static c5_event_bus_stats_t s_stats;
static uint32_t s_next_sequence;
static portMUX_TYPE s_event_bus_lock = portMUX_INITIALIZER_UNLOCKED;

static uint64_t c5_event_bus_now_ms(void)
{
    int64_t now_us = esp_timer_get_time();
    return now_us > 0 ? (uint64_t)(now_us / 1000) : 0U;
}

static bool c5_event_type_is_valid(c5_event_type_t type)
{
    return type >= C5_EVENT_BME_SAMPLE && type < C5_EVENT_MAX;
}

esp_err_t c5_event_bus_init(void)
{
    if (s_event_queue != NULL) {
        return ESP_OK;
    }

    s_event_queue = xQueueCreateStatic((UBaseType_t)C5_EVENT_BUS_QUEUE_LENGTH,
                                       sizeof(c5_event_t),
                                       s_event_queue_buffer,
                                       &s_event_queue_storage);
    if (s_event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t c5_event_bus_register_handler(c5_event_type_t type,
                                         c5_event_dispatch_handler_t handler,
                                         void *ctx)
{
    if (!c5_event_type_is_valid(type) || handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_event_bus_lock);
    s_handlers[type].handler = handler;
    s_handlers[type].ctx = ctx;
    portEXIT_CRITICAL(&s_event_bus_lock);
    return ESP_OK;
}

void c5_event_bus_note_drop(void)
{
    portENTER_CRITICAL(&s_event_bus_lock);
    if (s_stats.drop_count < UINT32_MAX) {
        s_stats.drop_count++;
    }
    portEXIT_CRITICAL(&s_event_bus_lock);
}

esp_err_t c5_event_bus_enqueue(c5_event_type_t type, c5_event_source_t source)
{
    if (!c5_event_type_is_valid(type)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_event_queue == NULL) {
        c5_event_bus_note_drop();
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

    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        c5_event_bus_note_drop();
        return ESP_ERR_TIMEOUT;
    }

    portENTER_CRITICAL(&s_event_bus_lock);
    if (s_stats.enqueue_count < UINT32_MAX) {
        s_stats.enqueue_count++;
    }
    portEXIT_CRITICAL(&s_event_bus_lock);
    return ESP_OK;
}

esp_err_t c5_event_bus_dispatch(uint32_t timeout_ms)
{
    if (s_event_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    c5_event_t event = {0};
    if (xQueueReceive(s_event_queue, &event, timeout_ticks) != pdTRUE) {
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
    if (s_stats.dispatch_count < UINT32_MAX) {
        s_stats.dispatch_count++;
    }
    portEXIT_CRITICAL(&s_event_bus_lock);

    if (handler == NULL) {
        c5_event_bus_note_drop();
        return ESP_ERR_NOT_FOUND;
    }

    return handler(&event, ctx);
}

void c5_event_bus_record_worker_latency(uint32_t latency_ms)
{
    portENTER_CRITICAL(&s_event_bus_lock);
    s_stats.last_worker_latency_ms = latency_ms;
    if (latency_ms > s_stats.max_worker_latency_ms) {
        s_stats.max_worker_latency_ms = latency_ms;
    }
    portEXIT_CRITICAL(&s_event_bus_lock);
}

uint32_t c5_event_bus_queue_depth(void)
{
    if (s_event_queue == NULL) {
        return 0U;
    }
    return (uint32_t)uxQueueMessagesWaiting(s_event_queue);
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
    case C5_EVENT_BME_SAMPLE:
        return "C5_EVENT_BME_SAMPLE";
    case C5_EVENT_HEARTBEAT:
        return "C5_EVENT_HEARTBEAT";
    case C5_EVENT_STATUS:
        return "C5_EVENT_STATUS";
    case C5_EVENT_COMMAND:
        return "C5_EVENT_COMMAND";
    case C5_EVENT_RADAR_HOME_SNAPSHOT:
        return "C5_EVENT_RADAR_HOME_SNAPSHOT";
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

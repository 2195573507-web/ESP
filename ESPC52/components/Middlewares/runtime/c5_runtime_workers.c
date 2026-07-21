/**
 * @file c5_runtime_workers.c
 * @brief C5 runtime workers for BME and system events.
 */

#include "c5_runtime_workers.h"

#include <stdint.h>

#include "bme_sensor_service.h"
#include "c5_event_bus.h"
#include "c5_memory.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "radar_home_snapshot_client.h"
#include "system_service.h"

static const char *TAG = "c5_workers";

#define C5_WORKER_TASK_STACK_WORDS \
    ((C5_WORKER_TASK_STACK + sizeof(StackType_t) - 1U) / sizeof(StackType_t))

static QueueHandle_t s_bme_worker_queue;
static QueueHandle_t s_system_worker_queue;
static StaticQueue_t s_bme_worker_queue_storage;
static StaticQueue_t s_system_worker_queue_storage;
static uint8_t s_bme_worker_queue_buffer[C5_WORKER_QUEUE_LENGTH * sizeof(c5_event_t)]
    __attribute__((aligned(portBYTE_ALIGNMENT)));
static uint8_t s_system_worker_queue_buffer[C5_WORKER_QUEUE_LENGTH * sizeof(c5_event_t)]
    __attribute__((aligned(portBYTE_ALIGNMENT)));
static TaskHandle_t s_bme_worker_task;
static TaskHandle_t s_system_worker_task;
static StackType_t *s_bme_worker_stack;
static StackType_t *s_system_worker_stack;
static StaticTask_t s_bme_worker_storage;
static StaticTask_t s_system_worker_storage;
static bool s_workers_paused;
static uint32_t s_worker_active_mask;
static portMUX_TYPE s_worker_state_lock = portMUX_INITIALIZER_UNLOCKED;

enum {
    C5_WORKER_ACTIVE_BME = 1U << 0,
    C5_WORKER_ACTIVE_SYSTEM = 1U << 1,
};

static uint64_t c5_worker_now_ms(void)
{
    int64_t now_us = esp_timer_get_time();
    return now_us > 0 ? (uint64_t)(now_us / 1000) : 0U;
}

static bool c5_worker_begin(uint32_t active_bit)
{
    bool allowed = false;
    portENTER_CRITICAL(&s_worker_state_lock);
    if (!s_workers_paused) {
        s_worker_active_mask |= active_bit;
        allowed = true;
    }
    portEXIT_CRITICAL(&s_worker_state_lock);
    return allowed;
}

static void c5_worker_end(uint32_t active_bit)
{
    portENTER_CRITICAL(&s_worker_state_lock);
    s_worker_active_mask &= ~active_bit;
    portEXIT_CRITICAL(&s_worker_state_lock);
}

static bool c5_workers_dispatch_allowed(void)
{
    bool allowed;
    portENTER_CRITICAL(&s_worker_state_lock);
    allowed = !s_workers_paused;
    portEXIT_CRITICAL(&s_worker_state_lock);
    return allowed;
}

static void c5_worker_record_latency(const c5_event_t *event)
{
    if (event == NULL) {
        return;
    }
    uint64_t timestamp_ms = c5_worker_now_ms();
    uint32_t latency_ms = timestamp_ms >= event->timestamp_ms ?
                              (uint32_t)(timestamp_ms - event->timestamp_ms) :
                              0U;
    c5_event_bus_record_worker_latency(latency_ms);
}

static void c5_worker_log_ret(const char *worker, const c5_event_t *event, esp_err_t ret)
{
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE || ret == ESP_ERR_NOT_FOUND) {
        return;
    }
    ESP_LOGW(TAG,
             "%s failed event=%s source=%s ret=%s",
             worker,
             event != NULL ? c5_event_type_name(event->type) : "none",
             event != NULL ? c5_event_source_name(event->source) : "none",
             esp_err_to_name(ret));
}

static esp_err_t c5_worker_enqueue(QueueHandle_t queue, const c5_event_t *event)
{
    if (queue == NULL || event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!c5_workers_dispatch_allowed()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(queue, event, 0) != pdTRUE) {
        c5_event_bus_note_drop();
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t c5_route_bme_event(const c5_event_t *event, void *ctx)
{
    (void)ctx;
    return c5_worker_enqueue(s_bme_worker_queue, event);
}

static esp_err_t c5_route_system_event(const c5_event_t *event, void *ctx)
{
    (void)ctx;
    return c5_worker_enqueue(s_system_worker_queue, event);
}

static void bme_worker(void *arg)
{
    (void)arg;
    c5_event_t event = {0};

    while (true) {
        if (xQueueReceive(s_bme_worker_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!c5_worker_begin(C5_WORKER_ACTIVE_BME)) {
            continue;
        }

        esp_err_t ret = bme_sensor_service_tick();
        c5_worker_log_ret("bme_worker", &event, ret);
        c5_worker_record_latency(&event);
        c5_worker_end(C5_WORKER_ACTIVE_BME);
    }
}

static void system_worker(void *arg)
{
    (void)arg;
    c5_event_t event = {0};

    while (true) {
        if (xQueueReceive(s_system_worker_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!c5_worker_begin(C5_WORKER_ACTIVE_SYSTEM)) {
            continue;
        }

        esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
        switch (event.type) {
        case C5_EVENT_HEARTBEAT:
            ret = system_service_tick_heartbeat();
            break;
        case C5_EVENT_STATUS:
            ret = system_service_tick_status();
            break;
        case C5_EVENT_COMMAND:
            ret = system_service_tick_command_poll();
            break;
        case C5_EVENT_RADAR_HOME_SNAPSHOT:
            radar_home_snapshot_client_poll(c5_worker_now_ms());
            ret = ESP_OK;
            break;
        default:
            break;
        }
        c5_worker_log_ret("system_worker", &event, ret);
        c5_worker_record_latency(&event);
        c5_worker_end(C5_WORKER_ACTIVE_SYSTEM);
    }
}

static esp_err_t create_queue(QueueHandle_t *queue,
                              StaticQueue_t *storage,
                              uint8_t *buffer)
{
    if (queue == NULL || storage == NULL || buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*queue == NULL) {
        *queue = xQueueCreateStatic((UBaseType_t)C5_WORKER_QUEUE_LENGTH,
                                    sizeof(c5_event_t), buffer, storage);
    }
    return *queue != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t create_task(TaskHandle_t *task,
                             StackType_t **stack,
                             StaticTask_t *storage,
                             TaskFunction_t entry,
                             const char *name,
                             c5_mem_type_t stack_memory,
                             const char *memory_stage_before,
                             const char *memory_stage_after,
                             const char *memory_stage_failed)
{
    if (task == NULL || stack == NULL || storage == NULL || entry == NULL || name == NULL ||
        memory_stage_before == NULL || memory_stage_after == NULL || memory_stage_failed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*task != NULL) {
        return ESP_OK;
    }

    c5_mem_log(memory_stage_before);
    if (*stack == NULL) {
        *stack = (StackType_t *)c5_mem_alloc(C5_WORKER_TASK_STACK,
                                             stack_memory,
                                             name);
    }
    if (*stack == NULL) {
        c5_mem_log(memory_stage_failed);
        return ESP_ERR_NO_MEM;
    }

    *task = xTaskCreateStatic(entry,
                              name,
                              C5_WORKER_TASK_STACK_WORDS,
                              NULL,
                              C5_WORKER_TASK_PRIORITY,
                              *stack,
                              storage);
    if (*task == NULL) {
        c5_mem_free(*stack, name);
        *stack = NULL;
        c5_mem_log(memory_stage_failed);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "TASK_CREATE task=%s stack=%u source=%s_static", name,
             (unsigned int)C5_WORKER_TASK_STACK,
             stack_memory == C5_MEM_PSRAM ? "psram" : "internal");
    c5_mem_log(memory_stage_after);
    return ESP_OK;
}

esp_err_t c5_runtime_workers_prepare(void)
{
    esp_err_t ret = c5_event_bus_init();
    if (ret == ESP_OK) {
        ret = create_queue(&s_bme_worker_queue,
                           &s_bme_worker_queue_storage,
                           s_bme_worker_queue_buffer);
    }
    if (ret == ESP_OK) {
        ret = create_queue(&s_system_worker_queue,
                           &s_system_worker_queue_storage,
                           s_system_worker_queue_buffer);
    }
    if (ret == ESP_OK) {
        ret = c5_event_bus_register_handler(C5_EVENT_BME_SAMPLE, c5_route_bme_event, NULL);
    }
    if (ret == ESP_OK) {
        ret = c5_event_bus_register_handler(C5_EVENT_HEARTBEAT, c5_route_system_event, NULL);
    }
    if (ret == ESP_OK) {
        ret = c5_event_bus_register_handler(C5_EVENT_STATUS, c5_route_system_event, NULL);
    }
    if (ret == ESP_OK) {
        ret = c5_event_bus_register_handler(C5_EVENT_COMMAND, c5_route_system_event, NULL);
    }
    if (ret == ESP_OK) {
        ret = c5_event_bus_register_handler(C5_EVENT_RADAR_HOME_SNAPSHOT,
                                            c5_route_system_event,
                                            NULL);
    }
    return ret;
}

esp_err_t c5_runtime_workers_start(void)
{
    esp_err_t ret = c5_runtime_workers_prepare();
    if (ret == ESP_OK) {
        ret = create_task(&s_bme_worker_task,
                          &s_bme_worker_stack,
                          &s_bme_worker_storage,
                          bme_worker,
                          "bme_worker",
                          C5_MEM_PSRAM,
                          "task_create_before_bme_worker",
                          "task_create_after_bme_worker",
                          "task_create_after_bme_worker_failed");
    }
    if (ret == ESP_OK) {
        ret = create_task(&s_system_worker_task,
                          &s_system_worker_stack,
                          &s_system_worker_storage,
                          system_worker,
                          "system_worker",
                          C5_MEM_PSRAM,
                          "task_create_before_system_worker",
                          "task_create_after_system_worker",
                          "task_create_after_system_worker_failed");
    }
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG,
             "C5 BME/system workers started priority=%u stack=%u queue_len=%u",
             (unsigned int)C5_WORKER_TASK_PRIORITY,
             (unsigned int)C5_WORKER_TASK_STACK,
             (unsigned int)C5_WORKER_QUEUE_LENGTH);
    return ESP_OK;
}

esp_err_t c5_runtime_workers_quiesce(uint32_t timeout_ms)
{
    portENTER_CRITICAL(&s_worker_state_lock);
    s_workers_paused = true;
    portEXIT_CRITICAL(&s_worker_state_lock);

    if (s_bme_worker_queue != NULL) {
        (void)xQueueReset(s_bme_worker_queue);
    }
    if (s_system_worker_queue != NULL) {
        (void)xQueueReset(s_system_worker_queue);
    }

    const uint64_t deadline_ms = c5_worker_now_ms() + (uint64_t)timeout_ms;
    while (true) {
        uint32_t active_mask;
        portENTER_CRITICAL(&s_worker_state_lock);
        active_mask = s_worker_active_mask;
        portEXIT_CRITICAL(&s_worker_state_lock);
        if (active_mask == 0U) {
            ESP_LOGI(TAG, "C5 workers quiesced timeout_ms=%u", (unsigned int)timeout_ms);
            return ESP_OK;
        }
        if (c5_worker_now_ms() >= deadline_ms) {
            ESP_LOGW(TAG,
                     "C5 worker quiesce timeout active_mask=0x%lx timeout_ms=%u",
                     (unsigned long)active_mask,
                     (unsigned int)timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void c5_runtime_workers_resume(void)
{
    portENTER_CRITICAL(&s_worker_state_lock);
    s_workers_paused = false;
    portEXIT_CRITICAL(&s_worker_state_lock);
}

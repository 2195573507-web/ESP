/**
 * @file c5_backpressure_controller.c
 * @brief C5 终端统一回压控制器和事件调度器。
 *
 * 本文件属于 ESP32-C5 终端调度层。它把本地 BME
 * 上报、heartbeat/status/command poll 转成 runtime event，按语音独占、S3 local
 * gateway 连接状态、待运行任务比例和 CPU 空闲估算放慢普通事件。业务函数只由
 * worker 执行；本模块不构造 Server API，也不绕过 ESPS3。
 */

#include "c5_backpressure_controller.h"

#include <stddef.h>

#include "app_main_config.h"
#include "app_runtime.h"
#include "bme_sensor_service.h"
#include "c5_event_bus.h"
#include "c5_memory.h"
#include "c5_runtime_workers.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "system_service.h"
#include "terminal_config.h"

static const char *TAG = "c5_scheduler";

#ifndef C5_BACKPRESSURE_LOW_QUEUE_LOAD
#define C5_BACKPRESSURE_LOW_QUEUE_LOAD 70U
#endif

#ifndef C5_BACKPRESSURE_HIGH_QUEUE_LOAD
#define C5_BACKPRESSURE_HIGH_QUEUE_LOAD 85U
#endif

#ifndef C5_BACKPRESSURE_LOW_CPU_IDLE
#define C5_BACKPRESSURE_LOW_CPU_IDLE 25U
#endif

#ifndef C5_BACKPRESSURE_CRITICAL_CPU_IDLE
#define C5_BACKPRESSURE_CRITICAL_CPU_IDLE 10U
#endif

#ifndef C5_BACKPRESSURE_MAX_INTERVAL_MS
#define C5_BACKPRESSURE_MAX_INTERVAL_MS 60000U
#endif

typedef struct {
    c5_task_type_t task_type;
    c5_event_type_t event_type;
    uint64_t next_run_ms;
} c5_scheduled_task_t;

static c5_backpressure_state_t s_backpressure_state = {
    .queue_load = 0,
    .gateway_state = LINK_DOWN,
    .voice_state = VOICE_IDLE,
    .voice_active = false,
    .cpu_idle_estimate = 100,
};
static c5_scheduled_task_t s_scheduler_timers[] = {
    {C5_TASK_TYPE_BME_SENSOR, C5_EVENT_BME_SAMPLE, 0},
    {C5_TASK_TYPE_SYSTEM_HEARTBEAT, C5_EVENT_HEARTBEAT, 0},
    {C5_TASK_TYPE_SYSTEM_STATUS, C5_EVENT_STATUS, 0},
    {C5_TASK_TYPE_SYSTEM_COMMAND_POLL, C5_EVENT_COMMAND, 0},
    {C5_TASK_TYPE_RADAR_HOME_SNAPSHOT, C5_EVENT_RADAR_HOME_SNAPSHOT, 0},
};
static portMUX_TYPE s_backpressure_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_dispatcher_task;
static StackType_t *s_dispatcher_stack;
static StaticTask_t s_dispatcher_storage;
#define C5_SCHEDULER_TASK_STACK_WORDS \
    ((C5_SCHEDULER_TASK_STACK + sizeof(StackType_t) - 1U) / sizeof(StackType_t))
static uint64_t s_last_diagnostic_log_ms;

/* 只使用 esp_timer uptime；C5 不依赖 Server 时间来驱动本地调度。 */
static uint64_t c5_scheduler_now_ms(void)
{
    int64_t now_us = esp_timer_get_time();
    return now_us > 0 ? (uint64_t)(now_us / 1000) : 0U;
}

static uint8_t c5_clamp_percent(uint8_t value)
{
    return value > 100U ? 100U : value;
}

static uint32_t c5_clamp_interval(uint32_t interval_ms)
{
    if (interval_ms == 0U) {
        return 1U;
    }
    if (interval_ms > C5_BACKPRESSURE_MAX_INTERVAL_MS) {
        return C5_BACKPRESSURE_MAX_INTERVAL_MS;
    }
    return interval_ms;
}

static bool c5_voice_state_is_active(voice_chain_state_t state)
{
    switch (state) {
    case VOICE_WAKE_ACK:
    case VOICE_RECORDING:
    case VOICE_WAITING_RESPONSE:
    case VOICE_PLAYING:
        return true;
    default:
        return false;
    }
}

static c5_task_priority_t c5_task_priority(c5_task_type_t task_type)
{
    switch (task_type) {
    case C5_TASK_TYPE_VOICE_HIGH:
        return C5_TASK_PRIORITY_HIGH;
    case C5_TASK_TYPE_LOW:
        return C5_TASK_PRIORITY_LOW;
    case C5_TASK_TYPE_SYSTEM_HEARTBEAT:
    case C5_TASK_TYPE_SYSTEM_STATUS:
    case C5_TASK_TYPE_SYSTEM_COMMAND_POLL:
    case C5_TASK_TYPE_RADAR_HOME_SNAPSHOT:
    case C5_TASK_TYPE_BME_SENSOR:
    case C5_TASK_TYPE_NORMAL:
    default:
        return C5_TASK_PRIORITY_NORMAL;
    }
}

const char *c5_task_type_name(c5_task_type_t task_type)
{
    switch (task_type) {
    case C5_TASK_TYPE_VOICE_HIGH:
        return "voice_high";
    case C5_TASK_TYPE_NORMAL:
        return "normal";
    case C5_TASK_TYPE_LOW:
        return "low";
    case C5_TASK_TYPE_SYSTEM_HEARTBEAT:
        return "system_heartbeat";
    case C5_TASK_TYPE_SYSTEM_STATUS:
        return "system_status";
    case C5_TASK_TYPE_SYSTEM_COMMAND_POLL:
        return "system_command_poll";
    case C5_TASK_TYPE_BME_SENSOR:
        return "bme_sensor";
    case C5_TASK_TYPE_RADAR_HOME_SNAPSHOT:
        return "radar_home_snapshot";
    default:
        return "unknown";
    }
}

void c5_backpressure_refresh(void)
{
    gateway_link_state_t gateway_state = gateway_link_get_state();
    voice_chain_state_t voice_state = voice_chain_get_state();
    bool voice_active = c5_voice_state_is_active(voice_state) ||
                        app_runtime_non_voice_is_paused();

    portENTER_CRITICAL(&s_backpressure_lock);
    s_backpressure_state.gateway_state = gateway_state;
    s_backpressure_state.voice_state = voice_state;
    s_backpressure_state.voice_active = voice_active;
    portEXIT_CRITICAL(&s_backpressure_lock);
}

void c5_backpressure_set_queue_load(uint8_t queue_load)
{
    portENTER_CRITICAL(&s_backpressure_lock);
    s_backpressure_state.queue_load = c5_clamp_percent(queue_load);
    portEXIT_CRITICAL(&s_backpressure_lock);
}

void c5_backpressure_set_cpu_idle_estimate(uint8_t cpu_idle_estimate)
{
    portENTER_CRITICAL(&s_backpressure_lock);
    s_backpressure_state.cpu_idle_estimate = c5_clamp_percent(cpu_idle_estimate);
    portEXIT_CRITICAL(&s_backpressure_lock);
}

c5_backpressure_state_t c5_backpressure_get_state(void)
{
    c5_backpressure_state_t state;

    portENTER_CRITICAL(&s_backpressure_lock);
    state = s_backpressure_state;
    portEXIT_CRITICAL(&s_backpressure_lock);

    return state;
}

bool c5_should_run(c5_task_type_t task_type)
{
    c5_backpressure_refresh();
    c5_backpressure_state_t state = c5_backpressure_get_state();
    c5_task_priority_t priority = c5_task_priority(task_type);

    if (priority == C5_TASK_PRIORITY_HIGH) {
        return true;
    }
    /*
     * 语音 turn 期间普通业务让出 socket 和 HTTP server 资源；S3 未 ready 时也不发
     * 普通上报，避免 C5 在断联阶段堆积无意义请求。
     */
    if (state.voice_active) {
        return false;
    }
    if (state.gateway_state != LINK_READY) {
        return false;
    }
    if (state.cpu_idle_estimate < C5_BACKPRESSURE_CRITICAL_CPU_IDLE) {
        return false;
    }
    if (priority == C5_TASK_PRIORITY_LOW) {
        if (state.queue_load >= C5_BACKPRESSURE_HIGH_QUEUE_LOAD ||
            state.cpu_idle_estimate < C5_BACKPRESSURE_LOW_CPU_IDLE) {
            return false;
        }
    }

    return true;
}

static uint32_t c5_base_interval_ms(c5_task_type_t task_type)
{
    switch (task_type) {
    case C5_TASK_TYPE_BME_SENSOR: {
        uint32_t period_ms = terminal_config_get_upload_period_ms();
        return period_ms > 0U ? period_ms : BME_SENSOR_READ_UPLOAD_PERIOD_MS;
    }
    case C5_TASK_TYPE_SYSTEM_HEARTBEAT:
        return SYSTEM_SERVICE_HEARTBEAT_INTERVAL_MS;
    case C5_TASK_TYPE_SYSTEM_STATUS:
        return SYSTEM_SERVICE_STATUS_INTERVAL_MS;
    case C5_TASK_TYPE_SYSTEM_COMMAND_POLL:
        return SYSTEM_SERVICE_COMMAND_POLL_INTERVAL_MS;
    case C5_TASK_TYPE_RADAR_HOME_SNAPSHOT:
        return 1000U;
    case C5_TASK_TYPE_VOICE_HIGH:
        return C5_SCHEDULER_MIN_SLEEP_MS;
    case C5_TASK_TYPE_LOW:
        return SYSTEM_SERVICE_COMMAND_POLL_INTERVAL_MS;
    case C5_TASK_TYPE_NORMAL:
    default:
        return SYSTEM_SERVICE_COMMAND_POLL_INTERVAL_MS;
    }
}

uint32_t c5_get_interval(c5_task_type_t task_type)
{
    c5_backpressure_refresh();
    c5_backpressure_state_t state = c5_backpressure_get_state();
    c5_task_priority_t priority = c5_task_priority(task_type);
    uint32_t interval_ms = c5_base_interval_ms(task_type);

    /* 回压只放大间隔，不缩短各业务模块原有 cadence。 */
    if (priority != C5_TASK_PRIORITY_HIGH && state.voice_active) {
        return c5_clamp_interval(C5_BACKPRESSURE_VOICE_BACKOFF_MS);
    }

    if (state.gateway_state != LINK_READY && priority != C5_TASK_PRIORITY_HIGH) {
        interval_ms *= 2U;
    }
    if (state.queue_load >= C5_BACKPRESSURE_HIGH_QUEUE_LOAD) {
        interval_ms *= priority == C5_TASK_PRIORITY_LOW ? 4U : 2U;
    } else if (state.queue_load >= C5_BACKPRESSURE_LOW_QUEUE_LOAD) {
        interval_ms *= priority == C5_TASK_PRIORITY_LOW ? 2U : 1U;
    }
    if (state.cpu_idle_estimate < C5_BACKPRESSURE_CRITICAL_CPU_IDLE) {
        interval_ms *= priority == C5_TASK_PRIORITY_LOW ? 4U : 2U;
    } else if (state.cpu_idle_estimate < C5_BACKPRESSURE_LOW_CPU_IDLE) {
        interval_ms *= priority == C5_TASK_PRIORITY_LOW ? 3U : 2U;
    }

    return c5_clamp_interval(interval_ms);
}

static void c5_scheduler_update_queue_load(uint64_t now_ms)
{
    size_t due_count = 0;
    size_t task_count = sizeof(s_scheduler_timers) / sizeof(s_scheduler_timers[0]);

    for (size_t i = 0; i < task_count; ++i) {
        if (s_scheduler_timers[i].next_run_ms == 0U ||
            now_ms >= s_scheduler_timers[i].next_run_ms) {
            ++due_count;
        }
    }

    uint32_t depth = c5_event_bus_queue_depth();
    uint32_t bus_load = (depth * 100U) / C5_EVENT_BUS_QUEUE_LENGTH;
    uint32_t due_load = task_count > 0U ? (uint32_t)((due_count * 100U) / task_count) : 0U;
    uint32_t load = bus_load > due_load ? bus_load : due_load;
    uint8_t queue_load = load > 100U ? 100U : (uint8_t)load;
    c5_backpressure_set_queue_load(queue_load);
}

static esp_err_t c5_scheduler_enqueue_event(c5_event_type_t event_type)
{
    return c5_event_bus_enqueue(event_type, C5_EVENT_SOURCE_TIMER);
}

void c5_scheduler_tick(void)
{
    uint64_t now_ms = c5_scheduler_now_ms();
    size_t task_count = sizeof(s_scheduler_timers) / sizeof(s_scheduler_timers[0]);

    c5_scheduler_update_queue_load(now_ms);
    for (size_t i = 0; i < task_count; ++i) {
        c5_scheduled_task_t *task = &s_scheduler_timers[i];
        if (task->next_run_ms != 0U && now_ms < task->next_run_ms) {
            continue;
        }

        esp_err_t ret = c5_scheduler_enqueue_event(task->event_type);
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG,
                     "event enqueue failed task=%s event=%s ret=%s",
                     c5_task_type_name(task->task_type),
                     c5_event_type_name(task->event_type),
                     esp_err_to_name(ret));
        }

        task->next_run_ms = now_ms + c5_get_interval(task->task_type);
    }
}

static uint32_t c5_scheduler_next_delay_ms(void)
{
    uint64_t now_ms = c5_scheduler_now_ms();
    uint64_t next_delay_ms = C5_SCHEDULER_MAX_SLEEP_MS;
    size_t task_count = sizeof(s_scheduler_timers) / sizeof(s_scheduler_timers[0]);

    for (size_t i = 0; i < task_count; ++i) {
        if (s_scheduler_timers[i].next_run_ms == 0U ||
            s_scheduler_timers[i].next_run_ms <= now_ms) {
            return C5_SCHEDULER_MIN_SLEEP_MS;
        }
        uint64_t delay_ms = s_scheduler_timers[i].next_run_ms - now_ms;
        if (delay_ms < next_delay_ms) {
            next_delay_ms = delay_ms;
        }
    }

    if (next_delay_ms < C5_SCHEDULER_MIN_SLEEP_MS) {
        next_delay_ms = C5_SCHEDULER_MIN_SLEEP_MS;
    }
    if (next_delay_ms > C5_SCHEDULER_MAX_SLEEP_MS) {
        next_delay_ms = C5_SCHEDULER_MAX_SLEEP_MS;
    }
    return (uint32_t)next_delay_ms;
}

static void c5_scheduler_update_idle_estimate(uint64_t work_start_ms, uint32_t delay_ms)
{
    uint64_t work_ms = c5_scheduler_now_ms() - work_start_ms;
    uint64_t window_ms = work_ms + delay_ms;
    uint8_t idle_estimate = window_ms > 0U ?
                            (uint8_t)((delay_ms * 100U) / window_ms) :
                            100U;
    c5_backpressure_set_cpu_idle_estimate(idle_estimate);
}

static void c5_event_dispatcher_log_diagnostics(uint64_t now_ms)
{
    if (s_last_diagnostic_log_ms != 0U &&
        now_ms - s_last_diagnostic_log_ms < C5_EVENT_DISPATCHER_DIAGNOSTIC_INTERVAL_MS) {
        return;
    }
    s_last_diagnostic_log_ms = now_ms;

    c5_event_bus_stats_t stats = {0};
    c5_event_bus_get_stats(&stats);
    ESP_LOGI(TAG,
             "event_queue_depth=%u drop_count=%u worker_latency=%u max_worker_latency=%u enqueue=%u dispatch=%u",
             (unsigned int)stats.event_queue_depth,
             (unsigned int)stats.drop_count,
             (unsigned int)stats.last_worker_latency_ms,
             (unsigned int)stats.max_worker_latency_ms,
             (unsigned int)stats.enqueue_count,
             (unsigned int)stats.dispatch_count);
}

static void c5_event_dispatcher_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG,
             "C5_EVENT_DISPATCHER started stack=%u priority=%u worker_priority=%u",
             (unsigned int)C5_SCHEDULER_TASK_STACK,
             (unsigned int)C5_SCHEDULER_TASK_PRIORITY,
             (unsigned int)C5_WORKER_TASK_PRIORITY);

    while (1) {
        uint64_t work_start_ms = c5_scheduler_now_ms();
        c5_scheduler_tick();
        while (c5_event_bus_dispatch(0) == ESP_OK) {
        }
        uint32_t delay_ms = c5_scheduler_next_delay_ms();
        c5_scheduler_update_idle_estimate(work_start_ms, delay_ms);
        c5_event_dispatcher_log_diagnostics(c5_scheduler_now_ms());

        (void)c5_event_bus_dispatch(delay_ms);
    }
}

esp_err_t c5_scheduler_start(void)
{
    if (s_dispatcher_task != NULL) {
        return ESP_OK;
    }

    esp_err_t ret = c5_event_bus_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = c5_runtime_workers_prepare();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "prepare C5 worker queues failed: %s", esp_err_to_name(ret));
        return ret;
    }

    c5_mem_log("task_create_before_c5_dispatcher");
    s_dispatcher_stack = (StackType_t *)c5_mem_alloc(C5_SCHEDULER_TASK_STACK,
                                                      C5_MEM_INTERNAL_CONTROL,
                                                      "c5_event_dispatcher_stack");
    if (s_dispatcher_stack == NULL) {
        ESP_LOGE(TAG, "allocate C5 event dispatcher stack failed");
        c5_mem_log("task_create_after_c5_dispatcher_failed");
        return ESP_ERR_NO_MEM;
    }
    s_dispatcher_task = xTaskCreateStatic(c5_event_dispatcher_task,
                                          "c5_event_dispatcher",
                                          C5_SCHEDULER_TASK_STACK_WORDS,
                                          NULL,
                                          C5_SCHEDULER_TASK_PRIORITY,
                                          s_dispatcher_stack,
                                          &s_dispatcher_storage);
    if (s_dispatcher_task == NULL) {
        c5_mem_free(s_dispatcher_stack, "c5_event_dispatcher_stack");
        s_dispatcher_stack = NULL;
        s_dispatcher_task = NULL;
        ESP_LOGE(TAG, "create static C5 event dispatcher task failed");
        c5_mem_log("task_create_after_c5_dispatcher_failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "TASK_CREATE task=c5_event_dispatcher stack=%u source=internal_static",
             (unsigned int)C5_SCHEDULER_TASK_STACK);
    c5_mem_log("task_create_after_c5_dispatcher");
    return ESP_OK;
}

bool c5_scheduler_is_dispatcher_ready(void)
{
    return s_dispatcher_task != NULL;
}

esp_err_t c5_scheduler_start_deferred_workers(void)
{
    if (s_dispatcher_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    c5_mem_log("scheduler_phase2_before_workers");
    const esp_err_t ret = c5_runtime_workers_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "C5 scheduler phase2 workers deferred ret=%s", esp_err_to_name(ret));
        c5_mem_log("scheduler_phase2_after_workers_failed");
        return ret;
    }
    c5_mem_log("scheduler_phase2_after_workers");
    return ESP_OK;
}

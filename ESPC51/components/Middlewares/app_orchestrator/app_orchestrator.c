/**
 * @file app_orchestrator.c
 * @brief C5 终端启动编排实现。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），负责把启动链路串起来：
 * app_main -> app_orchestrator_start -> LCD boot -> local sensors -> local runtime
 * -> WiFi/Gateway -> system/register/heartbeat/command -> network voice。
 * 本文件不实现 WiFi 状态机、BME 驱动、Mic/VAD、语音代理、命令协议或 radar 逻辑；
 * 这些职责分别由 wifi_manager、system_service、bme_sensor_service、voice_chain、
 * display_placeholder 和 radar_ld2450 等模块承担。
 */

#include "app_orchestrator.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"

#include "app_debug_config.h"
#include "app_main_config.h"
#include "app_stack_monitor.h"
#include "bme_sensor_service.h"
#include "c5_backpressure_controller.h"
#include "c5_memory.h"
#include "c5_resource_manager.h"
#include "gateway_link.h"
#include "lcd_service.h"
#include "radar_ble_runtime.h"
#include "radar_home_snapshot_client.h"
#include "screen_service.h"
#include "speaker_player.h"
#include "system_service.h"
#include "voice_chain.h"
#include "wifi_manager.h"

/* 日志标签：只在本文件使用，不作为调试参数。 */
static const char *TAG = "APP_MAIN";

#define C5_GATEWAY_STARTUP_WAIT_MS 15000U
#define C5_LCD_BOOTSTRAP_TASK_STACK 4096U
#define C5_LCD_BOOTSTRAP_TASK_PRIORITY 1U
#define C5_LCD_BOOTSTRAP_WAIT_MS 30000U
#define C5_DEFERRED_WORKER_TASK_STACK 4096U
#define C5_DEFERRED_WORKER_TASK_PRIORITY 1U
#define C5_LCD_EVENT_TASK_STACK 4096U
#define C5_LCD_EVENT_TASK_PRIORITY 1U
#define C5_LCD_BOOTSTRAP_TASK_STACK_WORDS \
    ((C5_LCD_BOOTSTRAP_TASK_STACK + sizeof(StackType_t) - 1U) / sizeof(StackType_t))
#define C5_DEFERRED_WORKER_TASK_STACK_WORDS \
    ((C5_DEFERRED_WORKER_TASK_STACK + sizeof(StackType_t) - 1U) / sizeof(StackType_t))
#define C5_LCD_EVENT_TASK_STACK_WORDS \
    ((C5_LCD_EVENT_TASK_STACK + sizeof(StackType_t) - 1U) / sizeof(StackType_t))
#define C5_LCD_EVENT_POLL_MS 100U
#define C5_LCD_EVENT_STACK_MONITOR_LOOPS 600U
#define C5_LCD_EVENT_STACK_LOW_BYTES 1024U
#define C5_LCD_BOOTSTRAP_RETRY_MS 1000U
#define C5_LCD_BOOTSTRAP_RETRY_MAX 5U
#define C5_LCD_BOOTSTRAP_RETRY_MAX_MS 8000U

enum {
    C5_LCD_DEGRADED_HOME_SNAPSHOT_UNAVAILABLE = 1U << 0,
};

enum {
    C5_LCD_BOOTSTRAP_READY_BIT = 1U << 0,
    C5_LCD_BOOTSTRAP_FAILED_BIT = 1U << 1,
    C5_LCD_BOOTSTRAP_DONE_BITS = C5_LCD_BOOTSTRAP_READY_BIT | C5_LCD_BOOTSTRAP_FAILED_BIT,
};

static TaskHandle_t s_lcd_bootstrap_task;
static TaskHandle_t s_orchestrator_continuation_task;
static TaskHandle_t s_deferred_worker_task;
static TaskHandle_t s_lcd_event_task;
static StackType_t s_lcd_bootstrap_stack[C5_LCD_BOOTSTRAP_TASK_STACK_WORDS];
static StackType_t *s_deferred_worker_stack;
static StackType_t *s_lcd_event_stack;
static StaticTask_t s_lcd_bootstrap_storage;
static StaticTask_t s_deferred_worker_storage;
static StaticTask_t s_lcd_event_storage;
static EventGroupHandle_t s_lcd_bootstrap_events;
static StaticEventGroup_t s_lcd_bootstrap_events_storage;
static uint32_t s_lcd_adapter_generation;
static uint32_t s_lcd_snapshot_generation;

typedef enum {
    C5_STARTUP_RESOURCE_DISPATCHER_PENDING = 0,
    C5_STARTUP_RESOURCE_DISPATCHER_READY,
    C5_STARTUP_RESOURCE_VOICE_INIT_STARTED,
    C5_STARTUP_RESOURCE_VOICE_INIT_DENIED,
} c5_startup_resource_state_t;

typedef enum {
    C5_STARTUP_BOOT = 0,
    C5_STARTUP_LCD_READY,
    C5_STARTUP_LOCAL_SENSOR_READY,
    C5_STARTUP_LOCAL_RUNTIME_READY,
    C5_STARTUP_NETWORK_START,
    C5_STARTUP_NETWORK_READY,
    C5_STARTUP_CLOUD_READY,
    C5_STARTUP_OFFLINE_MODE,
} c5_startup_stage_t;

static c5_startup_resource_state_t s_startup_resource_state =
    C5_STARTUP_RESOURCE_DISPATCHER_PENDING;
static volatile bool s_orchestrator_startup_complete;
static bool s_runtime_workers_started;
static c5_startup_stage_t s_startup_stage = C5_STARTUP_BOOT;

static void c5_orchestrator_continue(void);

static esp_err_t c5_start_runtime_workers(void)
{
    if (s_runtime_workers_started) {
        return ESP_OK;
    }
    const esp_err_t ret = c5_scheduler_start_deferred_workers();
    if (ret == ESP_OK) {
        s_runtime_workers_started = true;
    }
    return ret;
}

static void app_orchestrator_log_stack_before_wifi_manager_init(void)
{
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    uint8_t *stack_start = pxTaskGetStackStart(task);
    uint8_t *stack_end = stack_start != NULL ? stack_start + APP_STARTUP_TASK_STACK : NULL;
    const UBaseType_t high_water = uxTaskGetStackHighWaterMark(task) * sizeof(StackType_t);
    const bool stack_internal = stack_start != NULL && stack_end != NULL &&
                                esp_ptr_in_dram(stack_start) && esp_ptr_in_dram(stack_end - 1U);

    ESP_LOGI(TAG,
             "STARTUP_STACK stage=before_wifi_manager_init task=%p stack_start=%p stack_end=%p high_water=%u bytes internal=%s",
             (void *)task,
             (void *)stack_start,
             (void *)stack_end,
             (unsigned int)high_water,
             stack_internal ? "true" : "false");
    if (!stack_internal) {
        ESP_LOGE(TAG,
                 "STARTUP_STACK invalid stage=before_wifi_manager_init task=%p stack_start=%p stack_end=%p",
                 (void *)task,
                 (void *)stack_start,
                 (void *)stack_end);
    }
}

static const char *c5_startup_resource_state_name(c5_startup_resource_state_t state)
{
    switch (state) {
    case C5_STARTUP_RESOURCE_DISPATCHER_PENDING: return "dispatcher_pending";
    case C5_STARTUP_RESOURCE_DISPATCHER_READY: return "dispatcher_ready";
    case C5_STARTUP_RESOURCE_VOICE_INIT_STARTED: return "voice_init_started";
    case C5_STARTUP_RESOURCE_VOICE_INIT_DENIED: return "voice_init_denied";
    default: return "unknown";
    }
}

static void c5_startup_resource_transition(c5_startup_resource_state_t next,
                                           const char *reason,
                                           esp_err_t ret)
{
    const c5_startup_resource_state_t previous = s_startup_resource_state;
    s_startup_resource_state = next;
    ESP_LOGI(TAG,
             "C5_RESOURCE_STATE old=%s new=%s reason=%s ret=%s",
             c5_startup_resource_state_name(previous),
             c5_startup_resource_state_name(next),
             reason != NULL ? reason : "none",
             esp_err_to_name(ret));
}

static const char *c5_startup_stage_name(c5_startup_stage_t stage)
{
    switch (stage) {
    case C5_STARTUP_BOOT: return "BOOT";
    case C5_STARTUP_LCD_READY: return "LCD_READY";
    case C5_STARTUP_LOCAL_SENSOR_READY: return "LOCAL_SENSOR_READY";
    case C5_STARTUP_LOCAL_RUNTIME_READY: return "LOCAL_RUNTIME_READY";
    case C5_STARTUP_NETWORK_START: return "NETWORK_START";
    case C5_STARTUP_NETWORK_READY: return "NETWORK_READY";
    case C5_STARTUP_CLOUD_READY: return "CLOUD_READY";
    case C5_STARTUP_OFFLINE_MODE: return "OFFLINE_MODE";
    default: return "UNKNOWN";
    }
}

static void c5_startup_stage_transition(c5_startup_stage_t next,
                                        const char *reason,
                                        esp_err_t ret)
{
    const c5_startup_stage_t previous = s_startup_stage;
    s_startup_stage = next;
    ESP_LOGI(TAG,
             "BOOT_STAGE transition=%s->%s reason=%s ret=%s",
             c5_startup_stage_name(previous),
             c5_startup_stage_name(next),
             reason != NULL ? reason : "none",
             esp_err_to_name(ret));
}

static uint64_t c5_lcd_now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000U;
}

static TickType_t c5_block_ticks(uint32_t delay_ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(delay_ms);
    return ticks == 0U ? 1U : ticks;
}

static void c5_lcd_bootstrap_signal(EventBits_t result_bit)
{
    if (s_lcd_bootstrap_events != NULL) {
        (void)xEventGroupSetBits(s_lcd_bootstrap_events, result_bit);
    }
}

static lcd_voice_state_t c5_lcd_voice_state(voice_chain_state_t state)
{
    switch (state) {
    case VOICE_WAKE_ACK: return LCD_VOICE_WAKE;
    case VOICE_RECORDING: return LCD_VOICE_RECORDING;
    case VOICE_WAITING_RESPONSE: return LCD_VOICE_WAITING;
    case VOICE_PLAYING: return LCD_VOICE_PLAYING;
    case VOICE_ERROR: return LCD_VOICE_ERROR;
    case VOICE_IDLE:
    case VOICE_LISTENING:
    default: return LCD_VOICE_IDLE;
    }
}

static esp_err_t c5_lcd_submit_screen_command(void *context,
                                               const screen_service_command_t *command)
{
    (void)context;
    if (command == NULL || !command->valid) {
        return ESP_ERR_INVALID_ARG;
    }

    lcd_command_t lcd_command = {0};
    lcd_command.generation = command->generation;
    strlcpy(lcd_command.title, command->title, sizeof(lcd_command.title));
    strlcpy(lcd_command.text, command->clear ? "" : command->text, sizeof(lcd_command.text));
    const uint64_t now_ms = c5_lcd_now_ms();
    const uint64_t ttl_ms = command->clear || command->expires_at_ms <= now_ms ? 1U :
                            command->expires_at_ms - now_ms;
    lcd_command.ttl_ms = ttl_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ttl_ms;
    return lcd_service_post_command(&lcd_command);
}

static void c5_lcd_publish_snapshot(void)
{
    lcd_system_snapshot_t snapshot = {0};
    snapshot.timestamp_ms = c5_lcd_now_ms();
    snapshot.generation = ++s_lcd_snapshot_generation;
    if (snapshot.generation == 0U) {
        snapshot.generation = ++s_lcd_snapshot_generation;
    }
    snapshot.wifi_state = wifi_is_connected() ? LCD_WIFI_ONLINE : LCD_WIFI_OFFLINE;
    snapshot.gateway_online = gateway_link_is_ready();

    bme_sensor_service_reading_t bme = {0};
    bme_sensor_service_get_latest_reading(&bme);
    snapshot.bme_valid = bme.valid;
    snapshot.temperature_c = bme.temperature_c;
    snapshot.humidity_percent = bme.humidity_percent;
    snapshot.pressure_hpa = bme.pressure_hpa;
    snapshot.gas_resistance_ohm = bme.gas_resistance_ohm;
    snapshot.gas_valid = bme.gas_valid;
    snapshot.air_quality_valid = bme.air_quality_valid;
    snapshot.iaq = bme.air_quality_score;

    radar_home_snapshot_t home = {0};
    if (radar_home_snapshot_client_get(&home)) {
        for (uint8_t index = 0U; index < home.source_count; ++index) {
            const radar_home_snapshot_source_t *const source = &home.sources[index];
            if (source->source_id >= LCD_SYSTEM_SNAPSHOT_RADAR_SOURCES) {
                continue;
            }
            lcd_radar_source_snapshot_t *const lcd_source =
                &snapshot.radar_sources[source->source_id];
            lcd_source->healthy = source->online;
            lcd_source->presence = source->occupied;
            lcd_source->motion = source->motion == RADAR_HOME_MOTION_MOVING;
            lcd_source->person_count = source->person_count;
            if (source->source_id == RADAR_HOME_SNAPSHOT_LOCAL_SOURCE_ID) {
                snapshot.room_occupied = source->occupied;
            }
        }
        snapshot.home_occupied = home.home_occupied;
        snapshot.home_person_count = home.home_person_count;
    } else {
        snapshot.degraded_flags |= C5_LCD_DEGRADED_HOME_SNAPSHOT_UNAVAILABLE;
    }

    const voice_chain_state_t voice_state = voice_chain_get_state();
    snapshot.voice_state = c5_lcd_voice_state(voice_state);
    snapshot.speaker_active = voice_state == VOICE_PLAYING;
    snapshot.wake_allowed = voice_state == VOICE_LISTENING && gateway_link_is_ready() &&
                            !c5_resource_manager_is_voice_exclusive();
    (void)lcd_service_post_snapshot(&snapshot);
}

static void c5_lcd_event_note_stack(const char *stage,
                                    uint32_t loop_count,
                                    uint32_t event_count,
                                    UBaseType_t *minimum_free_bytes,
                                    uint32_t *last_low_log_loop)
{
    const UBaseType_t free_bytes = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
    if (*minimum_free_bytes == 0U || free_bytes < *minimum_free_bytes) {
        *minimum_free_bytes = free_bytes;
    }

    if (stage != NULL) {
        ESP_LOGI(TAG,
                 "LCD_EVENTS_STACK stage=%s free_min_bytes=%u event_count=%lu",
                 stage,
                 (unsigned int)*minimum_free_bytes,
                 (unsigned long)event_count);
    }
    if (*minimum_free_bytes < C5_LCD_EVENT_STACK_LOW_BYTES &&
        (loop_count == 0U || loop_count - *last_low_log_loop >= C5_LCD_EVENT_STACK_MONITOR_LOOPS)) {
        ESP_LOGW(TAG,
                 "LCD_EVENTS_STACK_LOW free_min_bytes=%u event_count=%lu",
                 (unsigned int)*minimum_free_bytes,
                 (unsigned long)event_count);
        *last_low_log_loop = loop_count;
    }
}

static void c5_lcd_event_task(void *arg)
{
    (void)arg;
    uint32_t loop_count = 0U;
    uint32_t event_count = 0U;
    uint32_t last_low_log_loop = 0U;
    UBaseType_t minimum_free_bytes = 0U;
    c5_lcd_event_note_stack("entry", loop_count, event_count,
                            &minimum_free_bytes, &last_low_log_loop);
    (void)app_runtime_guard_check_heap_integrity(TAG, "lcd_events_entry");
    while (lcd_service_is_started()) {
        lcd_wake_event_t wake_event = {0};
        while (lcd_service_take_wake_event(&wake_event)) {
            ++event_count;
            const voice_chain_state_t state = voice_chain_get_state();
            if (state != VOICE_LISTENING || c5_resource_manager_is_voice_exclusive()) {
                ESP_LOGW(TAG,
                         "LCD_WAKE_EVENT_DROPPED generation=%lu voice=%s exclusive=%u",
                         (unsigned long)wake_event.generation,
                         voice_chain_state_name(state),
                         c5_resource_manager_is_voice_exclusive() ? 1U : 0U);
                continue;
            }
            const esp_err_t wake_ret = voice_chain_request_local_wake();
            if (wake_ret != ESP_OK) {
                ESP_LOGW(TAG, "LCD_WAKE_EVENT_REJECTED ret=%s", esp_err_to_name(wake_ret));
            }
        }
        c5_lcd_publish_snapshot();
        ++loop_count;
        if ((loop_count % C5_LCD_EVENT_STACK_MONITOR_LOOPS) == 0U) {
            c5_lcd_event_note_stack("periodic", loop_count, event_count,
                                    &minimum_free_bytes, &last_low_log_loop);
            (void)app_runtime_guard_check_heap_integrity(TAG, "lcd_events_periodic");
        } else {
            c5_lcd_event_note_stack(NULL, loop_count, event_count,
                                    &minimum_free_bytes, &last_low_log_loop);
        }
        vTaskDelay(c5_block_ticks(C5_LCD_EVENT_POLL_MS));
    }
    s_lcd_event_task = NULL;
    vTaskDelete(NULL);
}

static bool c5_lcd_bootstrap_retryable(esp_err_t ret)
{
    /* A just-deleted startup task is reaped by Idle; wait for actual capacity. */
    return ret == ESP_ERR_INVALID_STATE || ret == ESP_ERR_NO_MEM;
}

static uint32_t c5_lcd_bootstrap_retry_delay_ms(uint32_t failed_attempt)
{
    uint32_t delay_ms = C5_LCD_BOOTSTRAP_RETRY_MS;
    while (failed_attempt > 1U && delay_ms < C5_LCD_BOOTSTRAP_RETRY_MAX_MS) {
        delay_ms <<= 1U;
        --failed_attempt;
    }
    return delay_ms > C5_LCD_BOOTSTRAP_RETRY_MAX_MS ?
               C5_LCD_BOOTSTRAP_RETRY_MAX_MS : delay_ms;
}

static esp_err_t c5_lcd_start_once(void)
{
    esp_err_t ret = ESP_OK;
    /* lcd_service owns its exact DMA/PSRAM admission checks; do not reject on a broad heap gate. */
    if (c5_resource_manager_is_voice_exclusive()) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK) {
        ret = lcd_service_start();
    }
    if (ret == ESP_OK) {
        const screen_service_lcd_adapter_t adapter = {
            .submit = c5_lcd_submit_screen_command,
            .context = NULL,
        };
        ret = screen_service_attach_lcd(&adapter);
        s_lcd_adapter_generation = screen_service_lcd_generation();
    }
    if (ret == ESP_OK) {
        c5_lcd_publish_snapshot();
        c5_mem_log("task_create_before_lcd_events");
        if (s_lcd_event_stack == NULL) {
            s_lcd_event_stack = (StackType_t *)c5_mem_alloc(C5_LCD_EVENT_TASK_STACK,
                                                             C5_MEM_PSRAM,
                                                             "lcd_events_stack");
        }
        if (s_lcd_event_stack == NULL) {
            ret = ESP_ERR_NO_MEM;
        } else {
            s_lcd_event_task = xTaskCreateStatic(c5_lcd_event_task,
                                                  "lcd_events",
                                                  C5_LCD_EVENT_TASK_STACK_WORDS,
                                                  NULL,
                                                  C5_LCD_EVENT_TASK_PRIORITY,
                                                  s_lcd_event_stack,
                                                  &s_lcd_event_storage);
            if (s_lcd_event_task == NULL) {
                c5_mem_free(s_lcd_event_stack, "lcd_events_stack");
                s_lcd_event_stack = NULL;
                ret = ESP_ERR_NO_MEM;
            } else {
                ESP_LOGI(TAG, "TASK_CREATE task=lcd_events stack=%u source=psram_static",
                         (unsigned int)C5_LCD_EVENT_TASK_STACK);
            }
        }
        c5_mem_log(ret == ESP_OK ? "task_create_after_lcd_events" :
                                  "task_create_after_lcd_events_failed");
    }
    return ret;
}

static void c5_lcd_bootstrap_task(void *arg)
{
    (void)arg;
    const uint64_t started_ms = c5_lcd_now_ms();
    ESP_LOGI(TAG,
             "BOOT_STAGE stage=lcd_bootstrap state=start priority=%u stack=%u source=internal_static",
             (unsigned int)C5_LCD_BOOTSTRAP_TASK_PRIORITY,
             (unsigned int)C5_LCD_BOOTSTRAP_TASK_STACK);

    /* Keep this task display-only: it must not delay the existing boot animation. */
    esp_err_t final_ret = ESP_ERR_INVALID_STATE;
    for (uint32_t attempt = 1U;
         !lcd_service_is_started() && attempt <= C5_LCD_BOOTSTRAP_RETRY_MAX;
         ++attempt) {
        c5_mem_log("lcd_bootstrap_before");
        const esp_err_t ret = c5_lcd_start_once();
        final_ret = ret;
        if (ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "LCD_START_FAILED attempt=%lu/%u ret=%s",
                     (unsigned long)attempt,
                     (unsigned int)C5_LCD_BOOTSTRAP_RETRY_MAX,
                     esp_err_to_name(ret));
            if (s_lcd_adapter_generation != 0U) {
                screen_service_detach_lcd(s_lcd_adapter_generation);
                s_lcd_adapter_generation = 0U;
            }
            const esp_err_t stop_ret = lcd_service_stop();
            if (stop_ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "LCD_START_CLEANUP_DEFERRED attempt=%lu ret=%s",
                         (unsigned long)attempt,
                         esp_err_to_name(stop_ret));
            }
            c5_mem_log("lcd_bootstrap_after_failed");
            if (c5_lcd_bootstrap_retryable(ret) && attempt < C5_LCD_BOOTSTRAP_RETRY_MAX) {
                const uint32_t retry_ms = c5_lcd_bootstrap_retry_delay_ms(attempt);
                ESP_LOGW(TAG,
                         "LCD_BOOTSTRAP_RETRY attempt=%lu/%u ret=%s retry_ms=%u",
                         (unsigned long)attempt,
                         (unsigned int)C5_LCD_BOOTSTRAP_RETRY_MAX,
                         esp_err_to_name(ret),
                         (unsigned int)retry_ms);
                vTaskDelay(c5_block_ticks(retry_ms));
                continue;
            }
            break;
        }
        ESP_LOGI(TAG, "LCD_START_OK legacy_dma_released=1 draw_dma_bytes=4800");
        final_ret = ESP_OK;
        c5_mem_log("lcd_bootstrap_after");
    }

    const uint64_t elapsed_ms = c5_lcd_now_ms() - started_ms;
    const UBaseType_t stack_hwm = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
    const bool lcd_ready = lcd_service_is_started();
    s_lcd_bootstrap_task = NULL;
    if (lcd_ready) {
        ESP_LOGI(TAG,
                 "BOOT_STAGE stage=lcd_bootstrap state=ready elapsed_ms=%llu priority=%u stack_source=internal_static stack_hwm=%u",
                 (unsigned long long)elapsed_ms,
                 (unsigned int)C5_LCD_BOOTSTRAP_TASK_PRIORITY,
                 (unsigned int)stack_hwm);
        if (s_orchestrator_startup_complete) {
            const esp_err_t boot_complete_ret = lcd_service_mark_boot_complete();
            if (boot_complete_ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "LCD_BOOT_COMPLETE_LATE_DEFERRED ret=%s",
                         esp_err_to_name(boot_complete_ret));
            } else {
                ESP_LOGI(TAG, "LCD_BOOT_COMPLETE_LATE_LATCHED");
            }
        }
        c5_lcd_bootstrap_signal(C5_LCD_BOOTSTRAP_READY_BIT);
    } else {
        ESP_LOGE(TAG,
                 "BOOT_STAGE stage=lcd_bootstrap state=failed err=%s elapsed_ms=%llu priority=%u stack_source=internal_static stack_hwm=%u",
                 esp_err_to_name(final_ret),
                 (unsigned long long)elapsed_ms,
                 (unsigned int)C5_LCD_BOOTSTRAP_TASK_PRIORITY,
                 (unsigned int)stack_hwm);
        ESP_LOGE(TAG,
                 "LCD_BOOTSTRAP_DEGRADED retries=%u; background services remain independent",
                 (unsigned int)C5_LCD_BOOTSTRAP_RETRY_MAX);
        c5_mem_log("lcd_bootstrap_degraded");
        c5_lcd_bootstrap_signal(C5_LCD_BOOTSTRAP_FAILED_BIT);
    }
    vTaskDelete(NULL);
}

static esp_err_t c5_schedule_lcd_bootstrap(void)
{
    if (s_lcd_bootstrap_task != NULL || lcd_service_is_started()) {
        c5_lcd_bootstrap_signal(C5_LCD_BOOTSTRAP_READY_BIT);
        return ESP_OK;
    }
    if (!esp_ptr_internal(s_lcd_bootstrap_stack)) {
        ESP_LOGE(TAG,
                 "LCD_BOOTSTRAP_TASK_FAILED ret=%s reason=stack_not_internal",
                 esp_err_to_name(ESP_ERR_INVALID_STATE));
        c5_lcd_bootstrap_signal(C5_LCD_BOOTSTRAP_FAILED_BIT);
        return ESP_ERR_INVALID_STATE;
    }
    c5_mem_log("task_create_before_lcd_bootstrap");
    s_lcd_bootstrap_task = xTaskCreateStatic(c5_lcd_bootstrap_task,
                                             "lcd_bootstrap",
                                             C5_LCD_BOOTSTRAP_TASK_STACK_WORDS,
                                             NULL,
                                             C5_LCD_BOOTSTRAP_TASK_PRIORITY,
                                             s_lcd_bootstrap_stack,
                                             &s_lcd_bootstrap_storage);
    if (s_lcd_bootstrap_task == NULL) {
        s_lcd_bootstrap_task = NULL;
        ESP_LOGE(TAG, "LCD_BOOTSTRAP_TASK_FAILED ret=%s", esp_err_to_name(ESP_ERR_NO_MEM));
        c5_mem_log("task_create_after_lcd_bootstrap_failed");
        c5_lcd_bootstrap_signal(C5_LCD_BOOTSTRAP_FAILED_BIT);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "TASK_CREATE task=lcd_bootstrap stack=%u priority=%u source=internal_static",
             (unsigned int)C5_LCD_BOOTSTRAP_TASK_STACK,
             (unsigned int)C5_LCD_BOOTSTRAP_TASK_PRIORITY);
    c5_mem_log("task_create_after_lcd_bootstrap");
    return ESP_OK;
}

static void c5_orchestrator_continuation_task(void *arg)
{
    (void)arg;
    const EventBits_t bits = xEventGroupWaitBits(s_lcd_bootstrap_events,
                                                 C5_LCD_BOOTSTRAP_DONE_BITS,
                                                 pdFALSE,
                                                 pdFALSE,
                                                 c5_block_ticks(C5_LCD_BOOTSTRAP_WAIT_MS));
    if ((bits & C5_LCD_BOOTSTRAP_READY_BIT) != 0U) {
        ESP_LOGI(TAG, "BOOT_STAGE stage=orchestrator state=continuing lcd=ready");
    } else if ((bits & C5_LCD_BOOTSTRAP_FAILED_BIT) != 0U) {
        ESP_LOGW(TAG, "BOOT_STAGE stage=orchestrator state=continuing lcd=failed_degraded");
    } else {
        ESP_LOGE(TAG,
                 "BOOT_STAGE stage=orchestrator state=continuing lcd=timeout timeout_ms=%u",
                 (unsigned int)C5_LCD_BOOTSTRAP_WAIT_MS);
    }

    c5_orchestrator_continue();
    ESP_LOGI(TAG,
             "BOOT_STAGE stage=orchestrator state=complete priority=%u stack_source=internal_dynamic_reclaimable stack_hwm=%u",
             (unsigned int)APP_STARTUP_TASK_PRIORITY,
             (unsigned int)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    s_orchestrator_continuation_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t c5_schedule_orchestrator_continuation(void)
{
    if (s_orchestrator_continuation_task != NULL) {
        return ESP_OK;
    }
    const BaseType_t created = xTaskCreateWithCaps(c5_orchestrator_continuation_task,
                                                   "app_orchestrator",
                                                   APP_STARTUP_TASK_STACK,
                                                   NULL,
                                                   APP_STARTUP_TASK_PRIORITY,
                                                   &s_orchestrator_continuation_task,
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (created != pdPASS || s_orchestrator_continuation_task == NULL) {
        s_orchestrator_continuation_task = NULL;
        ESP_LOGE(TAG,
                 "BOOT_STAGE stage=orchestrator state=failed err=%s reason=continuation_task_create",
                 esp_err_to_name(ESP_ERR_NO_MEM));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "TASK_CREATE task=app_orchestrator stack=%u priority=%u source=internal_dynamic_reclaimable",
             (unsigned int)APP_STARTUP_TASK_STACK,
             (unsigned int)APP_STARTUP_TASK_PRIORITY);
    return ESP_OK;
}

static void c5_deferred_worker_task(void *arg)
{
    (void)arg;

    /* Phase 2 is deliberately separate from display startup and runs after app_startup exits. */
    c5_mem_log("scheduler_phase2_bootstrap_before");
    const esp_err_t ret = c5_start_runtime_workers();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "STARTUP_PHASE_DEGRADED phase=scheduler_workers ret=%s",
                 esp_err_to_name(ret));
        c5_mem_log("scheduler_phase2_bootstrap_failed");
    } else {
        ESP_LOGI(TAG, "STARTUP_PHASE_READY phase=scheduler_workers");
        c5_mem_log("scheduler_phase2_bootstrap_after");
    }
    s_deferred_worker_task = NULL;
    vTaskDelete(NULL);
}

static void c5_schedule_deferred_workers(void)
{
    if (s_runtime_workers_started || s_deferred_worker_task != NULL ||
        !c5_scheduler_is_dispatcher_ready()) {
        return;
    }
    c5_mem_log("task_create_before_deferred_workers");
    if (s_deferred_worker_stack == NULL) {
        s_deferred_worker_stack = (StackType_t *)c5_mem_alloc(C5_DEFERRED_WORKER_TASK_STACK,
                                                               C5_MEM_PSRAM,
                                                               "deferred_worker_stack");
    }
    if (s_deferred_worker_stack == NULL) {
        ESP_LOGW(TAG,
                 "DEFERRED_WORKER_TASK_FAILED ret=%s",
                 esp_err_to_name(ESP_ERR_NO_MEM));
        c5_mem_log("task_create_after_deferred_workers_failed");
        return;
    }
    s_deferred_worker_task = xTaskCreateStatic(c5_deferred_worker_task,
                                               "deferred_workers",
                                               C5_DEFERRED_WORKER_TASK_STACK_WORDS,
                                               NULL,
                                               C5_DEFERRED_WORKER_TASK_PRIORITY,
                                               s_deferred_worker_stack,
                                               &s_deferred_worker_storage);
    if (s_deferred_worker_task == NULL) {
        c5_mem_free(s_deferred_worker_stack, "deferred_worker_stack");
        s_deferred_worker_stack = NULL;
        ESP_LOGW(TAG,
                 "DEFERRED_WORKER_TASK_FAILED ret=%s",
                 esp_err_to_name(ESP_ERR_NO_MEM));
        c5_mem_log("task_create_after_deferred_workers_failed");
        return;
    }
    ESP_LOGI(TAG,
             "TASK_CREATE task=deferred_workers stack=%u source=psram_static",
             (unsigned int)C5_DEFERRED_WORKER_TASK_STACK);
    c5_mem_log("task_create_after_deferred_workers");
}

static void c5_orchestrator_continue(void)
{
    char connected_ssid[33] = {0};
    bool gateway_ready = false;
    esp_err_t scheduler_ret = ESP_ERR_INVALID_STATE;

    app_stack_monitor_log(TAG, "app_startup_task", "orchestrator_enter");
    c5_mem_log("startup");

    /* LCD completion/timeout has already released this continuation. */
    c5_startup_stage_transition(C5_STARTUP_LCD_READY, "lcd_bootstrap_complete", ESP_OK);

    /* Local sensors are independent of Wi-Fi and degrade individually. */
    ESP_LOGI(TAG, "BOOT_STAGE stage=local_sensor state=start");
    if (MAIN_ENABLE_BME_SERVICE) {
        const esp_err_t bme_ret = bme_sensor_service_start();
        ESP_LOGI(TAG, "BOOT_STAGE stage=bme690 state=%s ret=%s",
                 bme_ret == ESP_OK ? "ready" : "degraded", esp_err_to_name(bme_ret));
    } else {
        ESP_LOGW(TAG, "BOOT_STAGE stage=bme690 state=disabled reason=MAIN_ENABLE_BME_SERVICE");
    }
    const esp_err_t radar_ble_ret = radar_ble_runtime_start();
    if (radar_ble_ret != ESP_OK) {
        ESP_LOGW(TAG, "BOOT_STAGE stage=radar_ble state=degraded ret=%s",
                 esp_err_to_name(radar_ble_ret));
    } else {
        ESP_LOGI(TAG, "BOOT_STAGE stage=radar_ble state=ready ble_runtime=enabled");
    }
    c5_startup_stage_transition(C5_STARTUP_LOCAL_SENSOR_READY, "sensor_start_attempts_complete", ESP_OK);
    app_stack_monitor_log(TAG, "app_startup_task", "after_local_sensor_start");

    /* EventBus, queues, handlers and dispatcher must exist before any network work. */
    scheduler_ret = c5_scheduler_start();
    if (scheduler_ret != ESP_OK) {
        ESP_LOGE(TAG, "C5 runtime dispatcher start failed: %s", esp_err_to_name(scheduler_ret));
        c5_startup_resource_transition(C5_STARTUP_RESOURCE_VOICE_INIT_DENIED,
                                       "dispatcher_unavailable", scheduler_ret);
    } else {
        const esp_err_t workers_ret = c5_start_runtime_workers();
        if (workers_ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "STARTUP_PHASE_DEGRADED phase=scheduler_workers ret=%s",
                     esp_err_to_name(workers_ret));
        } else {
            ESP_LOGI(TAG, "STARTUP_PHASE_READY phase=scheduler_workers before_network");
        }
        c5_startup_resource_transition(C5_STARTUP_RESOURCE_DISPATCHER_READY,
                                       "dispatcher_created_before_network", ESP_OK);
        c5_startup_stage_transition(C5_STARTUP_LOCAL_RUNTIME_READY,
                                     "event_bus_queue_handler_dispatcher_ready", ESP_OK);
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_local_runtime_start");

    /* Start local MIC/ADC/VAD now; PCM transport remains gated at voice-turn time. */
    if (MAIN_ENABLE_MIC_CHAIN && c5_scheduler_is_dispatcher_ready()) {
        c5_startup_resource_transition(C5_STARTUP_RESOURCE_VOICE_INIT_STARTED,
                                       "local_voice_admitted", ESP_OK);
        const esp_err_t voice_ret = voice_chain_start();
        if (voice_ret != ESP_OK) {
            ESP_LOGW(TAG, "BOOT_STAGE stage=local_voice state=degraded ret=%s",
                     esp_err_to_name(voice_ret));
            c5_startup_resource_transition(C5_STARTUP_RESOURCE_VOICE_INIT_DENIED,
                                           "local_voice_start_failed", voice_ret);
        } else {
            ESP_LOGI(TAG, "BOOT_STAGE stage=local_voice state=ready network_pcm=deferred");
        }
    } else if (MAIN_ENABLE_MIC_CHAIN) {
        ESP_LOGW(TAG, "BOOT_STAGE stage=local_voice state=degraded reason=dispatcher_unavailable");
    } else {
        ESP_LOGI(TAG, "Mic chain disabled by MAIN_ENABLE_MIC_CHAIN");
    }

    /* Network is a bounded, recoverable phase. Failure leaves local mode alive. */
    c5_startup_stage_transition(C5_STARTUP_NETWORK_START, "local_runtime_ready", ESP_OK);
    app_stack_monitor_log(TAG, "app_startup_task", "before_wifi_manager_init");
    app_stack_monitor_log_system_state(TAG, "before_wifi_manager_init");
    app_runtime_guard_check_heap_integrity(TAG, "before_wifi_manager_init");
    app_orchestrator_log_stack_before_wifi_manager_init();

    esp_err_t network_ret = wifi_manager_init();
    app_runtime_guard_check_heap_integrity(TAG, "after_wifi_manager_init");
    if (network_ret == ESP_OK) {
        network_ret = gateway_link_start();
        if (network_ret == ESP_OK) {
            network_ret = wifi_connect_to_ap();
        }
        if (network_ret == ESP_OK && wifi_get_connected_ssid(connected_ssid, sizeof(connected_ssid))) {
            ESP_LOGI(TAG, "WiFi connected: %s", connected_ssid);
        }
        if (network_ret == ESP_OK && wifi_is_stable()) {
            network_ret = gateway_link_wait_ready(C5_GATEWAY_STARTUP_WAIT_MS);
            gateway_ready = network_ret == ESP_OK;
        }
    }
    if (network_ret != ESP_OK) {
        ESP_LOGW(TAG, "NETWORK_DEGRADED ret=%s; entering offline-capable mode",
                 esp_err_to_name(network_ret));
        c5_startup_stage_transition(C5_STARTUP_OFFLINE_MODE, "wifi_or_gateway_unavailable", network_ret);
    } else {
        c5_startup_stage_transition(C5_STARTUP_NETWORK_READY, "wifi_connected", ESP_OK);
    }
    app_stack_monitor_log(TAG,
                          "app_startup_task",
                          gateway_ready ? "after_gateway_link_ready" : "gateway_degraded");
    ESP_LOGI(TAG,
             "heap summary: free=%u min_free=%u largest_8bit_block=%u",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    const esp_err_t system_ret = system_service_init();
    if (system_ret != ESP_OK) {
        ESP_LOGW(TAG, "Local gateway system service init degraded: %s", esp_err_to_name(system_ret));
    } else if (gateway_ready) {
        c5_startup_stage_transition(C5_STARTUP_CLOUD_READY, "system_service_ready", ESP_OK);
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_system_service_init");

    if (MAIN_ENABLE_SPEAKER_SELF_TEST) {
        /*
         * Speaker 自检直接走 speaker_player/IIS，不经过 server voice。
         * 用于区分硬件/IIS/功放问题和服务器 PCM/播放路径问题。
         */
        esp_err_t speaker_test_ret =
            audio_player_self_test_1khz(MAIN_SPEAKER_SELF_TEST_DURATION_MS);
        if (speaker_test_ret != ESP_OK) {
            ESP_LOGE(TAG, "Speaker self-test failed: %s", esp_err_to_name(speaker_test_ret));
        } else {
            ESP_LOGI(TAG, "Speaker self-test done");
        }
        esp_err_t speaker_release_ret =
            audio_player_release_session(AUDIO_PLAYER_DRAIN_TIMEOUT_MS);
        if (speaker_release_ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "Speaker self-test session release failed: %s",
                     esp_err_to_name(speaker_release_ret));
        }
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_speaker_self_test_gate");

    app_stack_monitor_log(TAG, "app_startup_task", "after_c5_scheduler_start");

    /* A late LCD READY observes this latch and performs the same one-way transition. */
    s_orchestrator_startup_complete = true;

    /* The display service performs the one-way boot-to-Home transition on its UI task. */
    const esp_err_t boot_complete_ret = lcd_service_mark_boot_complete();
    if (boot_complete_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "LCD_BOOT_COMPLETE_DEFERRED ret=%s",
                 esp_err_to_name(boot_complete_ret));
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_voice_chain_start");

    /* Phase-2 workers wait for this task to exit and release its internal stack. */
    c5_schedule_deferred_workers();
    ESP_LOGI(TAG, "C51_BOOT_STAGE stage=before_startup_task_exit");

    // All long-running services own their background tasks. app_startup's dynamic
    // internal stack is reclaimed by FreeRTOS after this task deletes itself.
    app_stack_monitor_log(TAG, "app_startup_task", "startup_complete");
}

void app_orchestrator_start(void)
{
    app_stack_monitor_log(TAG, "app_startup_task", "orchestrator_schedule_enter");
    if (s_orchestrator_continuation_task != NULL) {
        ESP_LOGW(TAG, "BOOT_STAGE stage=orchestrator state=already_scheduled");
        return;
    }
    s_orchestrator_startup_complete = false;
    if (s_lcd_bootstrap_events == NULL) {
        s_lcd_bootstrap_events = xEventGroupCreateStatic(&s_lcd_bootstrap_events_storage);
    }
    if (s_lcd_bootstrap_events == NULL) {
        ESP_LOGE(TAG,
                 "BOOT_STAGE stage=orchestrator state=failed err=%s reason=lcd_event_group_create",
                 esp_err_to_name(ESP_ERR_NO_MEM));
        return;
    }
    (void)xEventGroupClearBits(s_lcd_bootstrap_events, C5_LCD_BOOTSTRAP_DONE_BITS);

    /* Do not delay-poll here: priority 3 would repeatedly preempt priority-1 LCD on C5. */
    const esp_err_t continuation_ret = c5_schedule_orchestrator_continuation();
    const esp_err_t lcd_ret = c5_schedule_lcd_bootstrap();
    if (lcd_ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "BOOT_STAGE stage=lcd_bootstrap state=failed err=%s reason=task_create",
                 esp_err_to_name(lcd_ret));
    }
    if (continuation_ret != ESP_OK) {
        /* Keep the LCD-first experience alive, but no business startup can run without its internal stack. */
        ESP_LOGE(TAG,
                 "STARTUP_DEGRADED stage=orchestrator reason=continuation_unavailable lcd_task_ret=%s",
                 esp_err_to_name(lcd_ret));
    }
    app_stack_monitor_log(TAG, "app_startup_task", "orchestrator_schedule_return");
}

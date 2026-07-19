#ifndef APP_STACK_MONITOR_H
#define APP_STACK_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef APP_STACK_LOW_WATER_WARNING_BYTES
#define APP_STACK_LOW_WATER_WARNING_BYTES 1536U
#endif

#ifndef APP_STACK_MONITOR_INTERVAL_MS
#define APP_STACK_MONITOR_INTERVAL_MS 30000U
#endif

#ifndef APP_HEAP_MONITOR_INTERVAL_MS
#define APP_HEAP_MONITOR_INTERVAL_MS 30000U
#endif

#ifndef APP_HEAP_LOW_FREE_WARNING_BYTES
#define APP_HEAP_LOW_FREE_WARNING_BYTES (32U * 1024U)
#endif

#ifndef APP_HEAP_LOW_LARGEST_WARNING_BYTES
#define APP_HEAP_LOW_LARGEST_WARNING_BYTES (8U * 1024U)
#endif

static inline const char *app_stack_monitor_safe_text(const char *value)
{
    return value != NULL ? value : "(null)";
}

static inline UBaseType_t app_stack_monitor_high_water(void)
{
#if (INCLUDE_uxTaskGetStackHighWaterMark == 1)
    const UBaseType_t high_water_words = uxTaskGetStackHighWaterMark(NULL);
    return high_water_words * (UBaseType_t)sizeof(StackType_t);
#else
    return 0;
#endif
}

static inline int64_t app_stack_monitor_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static inline void app_stack_monitor_log_free_bytes(const char *tag,
                                                    const char *task_name,
                                                    const char *stage,
                                                    UBaseType_t free_bytes)
{
    const char *safe_tag = app_stack_monitor_safe_text(tag);
    const char *safe_task = app_stack_monitor_safe_text(task_name);
    const char *safe_stage = app_stack_monitor_safe_text(stage);

    ESP_LOGI(safe_tag,
             "TASK_STACK_MONITOR task=%s free_bytes=%u stage=%s",
             safe_task,
             (unsigned int)free_bytes,
             safe_stage);
    if (free_bytes > 0 && free_bytes < APP_STACK_LOW_WATER_WARNING_BYTES) {
        ESP_LOGW(safe_tag,
                 "TASK_STACK_MONITOR task=%s free_bytes=%u warning=low_stack threshold=%u stage=%s",
                 safe_task,
                 (unsigned int)free_bytes,
                 (unsigned int)APP_STACK_LOW_WATER_WARNING_BYTES,
                 safe_stage);
    }
}

static inline UBaseType_t app_stack_monitor_log(const char *tag,
                                                const char *task_name,
                                                const char *stage)
{
    UBaseType_t high_water = app_stack_monitor_high_water();
    app_stack_monitor_log_free_bytes(tag, task_name, stage, high_water);
    return high_water;
}

static inline UBaseType_t app_stack_monitor_log_periodic(const char *tag,
                                                         const char *task_name,
                                                         int64_t *last_log_ms,
                                                         uint32_t interval_ms)
{
    if (last_log_ms == NULL) {
        return app_stack_monitor_log(tag, task_name, "periodic");
    }

    const int64_t timestamp_ms = app_stack_monitor_now_ms();
    const uint32_t effective_interval =
        interval_ms > 0 ? interval_ms : APP_STACK_MONITOR_INTERVAL_MS;
    if (*last_log_ms != 0 &&
        timestamp_ms - *last_log_ms < (int64_t)effective_interval) {
        return app_stack_monitor_high_water();
    }
    *last_log_ms = timestamp_ms;

    return app_stack_monitor_log(tag, task_name, "periodic");
}

static inline void app_heap_monitor_log(const char *tag)
{
    const char *safe_tag = app_stack_monitor_safe_text(tag);
    const size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t largest_bytes = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const size_t minimum_bytes = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);

    ESP_LOGI(safe_tag,
             "HEAP_MONITOR free=%u largest=%u min=%u",
             (unsigned int)free_bytes,
             (unsigned int)largest_bytes,
             (unsigned int)minimum_bytes);
    if (free_bytes < APP_HEAP_LOW_FREE_WARNING_BYTES ||
        largest_bytes < APP_HEAP_LOW_LARGEST_WARNING_BYTES) {
        ESP_LOGW(safe_tag,
                 "HEAP_MONITOR free=%u largest=%u min=%u warning=low_or_fragmented_heap threshold_free=%u threshold_largest=%u",
                 (unsigned int)free_bytes,
                 (unsigned int)largest_bytes,
                 (unsigned int)minimum_bytes,
                 (unsigned int)APP_HEAP_LOW_FREE_WARNING_BYTES,
                 (unsigned int)APP_HEAP_LOW_LARGEST_WARNING_BYTES);
    }
}

static inline void app_heap_monitor_log_periodic(const char *tag,
                                                 int64_t *last_log_ms,
                                                 uint32_t interval_ms)
{
    if (last_log_ms == NULL) {
        app_heap_monitor_log(tag);
        return;
    }

    const int64_t timestamp_ms = app_stack_monitor_now_ms();
    const uint32_t effective_interval =
        interval_ms > 0 ? interval_ms : APP_HEAP_MONITOR_INTERVAL_MS;
    if (*last_log_ms != 0 &&
        timestamp_ms - *last_log_ms < (int64_t)effective_interval) {
        return;
    }
    *last_log_ms = timestamp_ms;

    app_heap_monitor_log(tag);
}

static inline bool app_task_wdt_add_current(const char *tag, const char *task_name)
{
    const char *safe_tag = app_stack_monitor_safe_text(tag);
    const char *safe_task = app_stack_monitor_safe_text(task_name);
    esp_err_t status = esp_task_wdt_status(NULL);
    if (status == ESP_OK) {
        return true;
    }
    if (status != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(safe_tag,
                 "task watchdog unavailable task=%s ret=%s",
                 safe_task,
                 app_stack_monitor_safe_text(esp_err_to_name(status)));
        return false;
    }

    esp_err_t ret = esp_task_wdt_add(NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(safe_tag,
                 "task watchdog add failed task=%s ret=%s",
                 safe_task,
                 app_stack_monitor_safe_text(esp_err_to_name(ret)));
        return false;
    }
    ESP_LOGI(safe_tag, "task watchdog added task=%s", safe_task);
    return true;
}

static inline void app_task_wdt_reset_current(bool registered)
{
    if (registered) {
        (void)esp_task_wdt_reset();
    }
}

static inline void app_task_wdt_delay_ms(bool registered, uint32_t delay_ms)
{
    uint32_t remaining_ms = delay_ms;
    while (remaining_ms > 0U) {
        const uint32_t chunk_ms = remaining_ms > 500U ? 500U : remaining_ms;
        app_task_wdt_reset_current(registered);
        vTaskDelay(pdMS_TO_TICKS(chunk_ms));
        remaining_ms -= chunk_ms;
    }
}

#endif /* APP_STACK_MONITOR_H */

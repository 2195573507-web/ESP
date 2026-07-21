#ifndef APP_STACK_MONITOR_H
#define APP_STACK_MONITOR_H

#include <stdbool.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef APP_STACK_LOW_WATER_WARNING_BYTES
#define APP_STACK_LOW_WATER_WARNING_BYTES 1024U
#endif

static inline UBaseType_t app_stack_monitor_high_water(void)
{
#if (INCLUDE_uxTaskGetStackHighWaterMark == 1)
    return uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
#else
    return 0;
#endif
}

static inline UBaseType_t app_stack_monitor_log(const char *tag,
                                                const char *task_name,
                                                const char *stage)
{
    const char *safe_task = task_name != NULL ? task_name : "task";
    const char *safe_stage = stage != NULL ? stage : "unknown";
    UBaseType_t high_water = app_stack_monitor_high_water();

    ESP_LOGI(tag,
             "%s stack stage=%s high_water=%u bytes",
             safe_task,
             safe_stage,
             (unsigned int)high_water);
    if (high_water > 0 && high_water < APP_STACK_LOW_WATER_WARNING_BYTES) {
        ESP_LOGW(tag,
                 "%s stack low_water stage=%s high_water=%u bytes threshold=%u",
                 safe_task,
                 safe_stage,
                 (unsigned int)high_water,
                 (unsigned int)APP_STACK_LOW_WATER_WARNING_BYTES);
    }
    return high_water;
}

void app_stack_monitor_log_system_state(const char *tag, const char *stage);

/* Invoke only at lifecycle boundaries; full heap walks are not suitable for
 * ISR or high-frequency audio callbacks. */
static inline bool app_runtime_guard_check_heap_integrity(const char *tag,
                                                         const char *stage)
{
    const bool integrity_ok = heap_caps_check_integrity_all(true);
    ESP_LOG_LEVEL_LOCAL(integrity_ok ? ESP_LOG_INFO : ESP_LOG_ERROR,
                        tag != NULL ? tag : "runtime_guard",
                        "RUNTIME_PROTECTION heap_integrity stage=%s result=%s",
                        stage != NULL ? stage : "<none>",
                        integrity_ok ? "ok" : "failed");
    return integrity_ok;
}

#endif /* APP_STACK_MONITOR_H */

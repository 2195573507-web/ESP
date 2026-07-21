#include "app_stack_monitor.h"

#define APP_STACK_SYSTEM_STATE_CAPACITY 24U

static TaskStatus_t s_task_status[APP_STACK_SYSTEM_STATE_CAPACITY];

void app_stack_monitor_log_system_state(const char *tag, const char *stage)
{
    const char *const safe_tag = tag != NULL ? tag : "stack_monitor";
    const char *const safe_stage = stage != NULL ? stage : "unknown";
    const UBaseType_t high_water = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
    const UBaseType_t task_count = uxTaskGetSystemState(s_task_status,
                                                        APP_STACK_SYSTEM_STATE_CAPACITY,
                                                        NULL);

    ESP_LOGI(safe_tag,
             "STARTUP_STACK stage=%s high_water=%u bytes task_count=%u",
             safe_stage,
             (unsigned int)high_water,
             (unsigned int)task_count);
    if (task_count == 0U) {
        ESP_LOGW(safe_tag,
                 "STARTUP_STACK_STATE stage=%s unavailable_or_capacity=%u",
                 safe_stage,
                 (unsigned int)APP_STACK_SYSTEM_STATE_CAPACITY);
        return;
    }

    for (UBaseType_t index = 0U; index < task_count; ++index) {
        const TaskStatus_t *const task = &s_task_status[index];
        ESP_LOGI(safe_tag,
                 "STARTUP_STACK_STATE stage=%s task=%s state=%u priority=%u high_water=%u bytes",
                 safe_stage,
                 task->pcTaskName,
                 (unsigned int)task->eCurrentState,
                 (unsigned int)task->uxCurrentPriority,
                 (unsigned int)(task->usStackHighWaterMark * sizeof(StackType_t)));
    }
}

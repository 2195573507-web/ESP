#include "c5_task_lifecycle.h"

#include "esp_log.h"
#include "freertos/idf_additions.h"

void c5_task_handle_publish(TaskHandle_t *slot,
                            portMUX_TYPE *lock,
                            TaskHandle_t task)
{
    if (slot == NULL || lock == NULL) {
        return;
    }

    portENTER_CRITICAL(lock);
    *slot = task;
    portEXIT_CRITICAL(lock);
}

bool c5_task_delete_safe(TaskHandle_t *slot,
                         portMUX_TYPE *lock,
                         const char *tag,
                         const char *task_name,
                         const char *reason)
{
    if (slot == NULL || lock == NULL) {
        return false;
    }

    TaskHandle_t task;
    portENTER_CRITICAL(lock);
    task = *slot;
    *slot = NULL;
    portEXIT_CRITICAL(lock);

    if (task == NULL) {
        return false;
    }

    ESP_LOGI(tag != NULL ? tag : "c5_task",
             "TASK_DELETE_BEGIN task=%s reason=%s",
             task_name != NULL ? task_name : "<unnamed>",
             reason != NULL ? reason : "unspecified");
    vTaskDelete(task);
    return true;
}

bool c5_task_delete_with_caps_safe(TaskHandle_t *slot,
                                   portMUX_TYPE *lock,
                                   const char *tag,
                                   const char *task_name,
                                   const char *reason)
{
    if (slot == NULL || lock == NULL) {
        return false;
    }

    TaskHandle_t task;
    portENTER_CRITICAL(lock);
    task = *slot;
    *slot = NULL;
    portEXIT_CRITICAL(lock);

    if (task == NULL) {
        return false;
    }

    ESP_LOGI(tag != NULL ? tag : "c5_task",
             "TASK_DELETE_BEGIN task=%s reason=%s allocator=caps",
             task_name != NULL ? task_name : "<unnamed>",
             reason != NULL ? reason : "unspecified");
    vTaskDeleteWithCaps(task);
    return true;
}

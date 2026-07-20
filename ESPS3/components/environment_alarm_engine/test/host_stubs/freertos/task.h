#ifndef FREERTOS_TASK_H
#define FREERTOS_TASK_H

#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

static inline BaseType_t xTaskCreate(TaskFunction_t task,
                                     const char *name,
                                     uint32_t stack_depth,
                                     void *argument,
                                     UBaseType_t priority,
                                     TaskHandle_t *out_handle)
{
    (void)task;
    (void)name;
    (void)stack_depth;
    (void)argument;
    (void)priority;
    if (out_handle != 0) {
        *out_handle = (TaskHandle_t)1;
    }
    return pdPASS;
}

static inline uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    (void)clear;
    (void)wait;
    return 0U;
}

static inline BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    (void)task;
    return pdPASS;
}

#endif /* FREERTOS_TASK_H */

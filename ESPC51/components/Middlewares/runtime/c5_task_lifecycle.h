#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

/*
 * A task handle slot has one owner.  Clearing it under the matching lock before
 * vTaskDelete() prevents a second cleanup path from deleting a stale TCB.
 */
void c5_task_handle_publish(TaskHandle_t *slot,
                            portMUX_TYPE *lock,
                            TaskHandle_t task);

bool c5_task_delete_safe(TaskHandle_t *slot,
                         portMUX_TYPE *lock,
                         const char *tag,
                         const char *task_name,
                         const char *reason);

bool c5_task_delete_with_caps_safe(TaskHandle_t *slot,
                                   portMUX_TYPE *lock,
                                   const char *tag,
                                   const char *task_name,
                                   const char *reason);

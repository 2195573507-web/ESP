#ifndef FREERTOS_SEMPHR_H
#define FREERTOS_SEMPHR_H

#include "freertos/FreeRTOS.h"

typedef void *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return (SemaphoreHandle_t)1;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t lock, TickType_t wait)
{
    (void)lock;
    (void)wait;
    return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t lock)
{
    (void)lock;
    return pdTRUE;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t lock)
{
    (void)lock;
}

#endif /* FREERTOS_SEMPHR_H */

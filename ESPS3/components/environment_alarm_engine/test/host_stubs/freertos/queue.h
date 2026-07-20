#ifndef FREERTOS_QUEUE_H
#define FREERTOS_QUEUE_H

#include "freertos/FreeRTOS.h"

typedef void *QueueHandle_t;

static inline QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    (void)length;
    (void)item_size;
    return (QueueHandle_t)1;
}

static inline BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait)
{
    (void)queue;
    (void)item;
    (void)wait;
    return pdTRUE;
}

static inline BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{
    (void)queue;
    (void)item;
    (void)wait;
    return pdFALSE;
}

#endif /* FREERTOS_QUEUE_H */

#ifndef FREERTOS_SEMPHR_H
#define FREERTOS_SEMPHR_H

typedef void *SemaphoreHandle_t;
typedef struct {
    unsigned char opaque[64];
} StaticSemaphore_t;

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage);
int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned int wait_ticks);
int xSemaphoreGive(SemaphoreHandle_t semaphore);

#endif /* FREERTOS_SEMPHR_H */

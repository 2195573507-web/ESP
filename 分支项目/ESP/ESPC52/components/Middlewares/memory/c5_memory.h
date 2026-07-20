#ifndef C5_MEMORY_H
#define C5_MEMORY_H

#include <stddef.h>

#include "esp_err.h"

typedef enum {
    C5_MEM_INTERNAL_DMA = 0,
    C5_MEM_INTERNAL_CONTROL,
    C5_MEM_PSRAM,
} c5_mem_type_t;

void *c5_mem_alloc(size_t size, c5_mem_type_t type, const char *owner);
void *c5_mem_calloc(size_t count, size_t size, c5_mem_type_t type, const char *owner);
void *c5_mem_realloc(void *ptr, size_t size, c5_mem_type_t type, const char *owner);
void c5_mem_free(void *ptr, const char *owner);
void c5_mem_log(const char *stage);

#endif /* C5_MEMORY_H */

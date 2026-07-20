#ifndef C5_MEMORY_H
#define C5_MEMORY_H

#include <stddef.h>

#include "esp_err.h"

typedef enum {
    C5_MEM_INTERNAL_DMA = 0,
    C5_MEM_INTERNAL_CONTROL,
    C5_MEM_PSRAM,
} c5_mem_type_t;

typedef struct {
    size_t free_bytes;
    size_t largest_block;
} c5_mem_capacity_t;

/** Capture both total and contiguous capacity for the requested heap class. */
c5_mem_capacity_t c5_mem_capacity(c5_mem_type_t type);

/**
 * Check the heap class before an allocation or module admission.  Both total
 * free memory and the largest contiguous block are required because DMA
 * allocations cannot be justified by total free memory alone.
 */
esp_err_t c5_mem_require(c5_mem_type_t type,
                         size_t required_free,
                         size_t required_largest,
                         const char *owner);

void *c5_mem_alloc(size_t size, c5_mem_type_t type, const char *owner);
void *c5_mem_calloc(size_t count, size_t size, c5_mem_type_t type, const char *owner);
void *c5_mem_realloc(void *ptr, size_t size, c5_mem_type_t type, const char *owner);
void c5_mem_free(void *ptr, const char *owner);
void c5_mem_log(const char *stage);

#endif /* C5_MEMORY_H */

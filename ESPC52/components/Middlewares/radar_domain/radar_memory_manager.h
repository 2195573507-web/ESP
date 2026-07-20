#ifndef RADAR_MEMORY_MANAGER_H
#define RADAR_MEMORY_MANAGER_H

#include <stddef.h>
#include <stdint.h>

void *radar_memory_alloc_psram(size_t size, const char *owner);
void radar_memory_free(void *ptr, size_t size, const char *owner);
void radar_memory_log(const char *stage);
size_t radar_memory_psram_bytes(void);

#endif

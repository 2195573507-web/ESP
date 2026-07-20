#include "c5_memory.h"

#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "c5_memory";

static uint32_t c5_mem_caps(c5_mem_type_t type)
{
    switch (type) {
    case C5_MEM_INTERNAL_DMA:
        return MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    case C5_MEM_INTERNAL_CONTROL:
        return MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    case C5_MEM_PSRAM:
        return MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    default:
        return 0;
    }
}

void c5_mem_log(const char *stage)
{
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ESP_LOGI(TAG,
             "C5_MEM stage=%s internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             stage != NULL ? stage : "<none>",
             (unsigned int)heap_caps_get_free_size(internal_caps),
             (unsigned int)heap_caps_get_largest_free_block(internal_caps),
             (unsigned int)heap_caps_get_free_size(psram_caps),
             (unsigned int)heap_caps_get_largest_free_block(psram_caps));
}

void *c5_mem_alloc(size_t size, c5_mem_type_t type, const char *owner)
{
    void *ptr = heap_caps_malloc(size, c5_mem_caps(type));
    if (ptr == NULL) {
        ESP_LOGE(TAG, "C5_MEM_ALLOC_FAIL owner=%s size=%u type=%d",
                 owner != NULL ? owner : "<none>", (unsigned int)size, (int)type);
        c5_mem_log("alloc_fail");
    }
    return ptr;
}

void *c5_mem_calloc(size_t count, size_t size, c5_mem_type_t type, const char *owner)
{
    if (count != 0 && size > SIZE_MAX / count) {
        return NULL;
    }
    void *ptr = heap_caps_calloc(count, size, c5_mem_caps(type));
    if (ptr == NULL) {
        ESP_LOGE(TAG, "C5_MEM_CALLOC_FAIL owner=%s size=%u type=%d",
                 owner != NULL ? owner : "<none>", (unsigned int)(count * size), (int)type);
        c5_mem_log("calloc_fail");
    }
    return ptr;
}

void *c5_mem_realloc(void *ptr, size_t size, c5_mem_type_t type, const char *owner)
{
    void *resized = heap_caps_realloc(ptr, size, c5_mem_caps(type));
    if (resized == NULL && size != 0) {
        ESP_LOGE(TAG, "C5_MEM_REALLOC_FAIL owner=%s size=%u type=%d",
                 owner != NULL ? owner : "<none>", (unsigned int)size, (int)type);
        c5_mem_log("realloc_fail");
    }
    return resized;
}

void c5_mem_free(void *ptr, const char *owner)
{
    (void)owner;
    heap_caps_free(ptr);
}

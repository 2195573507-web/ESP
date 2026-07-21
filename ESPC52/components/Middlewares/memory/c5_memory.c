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

static const char *c5_mem_region(c5_mem_type_t type)
{
    switch (type) {
    case C5_MEM_INTERNAL_DMA:
        return "internal_dma";
    case C5_MEM_INTERNAL_CONTROL:
        return "internal_control";
    case C5_MEM_PSRAM:
        return "psram";
    default:
        return "invalid";
    }
}

static void c5_mem_log_plan(c5_mem_type_t type, size_t size, const char *owner)
{
    ESP_LOGI(TAG,
             "MEM_ALLOC_PLAN owner=%s caps=0x%08lx size=%u region=%s",
             owner != NULL ? owner : "<none>",
             (unsigned long)c5_mem_caps(type),
             (unsigned int)size,
             c5_mem_region(type));
}

c5_mem_capacity_t c5_mem_capacity(c5_mem_type_t type)
{
    const uint32_t caps = c5_mem_caps(type);
    c5_mem_capacity_t capacity = {0};
    if (caps != 0U) {
        capacity.free_bytes = heap_caps_get_free_size(caps);
        capacity.largest_block = heap_caps_get_largest_free_block(caps);
    }
    return capacity;
}

esp_err_t c5_mem_require(c5_mem_type_t type,
                         size_t required_free,
                         size_t required_largest,
                         const char *owner)
{
    const c5_mem_capacity_t capacity = c5_mem_capacity(type);
    if (capacity.free_bytes >= required_free && capacity.largest_block >= required_largest) {
        return ESP_OK;
    }

    ESP_LOGE(TAG,
             "C5_MEM_ADMISSION_FAIL owner=%s type=%d free=%u largest=%u required_free=%u required_largest=%u",
             owner != NULL ? owner : "<none>",
             (int)type,
             (unsigned int)capacity.free_bytes,
             (unsigned int)capacity.largest_block,
             (unsigned int)required_free,
             (unsigned int)required_largest);
    return ESP_ERR_NO_MEM;
}

void c5_mem_log(const char *stage)
{
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t dma_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ESP_LOGI(TAG,
             "C5_MEM stage=%s internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u psram_free=%u psram_largest=%u",
             stage != NULL ? stage : "<none>",
             (unsigned int)heap_caps_get_free_size(internal_caps),
             (unsigned int)heap_caps_get_largest_free_block(internal_caps),
             (unsigned int)heap_caps_get_free_size(dma_caps),
             (unsigned int)heap_caps_get_largest_free_block(dma_caps),
             (unsigned int)heap_caps_get_free_size(psram_caps),
             (unsigned int)heap_caps_get_largest_free_block(psram_caps));
}

void *c5_mem_alloc(size_t size, c5_mem_type_t type, const char *owner)
{
    c5_mem_log_plan(type, size, owner);
    if (c5_mem_require(type, size, size, owner) != ESP_OK) {
        return NULL;
    }
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
    const size_t allocation_size = count * size;
    c5_mem_log_plan(type, allocation_size, owner);
    if (c5_mem_require(type, allocation_size, allocation_size, owner) != ESP_OK) {
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
    c5_mem_log_plan(type, size, owner);
    if (size != 0U && c5_mem_require(type, size, size, owner) != ESP_OK) {
        return NULL;
    }
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

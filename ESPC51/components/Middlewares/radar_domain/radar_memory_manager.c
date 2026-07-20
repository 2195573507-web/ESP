#include "radar_memory_manager.h"

#include <stdbool.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "radar_ble_binding_config.h"

static const char *TAG = "radar_memory";
static size_t s_psram_bytes;

#define RADAR_MEMORY_IDENTITY_FORMAT \
    " source_id=%u source=%s device_id=%s room=%s sequence=0"
#define RADAR_MEMORY_IDENTITY_ARGS \
    (unsigned int)RADAR_BLE_BINDING_LOCAL_ID, \
    (RADAR_BLE_BINDING_LOCAL_ID == 1 ? "C51" : "C52"), \
    RADAR_BLE_BINDING_DEVICE_ID, \
    RADAR_BLE_BINDING_ROOM_ID

void radar_memory_log(const char *stage)
{
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t dma = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    const uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ESP_LOGI(TAG,
             "RADAR_MEMORY stage=%s internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u psram_free=%u psram_largest=%u radar_psram_bytes=%u" RADAR_MEMORY_IDENTITY_FORMAT,
             stage != NULL ? stage : "none",
             (unsigned int)heap_caps_get_free_size(internal),
             (unsigned int)heap_caps_get_largest_free_block(internal),
             (unsigned int)heap_caps_get_free_size(dma),
             (unsigned int)heap_caps_get_largest_free_block(dma),
             (unsigned int)heap_caps_get_free_size(psram),
             (unsigned int)heap_caps_get_largest_free_block(psram),
             (unsigned int)__atomic_load_n(&s_psram_bytes, __ATOMIC_RELAXED),
             RADAR_MEMORY_IDENTITY_ARGS);
}

void *radar_memory_alloc_psram(size_t size, const char *owner)
{
    const uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const size_t free_bytes = heap_caps_get_free_size(psram);
    const size_t largest_block = heap_caps_get_largest_free_block(psram);
    if (size == 0U || size > free_bytes || size > largest_block) {
        ESP_LOGE(TAG,
                 "RADAR_PSRAM_ADMISSION_FAIL owner=%s size=%u psram_free=%u psram_largest=%u" RADAR_MEMORY_IDENTITY_FORMAT,
                 owner != NULL ? owner : "none", (unsigned int)size,
                 (unsigned int)free_bytes, (unsigned int)largest_block,
                 RADAR_MEMORY_IDENTITY_ARGS);
        radar_memory_log("admission_fail");
        return NULL;
    }
    void *ptr = heap_caps_malloc(size, psram);
    if (ptr == NULL) {
        ESP_LOGE(TAG, "RADAR_PSRAM_ALLOC_FAIL owner=%s size=%u" RADAR_MEMORY_IDENTITY_FORMAT,
                 owner != NULL ? owner : "none", (unsigned int)size,
                 RADAR_MEMORY_IDENTITY_ARGS);
        radar_memory_log("alloc_fail");
        return NULL;
    }
    (void)__atomic_fetch_add(&s_psram_bytes, size, __ATOMIC_RELAXED);
    ESP_LOGI(TAG,
             "RADAR_PSRAM_ALLOC owner=%s size=%u psram_free_before=%u psram_largest_before=%u" RADAR_MEMORY_IDENTITY_FORMAT,
             owner != NULL ? owner : "none", (unsigned int)size,
             (unsigned int)free_bytes, (unsigned int)largest_block,
             RADAR_MEMORY_IDENTITY_ARGS);
    return ptr;
}

void radar_memory_free(void *ptr, size_t size, const char *owner)
{
    if (ptr == NULL) {
        return;
    }
    heap_caps_free(ptr);
    size_t tracked = __atomic_load_n(&s_psram_bytes, __ATOMIC_RELAXED);
    while (!__atomic_compare_exchange_n(&s_psram_bytes, &tracked,
                                        tracked > size ? tracked - size : 0U,
                                        false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    ESP_LOGI(TAG, "RADAR_PSRAM_FREE owner=%s size=%u" RADAR_MEMORY_IDENTITY_FORMAT,
             owner != NULL ? owner : "none", (unsigned int)size,
             RADAR_MEMORY_IDENTITY_ARGS);
}

size_t radar_memory_psram_bytes(void)
{
    return __atomic_load_n(&s_psram_bytes, __ATOMIC_RELAXED);
}

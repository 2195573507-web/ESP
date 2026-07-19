#include "radar_memory_manager.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "radar_memory";
static size_t s_psram_bytes;

void radar_memory_log(const char *stage)
{
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ESP_LOGI(TAG,
             "RADAR_MEMORY stage=%s internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u radar_psram_bytes=%u",
             stage != NULL ? stage : "none",
             (unsigned int)heap_caps_get_free_size(internal),
             (unsigned int)heap_caps_get_largest_free_block(internal),
             (unsigned int)heap_caps_get_free_size(psram),
             (unsigned int)heap_caps_get_largest_free_block(psram),
             (unsigned int)s_psram_bytes);
}

void *radar_memory_alloc_psram(size_t size, const char *owner)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ESP_LOGE(TAG, "RADAR_PSRAM_ALLOC_FAIL owner=%s size=%u",
                 owner != NULL ? owner : "none", (unsigned int)size);
        radar_memory_log("alloc_fail");
        return NULL;
    }
    s_psram_bytes += size;
    return ptr;
}

void radar_memory_free(void *ptr, const char *owner)
{
    (void)owner;
    heap_caps_free(ptr);
}

size_t radar_memory_psram_bytes(void)
{
    return s_psram_bytes;
}

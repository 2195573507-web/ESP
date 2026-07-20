#include "radar_memory_manager.h"

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
    const uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ESP_LOGI(TAG,
             "RADAR_MEMORY stage=%s internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u radar_psram_bytes=%u" RADAR_MEMORY_IDENTITY_FORMAT,
             stage != NULL ? stage : "none",
             (unsigned int)heap_caps_get_free_size(internal),
             (unsigned int)heap_caps_get_largest_free_block(internal),
             (unsigned int)heap_caps_get_free_size(psram),
             (unsigned int)heap_caps_get_largest_free_block(psram),
             (unsigned int)s_psram_bytes,
             RADAR_MEMORY_IDENTITY_ARGS);
}

void *radar_memory_alloc_psram(size_t size, const char *owner)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ESP_LOGE(TAG, "RADAR_PSRAM_ALLOC_FAIL owner=%s size=%u" RADAR_MEMORY_IDENTITY_FORMAT,
                 owner != NULL ? owner : "none", (unsigned int)size,
                 RADAR_MEMORY_IDENTITY_ARGS);
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

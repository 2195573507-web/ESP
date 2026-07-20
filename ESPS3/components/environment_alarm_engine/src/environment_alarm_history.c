#include "environment_alarm_engine_internal.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#else
#include <stdlib.h>
#endif

esp_err_t alarm_history_allocate(alarm_history_t *h, uint16_t capacity)
{
    if (h == NULL || capacity == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (h->items == NULL) {
#ifdef ESP_PLATFORM
        h->items = heap_caps_calloc(capacity,
                                    sizeof(*h->items),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        h->items = calloc(capacity, sizeof(*h->items));
#endif
        if (h->items == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    alarm_history_init(h, capacity);
    return ESP_OK;
}

void alarm_history_release(alarm_history_t *h)
{
    if (h != NULL) {
#ifdef ESP_PLATFORM
        heap_caps_free(h->items);
#else
        free(h->items);
#endif
        h->items = NULL;
        h->head = 0U;
        h->count = 0U;
        h->capacity = 0U;
    }
}

void alarm_history_init(alarm_history_t *h, uint16_t capacity) { h->head=0; h->count=0; h->capacity=capacity; if (h->items != NULL) memset(h->items, 0, capacity * sizeof(*h->items)); }
void alarm_history_prune(alarm_history_t *h, uint64_t now_ms, uint32_t window_ms) {
    while (h->count && now_ms-h->items[h->head].timestamp_ms>window_ms) { h->head=(uint16_t)((h->head+1U)%h->capacity); --h->count; }
}
void alarm_history_push(alarm_history_t *h, uint64_t timestamp_ms, float value) {
    if (h->count==h->capacity) { h->head=(uint16_t)((h->head+1U)%h->capacity); --h->count; }
    const uint16_t index=(uint16_t)((h->head+h->count)%h->capacity); h->items[index]=(alarm_history_item_t){timestamp_ms,value}; ++h->count;
}
bool alarm_history_oldest(const alarm_history_t *h, float *value) { if (h->count<2U) return false; *value=h->items[h->head].value; return true; }
bool alarm_history_max(const alarm_history_t *h, float *value) {
    if (h->count < 2U) {
        return false;
    }
    float maximum=h->items[h->head].value;
    for (uint16_t i=1;i<h->count;i++) { const uint16_t index=(uint16_t)((h->head+i)%h->capacity); if (h->items[index].value>maximum) maximum=h->items[index].value; }
    *value=maximum; return true;
}

#include "environment_alarm_engine_internal.h"

void alarm_history_init(alarm_history_t *h, uint16_t capacity) { h->head=0; h->count=0; h->capacity=capacity; }
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

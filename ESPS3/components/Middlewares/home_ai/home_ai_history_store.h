#ifndef HOME_AI_HISTORY_STORE_H
#define HOME_AI_HISTORY_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOME_AI_HISTORY_CAPACITY 2048U
#define HOME_AI_HISTORY_PENDING_CAPACITY 32U
#define HOME_AI_HISTORY_EVENT_ID_LEN 64U
#define HOME_AI_HISTORY_ROOM_ID_LEN 32U
#define HOME_AI_HISTORY_EVENT_TYPE_LEN 32U
#define HOME_AI_HISTORY_PAYLOAD_BYTES 513U
#define HOME_AI_HISTORY_SLOT_BYTES 768U
#define HOME_AI_HISTORY_GUARANTEED_RETENTION_MS (24ULL * 60ULL * 60ULL * 1000ULL)
#define HOME_AI_HISTORY_BEST_EFFORT_RETENTION_MS (72ULL * 60ULL * 60ULL * 1000ULL)
#define HOME_AI_HISTORY_CAPACITY_WARNING_PERCENT 80U

typedef struct {
    bool valid;
    uint32_t sequence;
    uint8_t priority;
    uint64_t occurred_at_ms;
    char event_id[HOME_AI_HISTORY_EVENT_ID_LEN];
    char room_id[HOME_AI_HISTORY_ROOM_ID_LEN];
    char event_type[HOME_AI_HISTORY_EVENT_TYPE_LEN];
    char payload[HOME_AI_HISTORY_PAYLOAD_BYTES];
    uint16_t payload_len;
    uint16_t storage_slot;
    bool uploaded;
} home_ai_history_event_t;

typedef struct {
    uint32_t persisted_count;
    uint32_t pending_ram_count;
    uint32_t unuploaded_count;
    uint32_t dropped_unpersisted;
    uint32_t dropped_overwrite;
    uint32_t storage_errors;
    uint32_t retention_evictions;
    uint32_t protected_rejections;
    uint8_t capacity_percent;
    bool capacity_warning;
} home_ai_history_stats_t;

bool home_ai_history_store_init(void);
esp_err_t home_ai_history_enqueue(const home_ai_history_event_t *event);
size_t home_ai_history_flush(size_t max_items);
size_t home_ai_history_prune(uint64_t now_ms);
esp_err_t home_ai_history_peek_unuploaded(home_ai_history_event_t *out);
esp_err_t home_ai_history_mark_uploaded(uint32_t sequence);
size_t home_ai_history_pending_count(void);
home_ai_history_stats_t home_ai_history_get_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* HOME_AI_HISTORY_STORE_H */

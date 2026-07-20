#include "home_ai_history_store.h"

#include <stdio.h>
#include <string.h>

#ifndef HOME_AI_HISTORY_HOST_TEST
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
#include <stdlib.h>
#endif

#define HOME_AI_HISTORY_MAGIC 0x48414948UL
#define HOME_AI_HISTORY_VERSION 1U
#define HOME_AI_HISTORY_FLAG_UPLOADED 0x01U
#define HOME_AI_HISTORY_PARTITION_LABEL "home_ai"
#define HOME_AI_HISTORY_BASE_PATH "/home_ai"
#define HOME_AI_HISTORY_FILE_PATH "/home_ai/events.bin"
#define HOME_AI_HISTORY_META_PATH "/home_ai/meta.bin"
#define HOME_AI_HISTORY_INDEX_INVALID UINT16_MAX

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t next_sequence;
    uint16_t next_slot;
    uint16_t used_slots;
    uint32_t dropped_unpersisted;
    uint32_t dropped_overwrite;
} home_ai_history_meta_t;

typedef struct {
    bool used;
    bool uploaded;
    uint8_t priority;
    uint16_t slot;
    uint32_t sequence;
    uint64_t occurred_at_ms;
} home_ai_history_index_t;

typedef struct {
    bool valid;
    home_ai_history_event_t event;
} home_ai_history_pending_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t sequence;
    uint64_t occurred_at_ms;
    uint8_t priority;
    uint8_t flags;
    uint16_t payload_len;
    char event_id[HOME_AI_HISTORY_EVENT_ID_LEN];
    char room_id[HOME_AI_HISTORY_ROOM_ID_LEN];
    char event_type[HOME_AI_HISTORY_EVENT_TYPE_LEN];
    char payload[HOME_AI_HISTORY_PAYLOAD_BYTES];
    uint32_t crc32;
    uint8_t reserved[HOME_AI_HISTORY_SLOT_BYTES -
                     (sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint64_t) +
                      sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint16_t) +
                      HOME_AI_HISTORY_EVENT_ID_LEN + HOME_AI_HISTORY_ROOM_ID_LEN +
                      HOME_AI_HISTORY_EVENT_TYPE_LEN + HOME_AI_HISTORY_PAYLOAD_BYTES +
                      sizeof(uint32_t))];
} home_ai_history_record_t;

_Static_assert(sizeof(home_ai_history_record_t) == HOME_AI_HISTORY_SLOT_BYTES,
               "Home AI history slot size must remain fixed");

static home_ai_history_index_t *s_index;
static home_ai_history_pending_t *s_pending;
static home_ai_history_meta_t s_meta;
static home_ai_history_stats_t s_stats;
static FILE *s_events_file;
static FILE *s_meta_file;
static bool s_initialized;
static bool s_capacity_warning_active;

#ifndef HOME_AI_HISTORY_HOST_TEST
static SemaphoreHandle_t s_lock;
#define HISTORY_LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define HISTORY_UNLOCK() xSemaphoreGive(s_lock)
#else
static bool s_host_lock;
#define HISTORY_LOCK() ((void)(s_host_lock = true))
#define HISTORY_UNLOCK() ((void)(s_host_lock = false))
#endif

static uint32_t crc32_bytes(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t index = 0U; index < length; ++index) {
        crc ^= data[index];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xEDB88320UL & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

static void copy_text(char *out, size_t out_size, const char *value)
{
    if (out == NULL || out_size == 0U) return;
    size_t length = 0U;
    if (value != NULL) {
        while (length + 1U < out_size && value[length] != '\0') ++length;
        memcpy(out, value, length);
    }
    out[length] = '\0';
}

static bool valid_event(const home_ai_history_event_t *event)
{
    return event != NULL && event->event_id[0] != '\0' && event->event_type[0] != '\0' &&
           event->payload_len <= HOME_AI_HISTORY_PAYLOAD_BYTES - 1U &&
           event->payload[event->payload_len] == '\0';
}

static long slot_offset(uint16_t slot)
{
    return (long)slot * (long)sizeof(home_ai_history_record_t);
}

static bool seek_slot(FILE *file, uint16_t slot)
{
    return file != NULL && fseek(file, slot_offset(slot), SEEK_SET) == 0;
}

static uint32_t record_crc(const home_ai_history_record_t *record)
{
    return crc32_bytes((const uint8_t *)record,
                       offsetof(home_ai_history_record_t, crc32));
}

static bool read_record_locked(uint16_t slot, home_ai_history_record_t *out)
{
    if (out == NULL || !seek_slot(s_events_file, slot)) return false;
    memset(out, 0, sizeof(*out));
    if (fread(out, sizeof(*out), 1U, s_events_file) != 1U ||
        out->magic != HOME_AI_HISTORY_MAGIC || out->sequence == 0U ||
        out->payload_len >= HOME_AI_HISTORY_PAYLOAD_BYTES || record_crc(out) != out->crc32) {
        return false;
    }
    return true;
}

static bool write_record_locked(uint16_t slot, home_ai_history_record_t *record)
{
    if (record == NULL || !seek_slot(s_events_file, slot)) return false;
    record->crc32 = record_crc(record);
    if (fwrite(record, sizeof(*record), 1U, s_events_file) != 1U || fflush(s_events_file) != 0) {
        ++s_stats.storage_errors;
        return false;
    }
    return true;
}

static bool erase_record_locked(uint16_t slot)
{
    if (!seek_slot(s_events_file, slot)) return false;
    home_ai_history_record_t empty;
    memset(&empty, 0, sizeof(empty));
    if (fwrite(&empty, sizeof(empty), 1U, s_events_file) != 1U || fflush(s_events_file) != 0) {
        ++s_stats.storage_errors;
        return false;
    }
    memset(&s_index[slot], 0, sizeof(s_index[slot]));
    if (s_meta.used_slots > 0U) --s_meta.used_slots;
    if (s_stats.persisted_count > 0U) --s_stats.persisted_count;
    return true;
}

static uint8_t capacity_percent_locked(void)
{
    return (uint8_t)(((uint32_t)s_meta.used_slots * 100U) / HOME_AI_HISTORY_CAPACITY);
}

static void update_capacity_warning_locked(void)
{
    const uint8_t percent = capacity_percent_locked();
    const bool warning = percent >= HOME_AI_HISTORY_CAPACITY_WARNING_PERCENT;
    uint32_t unuploaded = 0U;
    for (uint16_t slot = 0U; slot < HOME_AI_HISTORY_CAPACITY; ++slot) {
        if (s_index != NULL && s_index[slot].used && !s_index[slot].uploaded) {
            ++unuploaded;
        }
    }
    s_stats.unuploaded_count = unuploaded;
#ifndef HOME_AI_HISTORY_HOST_TEST
    if (warning && !s_capacity_warning_active) {
        ESP_LOGW("home_ai_history",
                 "HOME_AI_HISTORY_CAPACITY percent=%u used=%u capacity=%u unuploaded=%lu",
                 (unsigned int)percent,
                 (unsigned int)s_meta.used_slots,
                 (unsigned int)HOME_AI_HISTORY_CAPACITY,
                 (unsigned long)unuploaded);
    }
#endif
    s_capacity_warning_active = warning;
}

static bool write_meta_locked(void)
{
    if (s_meta_file == NULL || fseek(s_meta_file, 0L, SEEK_SET) != 0 ||
        fwrite(&s_meta, sizeof(s_meta), 1U, s_meta_file) != 1U || fflush(s_meta_file) != 0) {
        ++s_stats.storage_errors;
        return false;
    }
    return true;
}

static void event_from_record(const home_ai_history_record_t *record,
                              uint16_t slot,
                              home_ai_history_event_t *out)
{
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->sequence = record->sequence;
    out->priority = record->priority;
    out->occurred_at_ms = record->occurred_at_ms;
    out->payload_len = record->payload_len;
    out->storage_slot = slot;
    out->uploaded = (record->flags & HOME_AI_HISTORY_FLAG_UPLOADED) != 0U;
    copy_text(out->event_id, sizeof(out->event_id), record->event_id);
    copy_text(out->room_id, sizeof(out->room_id), record->room_id);
    copy_text(out->event_type, sizeof(out->event_type), record->event_type);
    memcpy(out->payload, record->payload, record->payload_len + 1U);
}

static bool load_meta_locked(void)
{
    memset(&s_meta, 0, sizeof(s_meta));
    if (s_meta_file != NULL && fseek(s_meta_file, 0L, SEEK_SET) == 0 &&
        fread(&s_meta, sizeof(s_meta), 1U, s_meta_file) == 1U &&
        s_meta.magic == HOME_AI_HISTORY_MAGIC && s_meta.version == HOME_AI_HISTORY_VERSION &&
        s_meta.next_sequence != 0U && s_meta.next_slot < HOME_AI_HISTORY_CAPACITY) {
        return true;
    }
    memset(&s_meta, 0, sizeof(s_meta));
    s_meta.magic = HOME_AI_HISTORY_MAGIC;
    s_meta.version = HOME_AI_HISTORY_VERSION;
    s_meta.next_sequence = 1U;
    s_meta.next_slot = 0U;
    s_meta.used_slots = 0U;
    return write_meta_locked();
}

static void rebuild_index_locked(void)
{
    memset(s_index, 0, sizeof(*s_index) * HOME_AI_HISTORY_CAPACITY);
    uint32_t max_sequence = 0U;
    uint16_t used = 0U;
    for (uint16_t slot = 0U; slot < HOME_AI_HISTORY_CAPACITY; ++slot) {
        home_ai_history_record_t record;
        if (!read_record_locked(slot, &record)) continue;
        s_index[slot].used = true;
        s_index[slot].uploaded = (record.flags & HOME_AI_HISTORY_FLAG_UPLOADED) != 0U;
        s_index[slot].priority = record.priority;
        s_index[slot].slot = slot;
        s_index[slot].sequence = record.sequence;
        s_index[slot].occurred_at_ms = record.occurred_at_ms;
        ++used;
        if (record.sequence > max_sequence) max_sequence = record.sequence;
    }
    s_meta.used_slots = used;
    s_meta.next_sequence = max_sequence == UINT32_MAX ? 1U : max_sequence + 1U;
    uint16_t next_slot = 0U;
    uint32_t oldest = UINT32_MAX;
    for (uint16_t slot = 0U; slot < HOME_AI_HISTORY_CAPACITY; ++slot) {
        if (!s_index[slot].used) {
            next_slot = slot;
            break;
        }
        if (s_index[slot].sequence < oldest) {
            oldest = s_index[slot].sequence;
            next_slot = (uint16_t)((slot + 1U) % HOME_AI_HISTORY_CAPACITY);
        }
    }
    s_meta.next_slot = next_slot;
    s_stats.persisted_count = used;
    update_capacity_warning_locked();
    (void)write_meta_locked();
}

static bool allocate_storage(void)
{
#ifdef HOME_AI_HISTORY_HOST_TEST
    static home_ai_history_index_t index_storage[HOME_AI_HISTORY_CAPACITY];
    static home_ai_history_pending_t pending_storage[HOME_AI_HISTORY_PENDING_CAPACITY];
    memset(index_storage, 0, sizeof(index_storage));
    memset(pending_storage, 0, sizeof(pending_storage));
    s_index = index_storage;
    s_pending = pending_storage;
#else
    s_index = heap_caps_calloc(HOME_AI_HISTORY_CAPACITY,
                               sizeof(*s_index),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pending = heap_caps_calloc(HOME_AI_HISTORY_PENDING_CAPACITY,
                                 sizeof(*s_pending),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_index == NULL || s_pending == NULL) {
        heap_caps_free(s_index);
        heap_caps_free(s_pending);
        s_index = NULL;
        s_pending = NULL;
    }
#endif
    return s_index != NULL && s_pending != NULL;
}

static void close_storage_files(void)
{
    if (s_events_file != NULL) {
        fclose(s_events_file);
        s_events_file = NULL;
    }
    if (s_meta_file != NULL) {
        fclose(s_meta_file);
        s_meta_file = NULL;
    }
}

static void release_storage(void)
{
#ifndef HOME_AI_HISTORY_HOST_TEST
    heap_caps_free(s_index);
    heap_caps_free(s_pending);
#endif
    s_index = NULL;
    s_pending = NULL;
}

bool home_ai_history_store_init(void)
{
    if (s_initialized) return true;
    if (!allocate_storage()) return false;
#ifndef HOME_AI_HISTORY_HOST_TEST
    bool mounted_here = false;
    static StaticSemaphore_t lock_storage;
    s_lock = xSemaphoreCreateMutexStatic(&lock_storage);
    if (s_lock == NULL) goto fail;
    esp_vfs_spiffs_conf_t config = {
        .base_path = HOME_AI_HISTORY_BASE_PATH,
        .partition_label = HOME_AI_HISTORY_PARTITION_LABEL,
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t mount_ret = esp_vfs_spiffs_register(&config);
    if (mount_ret != ESP_OK && mount_ret != ESP_ERR_INVALID_STATE) goto fail;
    mounted_here = mount_ret == ESP_OK;
    s_events_file = fopen(HOME_AI_HISTORY_FILE_PATH, "r+b");
    if (s_events_file == NULL) s_events_file = fopen(HOME_AI_HISTORY_FILE_PATH, "w+b");
    s_meta_file = fopen(HOME_AI_HISTORY_META_PATH, "r+b");
    if (s_meta_file == NULL) s_meta_file = fopen(HOME_AI_HISTORY_META_PATH, "w+b");
#else
    s_events_file = fopen("/tmp/home_ai_events.bin", "r+b");
    if (s_events_file == NULL) s_events_file = fopen("/tmp/home_ai_events.bin", "w+b");
    s_meta_file = fopen("/tmp/home_ai_meta.bin", "r+b");
    if (s_meta_file == NULL) s_meta_file = fopen("/tmp/home_ai_meta.bin", "w+b");
#endif
    if (s_events_file == NULL || s_meta_file == NULL) goto fail;
    HISTORY_LOCK();
    if (!load_meta_locked()) {
        HISTORY_UNLOCK();
        goto fail;
    }
    rebuild_index_locked();
    s_initialized = true;
    s_capacity_warning_active = false;
    update_capacity_warning_locked();
    HISTORY_UNLOCK();
    return true;

fail:
    close_storage_files();
#ifndef HOME_AI_HISTORY_HOST_TEST
    if (s_lock != NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    if (mounted_here) {
        (void)esp_vfs_spiffs_unregister(HOME_AI_HISTORY_PARTITION_LABEL);
    }
#endif
    release_storage();
    memset(&s_meta, 0, sizeof(s_meta));
    memset(&s_stats, 0, sizeof(s_stats));
    s_capacity_warning_active = false;
    return false;
}

esp_err_t home_ai_history_enqueue(const home_ai_history_event_t *event)
{
    if (!s_initialized || !valid_event(event)) return ESP_ERR_INVALID_ARG;
    HISTORY_LOCK();
    size_t free_slot = HOME_AI_HISTORY_PENDING_CAPACITY;
    bool replacing = false;
    for (size_t index = 0U; index < HOME_AI_HISTORY_PENDING_CAPACITY; ++index) {
        if (!s_pending[index].valid) {
            free_slot = index;
            break;
        }
    }
    if (free_slot == HOME_AI_HISTORY_PENDING_CAPACITY) {
        size_t replacement = HOME_AI_HISTORY_PENDING_CAPACITY;
        for (size_t index = 0U; index < HOME_AI_HISTORY_PENDING_CAPACITY; ++index) {
            if (replacement == HOME_AI_HISTORY_PENDING_CAPACITY ||
                s_pending[index].event.priority < s_pending[replacement].event.priority) {
                replacement = index;
            }
        }
        if (replacement == HOME_AI_HISTORY_PENDING_CAPACITY ||
            event->priority <= s_pending[replacement].event.priority) {
            ++s_stats.dropped_unpersisted;
            HISTORY_UNLOCK();
            return ESP_ERR_NO_MEM;
        }
        free_slot = replacement;
        replacing = true;
        ++s_stats.dropped_unpersisted;
    }
    s_pending[free_slot].event = *event;
    s_pending[free_slot].valid = true;
    if (!replacing) ++s_stats.pending_ram_count;
    HISTORY_UNLOCK();
    return ESP_OK;
}

static bool record_older_than(const home_ai_history_index_t *item,
                              uint64_t now_ms,
                              uint64_t age_ms)
{
    return item != NULL && item->used && item->occurred_at_ms > 0U &&
           now_ms >= item->occurred_at_ms && now_ms - item->occurred_at_ms >= age_ms;
}

static int oldest_matching_locked(uint64_t now_ms,
                                  uint64_t minimum_age_ms,
                                  bool uploaded_only)
{
    int candidate = -1;
    for (uint16_t slot = 0U; slot < HOME_AI_HISTORY_CAPACITY; ++slot) {
        const home_ai_history_index_t *item = &s_index[slot];
        if (!item->used || (uploaded_only && !item->uploaded) ||
            !record_older_than(item, now_ms, minimum_age_ms)) continue;
        if (candidate < 0 || item->sequence < s_index[candidate].sequence) candidate = slot;
    }
    return candidate;
}

static int choose_storage_slot_locked(uint8_t incoming_priority, uint64_t now_ms)
{
    if (s_meta.used_slots < HOME_AI_HISTORY_CAPACITY) {
        for (uint16_t slot = 0U; slot < HOME_AI_HISTORY_CAPACITY; ++slot) {
            if (!s_index[slot].used) return slot;
        }
    }
    int candidate = oldest_matching_locked(now_ms,
                                           HOME_AI_HISTORY_BEST_EFFORT_RETENTION_MS,
                                           true);
    if (candidate >= 0) return candidate;
    candidate = oldest_matching_locked(now_ms,
                                       HOME_AI_HISTORY_GUARANTEED_RETENTION_MS,
                                       true);
    if (candidate >= 0) return candidate;

    /* Never evict a record inside the guaranteed 24-hour retention window. */
    candidate = -1;
    for (uint16_t slot = 0U; slot < HOME_AI_HISTORY_CAPACITY; ++slot) {
        if (!s_index[slot].used || s_index[slot].priority >= incoming_priority ||
            !record_older_than(&s_index[slot],
                               now_ms,
                               HOME_AI_HISTORY_GUARANTEED_RETENTION_MS)) continue;
        if (candidate < 0 || s_index[slot].priority < s_index[candidate].priority ||
            (s_index[slot].priority == s_index[candidate].priority &&
             s_index[slot].sequence < s_index[candidate].sequence)) candidate = slot;
    }
    return candidate;
}

size_t home_ai_history_prune(uint64_t now_ms)
{
    if (!s_initialized || now_ms == 0U) return 0U;
    size_t pruned = 0U;
    HISTORY_LOCK();
    for (uint16_t slot = 0U; slot < HOME_AI_HISTORY_CAPACITY; ++slot) {
        if (!s_index[slot].used || !s_index[slot].uploaded ||
            !record_older_than(&s_index[slot], now_ms, HOME_AI_HISTORY_BEST_EFFORT_RETENTION_MS)) {
            continue;
        }
        if (erase_record_locked(slot)) {
            ++pruned;
            ++s_stats.retention_evictions;
        }
    }
    if (pruned > 0U) (void)write_meta_locked();
    update_capacity_warning_locked();
    HISTORY_UNLOCK();
    return pruned;
}

size_t home_ai_history_flush(size_t max_items)
{
    if (!s_initialized || max_items == 0U) return 0U;
    size_t flushed = 0U;
    HISTORY_LOCK();
    while (flushed < max_items) {
        size_t pending_index = HOME_AI_HISTORY_PENDING_CAPACITY;
        for (size_t index = 0U; index < HOME_AI_HISTORY_PENDING_CAPACITY; ++index) {
            if (s_pending[index].valid &&
                (pending_index == HOME_AI_HISTORY_PENDING_CAPACITY ||
                 s_pending[index].event.priority > s_pending[pending_index].event.priority)) {
                pending_index = index;
            }
        }
        if (pending_index == HOME_AI_HISTORY_PENDING_CAPACITY) break;
        home_ai_history_pending_t pending = s_pending[pending_index];
        const int slot = choose_storage_slot_locked(pending.event.priority,
                                                    pending.event.occurred_at_ms);
        if (slot < 0) {
            ++s_stats.protected_rejections;
            s_pending[pending_index].valid = false;
            if (s_stats.pending_ram_count > 0U) --s_stats.pending_ram_count;
            break;
        }
        home_ai_history_record_t record;
        memset(&record, 0, sizeof(record));
        record.magic = HOME_AI_HISTORY_MAGIC;
        record.sequence = s_meta.next_sequence++;
        if (s_meta.next_sequence == 0U) s_meta.next_sequence = 1U;
        record.occurred_at_ms = pending.event.occurred_at_ms;
        record.priority = pending.event.priority;
        record.payload_len = pending.event.payload_len;
        memcpy(record.event_id, pending.event.event_id, sizeof(record.event_id));
        memcpy(record.room_id, pending.event.room_id, sizeof(record.room_id));
        memcpy(record.event_type, pending.event.event_type, sizeof(record.event_type));
        memcpy(record.payload, pending.event.payload, pending.event.payload_len + 1U);
        const bool replacing_persisted = s_index[slot].used;
        if (replacing_persisted && !s_index[slot].uploaded) ++s_stats.dropped_overwrite;
        if (!replacing_persisted) {
            ++s_meta.used_slots;
            ++s_stats.persisted_count;
        }
        if (!write_record_locked((uint16_t)slot, &record)) break;
        s_index[slot].used = true;
        s_index[slot].uploaded = false;
        s_index[slot].priority = record.priority;
        s_index[slot].slot = (uint16_t)slot;
        s_index[slot].sequence = record.sequence;
        s_index[slot].occurred_at_ms = record.occurred_at_ms;
        s_pending[pending_index].valid = false;
        if (s_stats.pending_ram_count > 0U) --s_stats.pending_ram_count;
        ++flushed;
        s_meta.next_slot = (uint16_t)((slot + 1) % HOME_AI_HISTORY_CAPACITY);
        update_capacity_warning_locked();
        (void)write_meta_locked();
    }
    HISTORY_UNLOCK();
    return flushed;
}

esp_err_t home_ai_history_peek_unuploaded(home_ai_history_event_t *out)
{
    if (!s_initialized || out == NULL) return ESP_ERR_INVALID_ARG;
    HISTORY_LOCK();
    int candidate = -1;
    for (uint16_t slot = 0U; slot < HOME_AI_HISTORY_CAPACITY; ++slot) {
        if (!s_index[slot].used || s_index[slot].uploaded) continue;
        if (candidate < 0 || s_index[slot].sequence < s_index[candidate].sequence) candidate = slot;
    }
    if (candidate < 0) {
        HISTORY_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    home_ai_history_record_t record;
    if (!read_record_locked((uint16_t)candidate, &record)) {
        ++s_stats.storage_errors;
        HISTORY_UNLOCK();
        return ESP_FAIL;
    }
    event_from_record(&record, (uint16_t)candidate, out);
    HISTORY_UNLOCK();
    return ESP_OK;
}

esp_err_t home_ai_history_mark_uploaded(uint32_t sequence)
{
    if (!s_initialized || sequence == 0U) return ESP_ERR_INVALID_ARG;
    HISTORY_LOCK();
    int slot = -1;
    for (uint16_t index = 0U; index < HOME_AI_HISTORY_CAPACITY; ++index) {
        if (s_index[index].used && s_index[index].sequence == sequence) {
            slot = index;
            break;
        }
    }
    if (slot < 0) {
        HISTORY_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    home_ai_history_record_t record;
    if (!read_record_locked((uint16_t)slot, &record)) {
        ++s_stats.storage_errors;
        HISTORY_UNLOCK();
        return ESP_FAIL;
    }
    record.flags |= HOME_AI_HISTORY_FLAG_UPLOADED;
    if (!write_record_locked((uint16_t)slot, &record)) {
        HISTORY_UNLOCK();
        return ESP_FAIL;
    }
    s_index[slot].uploaded = true;
    HISTORY_UNLOCK();
    return ESP_OK;
}

size_t home_ai_history_pending_count(void)
{
    if (!s_initialized) return 0U;
    size_t count = 0U;
    HISTORY_LOCK();
    count = s_stats.pending_ram_count;
    for (uint16_t slot = 0U; slot < HOME_AI_HISTORY_CAPACITY; ++slot) {
        if (s_index[slot].used && !s_index[slot].uploaded) ++count;
    }
    HISTORY_UNLOCK();
    return count;
}

home_ai_history_stats_t home_ai_history_get_stats(void)
{
    home_ai_history_stats_t stats = {0};
    if (!s_initialized) return stats;
    HISTORY_LOCK();
    stats = s_stats;
    stats.unuploaded_count = 0U;
    for (uint16_t slot = 0U; slot < HOME_AI_HISTORY_CAPACITY; ++slot) {
        if (s_index[slot].used && !s_index[slot].uploaded) ++stats.unuploaded_count;
    }
    stats.capacity_percent = capacity_percent_locked();
    stats.capacity_warning = stats.capacity_percent >= HOME_AI_HISTORY_CAPACITY_WARNING_PERCENT;
    HISTORY_UNLOCK();
    return stats;
}

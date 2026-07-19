#include "radar_buffer.h"

#include <string.h>

#include "radar_memory_manager.h"

esp_err_t radar_buffer_init(radar_buffer_t *buffer)
{
    if (buffer == NULL) return ESP_ERR_INVALID_ARG;
    memset(buffer, 0, sizeof(*buffer));
    buffer->frames = radar_memory_alloc_psram(sizeof(radar_raw_frame_t) * RADAR_RAW_RING_CAPACITY,
                                              "raw_ring");
    if (buffer->frames == NULL) return ESP_ERR_NO_MEM;
    buffer->lock = xSemaphoreCreateMutexStatic(&buffer->lock_storage);
    buffer->ready = xSemaphoreCreateCountingStatic(RADAR_RAW_RING_CAPACITY, 0,
                                                    &buffer->ready_storage);
    if (buffer->lock == NULL || buffer->ready == NULL) {
        radar_buffer_deinit(buffer);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void radar_buffer_deinit(radar_buffer_t *buffer)
{
    if (buffer == NULL) return;
    radar_memory_free(buffer->frames, "raw_ring");
    memset(buffer, 0, sizeof(*buffer));
}

bool radar_buffer_push_notify(radar_buffer_t *buffer,
                              const uint8_t *data,
                              size_t length,
                              uint64_t timestamp_ms)
{
    if (buffer == NULL || buffer->frames == NULL || data == NULL || length == 0U) return false;
    (void)__atomic_fetch_add(&buffer->notify_count, 1U, __ATOMIC_RELAXED);
    if (length > RADAR_RAW_FRAME_BYTES) length = RADAR_RAW_FRAME_BYTES;
    if (xSemaphoreTake(buffer->lock, 0) != pdTRUE) {
        (void)__atomic_fetch_add(&buffer->dropped, 1U, __ATOMIC_RELAXED);
        return false;
    }
    radar_raw_frame_t *slot = &buffer->frames[buffer->write_index];
    slot->timestamp_ms = timestamp_ms;
    slot->sequence = ++buffer->next_sequence;
    if (slot->sequence == 0U) slot->sequence = ++buffer->next_sequence;
    slot->length = (uint16_t)length;
    memcpy(slot->raw_data, data, length);
    buffer->write_index = (buffer->write_index + 1U) % RADAR_RAW_RING_CAPACITY;
    const bool was_full = buffer->used == RADAR_RAW_RING_CAPACITY;
    if (was_full) {
        buffer->read_index = (buffer->read_index + 1U) % RADAR_RAW_RING_CAPACITY;
        (void)__atomic_fetch_add(&buffer->dropped, 1U, __ATOMIC_RELAXED);
    } else {
        ++buffer->used;
    }
    xSemaphoreGive(buffer->lock);
    if (!was_full) (void)xSemaphoreGive(buffer->ready);
    return true;
}

bool radar_buffer_pop(radar_buffer_t *buffer, radar_raw_frame_t *out, TickType_t wait_ticks)
{
    if (buffer == NULL || out == NULL || buffer->frames == NULL ||
        xSemaphoreTake(buffer->ready, wait_ticks) != pdTRUE) return false;
    if (xSemaphoreTake(buffer->lock, portMAX_DELAY) != pdTRUE) return false;
    if (buffer->used == 0U) {
        xSemaphoreGive(buffer->lock);
        return false;
    }
    *out = buffer->frames[buffer->read_index];
    buffer->read_index = (buffer->read_index + 1U) % RADAR_RAW_RING_CAPACITY;
    --buffer->used;
    xSemaphoreGive(buffer->lock);
    return true;
}

void radar_buffer_get_stats(radar_buffer_t *buffer, radar_buffer_stats_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (buffer == NULL || buffer->lock == NULL ||
        xSemaphoreTake(buffer->lock, portMAX_DELAY) != pdTRUE) return;
    out->notify_count = __atomic_load_n(&buffer->notify_count, __ATOMIC_RELAXED);
    out->queue_depth = buffer->used;
    out->drop_count = __atomic_load_n(&buffer->dropped, __ATOMIC_RELAXED);
    xSemaphoreGive(buffer->lock);
}

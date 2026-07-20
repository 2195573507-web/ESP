#include "radar_upload_queue.h"

#include <string.h>

#include "radar_memory_manager.h"

esp_err_t radar_upload_queue_init(radar_upload_queue_t *queue)
{
    if (queue == NULL) return ESP_ERR_INVALID_ARG;
    memset(queue, 0, sizeof(*queue));
    queue->packets = radar_memory_alloc_psram(sizeof(radar_upload_packet_t) * RADAR_UPLOAD_QUEUE_CAPACITY,
                                              "upload_queue");
    if (queue->packets == NULL) return ESP_ERR_NO_MEM;
    queue->lock = xSemaphoreCreateMutexStatic(&queue->lock_storage);
    queue->ready = xSemaphoreCreateCountingStatic(RADAR_UPLOAD_QUEUE_CAPACITY, 0,
                                                   &queue->ready_storage);
    if (queue->lock == NULL || queue->ready == NULL) {
        radar_upload_queue_deinit(queue);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void radar_upload_queue_deinit(radar_upload_queue_t *queue)
{
    if (queue == NULL) return;
    radar_memory_free(queue->packets,
                      sizeof(radar_upload_packet_t) * RADAR_UPLOAD_QUEUE_CAPACITY,
                      "upload_queue");
    memset(queue, 0, sizeof(*queue));
}

bool radar_upload_queue_push(radar_upload_queue_t *queue, const radar_upload_packet_t *packet)
{
    if (queue == NULL || queue->packets == NULL || packet == NULL ||
        xSemaphoreTake(queue->lock, 0) != pdTRUE) return false;
    const bool was_full = queue->used == RADAR_UPLOAD_QUEUE_CAPACITY;
    if (was_full) {
        queue->read_index = (queue->read_index + 1U) % RADAR_UPLOAD_QUEUE_CAPACITY;
        --queue->used;
        ++queue->dropped;
    }
    queue->packets[queue->write_index] = *packet;
    queue->write_index = (queue->write_index + 1U) % RADAR_UPLOAD_QUEUE_CAPACITY;
    ++queue->used;
    xSemaphoreGive(queue->lock);
    if (!was_full) (void)xSemaphoreGive(queue->ready);
    return true;
}

bool radar_upload_queue_pop(radar_upload_queue_t *queue,
                            radar_upload_packet_t *out,
                            TickType_t wait_ticks)
{
    if (queue == NULL || out == NULL || queue->packets == NULL ||
        xSemaphoreTake(queue->ready, wait_ticks) != pdTRUE) return false;
    if (xSemaphoreTake(queue->lock, portMAX_DELAY) != pdTRUE) return false;
    if (queue->used == 0U) {
        xSemaphoreGive(queue->lock);
        return false;
    }
    *out = queue->packets[queue->read_index];
    queue->read_index = (queue->read_index + 1U) % RADAR_UPLOAD_QUEUE_CAPACITY;
    --queue->used;
    xSemaphoreGive(queue->lock);
    return true;
}

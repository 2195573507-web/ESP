#include "radar_ble_stream.h"

#include <string.h>

void radar_ble_stream_init(radar_ble_stream_t *stream)
{
    if (stream != NULL) {
        memset(stream, 0, sizeof(*stream));
#ifndef RADAR_BLE_HOST_TEST
        stream->lock = xSemaphoreCreateMutexStatic(&stream->lock_storage);
#endif
    }
}

size_t radar_ble_stream_push(radar_ble_stream_t *stream, const uint8_t *data, size_t length)
{
    if (stream == NULL || (data == NULL && length > 0U)) return 0U;
#ifndef RADAR_BLE_HOST_TEST
    if (stream->lock == NULL || xSemaphoreTake(stream->lock, 0) != pdTRUE) return 0U;
#endif
    stream->notify_count++;
    stream->notify_bytes += length > UINT32_MAX ? UINT32_MAX : (uint32_t)length;
    if (length > RADAR_BLE_STREAM_CAPACITY) {
        data += length - RADAR_BLE_STREAM_CAPACITY;
        length = RADAR_BLE_STREAM_CAPACITY;
        stream->overflow_count++;
        stream->resync_count++;
    }
    if (stream->length + length > RADAR_BLE_STREAM_CAPACITY) {
        size_t drop = stream->length + length - RADAR_BLE_STREAM_CAPACITY;
        memmove(stream->buffer, stream->buffer + drop, stream->length - drop);
        stream->length -= drop;
        stream->skipped_bytes += (uint32_t)drop;
        stream->overflow_count++;
        stream->resync_count++;
    }
    memcpy(stream->buffer + stream->length, data, length);
    stream->length += length;
#ifndef RADAR_BLE_HOST_TEST
    xSemaphoreGive(stream->lock);
#endif
    return length;
}

size_t radar_ble_stream_take(radar_ble_stream_t *stream, uint8_t *out, size_t out_size)
{
    if (stream == NULL || out == NULL || out_size == 0U) return 0U;
#ifndef RADAR_BLE_HOST_TEST
    if (stream->lock == NULL || xSemaphoreTake(stream->lock, 0) != pdTRUE) return 0U;
#endif
    size_t amount = stream->length < out_size ? stream->length : out_size;
    memcpy(out, stream->buffer, amount);
    if (amount < stream->length) memmove(stream->buffer, stream->buffer + amount, stream->length - amount);
    stream->length -= amount;
#ifndef RADAR_BLE_HOST_TEST
    xSemaphoreGive(stream->lock);
#endif
    return amount;
}

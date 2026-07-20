#ifndef RADAR_BLE_STREAM_H
#define RADAR_BLE_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifndef RADAR_BLE_HOST_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

#define RADAR_BLE_STREAM_CAPACITY 512U

typedef struct {
    uint8_t buffer[RADAR_BLE_STREAM_CAPACITY];
    size_t length;
#ifndef RADAR_BLE_HOST_TEST
    StaticSemaphore_t lock_storage;
    SemaphoreHandle_t lock;
#endif
    uint32_t notify_count;
    uint32_t notify_bytes;
    uint32_t overflow_count;
    uint32_t resync_count;
    uint32_t skipped_bytes;
} radar_ble_stream_t;

void radar_ble_stream_init(radar_ble_stream_t *stream);
size_t radar_ble_stream_push(radar_ble_stream_t *stream, const uint8_t *data, size_t length);
size_t radar_ble_stream_take(radar_ble_stream_t *stream, uint8_t *out, size_t out_size);

#endif

#ifndef RADAR_BUFFER_H
#define RADAR_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define RADAR_RAW_RING_CAPACITY 512U
#define RADAR_RAW_FRAME_BYTES 64U

typedef struct {
    uint64_t timestamp_ms;
    uint32_t sequence;
    uint16_t length;
    uint8_t raw_data[RADAR_RAW_FRAME_BYTES];
} radar_raw_frame_t;

typedef struct {
    uint32_t notify_count;
    uint32_t queue_depth;
    uint32_t drop_count;
} radar_buffer_stats_t;

typedef struct {
    radar_raw_frame_t *frames;
    uint32_t read_index;
    uint32_t write_index;
    uint32_t used;
    uint32_t dropped;
    uint32_t notify_count;
    uint32_t next_sequence;
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_storage;
    SemaphoreHandle_t ready;
    StaticSemaphore_t ready_storage;
} radar_buffer_t;

esp_err_t radar_buffer_init(radar_buffer_t *buffer);
void radar_buffer_deinit(radar_buffer_t *buffer);
bool radar_buffer_push_notify(radar_buffer_t *buffer,
                              const uint8_t *data,
                              size_t length,
                              uint64_t timestamp_ms);
bool radar_buffer_pop(radar_buffer_t *buffer, radar_raw_frame_t *out, TickType_t wait_ticks);
void radar_buffer_get_stats(radar_buffer_t *buffer, radar_buffer_stats_t *out);

#endif

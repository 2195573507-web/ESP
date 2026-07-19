#ifndef RADAR_UPLOAD_QUEUE_H
#define RADAR_UPLOAD_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "radar_target_sample.h"

#define RADAR_UPLOAD_QUEUE_CAPACITY 32U

typedef struct {
    radar_target_sample_t sample;
    uint32_t request_sequence;
    uint8_t attempts;
} radar_upload_packet_t;

typedef struct {
    radar_upload_packet_t *packets;
    uint32_t read_index;
    uint32_t write_index;
    uint32_t used;
    uint32_t dropped;
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_storage;
    SemaphoreHandle_t ready;
    StaticSemaphore_t ready_storage;
} radar_upload_queue_t;

esp_err_t radar_upload_queue_init(radar_upload_queue_t *queue);
void radar_upload_queue_deinit(radar_upload_queue_t *queue);
bool radar_upload_queue_push(radar_upload_queue_t *queue, const radar_upload_packet_t *packet);
bool radar_upload_queue_pop(radar_upload_queue_t *queue,
                            radar_upload_packet_t *out,
                            TickType_t wait_ticks);

#endif

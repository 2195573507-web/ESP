#ifndef RADAR_SERVICE_H
#define RADAR_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ld2450_parser.h"
#include "radar_target_sample.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ld2450_parser_diagnostics_t parser;
    uint32_t snapshot_updates;
    uint32_t frame_received_count;
    uint32_t last_target_count;
    uint8_t link_state;
} radar_service_diagnostics_t;

esp_err_t radar_service_init(void);
esp_err_t radar_service_start(void);
bool radar_service_is_started(void);
void radar_service_get_target_sample(radar_target_sample_t *out);
void radar_service_get_diagnostics(radar_service_diagnostics_t *out);
void radar_service_ingest_ble_bytes(const uint8_t *data, size_t data_len, uint64_t received_at_ms);
void radar_service_mark_ble_timeout(uint64_t timestamp_ms);
void radar_service_set_link_state(uint8_t link_state, bool link_online);

#ifdef __cplusplus
}
#endif

#endif

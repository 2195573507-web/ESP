#ifndef RADAR_SERVICE_H
#define RADAR_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ld2450_parser.h"
#include "ld2450_uart.h"
#include "radar_presence.h"
#include "radar_uart_recovery.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ld2450_parser_diagnostics_t parser;
    ld2450_uart_diagnostics_t uart;
    radar_presence_diagnostics_t presence;
    radar_uart_recovery_t recovery;
    uint32_t snapshot_updates;
    uint32_t pending_rx_bytes;
    uint32_t pending_rx_drops;
    uint32_t processed_rx_bytes;
} radar_service_diagnostics_t;

esp_err_t radar_service_init(const radar_presence_config_t *config);
esp_err_t radar_service_start(void);
/* Idempotent stop used by failed parent initialization and controlled retry. */
esp_err_t radar_service_stop(void);
bool radar_service_is_started(void);
/* UART RX owns capture; the adaptive processing task owns bounded parser consumption. */
size_t radar_service_process_pending(uint64_t processed_at_ms);
void radar_service_get_snapshot(radar_snapshot_t *out);
bool radar_service_get_latest_frame(radar_frame_t *out);
void radar_service_get_diagnostics(radar_service_diagnostics_t *out);

#ifdef __cplusplus
}
#endif

#endif

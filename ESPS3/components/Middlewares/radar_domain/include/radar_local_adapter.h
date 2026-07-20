#ifndef RADAR_LOCAL_ADAPTER_H
#define RADAR_LOCAL_ADAPTER_H

#include <stdint.h>

#include "esp_err.h"
#include "radar_spatial_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_err_t uart_start_result;
    uint32_t registry_update_count;
    uint32_t state_change_count;
} radar_local_adapter_diagnostics_t;

esp_err_t radar_local_adapter_start(void);
/* Stops only the local S3 UART adapter; remote C51/C52 contexts stay intact. */
esp_err_t radar_local_adapter_stop(void);
void radar_local_adapter_get_diagnostics(radar_local_adapter_diagnostics_t *out);
/* Internal diagnostics copy; zone-free consumers must use the readonly API below. */
bool radar_local_adapter_get_spatial_snapshot(radar_spatial_snapshot_t *out);
bool radar_local_adapter_get_readonly_snapshot(radar_readonly_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif

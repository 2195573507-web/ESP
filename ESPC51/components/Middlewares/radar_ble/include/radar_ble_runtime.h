#ifndef RADAR_BLE_RUNTIME_H
#define RADAR_BLE_RUNTIME_H

#include "esp_err.h"
#include "radar_ble_transport.h"

esp_err_t radar_ble_runtime_start(void);
void radar_ble_runtime_stop(void);
void radar_ble_runtime_get_status(radar_ble_transport_status_t *out);

#endif

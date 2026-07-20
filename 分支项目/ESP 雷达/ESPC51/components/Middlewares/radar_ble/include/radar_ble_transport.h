#ifndef RADAR_BLE_TRANSPORT_H
#define RADAR_BLE_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    RADAR_BLE_STATE_DISABLED = 0,
    RADAR_BLE_STATE_SCANNING = 1,
    RADAR_BLE_STATE_CONNECTING = 2,
    RADAR_BLE_STATE_DISCOVERING = 3,
    RADAR_BLE_STATE_PROFILE_FOUND = 4,
    RADAR_BLE_STATE_SUBSCRIBING = 5,
    RADAR_BLE_STATE_READY = 6,
    RADAR_BLE_STATE_BACKOFF = 7,
    RADAR_BLE_STATE_UNAVAILABLE = 8,
} radar_ble_state_t;

typedef void (*radar_ble_notify_cb_t)(const uint8_t *data, size_t length, void *ctx);

typedef struct {
    radar_ble_state_t state;
    bool configured;
    bool connected;
    bool notify_subscribed;
    bool data_ready;
    uint32_t reconnect_count;
    uint32_t unavailable_count;
    uint32_t backoff_ms;
} radar_ble_transport_status_t;

int radar_ble_transport_write(const uint8_t *data, size_t length);
int radar_ble_set_control_command(const uint8_t *data, size_t length);
int radar_ble_send_control_command(void);

int radar_ble_transport_start(radar_ble_notify_cb_t callback, void *ctx);
void radar_ble_transport_stop(void);
void radar_ble_transport_set_data_ready(bool ready);
void radar_ble_transport_get_status(radar_ble_transport_status_t *out);
const char *radar_ble_state_name(radar_ble_state_t state);

#endif

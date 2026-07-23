#ifndef RADAR_BLE_BINDING_CONFIG_H
#define RADAR_BLE_BINDING_CONFIG_H

#include <stdint.h>

#ifndef RADAR_BLE_RADAR_ENABLED
#define RADAR_BLE_RADAR_ENABLED 1
#endif
#ifndef RADAR_BLE_BINDING_ENABLED
#define RADAR_BLE_BINDING_ENABLED RADAR_BLE_RADAR_ENABLED
#endif
#define RADAR_BLE_ADDR_TYPE_PUBLIC 0U
#define RADAR_BLE_ADDR_TYPE_RANDOM 1U
#define RADAR_BLE_ADDR_TYPE_ANY 0xfeU
#define RADAR_BLE_ADDR_TYPE_UNSPECIFIED 0xffU
#define RADAR_BLE_BINDING_LOCAL_ID 2
#define RADAR_BLE_BINDING_DEVICE_ID "sensair_shuttle_02"
#define RADAR_BLE_BINDING_ROOM_ID "bedroom"
#define RADAR_BLE_BINDING_RADAR_ID "ld2450_01"
/* Exact MAC remains mandatory; type is learned by the fixed-MAC discovery pass. */
#define RADAR_BLE_BINDING_ADDRESS_TYPE RADAR_BLE_ADDR_TYPE_ANY
/* Canonical display order. NimBLE advertising addresses are compared in reverse byte order. */
#define RADAR_BLE_BINDING_MAC {0x8CU, 0xB1U, 0xF3U, 0xE1U, 0x15U, 0x41U}
/* GATT UUIDs are selected from discovered characteristics, never configured here. */
#define RADAR_BLE_START_COMMAND_PLACEHOLDER 1U
#define RADAR_BLE_START_COMMAND_BYTES {0U}
#define RADAR_BLE_START_COMMAND_LENGTH 0U
#endif

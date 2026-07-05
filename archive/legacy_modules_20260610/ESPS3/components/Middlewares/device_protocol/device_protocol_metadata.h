#ifndef DEVICE_PROTOCOL_METADATA_H
#define DEVICE_PROTOCOL_METADATA_H

#include <stddef.h>

#include "server_comm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_PROTOCOL_SCHEMA_VERSION "1"
#define DEVICE_PROTOCOL_DEVICE_TYPE "esp32c5_env_voice_node"
#define DEVICE_PROTOCOL_FIRMWARE_VERSION "0.1.0"
#define DEVICE_PROTOCOL_MAX_HEADERS 9U

typedef struct {
    server_comm_header_t headers[DEVICE_PROTOCOL_MAX_HEADERS];
    size_t header_count;
    char request_seq[24];
    char esp_uptime_ms[24];
    char esp_time_ms[24];
    char time_synced[8];
    char payload_type[80];
} device_protocol_metadata_t;

void device_protocol_prepare_metadata(device_protocol_metadata_t *metadata,
                                      const char *payload_type);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_PROTOCOL_METADATA_H */

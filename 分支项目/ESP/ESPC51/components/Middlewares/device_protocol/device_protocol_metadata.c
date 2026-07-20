/**
 * @file device_protocol_metadata.c
 * @brief C5 终端请求 metadata/header 组装。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），负责为 C5 -> S3 请求添加
 * X-Schema-Version、X-Device-Id、X-Gateway-Id、uptime/time sync 和 payload type。
 * 它不改变 C5 <-> S3 轻量 JSON body，也不构造 S3 -> Server 完整 envelope。
 */

#include "device_protocol_metadata.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_time_sync.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "server_comm_config.h"

static portMUX_TYPE s_request_seq_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned long long s_request_seq;

static unsigned long long device_protocol_next_request_seq(void)
{
    unsigned long long seq = 0;
    portENTER_CRITICAL(&s_request_seq_lock);
    s_request_seq++;
    seq = s_request_seq;
    portEXIT_CRITICAL(&s_request_seq_lock);
    return seq;
}

static void device_protocol_add_header(device_protocol_metadata_t *metadata,
                                       const char *key,
                                       const char *value)
{
    if (metadata == NULL || key == NULL || value == NULL ||
        metadata->header_count >= DEVICE_PROTOCOL_MAX_HEADERS) {
        return;
    }

    metadata->headers[metadata->header_count].key = key;
    metadata->headers[metadata->header_count].value = value;
    metadata->header_count++;
}

void device_protocol_prepare_metadata(device_protocol_metadata_t *metadata,
                                      const char *payload_type)
{
    if (metadata == NULL) {
        return;
    }

    memset(metadata, 0, sizeof(*metadata));
    snprintf(metadata->request_seq,
             sizeof(metadata->request_seq),
             "%llu",
             device_protocol_next_request_seq());
    snprintf(metadata->esp_uptime_ms,
             sizeof(metadata->esp_uptime_ms),
             "%lld",
             (long long)app_time_sync_get_uptime_ms());

    bool time_synced = app_time_sync_is_synced();
    strlcpy(metadata->time_synced,
            time_synced ? "true" : "false",
            sizeof(metadata->time_synced));
    if (time_synced) {
        snprintf(metadata->esp_time_ms,
                 sizeof(metadata->esp_time_ms),
                 "%lld",
                 (long long)app_time_sync_get_unix_ms());
    }

    strlcpy(metadata->payload_type,
            payload_type != NULL ? payload_type : "",
            sizeof(metadata->payload_type));
    strlcpy(metadata->gateway_id,
            server_comm_get_gateway_id(),
            sizeof(metadata->gateway_id));
    strlcpy(metadata->room_id,
            server_comm_get_room_id(),
            sizeof(metadata->room_id));

    device_protocol_add_header(metadata, "X-Schema-Version", DEVICE_PROTOCOL_SCHEMA_VERSION);
    device_protocol_add_header(metadata, "X-Device-Id", server_comm_get_device_id());
    device_protocol_add_header(metadata, "X-Gateway-Id", metadata->gateway_id);
    device_protocol_add_header(metadata, "X-Device-Type", DEVICE_PROTOCOL_DEVICE_TYPE);
    device_protocol_add_header(metadata, "X-Firmware-Version", DEVICE_PROTOCOL_FIRMWARE_VERSION);
    device_protocol_add_header(metadata, "X-Request-Seq", metadata->request_seq);
    device_protocol_add_header(metadata, "X-Esp-Uptime-Ms", metadata->esp_uptime_ms);
    if (metadata->esp_time_ms[0] != '\0') {
        device_protocol_add_header(metadata, "X-Esp-Time-Ms", metadata->esp_time_ms);
    }
    device_protocol_add_header(metadata, "X-Time-Synced", metadata->time_synced);
    device_protocol_add_header(metadata, "X-Payload-Type", metadata->payload_type);
    if (metadata->room_id[0] != '\0') {
        device_protocol_add_header(metadata, "X-Room-Id", metadata->room_id);
    }
}

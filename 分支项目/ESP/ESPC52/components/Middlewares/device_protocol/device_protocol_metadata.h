#ifndef DEVICE_PROTOCOL_METADATA_H
#define DEVICE_PROTOCOL_METADATA_H

/**
 * @file device_protocol_metadata.h
 * @brief C5 终端 HTTP metadata/header 接口。
 *
 * C5 body 采用轻量本地 JSON；本模块只补充诊断和兼容 header，供 S3 voice/sensor/
 * command 代理识别完整身份。完整 Server JSON 由 S3 protocol_adapter 负责。
 */

#include <stddef.h>

#include "esp111_protocol_common.h"
#include "server_comm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_PROTOCOL_SCHEMA_VERSION ESP111_PROTOCOL_SCHEMA_VERSION_STRING
#define DEVICE_PROTOCOL_DEVICE_TYPE ESP111_PROTOCOL_TERMINAL_DEVICE_TYPE
#define DEVICE_PROTOCOL_FIRMWARE_VERSION ESP111_PROTOCOL_FIRMWARE_VERSION
#define DEVICE_PROTOCOL_MAX_HEADERS 12U

typedef struct {
    server_comm_header_t headers[DEVICE_PROTOCOL_MAX_HEADERS];
    size_t header_count;
    char request_seq[24];
    char esp_uptime_ms[24];
    char esp_time_ms[24];
    char time_synced[8];
    char payload_type[80];
    char gateway_id[64];
    char room_id[64];
} device_protocol_metadata_t;

/**
 * @brief 准备一次 C5 -> S3 请求的 metadata header。
 *
 * 调用位置：BME、voice、system command 等请求发起前。
 * 调用时机：每次 HTTP 请求前新建 metadata 时。
 * @param metadata 输出结构体，不能为空；函数会先清零再填充。
 * @param payload_type 本次请求的完整消息类型字符串，可为空。
 * 返回值：无；参数为空时直接返回。
 * 失败处理：本函数不返回错误；header 数量超过上限时会静默停止追加，调用方仍按原请求流程处理。
 */
void device_protocol_prepare_metadata(device_protocol_metadata_t *metadata,
                                      const char *payload_type);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_PROTOCOL_METADATA_H */

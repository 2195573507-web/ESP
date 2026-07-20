#ifndef PROTOCOL_ADAPTER_H
#define PROTOCOL_ADAPTER_H

/**
 * @file protocol_adapter.h
 * @brief S3 网关协议适配接口。
 *
 * C5<->S3 使用短字段轻量 JSON；S3<->Server 使用完整 v1 JSON。本模块负责两者之间
 * 的结构映射和错误/命令码转换，不发 HTTP、不执行业务命令。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "csi_fusion.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROTOCOL_ADAPTER_TEXT_LEN 64U

typedef struct {
    cJSON *root;
    cJSON *payload;
    cJSON *capabilities;
    char message_type[PROTOCOL_ADAPTER_TEXT_LEN];
    char gateway_id[PROTOCOL_ADAPTER_TEXT_LEN];
    char device_id[PROTOCOL_ADAPTER_TEXT_LEN];
    char room_id[PROTOCOL_ADAPTER_TEXT_LEN];
    char alias[PROTOCOL_ADAPTER_TEXT_LEN];
    char firmware_version[PROTOCOL_ADAPTER_TEXT_LEN];
    uint8_t local_id;
    uint8_t local_protocol_version;
    uint8_t local_packet_type;
    uint8_t local_sensor_kind;
    uint8_t local_health_subtype;
    bool has_wifi_rssi;
    int wifi_rssi;
    uint32_t seq;
    int64_t timestamp_ms;
    int64_t uptime_ms;
} protocol_adapter_envelope_t;

typedef enum {
    PROTOCOL_ADAPTER_MESSAGE_UNKNOWN = 0,
    PROTOCOL_ADAPTER_MESSAGE_REGISTER,
    PROTOCOL_ADAPTER_MESSAGE_HEARTBEAT,
    PROTOCOL_ADAPTER_MESSAGE_STATUS,
    PROTOCOL_ADAPTER_MESSAGE_SENSOR_BME690,
    PROTOCOL_ADAPTER_MESSAGE_CSI_RESULT,
    PROTOCOL_ADAPTER_MESSAGE_COMMAND_ACK,
} protocol_adapter_message_kind_t;

/** @brief 解析完整 v1 JSON envelope；兼容入口，当前主要用于非轻量路径。 */
esp_err_t protocol_adapter_parse_envelope(const char *json,
                                          size_t json_len,
                                          protocol_adapter_envelope_t *out);

/**
 * @brief 解析 C5 轻量本地 JSON 并补全为完整 envelope。
 *
 * 调用位置：local_http_server 的 register/heartbeat/status/sensor/csi handler。
 * @param json 请求 body。
 * @param json_len 请求 body 长度。
 * @param out 输出 envelope，成功后需调用 protocol_adapter_release_envelope()。
 * @return ESP_OK 表示映射成功；格式、短 id、类型或 payload 不合法时返回错误码。
 * 失败处理：local_http_server 映射为本地 ok/e 错误响应。
 */
esp_err_t protocol_adapter_parse_local_envelope(const char *json,
                                                size_t json_len,
                                                protocol_adapter_envelope_t *out);
/** @brief 释放 envelope 内 cJSON root；parse 成功或部分失败后都可调用。 */
void protocol_adapter_release_envelope(protocol_adapter_envelope_t *envelope);
/** @brief 将完整 message_type 映射为枚举；adapter/handler 校验时调用。 */
protocol_adapter_message_kind_t protocol_adapter_message_kind(const char *message_type);
/** @brief 校验本地 envelope 的 gateway_id、allowlist 和 message_type；失败由 HTTP handler 转错误响应。 */
esp_err_t protocol_adapter_validate_local_envelope(const protocol_adapter_envelope_t *envelope);
/** @brief 将 envelope 构造成 S3 -> Server ingest JSON；sensor_aggregator 调用，返回 JSON 需释放。 */
esp_err_t protocol_adapter_build_server_ingest_json(const protocol_adapter_envelope_t *envelope,
                                                    char **out_json);
/** @brief 将 S3 fusion telemetry 构造成 canonical CSI event v2；不暴露 C5 device-specific 字段。 */
esp_err_t protocol_adapter_build_csi_event_v2_json(const csi_fusion_fact_t *fact,
                                                   const csi_fusion_telemetry_t *telemetry,
                                                   char **out_json);
/** @brief 将 C5 短 id 映射为完整 device_id；local HTTP 和 command router 调用。 */
const char *protocol_adapter_local_device_id_to_device_id(uint8_t local_id);
/** @brief 将 C5 短 id 映射为默认 alias；register payload 补全时调用。 */
const char *protocol_adapter_local_device_id_to_alias(uint8_t local_id);
/** @brief 将完整 device_id 映射回短 id；S3 给 C5 回包/命令时调用。 */
uint8_t protocol_adapter_device_id_to_local_id(const char *device_id);
/** @brief 释放 build_server_ingest_json() 返回的 JSON。 */
void protocol_adapter_free_json(char *json);
/** @brief 构造完整 v1 ok 响应；兼容入口。 */
esp_err_t protocol_adapter_build_ok_response(const char *device_id,
                                             uint32_t seq,
                                             char *out,
                                             size_t out_size);
/** @brief 构造 C5 轻量 ok 响应；local_http_server handler 成功时调用。 */
esp_err_t protocol_adapter_build_local_ok_response(uint8_t local_id,
                                                   char *out,
                                                   size_t out_size);
/** @brief 构造 C5 轻量错误响应；local_http_server handler 失败时调用。 */
esp_err_t protocol_adapter_build_local_error_response(unsigned int error_code,
                                                      char *out,
                                                      size_t out_size);
/** @brief 构造完整 JSON 错误响应；voice_proxy 返回非 PCM 错误时调用。 */
esp_err_t protocol_adapter_build_error_response(const char *error_code,
                                                const char *message,
                                                char *out,
                                                size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_ADAPTER_H */

#ifndef SERVER_COMM_CONFIG_H
#define SERVER_COMM_CONFIG_H

/**
 * @file server_comm_config.h
 * @brief C5 终端访问 ESPS3 /local/v1 的通信配置接口。
 *
 * 本模块只描述 C5 -> S3 本地网关 HTTP 目标和终端身份；完整 S3 -> Server 路径
 * 由 ESPS3/server_client 处理，C5 业务模块不要在这里引入公网 Server 协议。
 */

#include <stddef.h>
#include <stdint.h>

#include "esp111_protocol_common.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 本地网关通信配置：正式模式只指向 ESPS3 SoftAP 网关，不保存公网 server URL。 */
#ifndef SERVER_COMM_SCHEME
#define SERVER_COMM_SCHEME "http"
#endif

#ifndef SERVER_COMM_HOST
#define SERVER_COMM_HOST ESP111_PROTOCOL_GATEWAY_IP
#endif

#ifndef SERVER_COMM_PORT
#define SERVER_COMM_PORT 80
#endif

#ifndef SERVER_COMM_BASE_URL
#define SERVER_COMM_BASE_URL "http://" ESP111_PROTOCOL_GATEWAY_IP
#endif

#ifndef SERVER_COMM_DEVICE_ID
#define SERVER_COMM_DEVICE_ID "sensair_shuttle_02"
#endif

#ifndef SERVER_COMM_DEFAULT_TIMEOUT_MS
#define SERVER_COMM_DEFAULT_TIMEOUT_MS 5000U   // 默认 HTTP 超时，单位 ms。
#endif

#ifndef SERVER_COMM_URL_BUFFER_SIZE
#define SERVER_COMM_URL_BUFFER_SIZE 640U       // 拼接完整 URL 的内部缓存大小，需容纳编码后的设备 ID。
#endif

/** @brief 获取 C5 -> S3 的 base URL；业务模块记录日志或公共 HTTP 层拼 URL 时调用，返回静态字符串。 */
const char *server_comm_get_base_url(void);

/** @brief 获取 S3 网关 host；诊断日志使用，返回 terminal_config 中的 gateway_ip。 */
const char *server_comm_get_host(void);
/** @brief 获取 S3 本地 HTTP 端口；当前固定为 SERVER_COMM_PORT。 */
int server_comm_get_port(void);

/** @brief 获取完整终端 device_id；公共 HTTP header 和业务日志使用，返回静态字符串。 */
const char *server_comm_get_device_id(void);
/** @brief 获取 gateway_id；公共 HTTP header 和 metadata 使用，返回静态字符串。 */
const char *server_comm_get_gateway_id(void);
/** @brief 获取 room_id；metadata 可选 header 使用，返回静态字符串。 */
const char *server_comm_get_room_id(void);
/** @brief 获取 alias；注册/诊断使用，返回静态字符串。 */
const char *server_comm_get_alias(void);
/** @brief 获取固件版本；metadata header 使用，返回共享协议常量。 */
const char *server_comm_get_firmware_version(void);
/** @brief 获取终端设备类型；metadata header 使用，返回共享协议常量。 */
const char *server_comm_get_device_type(void);
/** @brief 获取能力 JSON；register metadata 使用，返回共享协议常量。 */
const char *server_comm_get_capabilities_json(void);

/** @brief 获取默认 HTTP 超时；业务请求 timeout_ms 传 0 时公共 HTTP 层使用。 */
uint32_t server_comm_get_default_timeout_ms(void);

/**
 * @brief 拼接 C5 -> S3 请求 URL。
 *
 * 调用位置：server_comm_http 内部。
 * 调用时机：每次发起 GET/POST/stream 前。
 * @param endpoint /local/v1 相对路径，不能为空；C5 不接受完整 http(s) URL 或 Server API 路径。
 * @param url 输出缓冲区。
 * @param url_size 输出缓冲区长度。
 * @return ESP_OK 表示 URL 已写入；参数错误返回 ESP_ERR_INVALID_ARG；禁用路径返回 ESP_ERR_NOT_ALLOWED；
 * 缓冲区不足返回 ESP_ERR_INVALID_SIZE。
 * 失败处理：上层放弃本次 HTTP 请求并把错误返回给 BME/voice/command 调用方。
 */
esp_err_t server_comm_build_url(const char *endpoint, char *url, size_t url_size);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_COMM_CONFIG_H */

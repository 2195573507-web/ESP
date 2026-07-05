#ifndef TERMINAL_CONFIG_H
#define TERMINAL_CONFIG_H

/**
 * @file terminal_config.h
 * @brief C5 终端身份和 S3 SoftAP 配置接口。
 *
 * 本头文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用）。ESPC51/ESPC52 默认身份由
 * 工程编译定义区分；除默认 device_id/name/short_id 外，业务协议和调用方式保持一致。
 * 本模块不定义 S3->Server 完整协议，只给 WiFi、server_comm 和业务模块提供本机配置。
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp111_protocol_common.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TERMINAL_CONFIG_TEXT_LEN 64U
#define TERMINAL_CONFIG_IP_LEN 16U

#ifndef TERMINAL_CONFIG_DEFAULT_DEVICE_ID
#define TERMINAL_CONFIG_DEFAULT_DEVICE_ID "sensair_shuttle_01"
#endif

#ifndef TERMINAL_CONFIG_DEFAULT_LOCAL_ID
#define TERMINAL_CONFIG_DEFAULT_LOCAL_ID ESP111_PROTOCOL_LOCAL_DEVICE_ID_C51
#endif

#ifndef TERMINAL_CONFIG_DEFAULT_GATEWAY_ID
#define TERMINAL_CONFIG_DEFAULT_GATEWAY_ID ESP111_PROTOCOL_GATEWAY_ID
#endif

#ifndef TERMINAL_CONFIG_DEFAULT_ROOM_ID
#define TERMINAL_CONFIG_DEFAULT_ROOM_ID "unassigned"
#endif

#ifndef TERMINAL_CONFIG_DEFAULT_ALIAS
#define TERMINAL_CONFIG_DEFAULT_ALIAS "SensaiShuttle"
#endif

#ifndef TERMINAL_CONFIG_DEFAULT_GATEWAY_SSID
#define TERMINAL_CONFIG_DEFAULT_GATEWAY_SSID ESP111_PROTOCOL_GATEWAY_SSID
#endif

#ifndef TERMINAL_CONFIG_DEFAULT_GATEWAY_PASSWORD
#define TERMINAL_CONFIG_DEFAULT_GATEWAY_PASSWORD ESP111_PROTOCOL_GATEWAY_PASSWORD
#endif

#ifndef TERMINAL_CONFIG_DEFAULT_GATEWAY_IP
#define TERMINAL_CONFIG_DEFAULT_GATEWAY_IP ESP111_PROTOCOL_GATEWAY_IP
#endif

#ifndef TERMINAL_CONFIG_DEFAULT_UPLOAD_PERIOD_MS
#define TERMINAL_CONFIG_DEFAULT_UPLOAD_PERIOD_MS 2000U
#endif

#define TERMINAL_CONFIG_FIRMWARE_ROLE ESP111_PROTOCOL_TERMINAL_ROLE
#define TERMINAL_CONFIG_FIRMWARE_VERSION ESP111_PROTOCOL_FIRMWARE_VERSION
#define TERMINAL_CONFIG_DEVICE_TYPE ESP111_PROTOCOL_TERMINAL_DEVICE_TYPE
#define TERMINAL_CONFIG_CAPABILITIES_JSON ESP111_PROTOCOL_TERMINAL_CAPABILITIES_JSON

typedef struct {
    char device_id[TERMINAL_CONFIG_TEXT_LEN];
    char gateway_id[TERMINAL_CONFIG_TEXT_LEN];
    char room_id[TERMINAL_CONFIG_TEXT_LEN];
    char alias[TERMINAL_CONFIG_TEXT_LEN];
    char gateway_ssid[TERMINAL_CONFIG_TEXT_LEN];
    char gateway_password[TERMINAL_CONFIG_TEXT_LEN];
    char gateway_ip[TERMINAL_CONFIG_IP_LEN];
    uint32_t upload_period_ms;
    bool debug_direct_server_enabled;
} terminal_runtime_config_t;

/**
 * @brief 加载终端运行配置。
 *
 * 调用位置：wifi_manager_init()、terminal_config_get() 间接调用。
 * 调用时机：启动早期首次读取配置时调用，可重复调用。
 * 输入参数：无。
 * 返回值：ESP_OK 表示默认值或 NVS 覆盖值已可用；NVS 打开失败时返回对应错误码但保留默认值。
 * 失败处理：上层继续使用默认配置，日志中记录 NVS 错误。
 */
esp_err_t terminal_config_load(void);

/**
 * @brief 获取当前终端运行配置快照。
 *
 * 调用位置：WiFi、server_comm、BME、voice、command 等 C5 模块。
 * 调用时机：需要读取 device_id、gateway_ip、upload_period_ms 等字段时。
 * 输入参数：无。
 * 返回值：指向模块内静态配置的只读指针，不需要释放。
 * 失败处理：内部会尝试 load；NVS 失败时仍返回默认配置。
 */
const terminal_runtime_config_t *terminal_config_get(void);

/** @brief 获取完整 device_id；调用方用于 HTTP header、日志或 local id 映射，返回静态字符串。 */
const char *terminal_config_get_device_id(void);
/** @brief 获取 gateway_id；调用方用于 X-Gateway-Id 和完整协议映射，返回静态字符串。 */
const char *terminal_config_get_gateway_id(void);
/** @brief 获取 room_id；调用方用于诊断 header，未配置时返回默认房间。 */
const char *terminal_config_get_room_id(void);
/** @brief 获取终端别名；调用方用于注册和诊断日志，返回静态字符串。 */
const char *terminal_config_get_alias(void);
/** @brief 获取 S3 SoftAP SSID；wifi_manager 连接 S3 时调用，返回静态字符串。 */
const char *terminal_config_get_gateway_ssid(void);
/** @brief 获取 S3 SoftAP 密码；wifi_manager 连接 S3 时调用，返回静态字符串。 */
const char *terminal_config_get_gateway_password(void);
/** @brief 获取 S3 本地网关 IP；server_comm 拼接 /local/v1 URL 时调用。 */
const char *terminal_config_get_gateway_ip(void);
/** @brief 获取 BME 上传周期；BME service 每轮读取前调用，返回毫秒值。 */
uint32_t terminal_config_get_upload_period_ms(void);
/** @brief 获取 C5<->S3 轻量协议短 id；S3 由该 id 还原完整 device_id。 */
uint8_t terminal_config_get_local_id(void);
/** @brief 获取调试直连开关；当前正式链路不依赖该开关。 */
bool terminal_config_debug_direct_server_enabled(void);
/** @brief 获取终端能力 JSON 字符串；注册/metadata 使用，返回共享协议常量。 */
const char *terminal_config_get_capabilities_json(void);

#ifdef __cplusplus
}
#endif

#endif /* TERMINAL_CONFIG_H */

#ifndef GATEWAY_CONFIG_H
#define GATEWAY_CONFIG_H

/**
 * @file gateway_config.h
 * @brief S3 网关身份、SoftAP、上云和子设备 allowlist 配置。
 *
 * 本模块属于 ESPS3 网关。C5<->S3 轻量协议允许的短 id 会在 protocol_adapter 中映射
 * 到这里 allowlist 中的完整 device_id；S3<->Server 的 base URL 也只由 server_client 使用。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp111_protocol_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GATEWAY_CONFIG_MAX_CHILDREN 4U
#define GATEWAY_CONFIG_ID ESP111_PROTOCOL_GATEWAY_ID
#define GATEWAY_CONFIG_HARDWARE_MODULE "esp32s3_n32r16"
#define GATEWAY_CONFIG_SOFTAP_IP ESP111_PROTOCOL_GATEWAY_IP
#define GATEWAY_CONFIG_SOFTAP_NETMASK "255.255.255.0"
#define GATEWAY_CONFIG_SOFTAP_GW ESP111_PROTOCOL_GATEWAY_IP
#define GATEWAY_CONFIG_LOCAL_HTTP_PORT 80U
#define GATEWAY_CONFIG_VOICE_UPLOAD_MAX_BYTES (384U * 1024U)
#define GATEWAY_CONFIG_LOCAL_HTTP_MAX_JSON_BYTES 4096U
#define GATEWAY_CONFIG_COMMAND_QUEUE_SIZE 8U
#define GATEWAY_CONFIG_HEARTBEAT_TIMEOUT_MS 30000U
#define GATEWAY_CONFIG_LINK_LOST_GRACE_MS 20000U
#define GATEWAY_CONFIG_SENSOR_FORWARD_PERIOD_MS 2000U

#ifndef GATEWAY_CONFIG_ENABLE_CSI_TRIGGER
#define GATEWAY_CONFIG_ENABLE_CSI_TRIGGER 1
#endif

#ifndef GATEWAY_CONFIG_ENABLE_CSI_RESULT_INGEST
#define GATEWAY_CONFIG_ENABLE_CSI_RESULT_INGEST 1
#endif

#ifndef GATEWAY_CONFIG_CSI_TRIGGER_INTERVAL_MS
#define GATEWAY_CONFIG_CSI_TRIGGER_INTERVAL_MS 50U
#endif

#ifndef GATEWAY_CONFIG_CSI_TRIGGER_UDP_PORT
#define GATEWAY_CONFIG_CSI_TRIGGER_UDP_PORT 33434U
#endif

#ifndef GATEWAY_CONFIG_CSI_TRIGGER_TARGET_DEVICE_ID
/* 空字符串表示对所有在线 C5 发送 CSI trigger。 */
#define GATEWAY_CONFIG_CSI_TRIGGER_TARGET_DEVICE_ID ""
#endif

#ifndef GATEWAY_CONFIG_SOFTAP_SSID
#define GATEWAY_CONFIG_SOFTAP_SSID ESP111_PROTOCOL_GATEWAY_SSID
#endif

#ifndef GATEWAY_CONFIG_SOFTAP_PASSWORD
#define GATEWAY_CONFIG_SOFTAP_PASSWORD ESP111_PROTOCOL_GATEWAY_PASSWORD
#endif

#ifndef GATEWAY_CONFIG_SOFTAP_CHANNEL
#define GATEWAY_CONFIG_SOFTAP_CHANNEL 6U
#endif

#ifndef GATEWAY_CONFIG_SOFTAP_MAX_CONNECTION
#define GATEWAY_CONFIG_SOFTAP_MAX_CONNECTION 2U
#endif

#ifndef GATEWAY_CONFIG_SERVER_BASE_URL
#define GATEWAY_CONFIG_SERVER_BASE_URL "http://124.221.162.188:3000"
#endif

#ifndef GATEWAY_CONFIG_AUTH_TOKEN
#define GATEWAY_CONFIG_AUTH_TOKEN ""
#endif

typedef struct {
    const char *ssid;
    const char *password;
} gateway_wifi_credential_t;

typedef struct {
    const char *gateway_id;
    const char *hardware_module;
    const char *softap_ssid;
    const char *softap_password;
    const char *softap_ip;
    const char *softap_netmask;
    const char *softap_gw;
    uint8_t softap_channel;
    uint8_t softap_max_connection;
    const gateway_wifi_credential_t *sta_credentials;
    size_t sta_credentials_count;
    const char *server_base_url;
    const char *auth_token;
    uint16_t local_http_port;
    size_t voice_upload_max_bytes;
    size_t local_http_max_json_bytes;
    uint32_t heartbeat_timeout_ms;
    uint32_t link_lost_grace_ms;
    uint32_t sensor_forward_period_ms;
    bool csi_trigger_enabled;
    bool csi_result_ingest_enabled;
    uint32_t csi_trigger_interval_ms;
    uint16_t csi_trigger_udp_port;
    const char *csi_trigger_target_device_id;
    const char *children_allowlist[GATEWAY_CONFIG_MAX_CHILDREN];
    size_t children_allowlist_count;
} gateway_runtime_config_t;

/** @brief 获取网关静态运行配置；各 S3 模块调用，返回静态只读指针。 */
const gateway_runtime_config_t *gateway_config_get(void);
/** @brief 判断是否配置 STA 上云凭据；gateway_wifi/server_client 用于离线降级判断。 */
bool gateway_config_sta_credentials_configured(void);
/** @brief 判断完整 device_id 是否在 allowlist；registry/voice/adapter 调用，空值返回 false。 */
bool gateway_config_child_allowed(const char *device_id);
/** @brief 启动时打印硬件/Flash/PSRAM/网络配置；gateway_orchestrator_start() 调用。 */
void gateway_config_log_boot_profile(void);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_CONFIG_H */

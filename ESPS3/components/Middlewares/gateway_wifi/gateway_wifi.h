#ifndef GATEWAY_WIFI_H
#define GATEWAY_WIFI_H

/**
 * @file gateway_wifi.h
 * @brief S3 网关 WiFi 状态接口。
 *
 * S3 同时提供 SoftAP 给 C5，并可通过 STA 访问 ESP-server；本模块只暴露启动和状态查询。
 */

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 S3 APSTA 网络。
 *
 * 调用位置：gateway_orchestrator_start()。
 * 调用时机：registry/router/proxy 初始化后，local_http_server_start() 之前。
 * 输入参数：无。
 * @return ESP_OK 表示 WiFi 已启动或已在运行；NVS/netif/WiFi 配置失败返回对应错误码。
 * 失败处理：gateway_orchestrator 使用 ESP_ERROR_CHECK 处理关键启动失败。
 */
esp_err_t gateway_wifi_start(void);
/** @brief 查询 SoftAP 是否已启动；health/heartbeat 日志调用。 */
bool gateway_wifi_is_softap_ready(void);
/** @brief 查询 STA 是否已连上上游网络；server_client 发起请求前调用。 */
bool gateway_wifi_is_sta_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_WIFI_H */

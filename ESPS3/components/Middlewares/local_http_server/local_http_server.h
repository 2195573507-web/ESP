#ifndef LOCAL_HTTP_SERVER_H
#define LOCAL_HTTP_SERVER_H

/**
 * @file local_http_server.h
 * @brief S3 网关 /local/v1 HTTP server 启动接口。
 *
 * 本模块只对 C5 暴露本地接口；完整 /api/... Server 路径由 server_client 作为客户端访问。
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 S3 本地 HTTP server 并注册 /local/v1 路由。
 *
 * 调用位置：gateway_orchestrator_start()。
 * 调用时机：SoftAP 启动后。
 * 输入参数：无。
 * @return ESP_OK 表示 server 已启动或已在运行；httpd 启动/路由注册失败返回对应错误码。
 * 失败处理：gateway_orchestrator 使用 ESP_ERROR_CHECK 处理关键启动失败。
 */
esp_err_t local_http_server_start(void);

#ifdef __cplusplus
}
#endif

#endif /* LOCAL_HTTP_SERVER_H */

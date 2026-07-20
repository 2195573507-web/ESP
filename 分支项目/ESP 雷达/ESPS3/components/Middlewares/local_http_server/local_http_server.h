#ifndef LOCAL_HTTP_SERVER_H
#define LOCAL_HTTP_SERVER_H

/**
 * @file local_http_server.h
 * @brief S3 网关 /local/v1 HTTP server 启动接口。
 *
 * 本模块只对 C5 暴露本地接口；完整 /api/... Server 路径由 server_client 作为客户端访问。
 */

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOCAL_HTTP_SERVER_STATE_STOPPED = 0,
    LOCAL_HTTP_SERVER_STATE_STARTING,
    LOCAL_HTTP_SERVER_STATE_RUNNING,
    LOCAL_HTTP_SERVER_STATE_STOPPING,
    LOCAL_HTTP_SERVER_STATE_FAILED,
} local_http_server_state_t;

/** @brief 返回本地 HTTP 生命周期状态的稳定名称，用于诊断日志。 */
const char *local_http_server_state_name(local_http_server_state_t state);
/** @brief 读取当前本地 HTTP 生命周期状态。 */
local_http_server_state_t local_http_server_get_state(void);
/** @brief 仅当句柄和全部本地路由都已注册时返回 true。 */
bool local_http_server_is_running(void);
/** @brief 读取是否仍保留 httpd 句柄，供重试前清理残留状态。 */
bool local_http_server_has_handle(void);

/**
 * @brief 启动 S3 本地 HTTP server 并注册 /local/v1 路由。
 *
 * 调用位置：network_worker。
 * 调用时机：worker 已观察到 SoftAP ready 后。
 * 输入参数：无。
 * @return ESP_OK 表示 server 已启动或已在运行；SoftAP 未 ready、httpd 启动或路由注册失败返回对应错误码。
 * 失败处理：network_worker 记录错误并按其生命周期策略重试。
 */
esp_err_t local_http_server_start(void);
/** @brief 带调用原因的启动入口；仅供 ESPS3 worker 生命周期诊断使用。 */
esp_err_t local_http_server_start_with_reason(const char *reason);
/** @brief 停止 S3 本地 HTTP server；保留给显式生命周期控制使用。 */
esp_err_t local_http_server_stop(void);
/** @brief 带调用原因的停止入口；成功停止后句柄必定清空。 */
esp_err_t local_http_server_stop_with_reason(const char *reason);

#ifdef __cplusplus
}
#endif

#endif /* LOCAL_HTTP_SERVER_H */

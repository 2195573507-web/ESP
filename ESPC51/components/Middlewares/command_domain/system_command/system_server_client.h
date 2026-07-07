#ifndef SYSTEM_SERVER_CLIENT_H
#define SYSTEM_SERVER_CLIENT_H

/**
 * @file system_server_client.h
 * @brief C5 终端系统消息本地网关客户端。
 *
 * 本模块只访问 ESPS3 /local/v1/register、heartbeat、status、commands 接口。
 * 命令执行仅覆盖当前 C5 支持的轻量命令；display 命令进入 placeholder，不接真实 LCD。
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化系统客户端并注册终端身份。
 *
 * 调用位置：system_service_init()。
 * 调用时机：WiFi 稳定后系统后台服务启动阶段。
 * 输入参数：无。
 * @return ESP_OK 表示注册成功或已注册；HTTP/JSON/内存失败返回对应错误码。
 * 失败处理：system_service 记录警告，后续 heartbeat/status/command poll 仍会继续重试。
 */
esp_err_t system_server_client_init(void);

/** @brief 向 ESPS3 上报 heartbeat；device_id 不能为空，失败由 system_service 周期性重试。 */
esp_err_t system_server_client_send_heartbeat(const char *device_id);

/** @brief 向 ESPS3 上报状态快照；device_id 不能为空，失败由 system_service 周期性重试。 */
esp_err_t system_server_client_send_status(const char *device_id);

/**
 * @brief 拉取并处理一条待执行命令。
 *
 * 调用位置：system_service_tick_command_poll()。
 * 调用时机：按 SYSTEM_SERVICE_COMMAND_POLL_INTERVAL_MS 周期调用。
 * @param device_id 完整终端 device_id，不能为空。
 * @return ESP_OK 表示拉取/执行/ack 成功；ESP_ERR_NOT_FOUND 表示无命令；其他值表示本轮失败。
 * 失败处理：system_service 限频打印日志并在下个周期继续轮询。
 */
esp_err_t system_server_client_poll_commands(const char *device_id);

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_SERVER_CLIENT_H */

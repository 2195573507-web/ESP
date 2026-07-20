#ifndef SCREEN_SERVER_CLIENT_H
#define SCREEN_SERVER_CLIENT_H

/**
 * @file screen_server_client.h
 * @brief C5 终端 screen server client placeholder。
 *
 * 当前 display 命令来自 system command 链路；本接口只保留边界，不接真实服务器轮询。
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化屏幕命令占位客户端；当前无状态，返回 ESP_OK。 */
esp_err_t screen_server_client_init(void);

/** @brief 轮询屏幕命令的占位入口；当前固定返回 ESP_ERR_NOT_SUPPORTED。 */
esp_err_t screen_server_client_poll_commands(const char *device_id);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_SERVER_CLIENT_H */

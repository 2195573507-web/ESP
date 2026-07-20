#ifndef SCREEN_SERVER_CLIENT_H
#define SCREEN_SERVER_CLIENT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 调用方法：screen service 需要轮询服务器命令前调用；当前仅预留接口。 */
esp_err_t screen_server_client_init(void);

/** 调用方法：按设备 ID 拉取屏幕显示命令；当前未接入服务器，返回 ESP_ERR_NOT_SUPPORTED。 */
esp_err_t screen_server_client_poll_commands(const char *device_id);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_SERVER_CLIENT_H */

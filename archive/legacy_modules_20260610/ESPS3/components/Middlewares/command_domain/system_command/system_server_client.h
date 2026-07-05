#ifndef SYSTEM_SERVER_CLIENT_H
#define SYSTEM_SERVER_CLIENT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 调用方法：system_service 初始化时调用一次；注册当前固件支持的命令能力。 */
esp_err_t system_server_client_init(void);

/** 调用方法：需要向服务器上报设备在线状态时调用；当前未接入服务器。 */
esp_err_t system_server_client_send_heartbeat(const char *device_id);

/** 调用方法：按设备 ID 拉取一条待执行命令，执行后向服务器回执。无命令返回 ESP_ERR_NOT_FOUND。 */
esp_err_t system_server_client_poll_commands(const char *device_id);

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_SERVER_CLIENT_H */

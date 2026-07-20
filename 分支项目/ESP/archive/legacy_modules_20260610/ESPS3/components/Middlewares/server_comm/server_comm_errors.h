#ifndef SERVER_COMM_ERRORS_H
#define SERVER_COMM_ERRORS_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t server_comm_err_t;

#define SERVER_COMM_ERR_WIFI_NOT_READY ESP_ERR_INVALID_STATE   // Wi-Fi 未就绪。
#define SERVER_COMM_ERR_BAD_STATUS ESP_ERR_INVALID_RESPONSE    // HTTP status 非 2xx。
#define SERVER_COMM_ERR_RESPONSE_OVERFLOW ESP_ERR_INVALID_SIZE // 响应缓存不足。
#define SERVER_COMM_ERR_BUSY ESP_ERR_TIMEOUT                   // 公共忙/超时错误。

/** 调用方法：日志中需要把 server_comm_err_t 转成可读字符串时调用。 */
const char *server_comm_err_to_name(server_comm_err_t err);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_COMM_ERRORS_H */

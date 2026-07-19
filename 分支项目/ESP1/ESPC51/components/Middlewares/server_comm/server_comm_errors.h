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
#define SERVER_COMM_ERR_BLOCKED_BY_VOICE_BUSY 0x7101          // voice exclusive 拦截普通请求。
#define SERVER_COMM_ERR_HEADER_BUFFER_TOO_SMALL 0x7102        // HTTP header buffer 不足。
#define SERVER_COMM_ERR_FETCH_HEADER_TIMEOUT 0x7103           // 等待响应头超时。
#define SERVER_COMM_ERR_STREAM_READ_FAILED 0x7104             // 流式响应读取失败。

/** 调用方法：日志中需要把 server_comm_err_t 转成可读字符串时调用。 */
const char *server_comm_err_to_name(server_comm_err_t err);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_COMM_ERRORS_H */

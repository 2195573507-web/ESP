#ifndef SERVER_COMM_CONFIG_H
#define SERVER_COMM_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 服务器公共配置：只放 ESP-server 地址和设备身份，不放任何云端 ASR/LLM/TTS 配置。 */
#ifndef SERVER_COMM_SCHEME
#define SERVER_COMM_SCHEME "http"              // 服务器协议，当前 ESP-server 使用 HTTP。
#endif

#ifndef SERVER_COMM_HOST
#define SERVER_COMM_HOST "124.221.162.188"     // 服务器主机/IP。
#endif

#ifndef SERVER_COMM_PORT
#define SERVER_COMM_PORT 3000                  // 服务器端口。
#endif

#ifndef SERVER_COMM_BASE_URL
#define SERVER_COMM_BASE_URL "http://124.221.162.188:3000" // base URL，不带接口路径。
#endif

#ifndef SERVER_COMM_DEVICE_ID
#define SERVER_COMM_DEVICE_ID "esp32-c5-whole-001" // 公共默认设备 ID。
#endif

#ifndef SERVER_COMM_DEFAULT_TIMEOUT_MS
#define SERVER_COMM_DEFAULT_TIMEOUT_MS 5000U   // 默认 HTTP 超时，单位 ms。
#endif

#ifndef SERVER_COMM_URL_BUFFER_SIZE
#define SERVER_COMM_URL_BUFFER_SIZE 640U       // 拼接完整 URL 的内部缓存大小，需容纳编码后的设备 ID。
#endif

/** 调用方法：业务模块需要打印或拼接服务器地址时调用，返回 SERVER_COMM_BASE_URL。 */
const char *server_comm_get_base_url(void);

/** 调用方法：需要单独拿 host/port 做诊断日志时调用。 */
const char *server_comm_get_host(void);
int server_comm_get_port(void);

/** 调用方法：server_comm 会自动写 X-Device-Id；业务日志也可调用本函数获取 ID。 */
const char *server_comm_get_device_id(void);

/** 调用方法：业务请求 timeout_ms 传 0 时，公共 HTTP 层会使用本默认值。 */
uint32_t server_comm_get_default_timeout_ms(void);

/** 调用方法：传入 "/api/xxx" 或完整 URL，输出完整请求 URL。 */
esp_err_t server_comm_build_url(const char *endpoint, char *url, size_t url_size);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_COMM_CONFIG_H */

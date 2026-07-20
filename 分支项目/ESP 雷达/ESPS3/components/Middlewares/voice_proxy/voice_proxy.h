#ifndef VOICE_PROXY_H
#define VOICE_PROXY_H

/**
 * @file voice_proxy.h
 * @brief S3 网关 voice turn 代理接口。
 *
 * 本模块只转发 C5 PCM 到 Server 并把 PCM 响应回传给 C5；不在 S3 固件中实现 ASR/LLM/TTS。
 */

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化单会话 voice proxy 锁；gateway_orchestrator_start() 调用，可重复调用。 */
esp_err_t voice_proxy_init(void);
/** @brief 查询当前是否有 voice turn 正在代理；health/heartbeat 日志调用。 */
bool voice_proxy_is_busy(void);
/**
 * @brief 处理 /local/v1/voice/turn 请求。
 *
 * 调用位置：local_http_server 的 voice route handler。
 * @param req httpd 请求对象，不能为空。
 * @return ESP_OK 表示已回传 PCM 或错误响应；参数/发送失败返回对应错误码。
 * 失败处理：本函数内部尽量向 C5 写 JSON 错误；Server 路径失败由 offline_policy 记录。
 */
esp_err_t voice_proxy_handle_turn(httpd_req_t *req);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_PROXY_H */

#ifndef WAKE_PROMPT_CACHE_GATEWAY_H
#define WAKE_PROMPT_CACHE_GATEWAY_H

/**
 * @file wake_prompt_cache_gateway.h
 * @brief S3 wake prompt 缓存网关接口。
 *
 * 本模块属于 ESPS3 网关。C5 只请求 /local/v1/audio/wake-prompt 并播放 PCM；
 * S3 负责从 Server 拉取可配置 wake prompt、保存 PCM/metadata，并以本地 HTTP
 * 二进制流返回给 C5。S3 不解析或下发提示词文本给 C5。
 */

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化 SPIFFS 缓存锁；gateway_orchestrator 启动时调用一次。 */
esp_err_t wake_prompt_cache_gateway_init(void);

/** @brief 处理 C5 GET /local/v1/audio/wake-prompt；local_http_server route 调用。 */
esp_err_t wake_prompt_cache_gateway_handle_http(httpd_req_t *req);

#ifdef __cplusplus
}
#endif

#endif /* WAKE_PROMPT_CACHE_GATEWAY_H */

#ifndef WAKE_PROMPT_CACHE_H
#define WAKE_PROMPT_CACHE_H

#include "esp_err.h"

/**
 * @file wake_prompt_cache.h
 * @brief C5 唤醒提示音流式客户端接口。
 *
 * 本模块不再在 C5 保存完整中文提示音，也不解析提示词文本。local_wake_word 在
 * WakeNet 命中后调用 wake_prompt_cache_play()，本模块短超时请求 S3
 * /local/v1/audio/wake-prompt 写入临时 SPIFFS 文件，关闭 HTTP 后播放；失败时返回错误给
 * 本地 short beep 兜底。
 */

#ifndef WAKE_PROMPT_CACHE_CONNECT_TIMEOUT_MS
#define WAKE_PROMPT_CACHE_CONNECT_TIMEOUT_MS 800U // 唤醒提示音 GET 建连/open 超时，避免拖慢录音窗口。
#endif

#ifndef WAKE_PROMPT_CACHE_FETCH_HEADERS_TIMEOUT_MS
#define WAKE_PROMPT_CACHE_FETCH_HEADERS_TIMEOUT_MS 5000U // 等待 S3 首次格式化/拉取 prompt 后返回响应头。
#endif

#ifndef WAKE_PROMPT_CACHE_READ_TIMEOUT_MS
#define WAKE_PROMPT_CACHE_READ_TIMEOUT_MS 3000U // 单次读取 S3 PCM chunk，覆盖约 1 秒提示音下载。
#endif

#ifndef WAKE_PROMPT_CACHE_TOTAL_TIMEOUT_MS
#define WAKE_PROMPT_CACHE_TOTAL_TIMEOUT_MS 9000U // 整段提示音请求总时限，失败后 fallback beep。
#endif

#ifndef WAKE_PROMPT_CACHE_HTTP_HEADER_BUFFER_BYTES
#define WAKE_PROMPT_CACHE_HTTP_HEADER_BUFFER_BYTES 4096U // S3 wake prompt 响应 header 接收缓冲。
#endif

#ifndef WAKE_PROMPT_CACHE_HTTP_TX_BUFFER_BYTES
#define WAKE_PROMPT_CACHE_HTTP_TX_BUFFER_BYTES 1024U // C5 metadata 请求 header 发送缓冲。
#endif

/** 调用方法：初始化 wake prompt 临时播放 spool；不预下载或持久缓存提示音。 */
esp_err_t wake_prompt_cache_start_async(void);

/** 调用方法：本地唤醒提示音播放阶段调用；成功播放 S3 PCM，失败返回原因给 short beep 回退。 */
esp_err_t wake_prompt_cache_play(void);

#endif /* WAKE_PROMPT_CACHE_H */

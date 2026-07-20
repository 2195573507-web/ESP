#ifndef SERVER_COMM_HTTP_H
#define SERVER_COMM_HTTP_H

/**
 * @file server_comm_http.h
 * @brief C5 终端本地网关 HTTP 公共接口。
 *
 * BME、voice、system command 等模块通过本层访问 ESPS3 /local/v1。本层负责
 * WiFi/heap 前置检查、URL 拼接、公共 header、状态码和流式读写；不改变 JSON 字段、
 * 不执行命令、不做 S3 -> Server 转发。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "server_comm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 公共 HTTP 保护参数：所有模块共享，调网络/内存边界时优先改这里。 */
#ifndef SERVER_COMM_HTTP_MIN_FREE_HEAP
#define SERVER_COMM_HTTP_MIN_FREE_HEAP 8192U       // 发起 HTTP 前要求的最小 free heap。
#endif

#ifndef SERVER_COMM_HTTP_MIN_LARGEST_BLOCK
#define SERVER_COMM_HTTP_MIN_LARGEST_BLOCK 4096U   // 发起 HTTP 前要求的最大连续空闲块。
#endif

#ifndef SERVER_COMM_HTTP_READ_CHUNK_BYTES
#define SERVER_COMM_HTTP_READ_CHUNK_BYTES 1024U    // 流式响应单次读取字节数。
#endif

#ifndef SERVER_COMM_HTTP_MAX_EMPTY_READS
#define SERVER_COMM_HTTP_MAX_EMPTY_READS 20        // 连续空读次数上限。
#endif

#ifndef SERVER_COMM_HTTP_EMPTY_READ_DELAY_MS
#define SERVER_COMM_HTTP_EMPTY_READ_DELAY_MS 20    // 空读后的短退避，单位 ms。
#endif

#ifndef SERVER_COMM_HTTP_STREAM_READ_TIMEOUT_MS
#define SERVER_COMM_HTTP_STREAM_READ_TIMEOUT_MS 1000 // 流式响应单次 read 超时，便于断联 abort 快速退出。
#endif

/** @brief 判断 STA 是否已连上 S3 SoftAP；业务模块可预检，公共 HTTP 内部也会检查。 */
bool server_comm_wifi_is_ready(void);

/** @brief 判断 HTTP status 是否为 2xx；调用方用于区分传输成功和业务拒绝。 */
bool server_comm_http_status_is_success(int status_code);

/** @brief 标记当前调用为语音 HTTP；仅 server_voice_client 在 /local/v1/voice/turn 前后使用。 */
void server_comm_http_set_voice_request_active(bool active);

/** @brief 标记当前调用为 local_wake wake prompt GET；不代表新的 voice turn。 */
void server_comm_http_set_wake_prompt_request_active(bool active);

/** @brief 由 app_runtime 设置语音独占 gate；普通 local_gateway_comm 请求会被安静跳过。 */
void server_comm_http_set_non_voice_paused(bool paused);

/** @brief Wait for normal HTTP requests admitted before a voice lease to finish. */
esp_err_t server_comm_http_wait_for_non_voice_idle(uint32_t timeout_ms);

/**
 * @brief 向 S3 local gateway 发起 GET JSON 请求。
 *
 * 调用位置：system command/wake prompt 等需要 GET 的模块。
 * @param endpoint /local/v1 路径；完整 URL 和 Server API 路径会被拒绝。
 * @param timeout_ms 本次请求超时，0 表示使用默认值。
 * @param response_body 可选响应缓存。
 * @param response_body_size 响应缓存长度。
 * @param response 可选 HTTP 元数据输出。
 * @return ESP_OK 表示 HTTP 传输和 status 均成功；否则返回 WiFi、内存、状态码或响应缓存错误。
 * 失败处理：上层按模块语义记录日志、稍后重试或结束当前 voice turn。
 */
esp_err_t server_comm_http_get_json(const char *endpoint,
                                    uint32_t timeout_ms,
                                    char *response_body,
                                    size_t response_body_size,
                                    server_comm_http_response_t *response);

/** @brief GET JSON 并附加 metadata header；参数/返回语义同 server_comm_http_get_json()。 */
esp_err_t server_comm_http_get_json_with_headers(const char *endpoint,
                                                 const server_comm_header_t *headers,
                                                 size_t header_count,
                                                 uint32_t timeout_ms,
                                                 char *response_body,
                                                 size_t response_body_size,
                                                 server_comm_http_response_t *response);

/** @brief POST JSON 到 S3 local gateway；公共层自动设置 Content-Type 和身份 header。 */
esp_err_t server_comm_http_post_json(const char *endpoint,
                                     const char *json_body,
                                     uint32_t timeout_ms,
                                     char *response_body,
                                     size_t response_body_size,
                                     server_comm_http_response_t *response);

/** @brief POST JSON 并附加业务 metadata header；BME/status/register/ack 等模块调用。 */
esp_err_t server_comm_http_post_json_with_headers(const char *endpoint,
                                                  const char *json_body,
                                                  const server_comm_header_t *headers,
                                                  size_t header_count,
                                                  uint32_t timeout_ms,
                                                  char *response_body,
                                                  size_t response_body_size,
                                                  server_comm_http_response_t *response);

/** @brief POST 原始 body；用于不需要边上传边读取响应的 PCM 或二进制请求。 */
esp_err_t server_comm_http_post_raw(const char *endpoint,
                                    const char *content_type,
                                    const uint8_t *body,
                                    size_t body_len,
                                    uint32_t timeout_ms,
                                    char *response_body,
                                    size_t response_body_size,
                                    server_comm_http_response_t *response);

/** @brief 开始 chunked raw POST；voice 客户端随后 write/finish/fetch/read/close。 */
esp_err_t server_comm_http_post_raw_stream_begin(const server_comm_raw_stream_config_t *config,
                                                server_comm_raw_stream_t **out_stream);

/** @brief 以固定 Content-Length POST raw body，并保留 stream 用于读取响应。 */
esp_err_t server_comm_http_post_raw_fixed_stream_begin(const server_comm_raw_stream_config_t *config,
                                                       const uint8_t *body,
                                                       size_t body_len,
                                                       server_comm_raw_stream_t **out_stream);

/** @brief 开始 GET raw stream；wake prompt 客户端用它边读取 S3 PCM 边播放。 */
esp_err_t server_comm_http_get_raw_stream_begin(const server_comm_raw_stream_config_t *config,
                                                server_comm_raw_stream_t **out_stream);

/** @brief 流式 POST 中追加一个 raw chunk；公共层负责 chunk framing。 */
esp_err_t server_comm_http_post_raw_stream_write(server_comm_raw_stream_t *stream,
                                                const uint8_t *data,
                                                size_t len);

/** @brief PCM 上传结束后调用，写入 chunked 结束符。 */
esp_err_t server_comm_http_post_raw_stream_finish_upload(server_comm_raw_stream_t *stream);

/** @brief 上传结束后先取响应头和 HTTP status；voice 客户端用它判断是否继续读 PCM。 */
esp_err_t server_comm_http_fetch_headers(server_comm_raw_stream_t *stream,
                                         server_comm_http_response_t *response);

/** @brief 取完 headers 后循环读取响应体，每个 chunk 交给 on_data。 */
esp_err_t server_comm_http_read_response(server_comm_raw_stream_t *stream,
                                         server_comm_on_data_cb_t on_data,
                                         void *user_ctx,
                                         server_comm_http_response_t *response);

/** @brief 释放 stream；任何成功或失败路径最终都要调用一次。 */
void server_comm_http_post_raw_stream_close(server_comm_raw_stream_t *stream);

/** @brief 请求 owning task 尽快中止 stream；实际 close/cleanup 仍由 owning task 执行。 */
void server_comm_http_post_raw_stream_request_abort(server_comm_raw_stream_t *stream);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_COMM_HTTP_H */

#ifndef SERVER_COMM_HTTP_H
#define SERVER_COMM_HTTP_H

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

/** 调用方法：业务模块发请求前可调用；公共 HTTP 函数内部也会自动检查。 */
bool server_comm_wifi_is_ready(void);

/** 调用方法：收到 response.status_code 后判断是否为 2xx。 */
bool server_comm_http_status_is_success(int status_code);

/** 调用方法：GET JSON 接口；endpoint 可传相对路径，response_body 可为 NULL。 */
esp_err_t server_comm_http_get_json(const char *endpoint,
                                    uint32_t timeout_ms,
                                    char *response_body,
                                    size_t response_body_size,
                                    server_comm_http_response_t *response);

/** 调用方法：GET JSON 接口并附加业务 header；用于 device protocol v1 metadata。 */
esp_err_t server_comm_http_get_json_with_headers(const char *endpoint,
                                                 const server_comm_header_t *headers,
                                                 size_t header_count,
                                                 uint32_t timeout_ms,
                                                 char *response_body,
                                                 size_t response_body_size,
                                                 server_comm_http_response_t *response);

/** 调用方法：POST JSON 字符串；公共层自动设置 Content-Type 和 X-Device-Id。 */
esp_err_t server_comm_http_post_json(const char *endpoint,
                                     const char *json_body,
                                     uint32_t timeout_ms,
                                     char *response_body,
                                     size_t response_body_size,
                                     server_comm_http_response_t *response);

/** 调用方法：POST JSON 并附加业务 header；用于 device protocol v1 metadata。 */
esp_err_t server_comm_http_post_json_with_headers(const char *endpoint,
                                                  const char *json_body,
                                                  const server_comm_header_t *headers,
                                                  size_t header_count,
                                                  uint32_t timeout_ms,
                                                  char *response_body,
                                                  size_t response_body_size,
                                                  server_comm_http_response_t *response);

/** 调用方法：POST 原始 body；PCM 或二进制 body 但不需要边写边读时使用。 */
esp_err_t server_comm_http_post_raw(const char *endpoint,
                                    const char *content_type,
                                    const uint8_t *body,
                                    size_t body_len,
                                    uint32_t timeout_ms,
                                    char *response_body,
                                    size_t response_body_size,
                                    server_comm_http_response_t *response);

/** 调用方法：开始 chunked raw POST；后续配合 write/finish/fetch/read/close。 */
esp_err_t server_comm_http_post_raw_stream_begin(const server_comm_raw_stream_config_t *config,
                                                server_comm_raw_stream_t **out_stream);

/** 调用方法：流式 POST 中追加一个 raw chunk，公共层负责 chunk framing。 */
esp_err_t server_comm_http_post_raw_stream_write(server_comm_raw_stream_t *stream,
                                                const uint8_t *data,
                                                size_t len);

/** 调用方法：PCM 上传结束后调用，写入 chunked 结束符。 */
esp_err_t server_comm_http_post_raw_stream_finish_upload(server_comm_raw_stream_t *stream);

/** 调用方法：上传结束后先取响应头和 HTTP status。 */
esp_err_t server_comm_http_fetch_headers(server_comm_raw_stream_t *stream,
                                         server_comm_http_response_t *response);

/** 调用方法：取完 headers 后循环读取响应体，每个 chunk 交给 on_data。 */
esp_err_t server_comm_http_read_response(server_comm_raw_stream_t *stream,
                                         server_comm_on_data_cb_t on_data,
                                         void *user_ctx,
                                         server_comm_http_response_t *response);

/** 调用方法：任何成功或失败路径最终都要调用一次释放 stream。 */
void server_comm_http_post_raw_stream_close(server_comm_raw_stream_t *stream);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_COMM_HTTP_H */

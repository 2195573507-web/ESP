#ifndef SERVER_COMM_TYPES_H
#define SERVER_COMM_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SERVER_COMM_CONTENT_TYPE_BUFFER_SIZE
#define SERVER_COMM_CONTENT_TYPE_BUFFER_SIZE 64U // response.content_type 缓存大小。
#endif

/* 调用方法：业务模块声明静态数组后传给 server_comm_raw_stream_config_t。 */
typedef struct {
    const char *key;
    const char *value;
} server_comm_header_t;

/* 调用方法：HTTP 调用完成后读取 status/body_len/content_type 等基础元数据。 */
typedef struct {
    int status_code;
    int64_t content_length;
    bool chunked;
    size_t body_len;
    bool body_overflow;
    char content_type[SERVER_COMM_CONTENT_TYPE_BUFFER_SIZE];
    char transfer_encoding[SERVER_COMM_CONTENT_TYPE_BUFFER_SIZE];
    char audio_format[SERVER_COMM_CONTENT_TYPE_BUFFER_SIZE];
    char audio_sample_rate[16];
    char audio_channels[16];
    char audio_version[64];
    char voice_config_hash[80];
} server_comm_http_response_t;

/* 调用方法：流式读取响应时注册，每收到一块数据就回调一次。 */
typedef esp_err_t (*server_comm_on_data_cb_t)(const uint8_t *data,
                                              size_t len,
                                              void *user_ctx);

typedef struct server_comm_raw_stream server_comm_raw_stream_t;

/* 调用方法：server_client 填 endpoint/content_type/headers 后传给 stream_begin。 */
typedef struct {
    const char *endpoint;
    const char *content_type;
    const server_comm_header_t *headers;
    size_t header_count;
    uint32_t timeout_ms;               // 建连/open/上传阶段超时，0 表示使用默认短超时。
    uint32_t fetch_headers_timeout_ms; // 上传完成后等待响应头的独立超时，0 表示沿用 timeout_ms。
    uint32_t read_timeout_ms;          // 响应体单次 read 超时，0 表示使用公共流式短超时。
    uint32_t total_timeout_ms;         // 整个 stream 生命周期上限，0 表示不额外限制。
    int buffer_size;        // 响应 header/body 接收缓冲。
    int tx_buffer_size;     // 请求行/header 发送缓冲。
} server_comm_raw_stream_config_t;

#ifdef __cplusplus
}
#endif

#endif /* SERVER_COMM_TYPES_H */

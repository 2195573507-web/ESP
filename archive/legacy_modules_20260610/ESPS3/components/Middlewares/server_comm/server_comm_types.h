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
    uint32_t timeout_ms;
    int buffer_size;
    int tx_buffer_size;
} server_comm_raw_stream_config_t;

#ifdef __cplusplus
}
#endif

#endif /* SERVER_COMM_TYPES_H */

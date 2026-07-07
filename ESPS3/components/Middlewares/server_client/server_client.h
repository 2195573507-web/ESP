#ifndef SERVER_CLIENT_H
#define SERVER_CLIENT_H

/**
 * @file server_client.h
 * @brief S3 网关到 ESP-server 的 HTTP 客户端接口。
 *
 * C5 从不直接调用本模块；local_http_server/protocol_adapter 结束 C5<->S3 轻量协议后，
 * S3 通过本模块访问完整 /api/device/v1/ingest、/kernel/csi_event、logs、
 * smart-home、/api/voice/turn。本层只做 HTTP transport，不推断或改写 schema。
 */

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SERVER_CLIENT_SMALL_BODY_BYTES 2048U

typedef esp_err_t (*server_client_data_cb_t)(const uint8_t *data, size_t len, void *user_ctx);
typedef void (*server_client_voice_meta_cb_t)(int64_t content_length, void *user_ctx);

/** @brief POST 完整 ingest JSON 到 Server；sensor_aggregator 调用，失败由 offline_policy 记录。 */
esp_err_t server_client_post_ingest_json(const char *json_body,
                                         char *response_body,
                                         size_t response_body_size,
                                         int *http_status);
/** @brief POST S3 canonical CSI event v2 到 Server；payload 已由 protocol_adapter 严格校验。 */
esp_err_t server_client_post_csi_event_json(const char *json_body,
                                            char *response_body,
                                            size_t response_body_size,
                                            int *http_status);
/** @brief POST S3 dashboard snapshot 到 Server；sensor_aggregator 调用，失败由 offline_policy 记录。 */
esp_err_t server_client_post_gateway_state_json(const char *json_body,
                                                char *response_body,
                                                size_t response_body_size,
                                                int *http_status);
/** @brief POST system log 到 Server。 */
esp_err_t server_client_post_system_log_json(const char *json_body,
                                             char *response_body,
                                             size_t response_body_size,
                                             int *http_status);
/** @brief POST alarm log 到 Server。 */
esp_err_t server_client_post_alarm_json(const char *json_body,
                                        char *response_body,
                                        size_t response_body_size,
                                        int *http_status);
/** @brief 从 smart-home 队列领取待执行命令；smart_home_gateway 调用。 */
esp_err_t server_client_get_smart_home_pending(char *response_body,
                                               size_t response_body_size,
                                               int *http_status);
/** @brief ACK smart-home 命令；无真实设备时由 smart_home_gateway 传入 failed body。 */
esp_err_t server_client_ack_smart_home_command(const char *command_id,
                                               const char *ack_json,
                                               char *response_body,
                                               size_t response_body_size,
                                               int *http_status);
/** @brief 从 Server 拉取完整 device_id 的待执行命令；command_router 调用。 */
esp_err_t server_client_get_pending_commands(const char *device_id,
                                             char *response_body,
                                             size_t response_body_size,
                                             int *http_status);
/** @brief 将 C5 命令 ack 映射后的完整 JSON 回传给 Server；command_router 调用。 */
esp_err_t server_client_ack_command(const char *command_id,
                                    const char *ack_json,
                                    char *response_body,
                                    size_t response_body_size,
                                    int *http_status);
/**
 * @brief 转发一次语音 turn 到 Server。
 *
 * 调用位置：voice_proxy_handle_turn()。
 * @param device_id 完整 C5 device_id，不能为空。
 * @param pcm C5 上传的 PCM16 数据，不能为空。
 * @param pcm_len PCM 字节数。
 * @param on_data Server 返回 PCM chunk 时调用的回调，可为空时仅丢弃响应数据。
 * @param user_ctx 回调上下文。
 * @param http_status 输出 Server HTTP status，可为空。
 * @param response_content_length 输出 Server 响应 Content-Length；chunked/未知时为 -1，可为空。
 * @param on_meta 收到 Server 响应头后调用；voice_proxy 用于在首个 PCM chunk 前记录 expected_bytes。
 * @param meta_ctx on_meta 上下文。
 * @return ESP_OK 表示 Server 返回 2xx；STA 未连接、HTTP 失败或非 2xx 返回错误码。
 * 失败处理：voice_proxy 将错误映射为本地 JSON 错误，offline_policy 记录降级状态。
 */
esp_err_t server_client_post_voice_turn(const char *device_id,
                                        const uint8_t *pcm,
                                        size_t pcm_len,
                                        server_client_data_cb_t on_data,
                                        void *user_ctx,
                                        int *http_status,
                                        int64_t *response_content_length,
                                        server_client_voice_meta_cb_t on_meta,
                                        void *meta_ctx);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_CLIENT_H */

#ifndef SERVER_CLIENT_H
#define SERVER_CLIENT_H

/**
 * @file server_client.h
 * @brief S3 网关到 ESP-server 的 HTTP 客户端接口。
 *
 * C5 从不直接调用本模块；local_http_server/protocol_adapter 结束 C5<->S3 轻量协议后，
 * S3 通过本模块访问完整 /api/device/v1/ingest、logs、
 * smart-home、/api/voice/turn。本层只做 HTTP transport，不推断或改写 schema。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SERVER_CLIENT_SMALL_BODY_BYTES 2048U

typedef esp_err_t (*server_client_data_cb_t)(const uint8_t *data, size_t len, void *user_ctx);
typedef struct {
    int64_t content_length;
    int http_status;
    bool chunked;
    const char *content_type;
    const char *audio_format;
    const char *audio_sample_rate;
    const char *audio_channels;
} server_client_voice_response_meta_t;

typedef esp_err_t (*server_client_voice_meta_cb_t)(const server_client_voice_response_meta_t *meta,
                                                   void *user_ctx);
typedef bool (*server_client_cancel_cb_t)(void *user_ctx);

/** @brief POST 完整 ingest JSON 到 Server；sensor_aggregator 调用，失败由 offline_policy 记录。 */
esp_err_t server_client_post_ingest_json(const char *json_body,
                                         char *response_body,
                                         size_t response_body_size,
                                         int *http_status);
/** @brief 可取消 ingest；callback 返回 true 时停止后续 retry。 */
esp_err_t server_client_post_ingest_json_cancellable(const char *json_body,
                                                     char *response_body,
                                                     size_t response_body_size,
                                                     int *http_status,
                                                     server_client_cancel_cb_t cancel_cb,
                                                     void *cancel_ctx);
/**
 * @brief Device-scoped cancellable ingest used by C5 sensor upload/replay.
 *
 * The existing cancellable API remains unscoped for compatibility. Requests made through this
 * variant can also be interrupted by server_client_cancel_peer_requests().
 */
esp_err_t server_client_post_ingest_json_cancellable_for_device(
    const char *device_id,
    const char *json_body,
    char *response_body,
    size_t response_body_size,
    int *http_status,
    server_client_cancel_cb_t cancel_cb,
    void *cancel_ctx);
/** @brief POST S3 gateway-state telemetry 到 Server。 */
esp_err_t server_client_post_gateway_state_json(const char *json_body,
                                                char *response_body,
                                                size_t response_body_size,
                                                int *http_status);
/**
 * @brief POST dashboard snapshot as best-effort traffic.
 *
 * Snapshot never occupies the core HTTP slot and returns ESP_ERR_NOT_FINISHED when higher
 * priority core or telemetry traffic is active. It performs one attempt only.
 */
esp_err_t server_client_post_gateway_snapshot_json(const char *json_body,
                                                   char *response_body,
                                                   size_t response_body_size,
                                                   int *http_status);
/** @brief True when core or telemetry work must run before a best-effort snapshot. */
bool server_client_snapshot_upload_should_skip(void);
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
/** @brief POST habit-rule event to ESP-server.  Reporter owns retries. */
esp_err_t server_client_post_habit_event_json(const char *json_body,
                                              char *response_body,
                                              size_t response_body_size,
                                              int *http_status);

/** @brief 请求 ESP-server 刷新其拥有的天气上下文；不传输或保存天气数据。 */
esp_err_t server_client_post_weather_refresh_json(const char *json_body,
                                                  char *response_body,
                                                  size_t response_body_size,
                                                  int *http_status);
/** @brief 轻量探测 ESP-server 是否可达；network_worker 在打开 LINK_STABLE 前调用。 */
esp_err_t server_client_probe_available(int *http_status);

/**
 * @brief HTTP memory admission classes for S3 external requests.
 *
 * BEST_EFFORT covers probe/overview, weather, snapshot, and BME replay.
 * TELEMETRY covers device ingest/logs/alarms. CORE covers command and voice.
 * Local SoftAP sensor receive does not use these gates.
 */
typedef enum {
    SERVER_CLIENT_HTTP_CLASS_CORE = 0,
    SERVER_CLIENT_HTTP_CLASS_TELEMETRY,
    SERVER_CLIENT_HTTP_CLASS_BEST_EFFORT,
} server_client_http_class_t;

/**
 * @brief Gate external HTTP/TLS when internal or DMA heap is under pressure.
 *
 * @return ESP_OK when a new connection may start.
 * @return ESP_ERR_HTTP_EAGAIN when pressure or unified backoff requires deferral.
 */
esp_err_t server_client_http_mem_admission(server_client_http_class_t http_class,
                                           const char *endpoint);

/** @brief True when best-effort external HTTP should stay deferred. */
bool server_client_http_best_effort_should_defer(void);
uint32_t server_client_http_mem_backoff_remaining_ms(server_client_http_class_t http_class);
/**
 * @brief STA epoch 失效或切换时取消旧 epoch 的活跃 HTTP 请求。
 *
 * current_epoch 为 0 表示当前无有效 STA IPv4；函数由 network_worker 调用，不在 WiFi callback
 * 中执行阻塞操作。
 */
void server_client_on_network_epoch_changed(uint32_t current_epoch, const char *reason);
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
/** @brief 可取消 pending 拉取；callback 返回 true 时停止后续 retry。 */
esp_err_t server_client_get_pending_commands_cancellable(const char *device_id,
                                                         char *response_body,
                                                         size_t response_body_size,
                                                         int *http_status,
                                                         server_client_cancel_cb_t cancel_cb,
                                                         void *cancel_ctx);
/**
 * @brief Interrupt active command-poll and sensor-ingest HTTP requests for one C5.
 *
 * Idempotent when no scoped request is active. Unrelated gateway, voice and ACK requests are
 * never registered and are therefore unaffected.
 */
esp_err_t server_client_cancel_peer_requests(const char *device_id);
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
 * @param on_meta 收到 Server 响应头后调用；voice_proxy 用于验证并透传 PCM 元数据。
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

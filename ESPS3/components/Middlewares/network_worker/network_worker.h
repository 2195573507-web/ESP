#ifndef NETWORK_WORKER_H
#define NETWORK_WORKER_H

/**
 * @file network_worker.h
 * @brief ESPS3 网络状态 worker 和 Server 上云 gate。
 *
 * WiFi/IP callback 只提交事件；本模块负责 STA connect/reconnect、LINK_STABLE 门控、
 * 以及把 Server JSON/command/snapshot 工作交给后台 worker。业务 cadence 仍由
 * s3_scheduler 决定，本模块只在网络稳定后执行上云动作。
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NETWORK_WORKER_EVENT_LINK_UP = 0,
    NETWORK_WORKER_EVENT_LINK_DOWN,
    NETWORK_WORKER_EVENT_IP_READY,
    NETWORK_WORKER_EVENT_SCAN_DONE,
} network_worker_event_t;

typedef enum {
    NETWORK_WORKER_SOURCE_UNKNOWN = 0,
    NETWORK_WORKER_SOURCE_SOFTAP_START,
    NETWORK_WORKER_SOURCE_SOFTAP_STOP,
    NETWORK_WORKER_SOURCE_AP_STA_CONNECTED,
    NETWORK_WORKER_SOURCE_AP_STA_DISCONNECTED,
    NETWORK_WORKER_SOURCE_STA_START,
    NETWORK_WORKER_SOURCE_STA_CONNECTED,
    NETWORK_WORKER_SOURCE_STA_STOP,
    NETWORK_WORKER_SOURCE_STA_DISCONNECTED,
    NETWORK_WORKER_SOURCE_STA_GOT_IP,
    NETWORK_WORKER_SOURCE_STA_LOST_IP,
    NETWORK_WORKER_SOURCE_STA_SCAN_DONE,
    NETWORK_WORKER_SOURCE_LOCAL_HTTP_ENABLE,
} network_worker_event_source_t;

typedef enum {
    NETWORK_WORKER_LINK_DOWN = 0,
    NETWORK_WORKER_LINK_UP,
    NETWORK_WORKER_LINK_IP_READY,
    NETWORK_WORKER_LINK_STABLE,
} network_worker_link_state_t;

typedef enum {
    NETWORK_WORKER_SERVER_JSON_INGEST = 0,
    NETWORK_WORKER_SERVER_JSON_GATEWAY_STATE,
    NETWORK_WORKER_SERVER_JSON_SYSTEM_LOG,
    NETWORK_WORKER_SERVER_JSON_ALARM,
    NETWORK_WORKER_SERVER_JSON_ENVIRONMENT_ALARM,
    NETWORK_WORKER_SERVER_JSON_HOME_AI_EVENTS,
    NETWORK_WORKER_SERVER_JSON_HOME_AI_VIRTUAL_STATE,
    NETWORK_WORKER_SERVER_JSON_HOME_AI_DEPLOYMENT,
    NETWORK_WORKER_SERVER_JSON_HOME_AI_HISTORY,
    NETWORK_WORKER_SERVER_JSON_COUNT,
} network_worker_server_json_type_t;

typedef void (*network_worker_environment_alarm_completion_fn)(uint64_t event_seq,
                                                                esp_err_t result,
                                                                int http_status,
                                                                void *context);

typedef struct {
    uint32_t snapshot_skip_count;
    uint32_t snapshot_upload_count;
    uint32_t snapshot_coalesce_count;
} network_worker_snapshot_stats_t;

/** @brief 初始化 network/upload/command worker 队列和任务；gateway_orchestrator 启动时调用。 */
esp_err_t network_worker_init(void);

/** @brief 请求 worker 在 SoftAP ready 后启动本地 HTTP；重复调用安全。 */
esp_err_t network_worker_enable_local_http_server(void);

/** @brief WiFi/IP callback 投递网络事件；函数只入队，不做阻塞网络操作。 */
esp_err_t network_worker_post_event(network_worker_event_t event,
                                    network_worker_event_source_t source,
                                    uint32_t ip_addr,
                                    uint8_t disconnect_reason);

/** @brief AP station callback variant that preserves MAC/AID for device-level release. */
esp_err_t network_worker_post_ap_station_event(network_worker_event_t event,
                                               network_worker_event_source_t source,
                                               uint32_t ip_addr,
                                               const uint8_t mac[6],
                                               uint8_t aid);

/** @brief 将已经通过 register/heartbeat/sensor 验证的 C5 identity 绑定到 pending SoftAP station。 */
esp_err_t network_worker_bind_ap_station_identity(const char *device_id, const char *peer_ip);

/** @brief 读取当前网关链路状态；诊断日志或 health 路径调用。 */
network_worker_link_state_t network_worker_get_link_state(void);

/** @brief 读取经连续成功/失败去抖后的 Server ready 状态。 */
bool network_worker_is_server_ready(void);

/** @brief 将链路状态转为稳定日志字符串。 */
const char *network_worker_link_state_name(network_worker_link_state_t state);

/**
 * @brief 提交一段已构造好的 Server JSON 给 upload worker。
 *
 * 所有权：成功入队后 json_body 由 network_worker 释放；失败时调用方仍负责释放。
 */
esp_err_t network_worker_submit_server_json(network_worker_server_json_type_t type,
                                            char *json_body,
                                            const char *source);

esp_err_t network_worker_submit_home_ai_events(char *json_body, const char *source);
esp_err_t network_worker_submit_home_ai_virtual_state(char *json_body, const char *source);
esp_err_t network_worker_submit_home_ai_deployment(char *json_body, const char *source);
esp_err_t network_worker_submit_home_ai_history(char *json_body, const char *source);

/** @brief Per-peer variant used by sensor paths for consumer-side lifecycle gating. */
esp_err_t network_worker_submit_peer_server_json(network_worker_server_json_type_t type,
                                                 char *json_body,
                                                 const char *device_id,
                                                 const char *source);

/**
 * @brief Submit one environment alarm and receive its terminal HTTP outcome.
 *
 * Ownership: successful enqueue transfers @p json_body to network_worker.
 * A link transition may retain the high-priority work until it can run; the
 * completion callback is invoked only after an actual attempt or a terminal
 * local drop. Existing telemetry/alarm queue behavior is unchanged.
 */
esp_err_t network_worker_submit_environment_alarm_json(
    char *json_body,
    uint64_t event_seq,
    network_worker_environment_alarm_completion_fn completion,
    void *completion_context,
    const char *source);

/**
 * @brief 提交一段已写入 BME cache 的 Server ingest JSON。
 *
 * 所有权：成功入队后 json_body 由 network_worker 释放；失败时调用方仍负责释放。
 * Server 返回 2xx 后按 cache_sequence 删除缓存头部；失败时缓存保留给 replay worker。
 */
esp_err_t network_worker_submit_bme_cached_json(char *json_body,
                                                uint32_t cache_sequence,
                                                const char *source);

/** @brief Per-peer cached BME submission; cache is retained when the peer is suspended. */
esp_err_t network_worker_submit_bme_cached_json_for_peer(char *json_body,
                                                         uint32_t cache_sequence,
                                                         const char *device_id,
                                                         const char *source);

/** @brief Drop queued live work for a peer without deleting retained cache/history. */
esp_err_t network_worker_release_peer_resources(const char *device_id);

/** @brief Wake retained sensor replay after a peer becomes RESTORING or ACTIVE. */
esp_err_t network_worker_restore_peer_resources(const char *device_id);

/** @brief 请求上传一次 dashboard/gateway snapshot；scheduler 周期调用。 */
esp_err_t network_worker_enqueue_snapshot_upload(void);

/** @brief 读取低优先级 dashboard snapshot 的累计调度统计。 */
network_worker_snapshot_stats_t network_worker_get_snapshot_stats(void);

/** @brief 请求从 Server 拉取 pending command；scheduler 周期调用。 */
esp_err_t network_worker_enqueue_command_pull(void);

/** @brief 提交 C5 command ack JSON；本函数会拷贝 ack_json 后入队。 */
esp_err_t network_worker_enqueue_command_ack(const char *command_id, const char *ack_json);

/** @brief 请求 smart-home pending/ack 轮询；当前无真实执行器时仍走失败 ACK 语义。 */
esp_err_t network_worker_enqueue_smart_home_poll(void);
/** @brief 请求现有 command worker 拉取并校验一次 Home AI rule package。 */
esp_err_t network_worker_enqueue_home_ai_rule_sync(void);
/** @brief Check the lightweight Server rule-version notification. */
esp_err_t network_worker_enqueue_home_ai_rule_notification(void);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_WORKER_H */

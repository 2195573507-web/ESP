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
} network_worker_event_t;

typedef enum {
    NETWORK_WORKER_SOURCE_UNKNOWN = 0,
    NETWORK_WORKER_SOURCE_SOFTAP_START,
    NETWORK_WORKER_SOURCE_SOFTAP_STOP,
    NETWORK_WORKER_SOURCE_AP_STA_CONNECTED,
    NETWORK_WORKER_SOURCE_AP_STA_DISCONNECTED,
    NETWORK_WORKER_SOURCE_STA_START,
    NETWORK_WORKER_SOURCE_STA_STOP,
    NETWORK_WORKER_SOURCE_STA_DISCONNECTED,
    NETWORK_WORKER_SOURCE_STA_GOT_IP,
} network_worker_event_source_t;

typedef enum {
    NETWORK_WORKER_LINK_DOWN = 0,
    NETWORK_WORKER_LINK_UP,
    NETWORK_WORKER_LINK_IP_READY,
    NETWORK_WORKER_LINK_STABLE,
} network_worker_link_state_t;

typedef enum {
    NETWORK_WORKER_SERVER_JSON_INGEST = 0,
    NETWORK_WORKER_SERVER_JSON_CSI_EVENT,
    NETWORK_WORKER_SERVER_JSON_GATEWAY_STATE,
    NETWORK_WORKER_SERVER_JSON_SYSTEM_LOG,
    NETWORK_WORKER_SERVER_JSON_ALARM,
} network_worker_server_json_type_t;

/** @brief 初始化 network/upload/command worker 队列和任务；gateway_orchestrator 启动时调用。 */
esp_err_t network_worker_init(void);

/** @brief WiFi/IP callback 投递网络事件；函数只入队，不做阻塞网络操作。 */
esp_err_t network_worker_post_event(network_worker_event_t event,
                                    network_worker_event_source_t source,
                                    uint32_t ip_addr);

/** @brief 读取当前网关链路状态；诊断日志或 health 路径调用。 */
network_worker_link_state_t network_worker_get_link_state(void);

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

/** @brief 请求上传一次 dashboard/gateway snapshot；scheduler 周期调用。 */
esp_err_t network_worker_enqueue_snapshot_upload(void);

/** @brief 请求从 Server 拉取 pending command；scheduler 周期调用。 */
esp_err_t network_worker_enqueue_command_pull(void);

/** @brief 提交 C5 command ack JSON；本函数会拷贝 ack_json 后入队。 */
esp_err_t network_worker_enqueue_command_ack(const char *command_id, const char *ack_json);

/** @brief 请求 smart-home pending/ack 轮询；当前无真实执行器时仍走失败 ACK 语义。 */
esp_err_t network_worker_enqueue_smart_home_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_WORKER_H */

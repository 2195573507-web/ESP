#ifndef S3_SCHEDULER_H
#define S3_SCHEDULER_H

/**
 * @file s3_scheduler.h
 * @brief ESPS3 优先级事件总线门面和自适应 runtime scheduler。
 *
 * HTTP、UDP stream、CSI、command、network state 和周期任务都先进入本模块。
 * s3_scheduler.c 保留为兼容入口和 worker 调度壳；实际 backpressure policy 由
 * s3_event_bus 执行：CRITICAL > REALTIME > STATE > BACKGROUND。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp111_protocol_common.h"
#include "resource_manager.h"
#include "s3_event_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

#define S3_RUNTIME_BUS_DID_LEN 16U
#define S3_RUNTIME_BUS_TYPE_LEN 8U
#define S3_RUNTIME_BUS_LID_LEN 16U
#define S3_RUNTIME_BUS_DEVICE_ID_LEN 48U
#define S3_RUNTIME_BUS_PEER_IP_LEN 16U
#define S3_RUNTIME_BUS_COMMAND_ID_LEN 48U
#define S3_RUNTIME_BUS_BODY_MAX 4096U

/* Dashboard snapshot is best-effort state, never an event backlog. */
#ifndef UPLOAD_SNAPSHOT_INTERVAL_MS
#define UPLOAD_SNAPSHOT_INTERVAL_MS 10000U
#endif

typedef enum {
    S3_RUNTIME_MSG_UNKNOWN = 0,
    S3_RUNTIME_MSG_CSI,
    S3_RUNTIME_MSG_SENSOR,
    S3_RUNTIME_MSG_STATUS,
    S3_RUNTIME_MSG_EVENT,
} s3_runtime_msg_kind_t;

typedef struct unified_msg {
    int64_t t;
    char did[S3_RUNTIME_BUS_DID_LEN];
    char type[S3_RUNTIME_BUS_TYPE_LEN];
    char lid[S3_RUNTIME_BUS_LID_LEN];
    float v1;
    float v2;
    float v3;
} unified_msg_t;

/** @brief S3 runtime bus ingress；body 是 C5 原始本地 payload，unified 是用于日志/快照的扁平摘要。 */
typedef struct {
    s3_runtime_msg_kind_t kind;
    unified_msg_t unified;
    char device_id[S3_RUNTIME_BUS_DEVICE_ID_LEN];
    char peer_ip[S3_RUNTIME_BUS_PEER_IP_LEN];
    char command_id[S3_RUNTIME_BUS_COMMAND_ID_LEN];
    bool is_stream_frame;
    int64_t rx_time_us;
    int64_t rx_time_ms;
    uint32_t resource_generation;
    resource_manager_session_state_t resource_state_at_rx;
    int64_t resource_state_since_ms_at_rx;
    size_t body_len;
    char body[S3_RUNTIME_BUS_BODY_MAX + 1U];
} s3_runtime_ingress_t;

typedef enum {
    S3_SCHEDULER_PRIORITY_HIGH = 0,
    S3_SCHEDULER_PRIORITY_NORMAL,
    S3_SCHEDULER_PRIORITY_LOW,
} s3_scheduler_priority_t;

typedef enum {
    S3_SCHEDULER_EVENT_NONE = 0,
    S3_SCHEDULER_EVENT_INGRESS,
    S3_SCHEDULER_EVENT_STREAM_FRAME,
    S3_SCHEDULER_EVENT_STREAM_SEND,
    S3_SCHEDULER_EVENT_NETWORK_STATE,
    S3_SCHEDULER_EVENT_VOICE_STATE,
    S3_SCHEDULER_EVENT_COMMAND_PULL,
    S3_SCHEDULER_EVENT_CSI_FUSION_FLUSH,
    S3_SCHEDULER_EVENT_BACKGROUND_STATS,
} s3_scheduler_event_type_t;

typedef enum {
    S3_SCHEDULER_NET_NOT_READY = 0,
    S3_SCHEDULER_STA_CONNECTED,
    S3_SCHEDULER_IP_READY,
    S3_SCHEDULER_LINK_STABLE,
    S3_SCHEDULER_NET_BLOCKED = S3_SCHEDULER_NET_NOT_READY,
    S3_SCHEDULER_NET_READY = S3_SCHEDULER_IP_READY,
} s3_scheduler_network_state_t;

typedef struct {
    size_t queue_depth;
    size_t high_depth;
    size_t normal_depth;
    size_t low_depth;
    size_t critical_depth;
    size_t realtime_depth;
    size_t state_depth;
    size_t background_depth;
    uint32_t drop_count;
    uint32_t coalesce_count;
    uint32_t csi_ingress_drop_count;
    uint32_t csi_ingress_coalesce_count;
    uint32_t csi_worker_yield_count;
    s3_event_bus_csi_latest_t csi_latest;
    s3_scheduler_network_state_t network_state;
    bool voice_busy;
    uint32_t csi_interval_ms;
    uint32_t upload_interval_ms;
    uint32_t smart_home_interval_ms;
} s3_scheduler_load_t;

/** @brief HTTP ingress admission timings captured without taking the bus lock twice. */
typedef struct {
    uint32_t event_bus_lock_wait_ms;
    uint32_t enqueue_duration_ms;
    bool event_bus_stats_valid;
    s3_event_bus_stats_t event_bus;
} s3_scheduler_enqueue_diagnostics_t;

/** scheduler 事件。入队后 payload/ingress 会被 scheduler 拷贝或接管，调用方不再持有。 */
typedef struct s3_scheduler_event {
    s3_scheduler_event_type_t type;
    s3_scheduler_priority_t priority;
    s3_event_bus_level_t bus_level;
    s3_event_bus_state_key_t state_key;
    s3_runtime_ingress_t *ingress;
    s3_scheduler_network_state_t network_state;
    bool voice_busy;
    char peer_ip[S3_RUNTIME_BUS_PEER_IP_LEN];
    char source[24];
    uint16_t peer_port;
    size_t payload_len;
    uint8_t *payload;
} s3_scheduler_event_t;

/** @brief 初始化 scheduler 队列、锁和 worker queue；启动前调用一次。 */
esp_err_t s3_scheduler_init(void);

/** @brief 启动 scheduler/protocol/stream worker 任务；可重复调用。 */
esp_err_t s3_scheduler_start(void);

/** @brief 入队一个通用事件；函数会拷贝事件中需要异步使用的数据。 */
esp_err_t s3_scheduler_enqueue_event(const s3_scheduler_event_t *event);

/** @brief 拷贝一份 C5 ingress 并入队；调用方仍拥有原 ingress。 */
esp_err_t s3_scheduler_enqueue_ingress(const s3_runtime_ingress_t *ingress,
                                       s3_scheduler_priority_t priority);

/** @brief 接管调用方分配的 ingress；无论成功失败，本函数都会释放或转移所有权。 */
esp_err_t s3_scheduler_enqueue_ingress_owned(s3_runtime_ingress_t *ingress,
                                             s3_scheduler_priority_t priority);

/**
 * @brief Transfer ingress ownership with a bounded event-bus lock wait.
 *
 * This is intentionally limited to local HTTP admission. Other scheduler
 * callers keep their established reliable retry semantics.
 */
esp_err_t s3_scheduler_enqueue_ingress_owned_timed(
    s3_runtime_ingress_t *ingress,
    s3_scheduler_priority_t priority,
    uint32_t event_bus_lock_timeout_ms,
    s3_scheduler_enqueue_diagnostics_t *out_diagnostics);

/** @brief 入队网络状态变化；network_worker/gateway_wifi 事件路径调用。 */
esp_err_t s3_scheduler_enqueue_network_state(s3_scheduler_network_state_t state);

/** @brief 入队一帧 C5 UDP/HTTP stream JSON，由 stream worker 解析。 */
esp_err_t s3_scheduler_enqueue_stream_frame(const char *json,
                                            size_t json_len,
                                            const char *peer_ip,
                                            const char *source);

/** @brief 入队一帧需要发回 C5 的 UDP payload；主要用于 CSI trigger。 */
esp_err_t s3_scheduler_enqueue_stream_send(const char *peer_ip,
                                           uint16_t peer_port,
                                           const void *payload,
                                           size_t payload_len,
                                           const char *source);

/** @brief 请求下一轮 Server command pull；scheduler 或外部事件可触发。 */
esp_err_t s3_scheduler_enqueue_command_pull(void);

/** @brief 清空 stream worker 队列；网络 gate 关闭或 LINK_STABLE 重新进入时调用。 */
void s3_scheduler_reset_stream_queue(const char *reason);

/** @brief Drop queued CSI ingress received at/before cutoff_us for one child session. */
void s3_scheduler_clear_csi_peer_before(const char *device_id,
                                        int64_t cutoff_us,
                                        const char *reason);

/** @brief Drop queued CSI ingress for one released child session. */
void s3_scheduler_clear_csi_peer(const char *device_id, const char *reason);

/** @brief 执行一次周期 tick；由 scheduler task 定期调用。 */
void s3_scheduler_tick(void);

/** @brief 读取当前队列负载和自适应后的周期参数。 */
s3_scheduler_load_t s3_scheduler_get_load(void);

/** @brief 设置 S3 voice proxy 忙状态；忙时普通上云和命令拉取会降频/暂停。 */
void s3_scheduler_set_voice_busy(bool busy);

/** @brief 查询 voice proxy 是否正在独占。 */
bool s3_scheduler_is_voice_busy(void);

/** @brief 设置 S3 网络状态；状态变化会输出一条 transition 日志。 */
void s3_scheduler_set_network_state(s3_scheduler_network_state_t state);

/** @brief 读取 scheduler 当前网络状态。 */
s3_scheduler_network_state_t s3_scheduler_get_network_state(void);

/** @brief 本地 C5<->S3 ingest 是否可用；只依赖 SoftAP/local services。 */
bool s3_scheduler_is_net_ready(void);

/** @brief STA 是否已经连接或进入更高状态。 */
bool s3_scheduler_is_sta_connected(void);

/** @brief 是否允许访问 ESP-server；只有 link stable 才允许。 */
bool s3_scheduler_is_server_upload_allowed(void);

/** @brief 将网络状态转为稳定日志字符串。 */
const char *s3_scheduler_network_state_name(s3_scheduler_network_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* S3_SCHEDULER_H */

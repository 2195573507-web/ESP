#ifndef CHILD_REGISTRY_H
#define CHILD_REGISTRY_H

/**
 * @file child_registry.h
 * @brief S3 网关子设备 allowlist 与在线状态接口。
 *
 * C5 短 id 先由 protocol_adapter 映射为完整 device_id，再由本模块校验 allowlist。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHILD_REGISTRY_DEVICE_ID_LEN 48U
#define CHILD_REGISTRY_ROOM_ID_LEN 48U
#define CHILD_REGISTRY_ALIAS_LEN 64U
#define CHILD_REGISTRY_CAPABILITIES_LEN 160U
#define CHILD_REGISTRY_PEER_MAC_LEN 6U

typedef enum {
    CHILD_REGISTRY_STATUS_OFFLINE = 0,
    CHILD_REGISTRY_STATUS_LINK_LOST,
    CHILD_REGISTRY_STATUS_ONLINE,
    CHILD_REGISTRY_STATUS_VOICE_BUSY,
} child_registry_status_t;

typedef struct {
    char device_id[CHILD_REGISTRY_DEVICE_ID_LEN];
    char room_id[CHILD_REGISTRY_ROOM_ID_LEN];
    char alias[CHILD_REGISTRY_ALIAS_LEN];
    char capabilities[CHILD_REGISTRY_CAPABILITIES_LEN];
    char peer_ip[16];
    uint8_t peer_mac[CHILD_REGISTRY_PEER_MAC_LEN];
    uint32_t last_seq;
    int64_t last_seen_ms;
    int64_t link_lost_since_ms;
    child_registry_status_t status;
    bool registered;
    bool online;
    bool peer_mac_valid;
} child_registry_entry_t;

/**
 * @brief child_registry 对外暴露的权威在线状态视图。
 *
 * 所有字段都只由本注册表的 register/heartbeat、link_lost grace 和 voice_busy
 * 状态机生成；调用方不得用 Server、网络上传或业务聚合时间戳重新判定 online/status。
 */
typedef struct {
    bool registered;
    bool online;
    child_registry_status_t status;
    int64_t last_seen_ms;
    bool link_lost;
    bool voice_busy;
    const char *offline_reason;
} child_registry_status_view_t;

/** @brief 初始化注册表锁和内存；gateway_orchestrator_start() 调用，返回 ESP_OK 或 ESP_ERR_NO_MEM。 */
esp_err_t child_registry_init(void);
/** @brief 判断完整 device_id 是否在 allowlist；local_http_server/voice_proxy 调用。 */
bool child_registry_is_allowed(const char *device_id);
/**
 * @brief 注册或更新一个 C5 子设备。
 *
 * 调用位置：/local/v1/register handler。
 * @param device_id 完整终端 device_id，不能为空且必须在 allowlist。
 * @param room_id 房间 ID，可为空。
 * @param alias 设备别名，可为空。
 * @param capabilities 能力 JSON 字符串，可为空。
 * @param seq 本次请求序号。
 * @return ESP_OK 表示已登记；不在 allowlist 返回 ESP_ERR_NOT_ALLOWED；表满返回 ESP_ERR_NO_MEM。
 * 失败处理：local_http_server 映射为 403/错误响应。
 */
esp_err_t child_registry_register_or_update(const char *device_id,
                                            const char *room_id,
                                            const char *alias,
                                            const char *capabilities,
                                            uint32_t seq);
/** @brief 刷新子设备 last_seen；heartbeat/status/sensor/voice/command 路径调用。 */
esp_err_t child_registry_touch(const char *device_id, uint32_t seq);
/** @brief Confirm validated session identity without changing retained config or sequence. */
esp_err_t child_registry_confirm_identity(const char *device_id);
/** @brief 仅记录合法活动时间和 seq，不改变 LINK_LOST/OFFLINE session 状态。 */
esp_err_t child_registry_note_activity(const char *device_id, uint32_t seq);
/** @brief 记录 C5 最近一次本地 HTTP 请求来源 IP；CSI trigger 使用该 IP 发送轻量 UDP 触发包。 */
esp_err_t child_registry_update_peer_ip(const char *device_id, const char *peer_ip);
/** @brief 读取 C5 当前记录的本地来源 IP；未登记或未记录时返回 false。 */
bool child_registry_get_peer_ip(const char *device_id, char *out, size_t out_size);
/** @brief 根据当前 peer IP 查找 device_id；仅用于发送前的设备级资源校验。 */
bool child_registry_find_device_by_peer_ip(const char *peer_ip,
                                           char *out_device_id,
                                           size_t out_size);
/** @brief 记录 device_id 对应的 SoftAP station MAC。 */
esp_err_t child_registry_update_peer_mac(const char *device_id,
                                         const uint8_t peer_mac[CHILD_REGISTRY_PEER_MAC_LEN]);
/** @brief 根据 SoftAP station MAC 查找 device_id。 */
bool child_registry_find_device_by_peer_mac(
    const uint8_t peer_mac[CHILD_REGISTRY_PEER_MAC_LEN],
    char *out_device_id,
    size_t out_size);
/** @brief WiFi AP_STADISCONNECTED 时进入 link_lost 宽限期，不立刻删除或标 offline。 */
void child_registry_mark_link_lost(const char *device_id, const char *reason);
/** @brief 使用 WiFi callback 时间作为 grace 起点的精确版本。 */
void child_registry_mark_link_lost_at(const char *device_id,
                                      int64_t disconnected_at_ms,
                                      const char *reason);
/** @brief 兼容入口：无法识别断开 station 时将所有已注册 child 标记为 link_lost。 */
void child_registry_mark_all_link_lost(const char *reason);
/** @brief voice_proxy 开始/结束时标记 voice_busy；期间普通 heartbeat 缺失不判 offline。 */
void child_registry_set_voice_busy(const char *device_id, bool busy);
/** @brief 根据 heartbeat 超时判断设备是否在线；状态/诊断调用。 */
bool child_registry_is_online(const char *device_id);
/** @brief 将当前状态刷新并返回是否发生变化；S3 事件上报调用。 */
bool child_registry_refresh_status_change(const char *device_id,
                                         child_registry_status_t *out_status,
                                         child_registry_status_t *out_previous_status);
/** @brief 读取带 voice_busy/link_lost/offline 细分的状态。 */
child_registry_status_t child_registry_get_status(const char *device_id);
/** @brief 读取状态并返回该 device 是否已有注册表记录；snapshot/dashboard 用于避免旧 last_seen 盖过 offline。 */
bool child_registry_get_status_info(const char *device_id, child_registry_status_t *out_status);
/**
 * @brief 读取 dashboard/gateway snapshot 所需的权威状态字段。
 *
 * 未登记设备返回 false，并在 out_view 中给出 offline + never_seen 的稳定默认值。
 * offline_reason 仅在 offline 时非 NULL：never_seen、heartbeat_timeout 或
 * link_lost_grace_expired。
 */
bool child_registry_get_status_view(const char *device_id,
                                    child_registry_status_view_t *out_view);
/** @brief 状态枚举转稳定字符串。 */
const char *child_registry_status_name(child_registry_status_t status);
/** @brief 拷贝注册表快照到调用方缓冲区；诊断或未来状态页使用，返回拷贝条数。 */
size_t child_registry_snapshot(child_registry_entry_t *entries, size_t entry_count);

#ifdef __cplusplus
}
#endif

#endif /* CHILD_REGISTRY_H */

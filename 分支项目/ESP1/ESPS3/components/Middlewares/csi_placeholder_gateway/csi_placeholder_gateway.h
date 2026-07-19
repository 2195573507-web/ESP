#ifndef CSI_PLACEHOLDER_GATEWAY_H
#define CSI_PLACEHOLDER_GATEWAY_H

/**
 * @file csi_placeholder_gateway.h
 * @brief S3 网关 CSI 轻量结果接收和触发接口。
 *
 * 本模块保留 C5 /local/v1/csi/result 的 canonical ingress 接口。它只接收
 * C5 已解释的 state/motion_score/confidence/link/timestamp 观测，并由 S3 融合状态机输出
 * CanonicalEvent v2。
 * 按 child registry 在线状态向 C5 发 UDP 小包触发 WiFi 交互仍由独立 trigger 开关控制。
 * 它不解析 raw CSI。
 */

#include <stdint.h>

#include "esp_err.h"
#include "csi_fusion.h"
#include "protocol_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化 CSI gateway；只有显式打开 trigger 时才创建触发任务。 */
void csi_placeholder_gateway_init(void);
/** @brief 启动 CSI feature gateway；周期 trigger/fusion 由 s3_scheduler 驱动。 */
esp_err_t csi_placeholder_gateway_start(void);
/** @brief 暂停 CSI feature gateway。 */
void csi_placeholder_gateway_stop(void);
bool csi_placeholder_gateway_is_running(void);
/** @brief 立即停用指定 C5 的 trigger、ingest、latest 日志和 fusion 实时状态。 */
esp_err_t csi_gateway_suspend_peer(const char *device_id);
/** @brief 停用指定 C5，并只清理 cutoff_us 及以前（或无时间戳）的排队 CSI。 */
esp_err_t csi_gateway_suspend_peer_at_us(const char *device_id, int64_t cutoff_us);
/** @brief 幂等恢复指定 C5 的 CSI 资源，并重新进入 fusion warmup。 */
esp_err_t csi_gateway_restore_peer(const char *device_id);
/** @brief 兼容 scheduler 调用；旧 latest-feature 调试输出已移除。 */
void csi_placeholder_gateway_log_latest_diagnostics(void);
/** @brief scheduler tick 调用：flush 一次 CSI fusion 输出。 */
esp_err_t csi_placeholder_gateway_flush_fusion(void);
/** @brief scheduler tick 调用：向在线 C5 发送一次 CSI trigger。 */
esp_err_t csi_placeholder_gateway_send_triggers(void);
/**
 * @brief 处理一条 CSI result envelope。
 *
 * 调用位置：local_http_server 的 /local/v1/csi/result handler。
 * @param envelope 已由 protocol_adapter 解析的 envelope，不能为空。
 * @return ESP_OK 表示本地已接收；只会上报 CanonicalEvent v2，不上传 raw CSI。
 * 失败处理：local_http_server 映射为本地 CSI 错误响应。
 */
esp_err_t csi_placeholder_gateway_handle_result(const protocol_adapter_envelope_t *envelope);
/** @brief Timestamp-aware compatibility extension used by queued ingress. */
esp_err_t csi_placeholder_gateway_handle_result_at(const protocol_adapter_envelope_t *envelope,
                                                   int64_t rx_time_ms);
/** @brief Validated CSI ingress that commits the confirmed peer IP before restore. */
esp_err_t csi_placeholder_gateway_handle_result_from_peer(
    const protocol_adapter_envelope_t *envelope,
    const char *peer_ip,
    int64_t rx_time_ms);
/** @brief Microsecond-ordered validated CSI ingress used by the live HTTP path. */
esp_err_t csi_placeholder_gateway_handle_result_from_peer_at_us(
    const protocol_adapter_envelope_t *envelope,
    const char *peer_ip,
    int64_t rx_time_us);
/** @brief 处理已解析的 C5 stream CSI feature；device_stream_gateway 调用。 */
esp_err_t csi_placeholder_gateway_handle_feature(const csi_fusion_feature_t *feature);

#ifdef __cplusplus
}
#endif

#endif /* CSI_PLACEHOLDER_GATEWAY_H */

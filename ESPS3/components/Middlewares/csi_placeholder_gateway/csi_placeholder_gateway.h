#ifndef CSI_PLACEHOLDER_GATEWAY_H
#define CSI_PLACEHOLDER_GATEWAY_H

/**
 * @file csi_placeholder_gateway.h
 * @brief S3 网关 CSI 轻量结果接收和触发接口。
 *
 * 本模块保留 C5 /local/v1/csi/result 的 occupancy 摘要接口。它只接收 summary，
 * 可上报 Server csi.motion；按 child registry 在线状态向 C5 发 UDP 小包触发 WiFi
 * 交互仍由独立 trigger 开关控制。它不解析 raw CSI。
 */

#include "esp_err.h"
#include "protocol_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化 CSI gateway；只有显式打开 trigger 时才创建触发任务。 */
void csi_placeholder_gateway_init(void);
/** @brief 低频打印每条 link_id 的 latest CSI summary 状态；不读取或上传 raw CSI。 */
void csi_placeholder_gateway_log_latest_diagnostics(void);
/**
 * @brief 处理一条 CSI result envelope。
 *
 * 调用位置：local_http_server 的 /local/v1/csi/result handler。
 * @param envelope 已由 protocol_adapter 解析的 envelope，不能为空。
 * @return ESP_OK 表示本地已接收；只会上报 summary，不上传 raw CSI。
 * 失败处理：local_http_server 映射为本地 CSI 错误响应。
 */
esp_err_t csi_placeholder_gateway_handle_result(const protocol_adapter_envelope_t *envelope);

#ifdef __cplusplus
}
#endif

#endif /* CSI_PLACEHOLDER_GATEWAY_H */

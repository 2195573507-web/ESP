#ifndef DEVICE_STREAM_GATEWAY_H
#define DEVICE_STREAM_GATEWAY_H

/**
 * @file device_stream_gateway.h
 * @brief S3 侧 C5 扁平 device stream ingress。
 *
 * 本模块只处理 C5 通过 UDP 或 /local/v1/device-stream 发送的短字段 stream frame。
 * CSI v2 envelope 仍走 local_http_server/s3_scheduler/protocol_adapter/csi_fusion 主路径。
 * HTTP/UDP 入口只入队，实际解析由 scheduler stream worker 调用。
 */

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化 stream gateway 的单调时间戳锁；启动前调用一次。 */
esp_err_t device_stream_gateway_init(void);

/** @brief 启动 UDP listener 并允许处理 stream frame；可重复调用。 */
esp_err_t device_stream_gateway_start(void);

/** @brief 暂停 stream gateway；UDP task 会在下一轮 net gate 检查中停收。 */
void device_stream_gateway_stop(void);

/** @brief 查询 stream gateway 是否处于运行状态。 */
bool device_stream_gateway_is_running(void);

/**
 * @brief 清空 stream timestamp 单调检查基线。
 *
 * @param device_id 完整子设备 ID；NULL 或空字符串表示清空全部 baseline。
 * @param reason 重置原因，用于串口验证日志。
 */
void device_stream_gateway_reset_timestamp_baseline(const char *device_id,
                                                    const char *reason);

/** @brief 解析并处理一帧 stream JSON；只由 scheduler stream worker 调用。 */
esp_err_t device_stream_gateway_process_json(const char *json,
                                             size_t json_len,
                                             const char *peer_ip);

/** @brief 将收到的 stream JSON 入队给 scheduler；HTTP/UDP ingress 调用。 */
esp_err_t device_stream_gateway_enqueue_frame(const char *json,
                                              size_t json_len,
                                              const char *peer_ip,
                                              const char *source);

/** @brief 入队一段要发回 C5 的 UDP payload；由 scheduler stream worker 执行发送。 */
esp_err_t device_stream_gateway_enqueue_udp(const char *peer_ip,
                                            uint16_t peer_port,
                                            const void *payload,
                                            size_t payload_len,
                                            const char *source);

/** @brief 立即发送 UDP payload；只在 stream worker 中调用，避免入口线程阻塞。 */
esp_err_t device_stream_gateway_send_udp_now(const char *peer_ip,
                                             uint16_t peer_port,
                                             const void *payload,
                                             size_t payload_len,
                                             const char *source);

/** @brief 处理 /local/v1/device-stream HTTP body；读取后只负责入队。 */
esp_err_t device_stream_gateway_handle_http(httpd_req_t *req);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_STREAM_GATEWAY_H */

#ifndef DEVICE_STREAM_CLIENT_H
#define DEVICE_STREAM_CLIENT_H

/**
 * @file device_stream_client.h
 * @brief C5 -> S3 统一轻量 telemetry stream 客户端。
 *
 * sensor/status/event 使用紧凑 stream frame。CSI feature 不再经本模块发送；
 * 它由 csi_server_client 直接 POST 到 /local/v1/csi/result。
 * 本模块只面向 ESPS3 local gateway，UDP 是普通 stream 的低延迟主路径，普通
 * publish 允许失败后走 /local/v1/stream HTTP fallback；不会构造 Server 完整 envelope。
 */

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化 stream client；当前无动态资源，保留给启动流程统一调用。 */
esp_err_t device_stream_client_init(void);

/** @brief 发布一帧普通 stream 数据；UDP 失败时走本地 HTTP fallback。 */
esp_err_t device_stream_client_publish(const char *type,
                                       const char *link_id,
                                       double v1,
                                       double v2,
                                       double v3);

/** @brief UDP-only best-effort publish；失败直接返回，不触发 HTTP fallback 或链路重试。 */
esp_err_t device_stream_client_publish_best_effort(const char *type,
                                                   const char *link_id,
                                                   double v1,
                                                   double v2,
                                                   double v3);

/** @brief 格式化普通 stream frame；type 只允许 sensor/status/event。 */
esp_err_t device_stream_client_format(const char *type,
                                      const char *link_id,
                                      double v1,
                                      double v2,
                                      double v3,
                                      char *out,
                                      size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_STREAM_CLIENT_H */

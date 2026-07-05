#ifndef CSI_SERVER_CLIENT_H
#define CSI_SERVER_CLIENT_H

/**
 * @file csi_server_client.h
 * @brief C5 终端 CSI 轻量结果上报接口。
 *
 * 只允许上传链路标识和 occupancy/motion_score/mean_amplitude/variance/cv/rssi/quality/sample_count 摘要；
 * 本接口不上传 raw CSI、I/Q 数组、相位序列或完整 Server envelope。
 */

#include <stdbool.h>
#include <stddef.h>

#include "csi_presence.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化 CSI 上报占位客户端；当前无状态，可重复调用，返回 ESP_OK。 */
esp_err_t csi_server_client_init(void);

/**
 * @brief 将一条 CSI presence 摘要序列化为 C5->S3 本地 JSON。
 *
 * 调用位置：csi_service 的低优先级周期任务，用同一份 JSON 做日志和 HTTP body。
 * 输出字段只包含轻量 summary；不会包含 raw CSI、I/Q 数组、子载波矩阵或相位信息。
 */
esp_err_t csi_server_client_format_presence_result(const csi_presence_result_t *result,
                                                   char *json_body,
                                                   size_t json_body_size);

/**
 * @brief 用同一份 CSI summary JSON 执行本地日志和/或 HTTP 输出。
 *
 * 调用位置：csi_service 周期任务。HTTP 失败只返回错误码，调用方等待下个窗口。
 */
esp_err_t csi_server_client_publish_presence_result(const csi_presence_result_t *result,
                                                    bool log_enabled,
                                                    bool http_enabled);

/**
 * @brief 上传一条 CSI presence 摘要到 ESPS3 /local/v1/csi/result。
 *
 * 调用位置：csi_service 的低优先级周期任务。
 * @param result presence 状态机输出，不能为空。
 * @return ESP_OK 表示 S3 已 ACK；WiFi/HTTP/状态码失败返回对应错误码。
 * 失败处理：调用方只记录并等待下个窗口，不重传 raw CSI。
 */
esp_err_t csi_server_client_upload_presence_result(const csi_presence_result_t *result);

/**
 * @brief 尝试上传 CSI 特征。
 *
 * 调用位置：预留给 CSI service；当前启动链路不调用。
 * @param device_id 完整终端 device_id，当前仅占位。
 * @param features_json CSI 特征 JSON，当前仅占位。
 * @return 固定返回 ESP_ERR_NOT_SUPPORTED。
 * 失败处理：该接口保留给旧调用方；Phase B 请使用 upload_presence_result()。
 */
esp_err_t csi_server_client_upload_features(const char *device_id,
                                            const char *features_json);

#ifdef __cplusplus
}
#endif

#endif /* CSI_SERVER_CLIENT_H */

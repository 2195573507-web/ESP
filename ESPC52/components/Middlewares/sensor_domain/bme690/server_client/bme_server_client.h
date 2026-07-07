#ifndef BME_SERVER_CLIENT_H
#define BME_SERVER_CLIENT_H

/**
 * @file bme_server_client.h
 * @brief C5 终端 BME690 本地网关上报接口。
 *
 * 本模块只发送 C5 -> S3 统一设备流；sensor_id 写入 lid，整机 did 由
 * device_stream_client 使用本机配置填充。
 */

#include "bme690.h"
#include "bme_air_quality.h"
#include "esp111_protocol_common.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* BME690 本地网关上传配置：C5 正式模式只向 ESPS3 统一 stream 上报。 */
#ifndef BME_SERVER_CLIENT_ENDPOINT
#define BME_SERVER_CLIENT_ENDPOINT ESP111_PROTOCOL_ROUTE_DEVICE_STREAM
#endif

#ifndef BME_SERVER_CLIENT_TIMEOUT_MS
#define BME_SERVER_CLIENT_TIMEOUT_MS 5000U // 单次上传超时，单位 ms。
#endif

#ifndef BME_SERVER_CLIENT_JSON_BUFFER_SIZE
#define BME_SERVER_CLIENT_JSON_BUFFER_SIZE ESP111_PROTOCOL_DEVICE_STREAM_MAX_BYTES
#endif

/** @brief 初始化 BME 上报客户端；BME service 启动时调用，当前无状态，可重复调用。 */
esp_err_t bme_server_client_init(void);

/**
 * @brief 上传一次 BME690 读数到 S3。
 *
 * 调用位置：bme_sensor_task() 每轮读数和空气质量计算成功后。
 * @param sensor_id BME sensor_id，不能为空；写入统一 stream 的 lid 字段。
 * @param data bme690_read() 输出，不能为空。
 * @param air_quality bme_air_quality_update() 输出，不能为空。
 * @return ESP_OK 表示 S3 local HTTP 接收成功；参数错误、JSON 过大、WiFi/HTTP 失败返回对应错误码。
 * 失败处理：BME service 记录日志并按上传周期进入下一轮重试。
 */
esp_err_t bme_server_client_upload_reading(const char *sensor_id,
                                           const bme690_data_t *data,
                                           const bme_air_quality_result_t *air_quality);

#ifdef __cplusplus
}
#endif

#endif /* BME_SERVER_CLIENT_H */

/**
 * @file bme_server_client.c
 * @brief C5 终端 BME690 轻量上报客户端。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），负责把 BME690 读数压缩为
 * C5 -> S3 统一设备流的 sensor 帧。它不构造
 * S3 -> Server 完整 sensor envelope，字段展开和 Server 转发由 ESPS3 protocol_adapter/
 * sensor_aggregator/server_client 完成。
 */

#include "bme_server_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "device_stream_client.h"
#include "esp111_protocol_common.h"
#include "esp_log.h"

static const char *TAG = "bme_server_client";

esp_err_t bme_server_client_init(void)
{
    return device_stream_client_init();
}

esp_err_t bme_server_client_upload_reading(const char *sensor_id,
                                           const bme690_data_t *data,
                                           const bme_air_quality_result_t *air_quality)
{
    if (sensor_id == NULL || sensor_id[0] == '\0' || data == NULL || air_quality == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = device_stream_client_publish(ESP111_PROTOCOL_DEVICE_STREAM_TYPE_SENSOR,
                                                 sensor_id,
                                                 data->temperature_c,
                                                 data->humidity_percent,
                                                 air_quality->air_quality_score);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG,
                 "BME stream sent temperature=%.2f humidity=%.2f quality=%d",
                 (double)data->temperature_c,
                 (double)data->humidity_percent,
                 air_quality->air_quality_score);
    }
    return ret;
}

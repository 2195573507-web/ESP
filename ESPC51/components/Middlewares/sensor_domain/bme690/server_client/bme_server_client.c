/**
 * @file bme_server_client.c
 * @brief C5 终端 BME690 轻量上报客户端。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），负责把 BME690 读数压缩为
 * C5 <-> S3 的本地轻量 JSON v 数组并 POST 到 /local/v1/sensor。它不构造
 * S3 -> Server 完整 sensor envelope，字段展开和 Server 转发由 ESPS3 protocol_adapter/
 * sensor_aggregator/server_client 完成。
 */

#include "bme_server_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "device_protocol_metadata.h"
#include "esp111_protocol_common.h"
#include "esp_log.h"
#include "server_comm_config.h"
#include "server_comm_http.h"
#include "terminal_config.h"

static const char *TAG = "bme_server_client";

esp_err_t bme_server_client_init(void)
{
    return ESP_OK;
}

esp_err_t bme_server_client_upload_reading(const char *device_id,
                                           const bme690_data_t *data,
                                           const bme_air_quality_result_t *air_quality)
{
    if (device_id == NULL || device_id[0] == '\0' || data == NULL || air_quality == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    device_protocol_metadata_t metadata = {0};
    device_protocol_prepare_metadata(&metadata, ESP111_PROTOCOL_MSG_SENSOR_BME690);
    char json_body[BME_SERVER_CLIENT_JSON_BUFFER_SIZE];
    int json_len = snprintf(json_body,
                            sizeof(json_body),
                            "{\"" ESP111_PROTOCOL_LOCAL_JSON_PROTOCOL_VERSION "\":%u,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_ID "\":%u,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_TYPE "\":%u,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_PAYLOAD_TYPE "\":\"%s\","
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_SENSOR_KIND "\":%u,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_UPTIME_MS "\":%s,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_SEQ "\":%s,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_TIME_SYNCED "\":%s,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_ROOM_ID "\":\"%s\","
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_VALUES "\":["
                            "%.2f,%.2f,%.2f,%.2f,%d,%.2f,%.4f,%d,%d,%u,%lu]}",
                            ESP111_PROTOCOL_LOCAL_SCHEMA_VERSION,
                            (unsigned int)terminal_config_get_local_id(),
                            ESP111_PROTOCOL_LOCAL_PACKET_SENSOR,
                            ESP111_PROTOCOL_MSG_SENSOR_BME690,
                            ESP111_PROTOCOL_LOCAL_SENSOR_KIND_BME690,
                            metadata.esp_uptime_ms,
                            metadata.request_seq,
                            metadata.time_synced,
                            metadata.room_id,
                            data->temperature_c,
                            data->humidity_percent,
                            data->pressure_hpa,
                            (float)data->gas_resistance_ohm,
                            air_quality->air_quality_score,
                            air_quality->gas_baseline_ohm,
                            air_quality->gas_ratio,
                            air_quality->gas_score,
                            air_quality->humidity_score,
                            (unsigned int)((air_quality->baseline_ready ? 1U : 0U) |
                                           (air_quality->warmup_done ? 2U : 0U)),
                            (unsigned long)air_quality->sample_count);
    if (json_len < 0 || json_len >= (int)sizeof(json_body)) {
        ESP_LOGE(TAG, "BME JSON body too large");
        return ESP_ERR_INVALID_SIZE;
    }

    server_comm_http_response_t response = {0};
    esp_err_t ret = server_comm_http_post_json_with_headers(BME_SERVER_CLIENT_ENDPOINT,
                                                            json_body,
                                                            metadata.headers,
                                                            metadata.header_count,
                                                            BME_SERVER_CLIENT_TIMEOUT_MS,
                                                            NULL,
                                                            0,
                                                            &response);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "BME upload status=%d", response.status_code);
    }
    return ret;
}

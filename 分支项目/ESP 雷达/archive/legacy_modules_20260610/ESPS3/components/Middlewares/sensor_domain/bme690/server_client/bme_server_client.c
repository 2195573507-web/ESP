#include "bme_server_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "device_protocol_metadata.h"
#include "esp_log.h"
#include "server_comm_config.h"
#include "server_comm_http.h"

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
    device_protocol_prepare_metadata(&metadata, "sensor.bme690");
    const bool time_synced = strcmp(metadata.time_synced, "true") == 0;
    const char *esp_time_json = time_synced ? metadata.esp_time_ms : "null";

    char json_body[BME_SERVER_CLIENT_JSON_BUFFER_SIZE];
    int json_len = snprintf(json_body,
                            sizeof(json_body),
                            "{\"schema_version\":1,"
                            "\"device_id\":\"%s\","
                            "\"device_type\":\"esp32c5_env_voice_node\","
                            "\"firmware_version\":\"0.1.0\","
                            "\"request_seq\":%s,"
                            "\"esp_uptime_ms\":%s,"
                            "\"esp_time_ms\":%s,"
                            "\"time_synced\":%s,"
                            "\"payload_type\":\"sensor.bme690\","
                            "\"payload\":{"
                            "\"sensor_id\":\"%s\","
                            "\"temperature_c\":%.2f,"
                            "\"humidity_percent\":%.2f,"
                            "\"pressure_hpa\":%.2f,"
                            "\"gas_resistance_ohm\":%.2f,"
                            "\"air_quality_score\":%d,"
                            "\"air_quality_level\":\"%s\","
                            "\"air_quality_confidence\":\"%s\","
                            "\"air_quality_algo_version\":\"%s\","
                            "\"air_quality_source\":\"%s\","
                            "\"gas_baseline_ohm\":%.2f,"
                            "\"gas_ratio\":%.4f,"
                            "\"gas_score\":%d,"
                            "\"humidity_score\":%d,"
                            "\"baseline_ready\":%s,"
                            "\"warmup_done\":%s,"
                            "\"sample_count\":%lu}}",
                            server_comm_get_device_id(),
                            metadata.request_seq,
                            metadata.esp_uptime_ms,
                            esp_time_json,
                            metadata.time_synced,
                            device_id,
                            data->temperature_c,
                            data->humidity_percent,
                            data->pressure_hpa,
                            (float)data->gas_resistance_ohm,
                            air_quality->air_quality_score,
                            air_quality->air_quality_level,
                            air_quality->air_quality_confidence,
                            air_quality->air_quality_algo_version,
                            air_quality->air_quality_source,
                            air_quality->gas_baseline_ohm,
                            air_quality->gas_ratio,
                            air_quality->gas_score,
                            air_quality->humidity_score,
                            air_quality->baseline_ready ? "true" : "false",
                            air_quality->warmup_done ? "true" : "false",
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

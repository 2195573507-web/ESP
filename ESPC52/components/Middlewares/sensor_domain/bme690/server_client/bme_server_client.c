/**
 * @file bme_server_client.c
 * @brief C5 终端 BME690 轻量上报客户端。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），负责把 BME690 读数打包为
 * C5 -> S3 本地 sensor envelope。它不构造 S3 -> Server 完整 sensor envelope，
 * Server 转发由 ESPS3 protocol_adapter/sensor_aggregator/server_client 完成。
 */

#include "bme_server_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "device_protocol_metadata.h"
#include "esp111_protocol_common.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "server_comm_http.h"
#include "terminal_config.h"

static const char *TAG = "bme_server_client";
static TickType_t s_bme_retry_not_before;
static uint32_t s_bme_failure_count;

static bool bme_server_client_retry_ready(void)
{
    return s_bme_retry_not_before == 0 ||
           (int32_t)(xTaskGetTickCount() - s_bme_retry_not_before) >= 0;
}

static void bme_server_client_note_upload_result(esp_err_t ret)
{
    if (ret == ESP_OK) {
        s_bme_failure_count = 0;
        s_bme_retry_not_before = 0;
        return;
    }
    if (s_bme_failure_count < 6U) {
        ++s_bme_failure_count;
    }
    const uint32_t delay_ms = 1000U << (s_bme_failure_count > 4U ? 4U : s_bme_failure_count);
    s_bme_retry_not_before = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms > 30000U ? 30000U : delay_ms);
    ESP_LOGW(TAG, "BME upload backoff failures=%lu retry_after_ms=%lu ret=%s",
             (unsigned long)s_bme_failure_count,
             (unsigned long)(delay_ms > 30000U ? 30000U : delay_ms),
             esp_err_to_name(ret));
}

static uint32_t bme_server_client_boot_id(void)
{
    static uint32_t boot_id;

    if (boot_id == 0U) {
        boot_id = esp_random();
        if (boot_id == 0U) {
            boot_id = 1U;
        }
    }
    return boot_id;
}

esp_err_t bme_server_client_init(void)
{
    return ESP_OK;
}

static bool bme_json_append_char(char *out, size_t out_size, size_t *out_len, char ch)
{
    if (out == NULL || out_size == 0U || out_len == NULL) {
        return false;
    }
    if (*out_len + 1U >= out_size) {
        out[out_size - 1U] = '\0';
        return false;
    }
    out[*out_len] = ch;
    *out_len += 1U;
    out[*out_len] = '\0';
    return true;
}

static bool bme_json_append_text(char *out, size_t out_size, size_t *out_len, const char *text)
{
    if (text == NULL) {
        return true;
    }
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        if (!bme_json_append_char(out, out_size, out_len, *cursor)) {
            return false;
        }
    }
    return true;
}

static bool bme_json_escape_string(const char *input, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return false;
    }
    out[0] = '\0';
    if (input == NULL) {
        return true;
    }

    size_t out_len = 0;
    for (const unsigned char *cursor = (const unsigned char *)input; *cursor != '\0'; cursor++) {
        const char *escape = NULL;
        char unicode_escape[7];
        switch (*cursor) {
        case '\"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\b':
            escape = "\\b";
            break;
        case '\f':
            escape = "\\f";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            if (*cursor < 0x20U) {
                int written = snprintf(unicode_escape, sizeof(unicode_escape), "\\u%04x", *cursor);
                if (written < 0 || written >= (int)sizeof(unicode_escape)) {
                    return false;
                }
                escape = unicode_escape;
            }
            break;
        }

        if (escape != NULL) {
            if (!bme_json_append_text(out, out_size, &out_len, escape)) {
                return false;
            }
        } else if (!bme_json_append_char(out, out_size, &out_len, (char)*cursor)) {
            return false;
        }
    }
    return true;
}

esp_err_t bme_server_client_upload_reading(const char *sensor_id,
                                           const bme690_data_t *data,
                                           const bme_air_quality_result_t *air_quality)
{
    if (sensor_id == NULL || sensor_id[0] == '\0' || data == NULL || air_quality == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!bme_server_client_retry_ready()) {
        return ESP_ERR_NOT_FINISHED;
    }
    char escaped_sensor_id[64];
    char escaped_level[24];
    char escaped_confidence[24];
    char escaped_algorithm[64];
    char escaped_v3_algorithm[64];
    char escaped_v3_level[24];
    char escaped_sensor_state[24];
    if (!bme_json_escape_string(sensor_id, escaped_sensor_id, sizeof(escaped_sensor_id)) ||
        !bme_json_escape_string(air_quality->air_quality_level,
                                escaped_level,
                                sizeof(escaped_level)) ||
        !bme_json_escape_string(air_quality->air_quality_confidence,
                                escaped_confidence,
                                sizeof(escaped_confidence)) ||
        !bme_json_escape_string(air_quality->air_quality_algo_version,
                                escaped_algorithm,
                                sizeof(escaped_algorithm)) ||
        !bme_json_escape_string(air_quality->air_quality.algorithm,
                                escaped_v3_algorithm,
                                sizeof(escaped_v3_algorithm)) ||
        !bme_json_escape_string(air_quality->air_quality.level,
                                escaped_v3_level,
                                sizeof(escaped_v3_level)) ||
        !bme_json_escape_string(air_quality->air_quality.sensor_state,
                                escaped_sensor_state,
                                sizeof(escaped_sensor_state))) {
        return ESP_ERR_INVALID_SIZE;
    }

    device_protocol_metadata_t metadata = {0};
    device_protocol_prepare_metadata(&metadata, ESP111_PROTOCOL_MSG_SENSOR_BME690);
    const char *sample_time_ms = metadata.esp_time_ms[0] != '\0' ? metadata.esp_time_ms :
                                                                    metadata.esp_uptime_ms;
    unsigned int flags = (air_quality->baseline_ready ? 0x01U : 0U) |
                         (air_quality->warmup_done ? 0x02U : 0U);

    char json_body[BME_SERVER_CLIENT_JSON_BUFFER_SIZE + 768U];
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
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_VALUES
                            "\":[%.3f,%.3f,%.3f,%lu,%d,%.3f,%.6f,%d,%d,%u,%lu],"
                            "\"sensor_id\":\"%s\","
                            "\"temperature_c\":%.3f,"
                            "\"humidity_percent\":%.3f,"
                            "\"pressure_hpa\":%.3f,"
                            "\"gas_resistance_ohm\":%lu,"
                            "\"gas_baseline_ohm\":%.3f,"
                            "\"gas_ratio\":%.6f,"
                            "\"gas_score\":%d,"
                            "\"humidity_score\":%d,"
                            "\"air_quality_score\":%d,"
                            "\"air_quality_level\":\"%s\","
                            "\"air_quality_confidence\":\"%s\","
                            "\"sample_count\":%lu,"
                            "\"algorithm_version\":\"%s\","
                            "\"air_quality_algo_version\":\"%s\","
                            "\"air_quality_source\":\"esp\","
                            "\"air_quality\":{"
                            "\"algorithm\":\"%s\","
                            "\"score\":%d,"
                            "\"level\":\"%s\","
                            "\"confidence\":%.6f,"
                            "\"gas_ratio\":%.6f,"
                            "\"stability_score\":%.6f,"
                            "\"sensor_state\":\"%s\","
                            "\"baseline_ready\":%s,"
                            "\"baseline_state\":{"
                            "\"ready\":%s,"
                            "\"created_time\":%llu,"
                            "\"update_time\":%llu}},"
                            "\"bme_diag\":{"
                            "\"gas_valid\":%s,"
                            "\"heat_stable\":%s,"
                            "\"heater_temp\":%u,"
                            "\"heater_time_ms\":%u,"
                            "\"gas_adc\":%u,"
                            "\"gas_range\":%u},"
                            "\"sample_time_ms\":%s,"
                            "\"esp_uptime_ms\":%s,"
                            "\"time_synced\":%s,"
                            "\"boot_id\":%lu,"
                            "\"request_seq\":%s}",
                            ESP111_PROTOCOL_LOCAL_SCHEMA_VERSION,
                            (unsigned int)terminal_config_get_local_id(),
                            ESP111_PROTOCOL_LOCAL_PACKET_SENSOR,
                            ESP111_PROTOCOL_MSG_SENSOR_BME690,
                            ESP111_PROTOCOL_LOCAL_SENSOR_KIND_BME690,
                            metadata.esp_uptime_ms,
                            metadata.request_seq,
                            metadata.time_synced,
                            metadata.room_id,
                            (double)data->temperature_c,
                            (double)data->humidity_percent,
                            (double)data->pressure_hpa,
                            (unsigned long)data->gas_resistance_ohm,
                            air_quality->air_quality_score,
                            (double)air_quality->gas_baseline_ohm,
                            (double)air_quality->gas_ratio,
                            air_quality->gas_score,
                            air_quality->humidity_score,
                            flags,
                            (unsigned long)air_quality->sample_count,
                            escaped_sensor_id,
                            (double)data->temperature_c,
                            (double)data->humidity_percent,
                            (double)data->pressure_hpa,
                            (unsigned long)data->gas_resistance_ohm,
                            (double)air_quality->gas_baseline_ohm,
                            (double)air_quality->gas_ratio,
                            air_quality->gas_score,
                            air_quality->humidity_score,
                            air_quality->air_quality_score,
                            escaped_level,
                            escaped_confidence,
                            (unsigned long)air_quality->sample_count,
                            escaped_algorithm,
                            escaped_algorithm,
                            escaped_v3_algorithm,
                            air_quality->air_quality.score,
                            escaped_v3_level,
                            (double)air_quality->air_quality.confidence,
                            (double)air_quality->air_quality.gas_ratio,
                            (double)air_quality->air_quality.stability_score,
                            escaped_sensor_state,
                            air_quality->air_quality.baseline_ready ? "true" : "false",
                            air_quality->air_quality.baseline_ready ? "true" : "false",
                            (unsigned long long)air_quality->air_quality.baseline_created_time_ms,
                            (unsigned long long)air_quality->air_quality.baseline_update_time_ms,
                            data->gas_valid ? "true" : "false",
                            data->heat_stable ? "true" : "false",
                            (unsigned int)data->heater_temp,
                            (unsigned int)data->heater_time_ms,
                            (unsigned int)data->gas_adc,
                            (unsigned int)data->gas_range,
                            sample_time_ms,
                            metadata.esp_uptime_ms,
                            metadata.time_synced,
                            (unsigned long)bme_server_client_boot_id(),
                            metadata.request_seq);
    if (json_len <= 0 || json_len >= (int)sizeof(json_body)) {
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
        ESP_LOGD(TAG,
                 "BME sensor sent temperature=%.2f humidity=%.2f quality=%d samples=%lu",
                 (double)data->temperature_c,
                 (double)data->humidity_percent,
                 air_quality->air_quality_score,
                 (unsigned long)air_quality->sample_count);
    }
    bme_server_client_note_upload_result(ret);
    return ret;
}

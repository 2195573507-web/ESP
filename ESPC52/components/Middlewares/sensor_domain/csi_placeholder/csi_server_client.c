/**
 * @file csi_server_client.c
 * @brief C5 终端 CSI 轻量结果上报客户端。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），只把 presence 摘要序列化为
 * C5<->S3 本地轻量 JSON 并 POST 到 /local/v1/csi/result。它不构造 Server 完整
 * envelope，不上传 raw CSI，也不发送 I/Q、相位或子载波数组。
 */

#include "csi_server_client.h"

#include <stdio.h>

#include "app_main_config.h"
#include "device_protocol_metadata.h"
#include "esp111_protocol_common.h"
#include "esp_log.h"
#include "server_comm_config.h"
#include "server_comm_http.h"
#include "terminal_config.h"

static const char *TAG = "csi_server_client";

#define CSI_SERVER_CLIENT_JSON_BUFFER_SIZE 1024U
#define CSI_SERVER_CLIENT_TIMEOUT_MS 5000U

esp_err_t csi_server_client_init(void)
{
    return ESP_OK;
}

static unsigned int csi_server_client_state_code(csi_presence_state_t state)
{
    switch (state) {
    case CSI_PRESENCE_STATE_OCCUPIED:
        return 1U;
    case CSI_PRESENCE_STATE_VACANT:
        return 2U;
    case CSI_PRESENCE_STATE_UNKNOWN:
    default:
        return 0U;
    }
}

static const char *csi_server_client_state_string(csi_presence_state_t state)
{
    switch (state) {
    case CSI_PRESENCE_STATE_OCCUPIED:
        return "occupied";
    case CSI_PRESENCE_STATE_VACANT:
        return "vacant";
    case CSI_PRESENCE_STATE_UNKNOWN:
    default:
        return "unknown";
    }
}

static const char *csi_server_client_quality_string(csi_sample_quality_t quality)
{
    switch (quality) {
    case CSI_SAMPLE_QUALITY_GOOD:
        return "good";
    case CSI_SAMPLE_QUALITY_WEAK:
        return "weak";
    case CSI_SAMPLE_QUALITY_INVALID:
    default:
        return "weak";
    }
}

static const char *csi_server_client_short_device_id(void)
{
    return terminal_config_get_local_id() == ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52 ? "C52" : "C51";
}

static const char *csi_server_client_link_id(void)
{
    return terminal_config_get_local_id() == ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52 ? "S3_TO_C52" : "S3_TO_C51";
}

static esp_err_t csi_server_client_format_presence_result_with_metadata(const csi_presence_result_t *result,
                                                                        const device_protocol_metadata_t *metadata,
                                                                        char *json_body,
                                                                        size_t json_body_size)
{
    if (result == NULL || metadata == NULL || json_body == NULL || json_body_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    float motion_score = result->motion_score;
    if (motion_score < 0.0f) {
        motion_score = 0.0f;
    } else if (motion_score > 1.0f) {
        motion_score = 1.0f;
    }

    const char *device_id = csi_server_client_short_device_id();
    const char *link_id = csi_server_client_link_id();
    const char *peer_id = "S3";
    const char *rx_device = device_id;
    const char *tx_device = peer_id;
    const char *quality = csi_server_client_quality_string(result->quality);

    int json_len = snprintf(json_body,
                            json_body_size,
                            "{\"" ESP111_PROTOCOL_LOCAL_JSON_PROTOCOL_VERSION "\":%u,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_ID "\":%u,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_TYPE "\":%u,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_PAYLOAD_TYPE "\":\"%s\","
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_UPTIME_MS "\":%s,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_SEQ "\":%s,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_TIME_SYNCED "\":%s,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_ROOM_ID "\":\"%s\","
                            "\"device_id\":\"%s\","
                            "\"peer_id\":\"%s\","
                            "\"link_id\":\"%s\","
                            "\"rx_device\":\"%s\","
                            "\"tx_device\":\"%s\","
                            "\"algorithm_version\":\"%s\","
                            "\"state\":\"%s\","
                            "\"motion_score\":%.2f,"
                            "\"mean_amplitude\":%.2f,"
                            "\"variance\":%.4f,"
                            "\"cv\":%.4f,"
                            "\"rssi\":%d,"
                            "\"quality\":\"%s\","
                            "\"sample_count\":%u,"
                            "\"updated_at_ms\":%llu,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_VALUES "\":["
                            "%u,%.2f,%.4f,%d,%u,%llu]}",
                            ESP111_PROTOCOL_LOCAL_SCHEMA_VERSION,
                            (unsigned int)terminal_config_get_local_id(),
                            ESP111_PROTOCOL_LOCAL_PACKET_CSI,
                            ESP111_PROTOCOL_MSG_CSI_MOTION,
                            metadata->esp_uptime_ms,
                            metadata->request_seq,
                            metadata->time_synced,
                            metadata->room_id,
                            device_id,
                            peer_id,
                            link_id,
                            rx_device,
                            tx_device,
                            CSI_ALGORITHM_VERSION,
                            csi_server_client_state_string(result->state),
                            (double)motion_score,
                            (double)result->mean_amplitude,
                            (double)result->variance,
                            (double)result->cv,
                            (int)result->rssi,
                            quality,
                            (unsigned int)result->sample_count,
                            (unsigned long long)result->updated_at_ms,
                            csi_server_client_state_code(result->state),
                            (double)motion_score,
                            (double)result->variance,
                            (int)result->rssi,
                            (unsigned int)result->sample_count,
                            (unsigned long long)result->updated_at_ms);
    if (json_len < 0 || json_len >= (int)json_body_size) {
        ESP_LOGE(TAG, "CSI result JSON body too large");
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t csi_server_client_format_presence_result(const csi_presence_result_t *result,
                                                   char *json_body,
                                                   size_t json_body_size)
{
    device_protocol_metadata_t metadata = {0};
    device_protocol_prepare_metadata(&metadata, ESP111_PROTOCOL_MSG_CSI_MOTION);
    return csi_server_client_format_presence_result_with_metadata(result,
                                                                  &metadata,
                                                                  json_body,
                                                                  json_body_size);
}

static esp_err_t csi_server_client_post_presence_body(const csi_presence_result_t *result,
                                                      const device_protocol_metadata_t *metadata,
                                                      const char *json_body)
{
    if (result == NULL || metadata == NULL || json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    server_comm_http_response_t response = {0};
    esp_err_t ret = server_comm_http_post_json_with_headers(ESP111_PROTOCOL_ROUTE_CSI_RESULT,
                                                            json_body,
                                                            metadata->headers,
                                                            metadata->header_count,
                                                            CSI_SERVER_CLIENT_TIMEOUT_MS,
                                                            NULL,
                                                            0,
                                                            &response);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG,
                 "CSI result uploaded status=%d link=%s state=%s motion=%.2f samples=%u",
                 response.status_code,
                 csi_server_client_link_id(),
                 csi_server_client_state_string(result->state),
                 (double)result->motion_score,
                 (unsigned int)result->sample_count);
    }
    return ret;
}

esp_err_t csi_server_client_publish_presence_result(const csi_presence_result_t *result,
                                                    bool log_enabled,
                                                    bool http_enabled)
{
    device_protocol_metadata_t metadata = {0};
    device_protocol_prepare_metadata(&metadata, ESP111_PROTOCOL_MSG_CSI_MOTION);

    char json_body[CSI_SERVER_CLIENT_JSON_BUFFER_SIZE];
    esp_err_t format_ret = csi_server_client_format_presence_result_with_metadata(result,
                                                                                  &metadata,
                                                                                  json_body,
                                                                                  sizeof(json_body));
    if (format_ret != ESP_OK) {
        return format_ret;
    }

    if (log_enabled) {
        ESP_LOGI(TAG, "csi summary %s", json_body);
    }
    if (!http_enabled) {
        return ESP_OK;
    }
    return csi_server_client_post_presence_body(result, &metadata, json_body);
}

esp_err_t csi_server_client_upload_presence_result(const csi_presence_result_t *result)
{
    return csi_server_client_publish_presence_result(result, false, true);
}

esp_err_t csi_server_client_upload_features(const char *device_id,
                                            const char *features_json)
{
    (void)device_id;
    (void)features_json;
    ESP_LOGD(TAG, "raw CSI feature upload remains unsupported");
    return ESP_ERR_NOT_SUPPORTED;
}

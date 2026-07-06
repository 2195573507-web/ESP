/**
 * @file csi_server_client.c
 * @brief C5 CSI feature upload client.
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

#define CSI_SERVER_CLIENT_JSON_BUFFER_SIZE 768U
#define CSI_SERVER_CLIENT_TIMEOUT_MS 5000U

esp_err_t csi_server_client_init(void)
{
    return ESP_OK;
}

static const char *short_device_id(void)
{
    return terminal_config_get_local_id() == ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52 ? "C52" : "C51";
}

static const char *link_id(void)
{
    return terminal_config_get_local_id() == ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52 ? "S3_TO_C52" : "S3_TO_C51";
}

static esp_err_t format_feature_with_metadata(const csi_feature_result_t *result,
                                              const device_protocol_metadata_t *metadata,
                                              char *json_body,
                                              size_t json_body_size)
{
    if (result == NULL || metadata == NULL || json_body == NULL || json_body_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

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
                            "\"link_id\":\"%s\","
                            "\"frame_energy\":%.3f,"
                            "\"variance\":%.5f,"
                            "\"quality\":%.5f,"
                            "\"rssi\":%d,"
                            "\"frame_seq\":%u,"
                            "\"timestamp\":%llu,"
                            "\"updated_at_ms\":%llu,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_VALUES "\":[%.3f,%.5f,%d,%llu,%u,%.5f]}",
                            ESP111_PROTOCOL_LOCAL_SCHEMA_VERSION,
                            (unsigned int)terminal_config_get_local_id(),
                            ESP111_PROTOCOL_LOCAL_PACKET_CSI,
                            ESP111_PROTOCOL_MSG_CSI_MOTION,
                            metadata->esp_uptime_ms,
                            metadata->request_seq,
                            metadata->time_synced,
                            metadata->room_id,
                            short_device_id(),
                            link_id(),
                            (double)result->frame_energy,
                            (double)result->variance,
                            (double)result->quality,
                            (int)result->rssi,
                            (unsigned int)result->frame_seq,
                            (unsigned long long)result->timestamp_ms,
                            (unsigned long long)result->timestamp_ms,
                            (double)result->frame_energy,
                            (double)result->variance,
                            (int)result->rssi,
                            (unsigned long long)result->timestamp_ms,
                            (unsigned int)result->frame_seq,
                            (double)result->quality);
    if (json_len < 0 || json_len >= (int)json_body_size) {
        ESP_LOGE(TAG, "CSI feature JSON body too large");
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t csi_server_client_format_feature_result(const csi_feature_result_t *result,
                                                  char *json_body,
                                                  size_t json_body_size)
{
    device_protocol_metadata_t metadata = {0};
    device_protocol_prepare_metadata(&metadata, ESP111_PROTOCOL_MSG_CSI_MOTION);
    return format_feature_with_metadata(result, &metadata, json_body, json_body_size);
}

static esp_err_t post_feature_body(const csi_feature_result_t *result,
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
                 "CSI feature uploaded status=%d link=%s energy=%.3f variance=%.5f ts=%llu",
                 response.status_code,
                 link_id(),
                 (double)result->frame_energy,
                 (double)result->variance,
                 (unsigned long long)result->timestamp_ms);
    }
    return ret;
}

esp_err_t csi_server_client_publish_feature_result(const csi_feature_result_t *result,
                                                   bool log_enabled,
                                                   bool http_enabled)
{
    device_protocol_metadata_t metadata = {0};
    device_protocol_prepare_metadata(&metadata, ESP111_PROTOCOL_MSG_CSI_MOTION);

    char json_body[CSI_SERVER_CLIENT_JSON_BUFFER_SIZE];
    esp_err_t format_ret = format_feature_with_metadata(result,
                                                        &metadata,
                                                        json_body,
                                                        sizeof(json_body));
    if (format_ret != ESP_OK) {
        return format_ret;
    }

    if (log_enabled) {
        ESP_LOGI(TAG, "csi feature %s", json_body);
    }
    if (!http_enabled) {
        return ESP_OK;
    }
    return post_feature_body(result, &metadata, json_body);
}

esp_err_t csi_server_client_upload_feature_result(const csi_feature_result_t *result)
{
    return csi_server_client_publish_feature_result(result, false, true);
}

esp_err_t csi_server_client_upload_features(const char *device_id,
                                            const char *features_json)
{
    (void)device_id;
    (void)features_json;
    ESP_LOGD(TAG, "raw CSI feature upload remains unsupported");
    return ESP_ERR_NOT_SUPPORTED;
}

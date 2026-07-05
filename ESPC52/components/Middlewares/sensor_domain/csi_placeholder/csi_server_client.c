/**
 * @file csi_server_client.c
 * @brief C5 终端 CSI 轻量结果上报客户端。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），只把 presence 摘要压缩为
 * C5<->S3 本地轻量 JSON v 数组并 POST 到 /local/v1/csi/result。它不构造
 * Server 完整 envelope，不上传 raw CSI，也不发送 I/Q、相位或子载波数组。
 */

#include "csi_server_client.h"

#include <stdio.h>

#include "device_protocol_metadata.h"
#include "esp111_protocol_common.h"
#include "esp_log.h"
#include "server_comm_config.h"
#include "server_comm_http.h"
#include "terminal_config.h"

static const char *TAG = "csi_server_client";

#define CSI_SERVER_CLIENT_JSON_BUFFER_SIZE 384U
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

esp_err_t csi_server_client_upload_presence_result(const csi_presence_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    float motion_score = result->motion_score;
    if (motion_score < 0.0f) {
        motion_score = 0.0f;
    } else if (motion_score > 1.0f) {
        motion_score = 1.0f;
    }

    device_protocol_metadata_t metadata = {0};
    device_protocol_prepare_metadata(&metadata, ESP111_PROTOCOL_MSG_CSI_MOTION);

    char json_body[CSI_SERVER_CLIENT_JSON_BUFFER_SIZE];
    int json_len = snprintf(json_body,
                            sizeof(json_body),
                            "{\"" ESP111_PROTOCOL_LOCAL_JSON_PROTOCOL_VERSION "\":%u,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_ID "\":%u,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_TYPE "\":%u,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_PAYLOAD_TYPE "\":\"%s\","
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_UPTIME_MS "\":%s,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_SEQ "\":%s,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_TIME_SYNCED "\":%s,"
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_ROOM_ID "\":\"%s\","
                            "\"" ESP111_PROTOCOL_LOCAL_JSON_VALUES "\":["
                            "%u,%.2f,%.4f,%d,%u,%llu]}",
                            ESP111_PROTOCOL_LOCAL_SCHEMA_VERSION,
                            (unsigned int)terminal_config_get_local_id(),
                            ESP111_PROTOCOL_LOCAL_PACKET_CSI,
                            ESP111_PROTOCOL_MSG_CSI_MOTION,
                            metadata.esp_uptime_ms,
                            metadata.request_seq,
                            metadata.time_synced,
                            metadata.room_id,
                            csi_server_client_state_code(result->state),
                            (double)motion_score,
                            (double)result->variance,
                            (int)result->rssi,
                            (unsigned int)result->sample_count,
                            (unsigned long long)result->updated_at_ms);
    if (json_len < 0 || json_len >= (int)sizeof(json_body)) {
        ESP_LOGE(TAG, "CSI result JSON body too large");
        return ESP_ERR_INVALID_SIZE;
    }

    server_comm_http_response_t response = {0};
    esp_err_t ret = server_comm_http_post_json_with_headers(ESP111_PROTOCOL_ROUTE_CSI_RESULT,
                                                            json_body,
                                                            metadata.headers,
                                                            metadata.header_count,
                                                            CSI_SERVER_CLIENT_TIMEOUT_MS,
                                                            NULL,
                                                            0,
                                                            &response);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG,
                 "CSI result uploaded status=%d state=%u motion=%.2f samples=%u",
                 response.status_code,
                 csi_server_client_state_code(result->state),
                 (double)motion_score,
                 (unsigned int)result->sample_count);
    }
    return ret;
}

esp_err_t csi_server_client_upload_features(const char *device_id,
                                            const char *features_json)
{
    (void)device_id;
    (void)features_json;
    ESP_LOGD(TAG, "raw CSI feature upload remains unsupported");
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @file csi_server_client.c
 * @brief C5 CSI feature 本地上报客户端。
 *
 * 本文件只负责把 csi_phase_a 生成的轻量 feature 格式化成 v2 envelope，并 POST 到
 * ESPS3 /local/v1/csi/result；不会构造 ESP-server payload，也不会携带 raw CSI。
 */

#include "csi_server_client.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_main_config.h"
#include "envelope_builder.h"
#include "esp111_protocol_common.h"
#include "esp_log.h"
#include "server_comm_errors.h"
#include "server_comm_http.h"
#include "terminal_config.h"

static const char *TAG = "csi_server_client";

#ifndef CSI_FEATURE_HTTP_TIMEOUT_MS
#define CSI_FEATURE_HTTP_TIMEOUT_MS 5000U
#endif

#define CSI_LOCAL_LINK_ID "S3_TO_C51"
#define CSI_LOCAL_REPORT_ID "1"

esp_err_t csi_server_client_init(void)
{
    return ESP_OK;
}

const char *csi_server_client_local_link_id(void)
{
    return CSI_LOCAL_LINK_ID;
}

static const char *csi_server_client_local_report_id(void)
{
    return CSI_LOCAL_REPORT_ID;
}

static esp_err_t format_feature_report(const csi_feature_frame_t *result,
                                       char *json_body,
                                       size_t json_body_size)
{
    if (result == NULL || json_body == NULL || json_body_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *feature_link_id = csi_server_client_local_link_id();
    envelope_metrics_t metrics = {
        .frame_energy = result->metrics.frame_energy,
        .variance = result->metrics.variance,
        .cv = result->metrics.cv,
        .rssi = result->metrics.rssi,
        .quality = result->metrics.quality,
    };
    envelope_builder_input_t input = {
        .local_id = csi_server_client_local_report_id(),
        .device_id = terminal_config_get_device_id(),
        .link_id = feature_link_id,
        .timestamp_ms = (int64_t)result->timestamp_ms,
        .metrics = metrics,
        .state_hint = result->state_hint,
        .motion_score = result->motion_score,
        .confidence = result->confidence,
        .source = ENVELOPE_BUILDER_SOURCE_CSI_PHASE_A,
    };
    return envelope_builder_format_local_csi_report(&input, json_body, json_body_size);
}

esp_err_t csi_server_client_format_feature_result(const csi_feature_frame_t *result,
                                                  char *json_body,
                                                  size_t json_body_size)
{
    return format_feature_report(result, json_body, json_body_size);
}

esp_err_t csi_server_client_publish_feature_result(const csi_feature_frame_t *result,
                                                   bool log_enabled,
                                                   bool http_enabled)
{
    char json_body[ENVELOPE_BUILDER_JSON_MAX_BYTES];
    esp_err_t format_ret = format_feature_report(result, json_body, sizeof(json_body));
    if (format_ret != ESP_OK) {
        return format_ret;
    }

    const char *feature_id = csi_server_client_local_report_id();
    const char *feature_link_id = csi_server_client_local_link_id();
    if (!http_enabled) {
        return ESP_OK;
    }
    if (log_enabled) {
        ESP_LOGI(TAG,
                 "CSI_TX id=%s lid=%s state=%s motion_score=%.3f confidence=%.3f t=%llu v=[%.6g,%.6g,%.6g,%d,%.6g]",
                 feature_id,
                 feature_link_id,
                 envelope_builder_state_hint_to_string(result->state_hint),
                 (double)result->motion_score,
                 (double)result->confidence,
                 (unsigned long long)result->timestamp_ms,
                 (double)result->metrics.frame_energy,
                 (double)result->metrics.variance,
                 (double)result->metrics.cv,
                 (int)result->metrics.rssi,
                 (double)result->metrics.quality);
    }
    server_comm_http_response_t response = {0};
    esp_err_t ret = server_comm_http_post_json(ESP111_PROTOCOL_ROUTE_CSI_RESULT,
                                               json_body,
                                               CSI_FEATURE_HTTP_TIMEOUT_MS,
                                               NULL,
                                               0,
                                               &response);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "CSI_REPORT_HTTP id=%s link=%s bytes=%d result=OK",
                 feature_id,
                 feature_link_id,
                 (int)strlen(json_body));
    } else {
        ESP_LOGW(TAG, "CSI_REPORT_HTTP_FAILED error=%s", server_comm_err_to_name(ret));
    }
    return ret;
}

esp_err_t csi_server_client_upload_feature_result(const csi_feature_frame_t *result)
{
    return csi_server_client_publish_feature_result(result, false, true);
}

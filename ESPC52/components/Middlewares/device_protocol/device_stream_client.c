/**
 * @file device_stream_client.c
 * @brief C5 -> S3 统一轻量 telemetry stream 客户端。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用）。它负责把本地 sensor/status/event
 * 压成短 JSON frame，然后发给 ESPS3 本地网关。
 */

#include "device_stream_client.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "app_time_sync.h"
#include "device_protocol_metadata.h"
#include "esp111_protocol_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "server_comm_config.h"
#include "server_comm_http.h"

static const char *TAG = "device_stream";

static int64_t s_last_stream_timestamp_ms;
static portMUX_TYPE s_timestamp_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t device_stream_client_init(void)
{
    return ESP_OK;
}

static bool stream_type_is_allowed(const char *type)
{
    return type != NULL &&
           (strcmp(type, ESP111_PROTOCOL_DEVICE_STREAM_TYPE_SENSOR) == 0 ||
            strcmp(type, ESP111_PROTOCOL_DEVICE_STREAM_TYPE_STATUS) == 0 ||
            strcmp(type, ESP111_PROTOCOL_DEVICE_STREAM_TYPE_EVENT) == 0);
}

static int64_t next_stream_timestamp_ms(void)
{
    int64_t now_ms = app_time_sync_is_synced() ? app_time_sync_get_unix_ms() :
                                                 app_time_sync_get_uptime_ms();
    if (now_ms <= 0) {
        now_ms = esp_timer_get_time() / 1000;
    }
    int64_t monotonic_ms = now_ms;
    /* 单调化同一 C5 的 stream 时间戳，避免 S3 latest cache 被相同时间戳覆盖顺序干扰。 */
    portENTER_CRITICAL(&s_timestamp_lock);
    if (monotonic_ms <= s_last_stream_timestamp_ms) {
        monotonic_ms = s_last_stream_timestamp_ms + 1;
    }
    s_last_stream_timestamp_ms = monotonic_ms;
    portEXIT_CRITICAL(&s_timestamp_lock);
    return monotonic_ms;
}

static const char *default_link_id(const char *type)
{
    (void)type;
    return "S3";
}

esp_err_t device_stream_client_format(const char *type,
                                      const char *link_id,
                                      double v1,
                                      double v2,
                                      double v3,
                                      char *out,
                                      size_t out_size)
{
    if (!stream_type_is_allowed(type) || out == NULL || out_size == 0U ||
        !isfinite(v1) || !isfinite(v2) || !isfinite(v3)) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *did = server_comm_get_device_id();
    const char *lid = link_id != NULL && link_id[0] != '\0' ? link_id : default_link_id(type);
    if (did == NULL || did[0] == '\0' || lid == NULL || lid[0] == '\0' ||
        strchr(did, '"') != NULL || strchr(did, '\\') != NULL ||
        strchr(type, '"') != NULL || strchr(type, '\\') != NULL ||
        strchr(lid, '"') != NULL || strchr(lid, '\\') != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out,
                           out_size,
                           "{\"t\":%lld,\"did\":\"%s\",\"type\":\"%s\",\"lid\":\"%s\","
                           "\"v1\":%.6g,\"v2\":%.6g,\"v3\":%.6g}",
                           (long long)next_stream_timestamp_ms(),
                           did,
                           type,
                           lid,
                           v1,
                           v2,
                           v3);
    if (written <= 0 || written >= (int)out_size ||
        written >= (int)ESP111_PROTOCOL_DEVICE_STREAM_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t send_udp_frame(const char *json_body)
{
    if (json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        return ESP_FAIL;
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(ESP111_PROTOCOL_DEVICE_STREAM_UDP_PORT);
    if (inet_pton(AF_INET, server_comm_get_host(), &dest.sin_addr) != 1) {
        close(sock);
        return ESP_ERR_INVALID_ARG;
    }

    size_t body_len = strlen(json_body);
    /* UDP 发送保持一次性 best-effort；不在 C5 上缓存重试，避免断联时占用语音资源。 */
    int sent = sendto(sock,
                      json_body,
                      body_len,
                      0,
                      (const struct sockaddr *)&dest,
                      sizeof(dest));
    close(sock);
    return sent == (int)body_len ? ESP_OK : ESP_FAIL;
}

esp_err_t device_stream_client_publish_best_effort(const char *type,
                                                   const char *link_id,
                                                   double v1,
                                                   double v2,
                                                   double v3)
{
    char json_body[ESP111_PROTOCOL_DEVICE_STREAM_MAX_BYTES];
    esp_err_t ret = device_stream_client_format(type,
                                                link_id,
                                                v1,
                                                v2,
                                                v3,
                                                json_body,
                                                sizeof(json_body));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "stream frame format failed type=%s ret=%s", type, esp_err_to_name(ret));
        return ret;
    }

    ret = send_udp_frame(json_body);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "stream udp sent %s", json_body);
    }
    return ret;
}

esp_err_t device_stream_client_publish(const char *type,
                                       const char *link_id,
                                       double v1,
                                       double v2,
                                       double v3)
{
    char json_body[ESP111_PROTOCOL_DEVICE_STREAM_MAX_BYTES];
    esp_err_t ret = device_stream_client_format(type,
                                                link_id,
                                                v1,
                                                v2,
                                                v3,
                                                json_body,
                                                sizeof(json_body));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "stream frame format failed type=%s ret=%s", type, esp_err_to_name(ret));
        return ret;
    }

    ret = send_udp_frame(json_body);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "stream udp sent %s", json_body);
        return ESP_OK;
    }

    /* Normal stream traffic may use the local HTTP fallback. */
    device_protocol_metadata_t metadata = {0};
    device_protocol_prepare_metadata(&metadata, type);
    server_comm_http_response_t response = {0};
    ret = server_comm_http_post_json_with_headers(ESP111_PROTOCOL_ROUTE_DEVICE_STREAM,
                                                  json_body,
                                                  metadata.headers,
                                                  metadata.header_count,
                                                  server_comm_get_default_timeout_ms(),
                                                  NULL,
                                                  0,
                                                  &response);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG,
                 "stream fallback failed type=%s ret=%s status=%d",
                 type,
                 esp_err_to_name(ret),
                 response.status_code);
    }
    return ret;
}

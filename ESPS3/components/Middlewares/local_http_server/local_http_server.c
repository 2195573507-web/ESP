/**
 * @file local_http_server.c
 * @brief S3 网关 /local/v1 HTTP 入口。
 *
 * 本文件属于 ESPS3 网关，负责暴露 C5<->S3 本地接口：register、heartbeat、status、
 * sensor、voice、commands 和 CSI placeholder。POST 输入 handler 做轻量协议校验后
 * 只把 body 通过 s3_scheduler 入队；状态更新、CSI fusion 和 Server 适配统一在 runtime
 * worker 内完成。
 */

#include "local_http_server.h"

#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "command_router.h"
#include "device_stream_gateway.h"
#include "esp111_protocol_common.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gateway_config.h"
#include "gateway_wifi.h"
#include "offline_policy.h"
#include "protocol_adapter.h"
#include "s3_scheduler.h"
#include "voice_proxy.h"
#include "wake_prompt_cache_gateway.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "local_http";

static httpd_handle_t s_server;
static int64_t s_last_csi_rx_log_ms;
static int64_t s_last_csi_rx_reject_log_ms;

#define CSI_RX_LOG_INTERVAL_MS 1000LL

static bool should_log_csi_rx(int64_t *last_log_ms)
{
    if (last_log_ms == NULL) {
        return false;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (*last_log_ms != 0 &&
        now_ms - *last_log_ms < CSI_RX_LOG_INTERVAL_MS) {
        return false;
    }
    *last_log_ms = now_ms;
    return true;
}

static cJSON *json_item(cJSON *root, const char *key)
{
    return root != NULL ? cJSON_GetObjectItemCaseSensitive(root, key) : NULL;
}

static const char *json_diag_string(cJSON *root, const char *key)
{
    cJSON *value = json_item(root, key);
    return cJSON_IsString(value) && value->valuestring != NULL && value->valuestring[0] != '\0'
               ? value->valuestring
               : NULL;
}

static const char *csi_diag_string(const protocol_adapter_envelope_t *envelope,
                                   const char *key)
{
    const char *value = envelope != NULL ? json_diag_string(envelope->payload, key) : NULL;
    if (value == NULL && envelope != NULL) {
        value = json_diag_string(envelope->root, key);
    }
    return value != NULL ? value : "-";
}

static cJSON *csi_diag_number_item(const protocol_adapter_envelope_t *envelope,
                                   const char *key)
{
    cJSON *value = envelope != NULL ? json_item(envelope->payload, key) : NULL;
    if (!cJSON_IsNumber(value) && envelope != NULL) {
        value = json_item(envelope->root, key);
    }
    return cJSON_IsNumber(value) ? value : NULL;
}

static const char *csi_diag_state(const protocol_adapter_envelope_t *envelope)
{
    const char *state = csi_diag_string(envelope, "state");
    if (strcmp(state, "-") == 0) {
        state = csi_diag_string(envelope, "state_hint");
    }
    return state;
}

static void csi_diag_motion_score(const protocol_adapter_envelope_t *envelope,
                                  char *out,
                                  size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    strlcpy(out, "-", out_size);

    cJSON *score = csi_diag_number_item(envelope, "motion_score");
    if (score == NULL) {
        score = csi_diag_number_item(envelope, "confidence");
    }
    if (score != NULL) {
        int written = snprintf(out, out_size, "%.3f", score->valuedouble);
        if (written <= 0 || written >= (int)out_size) {
            strlcpy(out, "-", out_size);
        }
    }
}

static void csi_json_error_text(const char *body,
                                size_t body_len,
                                char *out,
                                size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    strlcpy(out, "-", out_size);

    const char *error = cJSON_GetErrorPtr();
    if (error == NULL) {
        return;
    }
    if (body != NULL && error >= body && error <= body + body_len) {
        size_t offset = (size_t)(error - body);
        int written = snprintf(out, out_size, "offset_%u", (unsigned int)offset);
        if (written <= 0 || written >= (int)out_size) {
            strlcpy(out, "offset", out_size);
        }
        return;
    }
    strlcpy(out, "parse_error", out_size);
}

static cJSON *csi_diag_item(cJSON *root, const char *primary, const char *fallback)
{
    cJSON *value = json_item(root, primary);
    if (value == NULL && fallback != NULL) {
        value = json_item(root, fallback);
    }
    return value;
}

static void csi_diag_copy_value(cJSON *root,
                                const char *primary,
                                const char *fallback,
                                char *out,
                                size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    strlcpy(out, "-", out_size);

    cJSON *value = csi_diag_item(root, primary, fallback);
    if (cJSON_IsString(value) && value->valuestring != NULL && value->valuestring[0] != '\0') {
        strlcpy(out, value->valuestring, out_size);
        return;
    }
    if (cJSON_IsNumber(value) && isfinite(value->valuedouble)) {
        int written = snprintf(out, out_size, "%.0f", value->valuedouble);
        if (written <= 0 || written >= (int)out_size) {
            strlcpy(out, "number", out_size);
        }
    }
}

static bool csi_diag_has_text_or_number(cJSON *item)
{
    if (cJSON_IsString(item)) {
        return item->valuestring != NULL && item->valuestring[0] != '\0';
    }
    return cJSON_IsNumber(item) && isfinite(item->valuedouble);
}

static bool csi_diag_array_has_invalid_number(cJSON *array)
{
    if (!cJSON_IsArray(array)) {
        return false;
    }
    int count = cJSON_GetArraySize(array);
    for (int i = 0; i < count; ++i) {
        cJSON *item = cJSON_GetArrayItem(array, i);
        if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) {
            return true;
        }
    }
    cJSON *quality = cJSON_GetArrayItem(array, 4);
    if (cJSON_IsNumber(quality) &&
        (quality->valuedouble < 0.0 || quality->valuedouble > 1.0)) {
        return true;
    }
    return false;
}

static void csi_diag_reject_reason(cJSON *root, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    strlcpy(out, "-", out_size);
    if (root == NULL) {
        strlcpy(out, "invalid json", out_size);
        return;
    }

    cJSON *id = csi_diag_item(root, ESP111_PROTOCOL_LOCAL_JSON_ID, ESP111_PROTOCOL_JSON_DEVICE_ID);
    cJSON *lid = csi_diag_item(root, ESP111_PROTOCOL_DEVICE_STREAM_JSON_LINK_ID, "link_id");
    cJSON *timestamp = csi_diag_item(root,
                                     ESP111_PROTOCOL_DEVICE_STREAM_JSON_TIMESTAMP,
                                     ESP111_PROTOCOL_JSON_TIMESTAMP_MS);
    cJSON *values = json_item(root, ESP111_PROTOCOL_LOCAL_JSON_VALUES);
    cJSON *metrics = json_item(root, "metrics");

    if (!csi_diag_has_text_or_number(id)) {
        strlcpy(out, "missing id", out_size);
        return;
    }
    if (!csi_diag_has_text_or_number(lid)) {
        strlcpy(out, "missing lid", out_size);
        return;
    }
    if (!cJSON_IsObject(metrics) && (!cJSON_IsArray(values) || cJSON_GetArraySize(values) != 5)) {
        strlcpy(out, "invalid v length", out_size);
        return;
    }
    if (!cJSON_IsNumber(timestamp) || !isfinite(timestamp->valuedouble) ||
        timestamp->valuedouble <= 0.0 ||
        csi_diag_array_has_invalid_number(values)) {
        strlcpy(out, "invalid number", out_size);
    }
}

static void log_csi_rx(const protocol_adapter_envelope_t *envelope, size_t bytes)
{
    if (!should_log_csi_rx(&s_last_csi_rx_log_ms)) {
        return;
    }

    char motion_score[24];
    csi_diag_motion_score(envelope, motion_score, sizeof(motion_score));
    ESP_LOGI(TAG,
             "CSI_RX device_id=%s link_id=%s bytes=%u state=%s motion_score=%s",
             envelope != NULL && envelope->device_id[0] != '\0' ? envelope->device_id : "-",
             csi_diag_string(envelope, "link_id"),
             (unsigned int)bytes,
             csi_diag_state(envelope),
             motion_score);
}

static void log_csi_rx_reject(esp_err_t ret, const char *body, size_t body_len, const char *json_error)
{
    if (!should_log_csi_rx(&s_last_csi_rx_reject_log_ms)) {
        return;
    }

    cJSON *root = body != NULL ? cJSON_ParseWithLength(body, body_len) : NULL;
    char id[PROTOCOL_ADAPTER_TEXT_LEN];
    char lid[PROTOCOL_ADAPTER_TEXT_LEN];
    char timestamp[32];
    char reason[32];
    strlcpy(id, "-", sizeof(id));
    strlcpy(lid, "-", sizeof(lid));
    strlcpy(timestamp, "-", sizeof(timestamp));
    strlcpy(reason, "-", sizeof(reason));
    int v_len = -1;

    if (root != NULL) {
        csi_diag_copy_value(root,
                            ESP111_PROTOCOL_LOCAL_JSON_ID,
                            ESP111_PROTOCOL_JSON_DEVICE_ID,
                            id,
                            sizeof(id));
        csi_diag_copy_value(root,
                            ESP111_PROTOCOL_DEVICE_STREAM_JSON_LINK_ID,
                            "link_id",
                            lid,
                            sizeof(lid));
        csi_diag_copy_value(root,
                            ESP111_PROTOCOL_DEVICE_STREAM_JSON_TIMESTAMP,
                            ESP111_PROTOCOL_JSON_TIMESTAMP_MS,
                            timestamp,
                            sizeof(timestamp));
        cJSON *values = json_item(root, ESP111_PROTOCOL_LOCAL_JSON_VALUES);
        if (cJSON_IsArray(values)) {
            v_len = cJSON_GetArraySize(values);
        }
    }
    csi_diag_reject_reason(root, reason, sizeof(reason));
    const char *log_body = body != NULL ? body : "-";
    int log_body_len = body != NULL ? (int)body_len : 1;

    ESP_LOGW(TAG,
             "CSI_RX_REJECT ret=%s body=%.*s id=%s lid=%s t=%s v_len=%d reason=%s json_error=%s",
             esp_err_to_name(ret),
             log_body_len,
             log_body,
             id,
             lid,
             timestamp,
             v_len,
             reason,
             json_error != NULL ? json_error : "-");
    cJSON_Delete(root);
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_type(req, "application/json");
    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_error(httpd_req_t *req,
                            const char *status,
                            const char *code,
                            const char *message)
{
    char body[192];
    unsigned int local_code = ESP111_PROTOCOL_LOCAL_ERROR_UNKNOWN;
    if (code != NULL) {
        if (strcmp(code, ESP111_PROTOCOL_ERROR_UNSUPPORTED_COMMAND) == 0) {
            local_code = ESP111_PROTOCOL_LOCAL_ERROR_UNSUPPORTED_COMMAND;
        } else if (strcmp(code, ESP111_PROTOCOL_ERROR_INVALID_COMMAND_PAYLOAD) == 0 ||
                   strcmp(code, ESP111_PROTOCOL_ERROR_INVALID_ENVELOPE) == 0 ||
                   strcmp(code, ESP111_PROTOCOL_ERROR_INVALID_ACK) == 0 ||
                   strcmp(code, ESP111_PROTOCOL_ERROR_INVALID_DEVICE_ID) == 0 ||
                   strcmp(code, ESP111_PROTOCOL_ERROR_INVALID_COMMAND_ID) == 0) {
            local_code = ESP111_PROTOCOL_LOCAL_ERROR_INVALID_PAYLOAD;
        } else if (strcmp(code, ESP111_PROTOCOL_ERROR_TIMEOUT) == 0) {
            local_code = ESP111_PROTOCOL_LOCAL_ERROR_TIMEOUT;
        } else if (strcmp(code, ESP111_PROTOCOL_ERROR_COMMAND_FAILED) == 0 ||
                   strcmp(code, ESP111_PROTOCOL_ERROR_ACK_FAILED) == 0 ||
                   strcmp(code, ESP111_PROTOCOL_ERROR_COMMAND_POLL_FAILED) == 0 ||
                   strcmp(code, ESP111_PROTOCOL_ERROR_INTERNAL) == 0) {
            local_code = ESP111_PROTOCOL_LOCAL_ERROR_COMMAND_FAILED;
        }
    }
    (void)message;
    protocol_adapter_build_local_error_response(local_code, body, sizeof(body));
    return send_json(req, status, body);
}

static esp_err_t send_local_ok(httpd_req_t *req, uint8_t local_id, const char *status)
{
    char response[192];
    esp_err_t ret = protocol_adapter_build_local_ok_response(local_id, response, sizeof(response));
    if (ret != ESP_OK) {
        return send_error(req,
                          "500 Internal Server Error",
                          ESP111_PROTOCOL_ERROR_INTERNAL,
                          esp_err_to_name(ret));
    }
    return send_json(req, status != NULL ? status : "200 OK", response);
}

static esp_err_t read_json_body(httpd_req_t *req, char **out_body, size_t *out_len)
{
    if (req == NULL || out_body == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_body = NULL;
    *out_len = 0;

    if (req->content_len <= 0 ||
        (size_t)req->content_len > gateway_config_get()->local_http_max_json_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *body = heap_caps_calloc(1, (size_t)req->content_len + 1U, MALLOC_CAP_8BIT);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int remaining = req->content_len;
    int offset = 0;
    while (remaining > 0) {
        int read = httpd_req_recv(req, body + offset, remaining);
        if (read <= 0) {
            heap_caps_free(body);
            return read == HTTPD_SOCK_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
        offset += read;
        remaining -= read;
    }

    *out_body = body;
    *out_len = (size_t)req->content_len;
    return ESP_OK;
}

static void read_peer_ip(httpd_req_t *req, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    out[0] = '\0';

    int sock = httpd_req_to_sockfd(req);
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    if (sock >= 0 && getpeername(sock, (struct sockaddr *)&addr, &addr_len) == 0 &&
        addr.ss_family == AF_INET) {
        const struct sockaddr_in *addr_in = (const struct sockaddr_in *)&addr;
        (void)inet_ntop(AF_INET, &addr_in->sin_addr, out, out_size);
    }
}

static esp_err_t enqueue_body_buffer(httpd_req_t *req,
                                     const char *body,
                                     size_t body_len,
                                     s3_runtime_msg_kind_t kind,
                                     const char *command_id,
                                     s3_scheduler_priority_t priority)
{
    if (body == NULL || body_len == 0U || body_len > S3_RUNTIME_BUS_BODY_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    s3_runtime_ingress_t *ingress =
        heap_caps_calloc(1, sizeof(*ingress), MALLOC_CAP_8BIT);
    if (ingress == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ingress->kind = kind;
    ingress->body_len = body_len;
    memcpy(ingress->body, body, body_len);
    ingress->body[body_len] = '\0';
    ingress->unified.t = esp_timer_get_time() / 1000;
    if (command_id != NULL) {
        strlcpy(ingress->command_id, command_id, sizeof(ingress->command_id));
    }
    read_peer_ip(req, ingress->peer_ip, sizeof(ingress->peer_ip));

    return s3_scheduler_enqueue_ingress_owned(ingress, priority);
}

static esp_err_t validate_local_body(const char *body,
                                     size_t body_len,
                                     uint8_t *out_local_id)
{
    if (body == NULL || body_len == 0U || out_local_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_local_id = 0;
    protocol_adapter_envelope_t envelope = {0};
    esp_err_t ret = protocol_adapter_parse_local_envelope(body, body_len, &envelope);
    if (ret == ESP_OK) {
        ret = protocol_adapter_validate_local_envelope(&envelope);
    }
    if (ret == ESP_OK) {
        *out_local_id = protocol_adapter_device_id_to_local_id(envelope.device_id);
    }
    protocol_adapter_release_envelope(&envelope);
    return ret;
}

static esp_err_t enqueue_local_or_error(httpd_req_t *req,
                                        s3_runtime_msg_kind_t kind,
                                        s3_scheduler_priority_t priority,
                                        const char *status,
                                        const char *error_code)
{
    char *body = NULL;
    size_t body_len = 0;
    esp_err_t ret = read_json_body(req, &body, &body_len);
    if (ret != ESP_OK) {
        heap_caps_free(body);
        return send_error(req, "400 Bad Request", error_code, esp_err_to_name(ret));
    }

    uint8_t local_id = 0;
    ret = validate_local_body(body, body_len, &local_id);
    if (ret == ESP_OK) {
        ret = enqueue_body_buffer(req, body, body_len, kind, NULL, priority);
    }
    heap_caps_free(body);

    if (ret != ESP_OK) {
        const char *http_status =
            (ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_NO_MEM ||
             ret == ESP_ERR_INVALID_STATE) ? "503 Service Unavailable" : "400 Bad Request";
        const char *local_error =
            ret == ESP_ERR_TIMEOUT ? ESP111_PROTOCOL_ERROR_TIMEOUT : error_code;
        return send_error(req, http_status, local_error, esp_err_to_name(ret));
    }
    return send_local_ok(req, local_id, status);
}

static uint8_t local_id_from_json_body(const char *body, size_t body_len)
{
    uint8_t local_id = 0;
    cJSON *root = body != NULL ? cJSON_ParseWithLength(body, body_len) : NULL;
    if (root != NULL) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(root, ESP111_PROTOCOL_LOCAL_JSON_ID);
        if (cJSON_IsNumber(id)) {
            local_id = (uint8_t)id->valueint;
        }
        cJSON_Delete(root);
    }
    return local_id;
}

static esp_err_t health_handler(httpd_req_t *req)
{
    char body[512];
    int written = snprintf(body,
                           sizeof(body),
                           "{\"ok\":true,\"gateway_id\":\"%s\",\"role\":\"gateway\",\"softap_ready\":%s,\"sta_connected\":%s,\"server_available\":%s,\"voice_busy\":%s,\"last_error\":\"%s\"}",
                           gateway_config_get()->gateway_id,
                           gateway_wifi_is_softap_ready() ? "true" : "false",
                           gateway_wifi_is_sta_connected() ? "true" : "false",
                           offline_policy_server_available() ? "true" : "false",
                           voice_proxy_is_busy() ? "true" : "false",
                           offline_policy_last_error_code());
    if (written <= 0 || written >= (int)sizeof(body)) {
        return send_error(req,
                          "500 Internal Server Error",
                          ESP111_PROTOCOL_ERROR_INTERNAL,
                          "health body overflow");
    }
    return send_json(req, "200 OK", body);
}

static esp_err_t register_handler(httpd_req_t *req)
{
    return enqueue_local_or_error(req,
                                  S3_RUNTIME_MSG_STATUS,
                                  S3_SCHEDULER_PRIORITY_HIGH,
                                  "200 OK",
                                  ESP111_PROTOCOL_ERROR_INVALID_ENVELOPE);
}

static esp_err_t heartbeat_handler(httpd_req_t *req)
{
    return enqueue_local_or_error(req,
                                  S3_RUNTIME_MSG_STATUS,
                                  S3_SCHEDULER_PRIORITY_HIGH,
                                  "200 OK",
                                  ESP111_PROTOCOL_ERROR_INVALID_HEARTBEAT);
}

static esp_err_t health_update_handler(httpd_req_t *req)
{
    return enqueue_local_or_error(req,
                                  S3_RUNTIME_MSG_STATUS,
                                  S3_SCHEDULER_PRIORITY_HIGH,
                                  "202 Accepted",
                                  ESP111_PROTOCOL_ERROR_INVALID_HEARTBEAT);
}

static esp_err_t status_or_sensor_handler(httpd_req_t *req)
{
    s3_runtime_msg_kind_t kind =
        strcmp(req->uri, ESP111_PROTOCOL_ROUTE_SENSOR) == 0 ? S3_RUNTIME_MSG_SENSOR :
                                                              S3_RUNTIME_MSG_STATUS;
    return enqueue_local_or_error(req,
                                  kind,
                                  kind == S3_RUNTIME_MSG_SENSOR ? S3_SCHEDULER_PRIORITY_NORMAL :
                                                                  S3_SCHEDULER_PRIORITY_HIGH,
                                  "202 Accepted",
                                  ESP111_PROTOCOL_ERROR_INVALID_ENVELOPE);
}

static esp_err_t csi_result_handler(httpd_req_t *req)
{
    char *body = NULL;
    size_t body_len = 0;
    esp_err_t ret = read_json_body(req, &body, &body_len);
    if (ret != ESP_OK) {
        log_csi_rx_reject(ret, NULL, 0U, "-");
        heap_caps_free(body);
        return send_error(req,
                          "400 Bad Request",
                          ESP111_PROTOCOL_ERROR_INVALID_CSI_RESULT,
                          esp_err_to_name(ret));
    }

    protocol_adapter_envelope_t envelope = {0};
    char json_error[32];
    strlcpy(json_error, "-", sizeof(json_error));
    ret = protocol_adapter_parse_local_envelope(body, body_len, &envelope);
    if (ret != ESP_OK) {
        csi_json_error_text(body, body_len, json_error, sizeof(json_error));
        log_csi_rx_reject(ret, body, body_len, json_error);
        protocol_adapter_release_envelope(&envelope);
        heap_caps_free(body);
        return send_error(req,
                          "400 Bad Request",
                          ESP111_PROTOCOL_ERROR_INVALID_CSI_RESULT,
                          esp_err_to_name(ret));
    }

    ret = protocol_adapter_validate_local_envelope(&envelope);
    uint8_t local_id = protocol_adapter_device_id_to_local_id(envelope.device_id);
    if (ret == ESP_OK) {
        ret = enqueue_body_buffer(req,
                                  body,
                                  body_len,
                                  S3_RUNTIME_MSG_CSI,
                                  NULL,
                                  S3_SCHEDULER_PRIORITY_NORMAL);
    }

    if (ret == ESP_OK) {
        log_csi_rx(&envelope, body_len);
    } else {
        log_csi_rx_reject(ret, body, body_len, "-");
    }

    protocol_adapter_release_envelope(&envelope);
    heap_caps_free(body);
    if (ret != ESP_OK) {
        const char *http_status =
            (ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_NO_MEM ||
             ret == ESP_ERR_INVALID_STATE) ? "503 Service Unavailable" : "400 Bad Request";
        return send_error(req,
                          http_status,
                          ret == ESP_ERR_TIMEOUT ? ESP111_PROTOCOL_ERROR_TIMEOUT :
                                                   ESP111_PROTOCOL_ERROR_INVALID_CSI_RESULT,
                          esp_err_to_name(ret));
    }
    return send_local_ok(req, local_id, "202 Accepted");
}

static esp_err_t device_stream_handler(httpd_req_t *req)
{
    esp_err_t ret = device_stream_gateway_handle_http(req);
    return ret == ESP_OK ? send_json(req, "202 Accepted", "{\"ok\":1}")
                         : send_error(req,
                                      "400 Bad Request",
                                      ESP111_PROTOCOL_ERROR_INVALID_ENVELOPE,
                                      esp_err_to_name(ret));
}

static esp_err_t commands_pending_handler(httpd_req_t *req)
{
    char query[128] = {0};
    char local_id_text[8] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query,
                              ESP111_PROTOCOL_LOCAL_JSON_ID,
                              local_id_text,
                              sizeof(local_id_text)) != ESP_OK) {
        return send_error(req,
                          "400 Bad Request",
                          ESP111_PROTOCOL_ERROR_INVALID_DEVICE_ID,
                          "id query is required");
    }

    uint8_t local_id = (uint8_t)atoi(local_id_text);
    const char *device_id = protocol_adapter_local_device_id_to_device_id(local_id);
    if (device_id == NULL) {
        return send_error(req,
                          "400 Bad Request",
                          ESP111_PROTOCOL_ERROR_INVALID_DEVICE_ID,
                          "id is not allowed");
    }

    char body[2048];
    esp_err_t ret = command_router_build_pending_json(device_id, body, sizeof(body));
    return ret == ESP_OK ? send_json(req, "200 OK", body)
                         : send_error(req,
                                      "400 Bad Request",
                                      ESP111_PROTOCOL_ERROR_COMMAND_POLL_FAILED,
                                      esp_err_to_name(ret));
}

static esp_err_t command_ack_handler(httpd_req_t *req)
{
    const char *prefix = ESP111_PROTOCOL_ROUTE_COMMANDS_PREFIX;
    const char *suffix = ESP111_PROTOCOL_ROUTE_COMMAND_ACK_SUFFIX;
    const char *start = req->uri + strlen(prefix);
    const char *end = strstr(start, suffix);
    if (end == NULL || end <= start) {
        return send_error(req,
                          "400 Bad Request",
                          ESP111_PROTOCOL_ERROR_INVALID_COMMAND_ID,
                          "command_id is required");
    }

    char command_id[48] = {0};
    size_t len = (size_t)(end - start);
    if (len >= sizeof(command_id)) {
        return send_error(req,
                          "414 URI Too Long",
                          ESP111_PROTOCOL_ERROR_INVALID_COMMAND_ID,
                          "command_id is too long");
    }
    memcpy(command_id, start, len);

    char *body = NULL;
    size_t body_len = 0;
    esp_err_t ret = read_json_body(req, &body, &body_len);
    if (ret != ESP_OK) {
        heap_caps_free(body);
        return send_error(req,
                          "400 Bad Request",
                          ESP111_PROTOCOL_ERROR_INVALID_ACK,
                          esp_err_to_name(ret));
    }

    uint8_t local_id = local_id_from_json_body(body, body_len);
    ret = enqueue_body_buffer(req,
                              body,
                              body_len,
                              S3_RUNTIME_MSG_EVENT,
                              command_id,
                              S3_SCHEDULER_PRIORITY_HIGH);
    heap_caps_free(body);
    if (ret != ESP_OK) {
        const char *http_status =
            (ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_NO_MEM ||
             ret == ESP_ERR_INVALID_STATE) ? "503 Service Unavailable" : "400 Bad Request";
        return send_error(req,
                          http_status,
                          ret == ESP_ERR_TIMEOUT ? ESP111_PROTOCOL_ERROR_TIMEOUT :
                                                   ESP111_PROTOCOL_ERROR_ACK_FAILED,
                          esp_err_to_name(ret));
    }

    return send_local_ok(req, local_id, "200 OK");
}

esp_err_t local_http_server_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = gateway_config_get()->local_http_port;
    config.max_uri_handlers = 13;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        return ret;
    }

    const httpd_uri_t routes[] = {
        /* /local/v1 是 C5<->S3 边界；/api/... Server 路径不在本地 HTTP server 暴露。 */
        {.uri = ESP111_PROTOCOL_ROUTE_HEALTH, .method = HTTP_GET, .handler = health_handler},
        {.uri = ESP111_PROTOCOL_ROUTE_HEALTH, .method = HTTP_POST, .handler = health_update_handler},
        {.uri = ESP111_PROTOCOL_ROUTE_REGISTER, .method = HTTP_POST, .handler = register_handler},
        {.uri = ESP111_PROTOCOL_ROUTE_HEARTBEAT, .method = HTTP_POST, .handler = heartbeat_handler},
        {.uri = ESP111_PROTOCOL_ROUTE_STATUS, .method = HTTP_POST, .handler = status_or_sensor_handler},
        {.uri = ESP111_PROTOCOL_ROUTE_SENSOR, .method = HTTP_POST, .handler = status_or_sensor_handler},
        {.uri = ESP111_PROTOCOL_ROUTE_CSI_RESULT, .method = HTTP_POST, .handler = csi_result_handler},
        {.uri = ESP111_PROTOCOL_ROUTE_DEVICE_STREAM, .method = HTTP_POST, .handler = device_stream_handler},
        {.uri = ESP111_PROTOCOL_ROUTE_VOICE_TURN, .method = HTTP_POST, .handler = voice_proxy_handle_turn},
        {.uri = ESP111_PROTOCOL_ROUTE_WAKE_PROMPT_AUDIO, .method = HTTP_GET, .handler = wake_prompt_cache_gateway_handle_http},
        {.uri = ESP111_PROTOCOL_ROUTE_COMMANDS_PENDING, .method = HTTP_GET, .handler = commands_pending_handler},
        {.uri = ESP111_PROTOCOL_ROUTE_COMMAND_ACK_WILDCARD, .method = HTTP_POST, .handler = command_ack_handler},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ret = httpd_register_uri_handler(s_server, &routes[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "register route failed uri=%s ret=%s", routes[i].uri, esp_err_to_name(ret));
            return ret;
        }
    }

    ESP_LOGI(TAG, "local HTTP server started port=%u base=%s",
             (unsigned int)gateway_config_get()->local_http_port,
             ESP111_PROTOCOL_LOCAL_BASE);
    return ESP_OK;
}

esp_err_t local_http_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    httpd_handle_t server = s_server;
    s_server = NULL;
    esp_err_t ret = httpd_stop(server);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "local HTTP server stopped");
    }
    return ret;
}

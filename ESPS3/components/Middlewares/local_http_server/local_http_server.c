/**
 * @file local_http_server.c
 * @brief S3 网关 /local/v1 HTTP 入口。
 *
 * 本文件属于 ESPS3 网关，负责暴露 C5<->S3 本地接口：register、heartbeat、status、
 * sensor、voice、commands 和 CSI placeholder。它在本地 handler 内结束轻量协议解析，
 * 再把需要上云的内容交给 protocol_adapter/server_client；不实现真实 CSI，也不实现
 * ASR/LLM/TTS。
 */

#include "local_http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "child_registry.h"
#include "command_router.h"
#include "csi_placeholder_gateway.h"
#include "esp111_protocol_common.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "gateway_config.h"
#include "gateway_wifi.h"
#include "offline_policy.h"
#include "protocol_adapter.h"
#include "sensor_aggregator.h"
#include "voice_proxy.h"
#include "wake_prompt_cache_gateway.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "local_http";

static httpd_handle_t s_server;

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

static esp_err_t parse_and_validate(httpd_req_t *req,
                                    protocol_adapter_envelope_t *envelope,
                                    char **body)
{
    size_t body_len = 0;
    esp_err_t ret = read_json_body(req, body, &body_len);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = protocol_adapter_parse_local_envelope(*body, body_len, envelope);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = protocol_adapter_validate_local_envelope(envelope);
    if (ret == ESP_OK) {
        int sock = httpd_req_to_sockfd(req);
        struct sockaddr_storage addr;
        socklen_t addr_len = sizeof(addr);
        char peer_ip[16] = {0};
        if (sock >= 0 && getpeername(sock, (struct sockaddr *)&addr, &addr_len) == 0 &&
            addr.ss_family == AF_INET) {
            const struct sockaddr_in *addr_in = (const struct sockaddr_in *)&addr;
            if (inet_ntop(AF_INET, &addr_in->sin_addr, peer_ip, sizeof(peer_ip)) != NULL) {
                (void)child_registry_update_peer_ip(envelope->device_id, peer_ip);
            }
        }
    }
    return ret;
}

static char *capabilities_to_string(const protocol_adapter_envelope_t *envelope)
{
    if (envelope == NULL || envelope->capabilities == NULL) {
        return NULL;
    }
    return cJSON_PrintUnformatted(envelope->capabilities);
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
    char *body = NULL;
    protocol_adapter_envelope_t envelope = {0};
    esp_err_t ret = parse_and_validate(req, &envelope, &body);
    if (ret != ESP_OK) {
        heap_caps_free(body);
        protocol_adapter_release_envelope(&envelope);
        return send_error(req,
                          "400 Bad Request",
                          ESP111_PROTOCOL_ERROR_INVALID_ENVELOPE,
                          esp_err_to_name(ret));
    }

    char *capabilities = capabilities_to_string(&envelope);
    ret = child_registry_register_or_update(envelope.device_id,
                                            envelope.room_id,
                                            envelope.alias,
                                            capabilities,
                                            envelope.seq);
    if (ret == ESP_OK) {
        sensor_aggregator_result_t result = {0};
        ret = sensor_aggregator_handle_envelope(&envelope, &result);
    }
    cJSON_free(capabilities);
    heap_caps_free(body);

    uint8_t local_id = protocol_adapter_device_id_to_local_id(envelope.device_id);
    protocol_adapter_release_envelope(&envelope);
    return ret == ESP_OK ? send_local_ok(req, local_id, "200 OK")
                         : send_error(req,
                                      "403 Forbidden",
                                      ESP111_PROTOCOL_ERROR_INVALID_DEVICE_ID,
                                      esp_err_to_name(ret));
}

static esp_err_t heartbeat_handler(httpd_req_t *req)
{
    char *body = NULL;
    protocol_adapter_envelope_t envelope = {0};
    esp_err_t ret = parse_and_validate(req, &envelope, &body);
    if (ret == ESP_OK) {
        ret = child_registry_touch(envelope.device_id, envelope.seq);
        if (ret == ESP_OK) {
            sensor_aggregator_result_t result = {0};
            ret = sensor_aggregator_handle_envelope(&envelope, &result);
        }
    }
    heap_caps_free(body);

    uint8_t local_id = protocol_adapter_device_id_to_local_id(envelope.device_id);
    protocol_adapter_release_envelope(&envelope);
    return ret == ESP_OK ? send_local_ok(req, local_id, "200 OK")
                         : send_error(req,
                                      "400 Bad Request",
                                      ESP111_PROTOCOL_ERROR_INVALID_HEARTBEAT,
                                      esp_err_to_name(ret));
}

static esp_err_t health_update_handler(httpd_req_t *req)
{
    char *body = NULL;
    protocol_adapter_envelope_t envelope = {0};
    esp_err_t ret = parse_and_validate(req, &envelope, &body);
    if (ret != ESP_OK) {
        heap_caps_free(body);
        protocol_adapter_release_envelope(&envelope);
        return send_error(req,
                          "400 Bad Request",
                          ESP111_PROTOCOL_ERROR_INVALID_HEARTBEAT,
                          esp_err_to_name(ret));
    }

    protocol_adapter_message_kind_t kind = protocol_adapter_message_kind(envelope.message_type);
    if (kind == PROTOCOL_ADAPTER_MESSAGE_REGISTER) {
        char *capabilities = capabilities_to_string(&envelope);
        ret = child_registry_register_or_update(envelope.device_id,
                                                envelope.room_id,
                                                envelope.alias,
                                                capabilities,
                                                envelope.seq);
        cJSON_free(capabilities);
    } else if (kind == PROTOCOL_ADAPTER_MESSAGE_HEARTBEAT ||
               kind == PROTOCOL_ADAPTER_MESSAGE_STATUS) {
        ret = child_registry_touch(envelope.device_id, envelope.seq);
    } else {
        ret = ESP_ERR_NOT_SUPPORTED;
    }

    if (ret == ESP_OK) {
        sensor_aggregator_result_t result = {0};
        ret = sensor_aggregator_handle_envelope(&envelope, &result);
    }

    uint8_t local_id = protocol_adapter_device_id_to_local_id(envelope.device_id);
    heap_caps_free(body);
    protocol_adapter_release_envelope(&envelope);
    return ret == ESP_OK ? send_local_ok(req, local_id, "202 Accepted")
                         : send_error(req,
                                      "400 Bad Request",
                                      ESP111_PROTOCOL_ERROR_INVALID_HEARTBEAT,
                                      esp_err_to_name(ret));
}

static esp_err_t status_or_sensor_handler(httpd_req_t *req)
{
    char *body = NULL;
    protocol_adapter_envelope_t envelope = {0};
    esp_err_t ret = parse_and_validate(req, &envelope, &body);
    if (ret != ESP_OK) {
        heap_caps_free(body);
        protocol_adapter_release_envelope(&envelope);
        return send_error(req,
                          "400 Bad Request",
                          ESP111_PROTOCOL_ERROR_INVALID_ENVELOPE,
                          esp_err_to_name(ret));
    }

    child_registry_touch(envelope.device_id, envelope.seq);
    sensor_aggregator_result_t result = {0};
    /* C5 轻量 status/sensor 到此完成本地校验；下一步由 sensor_aggregator 转成完整 Server ingest。 */
    ret = sensor_aggregator_handle_envelope(&envelope, &result);

    uint8_t local_id = protocol_adapter_device_id_to_local_id(envelope.device_id);
    heap_caps_free(body);
    protocol_adapter_release_envelope(&envelope);
    if (ret != ESP_OK) {
        return send_error(req,
                          "500 Internal Server Error",
                          ESP111_PROTOCOL_ERROR_INTERNAL,
                          esp_err_to_name(ret));
    }
    return send_local_ok(req, local_id, "202 Accepted");
}

static esp_err_t csi_result_handler(httpd_req_t *req)
{
    char *body = NULL;
    protocol_adapter_envelope_t envelope = {0};
    esp_err_t ret = parse_and_validate(req, &envelope, &body);
    if (ret == ESP_OK) {
        child_registry_touch(envelope.device_id, envelope.seq);
        ret = csi_placeholder_gateway_handle_result(&envelope);
    }

    uint8_t local_id = protocol_adapter_device_id_to_local_id(envelope.device_id);
    heap_caps_free(body);
    protocol_adapter_release_envelope(&envelope);
    return ret == ESP_OK ? send_local_ok(req, local_id, "202 Accepted")
                         : send_error(req,
                                      "400 Bad Request",
                                      ESP111_PROTOCOL_ERROR_INVALID_CSI_RESULT,
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

    ret = command_router_ack(command_id, body);
    uint8_t local_id = 0;
    cJSON *ack_root = body != NULL ? cJSON_Parse(body) : NULL;
    if (ack_root != NULL) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(ack_root, ESP111_PROTOCOL_LOCAL_JSON_ID);
        if (cJSON_IsNumber(id)) {
            local_id = (uint8_t)id->valueint;
        }
        cJSON_Delete(ack_root);
    }
    heap_caps_free(body);
    if (ret != ESP_OK) {
        return send_error(req,
                          "400 Bad Request",
                          ESP111_PROTOCOL_ERROR_ACK_FAILED,
                          esp_err_to_name(ret));
    }

    char response[64];
    protocol_adapter_build_local_ok_response(local_id, response, sizeof(response));
    return send_json(req, "200 OK", response);
}

esp_err_t local_http_server_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = gateway_config_get()->local_http_port;
    config.max_uri_handlers = 12;
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

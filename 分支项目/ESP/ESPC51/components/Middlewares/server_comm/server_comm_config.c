/**
 * @file server_comm_config.c
 * @brief C5 终端本地 HTTP 目标配置。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），负责把 terminal_config 中的
 * S3 gateway_ip/device_id/gateway_id 暴露给 server_comm_http。它不保存公网
 * Server URL，不决定业务路径，也不做 JSON 编解码。
 */

#include "server_comm_config.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "terminal_config.h"

const char *server_comm_get_base_url(void)
{
    static char base_url[64];
    const char *gateway_ip = terminal_config_get_gateway_ip();
    int written = snprintf(base_url, sizeof(base_url), "http://%s", gateway_ip);
    if (written <= 0 || written >= (int)sizeof(base_url)) {
        return SERVER_COMM_BASE_URL;
    }
    return base_url;
}

const char *server_comm_get_host(void)
{
    return terminal_config_get_gateway_ip();
}

int server_comm_get_port(void)
{
    return SERVER_COMM_PORT;
}

const char *server_comm_get_device_id(void)
{
    return terminal_config_get_device_id();
}

const char *server_comm_get_gateway_id(void)
{
    return terminal_config_get_gateway_id();
}

const char *server_comm_get_room_id(void)
{
    return terminal_config_get_room_id();
}

const char *server_comm_get_alias(void)
{
    return terminal_config_get_alias();
}

const char *server_comm_get_firmware_version(void)
{
    return TERMINAL_CONFIG_FIRMWARE_VERSION;
}

const char *server_comm_get_device_type(void)
{
    return TERMINAL_CONFIG_DEVICE_TYPE;
}

const char *server_comm_get_capabilities_json(void)
{
    return terminal_config_get_capabilities_json();
}

uint32_t server_comm_get_default_timeout_ms(void)
{
    return SERVER_COMM_DEFAULT_TIMEOUT_MS;
}

esp_err_t server_comm_build_url(const char *endpoint, char *url, size_t url_size)
{
    if (endpoint == NULL || endpoint[0] == '\0' || url == NULL || url_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strncmp(endpoint, "http://", strlen("http://")) == 0 ||
        strncmp(endpoint, "https://", strlen("https://")) == 0) {
        /*
         * C5 正式链路只允许访问 ESPS3 暴露的 /local/v1 相对路径。
         * Server-facing HTTP 统一放在 ESPS3，避免 NVS 或调用方传入公网 URL 后绕过网关。
         */
        return ESP_ERR_NOT_ALLOWED;
    }

    size_t local_base_len = strlen(ESP111_PROTOCOL_LOCAL_BASE);
    if (strncmp(endpoint, ESP111_PROTOCOL_LOCAL_BASE, local_base_len) != 0 ||
        (endpoint[local_base_len] != '\0' &&
         endpoint[local_base_len] != '/' &&
         endpoint[local_base_len] != '?' &&
         endpoint[local_base_len] != '#')) {
        return ESP_ERR_NOT_ALLOWED;
    }

    const char *base = server_comm_get_base_url();
    size_t base_len = strlen(base);
    while (base_len > 0 && base[base_len - 1] == '/') {
        base_len--;
    }

    const char *path = endpoint;
    bool endpoint_has_slash = endpoint[0] == '/';
    int written = snprintf(url,
                           url_size,
                           "%.*s%s%s",
                           (int)base_len,
                           base,
                           endpoint_has_slash ? "" : "/",
                           path);
    if (written < 0 || written >= (int)url_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

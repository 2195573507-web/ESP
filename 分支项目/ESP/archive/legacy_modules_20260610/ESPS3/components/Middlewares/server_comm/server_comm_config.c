#include "server_comm_config.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

const char *server_comm_get_base_url(void)
{
    return SERVER_COMM_BASE_URL;
}

const char *server_comm_get_host(void)
{
    return SERVER_COMM_HOST;
}

int server_comm_get_port(void)
{
    return SERVER_COMM_PORT;
}

const char *server_comm_get_device_id(void)
{
    return SERVER_COMM_DEVICE_ID;
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
        if (strlcpy(url, endpoint, url_size) >= url_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        return ESP_OK;
    }

    const char *base = SERVER_COMM_BASE_URL;
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

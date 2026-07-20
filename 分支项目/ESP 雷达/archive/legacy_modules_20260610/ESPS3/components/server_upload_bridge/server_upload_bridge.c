#include "server_upload_bridge.h"

#include <stdio.h>

#include "esp_log.h"
#include "server_comm_http.h"

static const char *TAG = "server_upload_bridge";

esp_err_t server_upload_bridge_init(void)
{
    /* Reserved for future server URL, device ID, and auth token configuration. */
    return ESP_OK;
}

esp_err_t server_upload_bridge_upload_bme690(const server_upload_bme690_data_t *data)
{
    if (data == NULL || data->device_id == NULL) {
        ESP_LOGE(TAG, "invalid bme690 upload data");
        return ESP_ERR_INVALID_ARG;
    }

    char json_body[SERVER_UPLOAD_BRIDGE_JSON_BUFFER_SIZE];
    int json_len = snprintf(json_body,
                            sizeof(json_body),
                            "{\"device_id\":\"%s\",\"temperature\":%.2f,"
                            "\"humidity\":%.2f,\"pressure\":%.2f,"
                            "\"gas_resistance\":%.2f}",
                            data->device_id,
                            data->temperature,
                            data->humidity,
                            data->pressure,
                            data->gas_resistance);
    if (json_len < 0 || json_len >= (int)sizeof(json_body)) {
        ESP_LOGE(TAG, "upload bme690 failed: %s", esp_err_to_name(ESP_ERR_INVALID_SIZE));
        return ESP_ERR_INVALID_SIZE;
    }

    server_comm_http_response_t response = {0};
    esp_err_t ret = server_comm_http_post_json(SERVER_UPLOAD_BRIDGE_ENDPOINT,
                                               json_body,
                                               SERVER_UPLOAD_BRIDGE_TIMEOUT_MS,
                                               NULL,
                                               0,
                                               &response);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "upload bme690 ok, status=%d", response.status_code);
    } else {
        ESP_LOGE(TAG, "upload bme690 failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

#include "screen_server_client.h"

#include "esp_log.h"

static const char *TAG = "screen_server_client";

esp_err_t screen_server_client_init(void)
{
    ESP_LOGI(TAG, "screen server client reserved");
    return ESP_OK;
}

esp_err_t screen_server_client_poll_commands(const char *device_id)
{
    (void)device_id;
    ESP_LOGD(TAG, "screen command polling reserved");
    return ESP_ERR_NOT_SUPPORTED;
}

#include "csi_server_client.h"

#include "esp_log.h"

static const char *TAG = "csi_server_client";

esp_err_t csi_server_client_init(void)
{
    ESP_LOGI(TAG, "CSI server client reserved");
    return ESP_OK;
}

esp_err_t csi_server_client_upload_features(const char *device_id,
                                            const char *features_json)
{
    (void)device_id;
    (void)features_json;
    ESP_LOGD(TAG, "CSI feature upload reserved");
    return ESP_ERR_NOT_SUPPORTED;
}

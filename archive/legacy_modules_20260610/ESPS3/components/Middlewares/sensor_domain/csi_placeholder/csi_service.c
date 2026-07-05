#include "csi_service.h"

#include <stdbool.h>

#include "esp_log.h"

static const char *TAG = "csi_service";

static bool s_csi_started;
static bool s_csi_paused;

esp_err_t csi_service_init(void)
{
    ESP_LOGI(TAG, "CSI service reserved");
    return ESP_OK;
}

esp_err_t csi_service_start(void)
{
    s_csi_started = true;
    s_csi_paused = false;
    ESP_LOGI(TAG, "CSI service start reserved");
    return ESP_OK;
}

void csi_service_pause(void)
{
    if (s_csi_started) {
        s_csi_paused = true;
    }
}

void csi_service_resume(void)
{
    if (s_csi_started) {
        s_csi_paused = false;
    }
}

/**
 * @file gateway_config.c
 * @brief S3 网关静态运行配置。
 *
 * 本文件属于 ESPS3 网关，集中提供 gateway_id、SoftAP、STA、Server base URL、
 * 子设备 allowlist 和本地 HTTP 限制。它不启动 WiFi、不发起 HTTP、不改变协议字段；
 * gateway_wifi、local_http_server、server_client 和 child_registry 只读取本配置。
 */

#include "gateway_config.h"

#include <string.h>

#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "gateway_wifi_credentials.h"
#include "sdkconfig.h"

static const char *TAG = "gateway_config";

static bool sta_credential_configured(const gateway_wifi_credential_t *credential)
{
    return credential != NULL && credential->ssid != NULL && credential->ssid[0] != '\0';
}

static size_t sta_configured_credential_count(void)
{
    size_t configured_count = 0U;

    for (size_t i = 0; i < GATEWAY_WIFI_STA_CREDENTIAL_COUNT; i++) {
        if (sta_credential_configured(&gateway_wifi_sta_credentials[i])) {
            configured_count++;
        }
    }

    return configured_count;
}

static const gateway_runtime_config_t s_config = {
    .gateway_id = GATEWAY_CONFIG_ID,
    .hardware_module = GATEWAY_CONFIG_HARDWARE_MODULE,
    .softap_ssid = GATEWAY_CONFIG_SOFTAP_SSID,
    .softap_password = GATEWAY_CONFIG_SOFTAP_PASSWORD,
    .softap_ip = GATEWAY_CONFIG_SOFTAP_IP,
    .softap_netmask = GATEWAY_CONFIG_SOFTAP_NETMASK,
    .softap_gw = GATEWAY_CONFIG_SOFTAP_GW,
    .softap_channel = GATEWAY_CONFIG_SOFTAP_CHANNEL,
    .softap_max_connection = GATEWAY_CONFIG_SOFTAP_MAX_CONNECTION,
    .sta_credentials = gateway_wifi_sta_credentials,
    .sta_credentials_count = GATEWAY_WIFI_STA_CREDENTIAL_COUNT,
    .server_base_url = GATEWAY_CONFIG_SERVER_BASE_URL,
    .auth_token = GATEWAY_CONFIG_AUTH_TOKEN,
    .local_http_port = GATEWAY_CONFIG_LOCAL_HTTP_PORT,
    .voice_upload_max_bytes = GATEWAY_CONFIG_VOICE_UPLOAD_MAX_BYTES,
    .local_http_max_json_bytes = GATEWAY_CONFIG_LOCAL_HTTP_MAX_JSON_BYTES,
    .heartbeat_timeout_ms = GATEWAY_CONFIG_HEARTBEAT_TIMEOUT_MS,
    .link_lost_grace_ms = GATEWAY_CONFIG_LINK_LOST_GRACE_MS,
    .sensor_forward_period_ms = GATEWAY_CONFIG_SENSOR_FORWARD_PERIOD_MS,
    .csi_trigger_enabled = GATEWAY_CONFIG_ENABLE_CSI_TRIGGER != 0,
    .csi_result_ingest_enabled = GATEWAY_CONFIG_ENABLE_CSI_RESULT_INGEST != 0,
    .csi_trigger_interval_ms = GATEWAY_CONFIG_CSI_TRIGGER_INTERVAL_MS,
    .csi_trigger_udp_port = GATEWAY_CONFIG_CSI_TRIGGER_UDP_PORT,
    .csi_trigger_target_device_id = GATEWAY_CONFIG_CSI_TRIGGER_TARGET_DEVICE_ID,
    .children_allowlist = {
        "sensair_shuttle_01",
        "sensair_shuttle_02",
    },
    .children_allowlist_count = 2U,
};

const gateway_runtime_config_t *gateway_config_get(void)
{
    return &s_config;
}

bool gateway_config_sta_credentials_configured(void)
{
    return sta_configured_credential_count() > 0U;
}

bool gateway_config_child_allowed(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return false;
    }

    for (size_t i = 0; i < s_config.children_allowlist_count; i++) {
        if (s_config.children_allowlist[i] != NULL &&
            strcmp(device_id, s_config.children_allowlist[i]) == 0) {
            return true;
        }
    }

    return false;
}

void gateway_config_log_boot_profile(void)
{
    uint32_t flash_size = 0;
    esp_err_t flash_ret = esp_flash_get_size(NULL, &flash_size);
    size_t psram_size = esp_psram_is_initialized() ? esp_psram_get_size() : 0U;
    size_t psram_heap = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG,
             "role=gateway gateway_id=%s hardware=%s softap=%s ip=%s http_port=%u",
             s_config.gateway_id,
             s_config.hardware_module,
             s_config.softap_ssid,
             s_config.softap_ip,
             (unsigned int)s_config.local_http_port);
    ESP_LOGI(TAG,
             "build target=%s flash_config=%s flash_detected=%u psram_enabled=%d psram_size=%u psram_heap=%u",
             CONFIG_IDF_TARGET,
             CONFIG_ESPTOOLPY_FLASHSIZE,
             flash_ret == ESP_OK ? (unsigned int)flash_size : 0U,
             esp_psram_is_initialized() ? 1 : 0,
             (unsigned int)psram_size,
             (unsigned int)psram_heap);

    if (strcmp(CONFIG_ESPTOOLPY_FLASHSIZE, "32MB") != 0) {
        ESP_LOGE(TAG, "flash config is not 32MB: %s", CONFIG_ESPTOOLPY_FLASHSIZE);
    }

    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM is not initialized; N32R16 runtime check failed");
    }

    const size_t configured_sta_count = sta_configured_credential_count();
    ESP_LOGI(TAG,
             "STA credential candidates=%u configured=%u",
             (unsigned int)s_config.sta_credentials_count,
             (unsigned int)configured_sta_count);

    if (configured_sta_count == 0U) {
        ESP_LOGW(TAG, "STA credentials are empty; SoftAP/local HTTP will start, server uplink remains offline");
    }
    ESP_LOGI(TAG,
             "CSI trigger enabled=%d result_ingest=%d interval_ms=%u udp_port=%u target=%s",
             s_config.csi_trigger_enabled ? 1 : 0,
             s_config.csi_result_ingest_enabled ? 1 : 0,
             (unsigned int)s_config.csi_trigger_interval_ms,
             (unsigned int)s_config.csi_trigger_udp_port,
             s_config.csi_trigger_target_device_id[0] != '\0' ? s_config.csi_trigger_target_device_id : "all");
}

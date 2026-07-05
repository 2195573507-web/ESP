/**
 * @file gateway_wifi.c
 * @brief S3 网关 SoftAP + STA 网络初始化。
 *
 * 本文件属于 ESPS3 网关，负责创建 C5 连接的 SoftAP，并在 STA 凭据存在时连接上游网络。
 * 它不处理 /local/v1 HTTP、不维护 child registry、不访问 ESP-server 业务接口。
 */

#include "gateway_wifi.h"

#include <string.h>

#include "child_registry.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "gateway_config.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"

static const char *TAG = "gateway_wifi";

static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static bool s_started;
static bool s_softap_ready;
static bool s_sta_connected;
static size_t s_sta_credential_index;

static void handle_sta_start(void);
static void handle_sta_disconnected(void);

static esp_err_t ensure_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        s_softap_ready = true;
        ESP_LOGI(TAG, "SoftAP started at %s", gateway_config_get()->softap_ip);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "child station connected aid=%u", (unsigned int)event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "child station disconnected aid=%u", (unsigned int)event->aid);
        child_registry_mark_all_link_lost("ap_sta_disconnected");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        handle_sta_start();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        handle_sta_disconnected();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_sta_connected = true;
        ESP_LOGI(TAG, "STA got IP " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t set_softap_ip(void)
{
    esp_netif_ip_info_t ip_info = {0};

    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    esp_err_t ret = esp_netif_dhcps_stop(s_ap_netif);
    if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return ret;
    }

    ret = esp_netif_set_ip_info(s_ap_netif, &ip_info);
    if (ret != ESP_OK) {
        return ret;
    }

    return esp_netif_dhcps_start(s_ap_netif);
}

static bool sta_credential_configured(const gateway_wifi_credential_t *credential)
{
    return credential != NULL && credential->ssid != NULL && credential->ssid[0] != '\0';
}

static const gateway_wifi_credential_t *current_sta_credential(void)
{
    const gateway_runtime_config_t *config = gateway_config_get();

    if (config->sta_credentials == NULL ||
        config->sta_credentials_count == 0U ||
        s_sta_credential_index >= config->sta_credentials_count) {
        return NULL;
    }

    const gateway_wifi_credential_t *credential =
        &config->sta_credentials[s_sta_credential_index];
    return sta_credential_configured(credential) ? credential : NULL;
}

static esp_err_t configure_sta_credential(size_t start_index)
{
    const gateway_runtime_config_t *config = gateway_config_get();

    if (config->sta_credentials == NULL || config->sta_credentials_count == 0U) {
        return ESP_ERR_NOT_FOUND;
    }

    for (size_t offset = 0; offset < config->sta_credentials_count; offset++) {
        const size_t index = (start_index + offset) % config->sta_credentials_count;
        const gateway_wifi_credential_t *credential = &config->sta_credentials[index];

        if (!sta_credential_configured(credential)) {
            continue;
        }

        wifi_config_t sta_config = {0};
        strlcpy((char *)sta_config.sta.ssid,
                credential->ssid,
                sizeof(sta_config.sta.ssid));
        strlcpy((char *)sta_config.sta.password,
                credential->password != NULL ? credential->password : "",
                sizeof(sta_config.sta.password));
        sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

        esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        if (ret != ESP_OK) {
            return ret;
        }

        s_sta_credential_index = index;
        ESP_LOGI(TAG,
                 "STA credential selected index=%u ssid=%s",
                 (unsigned int)index,
                 credential->ssid);
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t configure_next_sta_credential(void)
{
    const gateway_runtime_config_t *config = gateway_config_get();
    size_t next_index = s_sta_credential_index + 1U;

    if (config->sta_credentials_count > 0U && next_index >= config->sta_credentials_count) {
        next_index = 0U;
    }

    return configure_sta_credential(next_index);
}

static void connect_current_sta(void)
{
    const gateway_wifi_credential_t *credential = current_sta_credential();
    if (credential == NULL) {
        ESP_LOGW(TAG, "STA connect skipped: no configured credential");
        return;
    }

    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "STA connect request failed ssid=%s err=%s",
                 credential->ssid,
                 esp_err_to_name(ret));
    }
}

static void reconnect_next_sta(void)
{
    esp_err_t ret = configure_next_sta_credential();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "STA reconnect skipped: %s", esp_err_to_name(ret));
        return;
    }

    connect_current_sta();
}

static void handle_sta_start(void)
{
    if (gateway_config_sta_credentials_configured()) {
        connect_current_sta();
    }
}

static void handle_sta_disconnected(void)
{
    s_sta_connected = false;

    if (gateway_config_sta_credentials_configured()) {
        ESP_LOGW(TAG, "STA disconnected, trying next configured WiFi");
        reconnect_next_sta();
    }
}

esp_err_t gateway_wifi_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ensure_nvs(), TAG, "nvs init failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");

    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_ap_netif == NULL || s_sta_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(set_softap_ip(), TAG, "set SoftAP IP failed");

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            wifi_event_handler,
                                                            NULL,
                                                            NULL),
                        TAG,
                        "register wifi handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            wifi_event_handler,
                                                            NULL,
                                                            NULL),
                        TAG,
                        "register ip handler failed");

    const gateway_runtime_config_t *config = gateway_config_get();
    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, config->softap_ssid, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password,
            config->softap_password,
            sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(config->softap_ssid);
    ap_config.ap.channel = config->softap_channel;
    ap_config.ap.max_connection = config->softap_max_connection;
    ap_config.ap.authmode = strlen(config->softap_password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap_config.ap.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set APSTA mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "set AP config failed");

    if (gateway_config_sta_credentials_configured()) {
        ESP_RETURN_ON_ERROR(configure_sta_credential(0U),
                            TAG,
                            "set STA config failed");
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    s_started = true;
    ESP_LOGI(TAG,
             "APSTA initialized softap_ssid=%s softap_ip=%s sta_configured=%d",
             config->softap_ssid,
             config->softap_ip,
             gateway_config_sta_credentials_configured() ? 1 : 0);
    return ESP_OK;
}

bool gateway_wifi_is_softap_ready(void)
{
    return s_softap_ready;
}

bool gateway_wifi_is_sta_connected(void)
{
    return s_sta_connected;
}

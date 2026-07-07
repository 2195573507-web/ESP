/**
 * @file gateway_wifi.c
 * @brief S3 网关 SoftAP + STA 网络初始化。
 *
 * 本文件属于 ESPS3 网关，负责创建 C5 连接的 SoftAP，并在 STA 凭据存在时连接上游网络。
 * 它不处理 /local/v1 HTTP、不维护 child registry、不访问 ESP-server 业务接口。
 */

#include "gateway_wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "gateway_config.h"
#include "lwip/ip4_addr.h"
#include "network_worker.h"
#include "nvs_flash.h"

static const char *TAG = "gateway_wifi";

volatile bool g_net_ready = false;

static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static bool s_started;
static bool s_softap_ready;
static bool s_sta_started;
static bool s_sta_connected;
static uint8_t s_ap_sta_connected_count;
static size_t s_sta_credential_index;

void gateway_wifi_set_net_ready_gate(bool ready, const char *reason)
{
    if (g_net_ready == ready) {
        return;
    }

    g_net_ready = ready;
    ESP_LOGI(TAG,
             "NET_READY transition ready=%d reason=%s softap=%d sta=%d",
             g_net_ready ? 1 : 0,
             reason != NULL ? reason : "unknown",
             s_softap_ready ? 1 : 0,
             s_sta_connected ? 1 : 0);
}

static void post_network_event(network_worker_event_t event,
                               network_worker_event_source_t source,
                               uint32_t ip_addr)
{
    esp_err_t ret = network_worker_post_event(event, source, ip_addr);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "network event post failed event=%d source=%d ret=%s",
                 (int)event,
                 (int)source,
                 esp_err_to_name(ret));
    }
}

static void log_ap_station_connected(const void *event_data)
{
    const gateway_runtime_config_t *config = gateway_config_get();
    uint8_t max_connection = config->softap_max_connection;

    if (event_data == NULL) {
        ESP_LOGW(TAG,
                 "SoftAP station connected event missing data count=%u/%u",
                 (unsigned int)s_ap_sta_connected_count,
                 (unsigned int)max_connection);
        return;
    }

    const wifi_event_ap_staconnected_t *event =
        (const wifi_event_ap_staconnected_t *)event_data;
    if (max_connection == 0U || event->aid == 0U || event->aid > max_connection) {
        ESP_LOGW(TAG,
                 "SoftAP station connected boundary aid=%u max=%u",
                 (unsigned int)event->aid,
                 (unsigned int)max_connection);
    }
    if (max_connection > 0U && s_ap_sta_connected_count >= max_connection) {
        ESP_LOGW(TAG,
                 "SoftAP station connected over capacity count=%u max=%u",
                 (unsigned int)s_ap_sta_connected_count,
                 (unsigned int)max_connection);
    } else if (s_ap_sta_connected_count < UINT8_MAX) {
        ++s_ap_sta_connected_count;
    }

    ESP_LOGI(TAG,
             "SoftAP station connected mac=%02x:%02x:%02x:%02x:%02x:%02x aid=%u mesh=%d count=%u/%u",
             event->mac[0],
             event->mac[1],
             event->mac[2],
             event->mac[3],
             event->mac[4],
             event->mac[5],
             (unsigned int)event->aid,
             event->is_mesh_child ? 1 : 0,
             (unsigned int)s_ap_sta_connected_count,
             (unsigned int)max_connection);
}

static void log_ap_station_disconnected(const void *event_data)
{
    const gateway_runtime_config_t *config = gateway_config_get();
    uint8_t max_connection = config->softap_max_connection;

    if (event_data == NULL) {
        ESP_LOGW(TAG,
                 "SoftAP station disconnected event missing data count=%u/%u",
                 (unsigned int)s_ap_sta_connected_count,
                 (unsigned int)max_connection);
        return;
    }

    const wifi_event_ap_stadisconnected_t *event =
        (const wifi_event_ap_stadisconnected_t *)event_data;
    if (s_ap_sta_connected_count == 0U) {
        ESP_LOGW(TAG,
                 "SoftAP station disconnect underflow aid=%u reason=%u",
                 (unsigned int)event->aid,
                 (unsigned int)event->reason);
    } else {
        --s_ap_sta_connected_count;
    }
    if (max_connection == 0U || event->aid == 0U || event->aid > max_connection) {
        ESP_LOGW(TAG,
                 "SoftAP station disconnected boundary aid=%u max=%u reason=%u",
                 (unsigned int)event->aid,
                 (unsigned int)max_connection,
                 (unsigned int)event->reason);
    }

    ESP_LOGI(TAG,
             "SoftAP station disconnected mac=%02x:%02x:%02x:%02x:%02x:%02x aid=%u mesh=%d reason=%u count=%u/%u",
             event->mac[0],
             event->mac[1],
             event->mac[2],
             event->mac[3],
             event->mac[4],
             event->mac[5],
             (unsigned int)event->aid,
             event->is_mesh_child ? 1 : 0,
             (unsigned int)event->reason,
             (unsigned int)s_ap_sta_connected_count,
             (unsigned int)max_connection);
}

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
        post_network_event(NETWORK_WORKER_EVENT_LINK_UP,
                           NETWORK_WORKER_SOURCE_SOFTAP_START,
                           0U);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        s_softap_ready = false;
        s_ap_sta_connected_count = 0U;
        post_network_event(NETWORK_WORKER_EVENT_LINK_DOWN,
                           NETWORK_WORKER_SOURCE_SOFTAP_STOP,
                           0U);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        log_ap_station_connected(event_data);
        post_network_event(NETWORK_WORKER_EVENT_LINK_UP,
                           NETWORK_WORKER_SOURCE_AP_STA_CONNECTED,
                           0U);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        log_ap_station_disconnected(event_data);
        post_network_event(NETWORK_WORKER_EVENT_LINK_DOWN,
                           NETWORK_WORKER_SOURCE_AP_STA_DISCONNECTED,
                           0U);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_sta_started = true;
        post_network_event(NETWORK_WORKER_EVENT_LINK_UP,
                           NETWORK_WORKER_SOURCE_STA_START,
                           0U);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        post_network_event(NETWORK_WORKER_EVENT_LINK_DOWN,
                           NETWORK_WORKER_SOURCE_STA_DISCONNECTED,
                           0U);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
        s_sta_started = false;
        s_sta_connected = false;
        post_network_event(NETWORK_WORKER_EVENT_LINK_DOWN,
                           NETWORK_WORKER_SOURCE_STA_STOP,
                           0U);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_sta_connected = true;
        post_network_event(NETWORK_WORKER_EVENT_IP_READY,
                           NETWORK_WORKER_SOURCE_STA_GOT_IP,
                           event != NULL ? event->ip_info.ip.addr : 0U);
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

esp_err_t gateway_wifi_connect_sta_current(void)
{
    const gateway_wifi_credential_t *credential = current_sta_credential();
    if (credential == NULL) {
        ESP_LOGW(TAG, "STA connect skipped: no configured credential");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "STA connect request failed ssid=%s err=%s",
                 credential->ssid,
                 esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t gateway_wifi_reconnect_sta_next(void)
{
    esp_err_t ret = configure_next_sta_credential();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "STA reconnect skipped: %s", esp_err_to_name(ret));
        return ret;
    }

    return gateway_wifi_connect_sta_current();
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
    if (ap_config.ap.max_connection > GATEWAY_CONFIG_MAX_CHILDREN) {
        ESP_LOGW(TAG,
                 "SoftAP max_connection=%u exceeds registry max_children=%u",
                 (unsigned int)ap_config.ap.max_connection,
                 (unsigned int)GATEWAY_CONFIG_MAX_CHILDREN);
    }
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

bool gateway_wifi_is_sta_started(void)
{
    return s_sta_started;
}

bool gateway_wifi_is_sta_connected(void)
{
    return s_sta_connected;
}

bool gateway_wifi_is_net_ready(void)
{
    return g_net_ready;
}

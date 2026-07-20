/**
 * @file gateway_wifi.c
 * @brief S3 网关 SoftAP + STA 网络初始化。
 *
 * 本文件属于 ESPS3 网关，负责创建 C5 连接的 SoftAP，并在 STA 凭据存在时连接上游网络。
 * 它不处理 /local/v1 HTTP、不维护 child registry、不访问 ESP-server 业务接口。
 */

#include "gateway_wifi.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_ap_get_sta_list.h"
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
static bool s_sta_got_ip;
static volatile uint32_t s_sta_network_epoch;
static uint8_t s_ap_sta_connected_count;
static size_t s_sta_credential_index;
static uint8_t *s_sta_credential_failures;
static size_t s_sta_credential_failure_slots;
static uint8_t s_sta_untracked_failure_count;

typedef struct {
    wifi_ap_record_t ap;
    size_t credential_index;
} sta_scan_candidate_t;

static sta_scan_candidate_t *s_sta_scan_candidates;
static size_t s_sta_scan_candidate_slots;
static size_t s_sta_scan_candidate_count;
static sta_scan_candidate_t s_sta_selected_candidate;
static bool s_sta_selected_candidate_valid;

static bool station_mac_is_valid(const uint8_t mac[6])
{
    if (mac == NULL || (mac[0] & 0x01U) != 0U) {
        return false;
    }

    bool any_nonzero = false;
    for (size_t i = 0; i < 6U; ++i) {
        any_nonzero = any_nonzero || mac[i] != 0U;
    }
    return any_nonzero;
}

void gateway_wifi_set_net_ready_gate(bool ready, const char *reason)
{
    if (g_net_ready == ready) {
        return;
    }

    g_net_ready = ready;
    ESP_LOGI(TAG,
             "LOCAL_INGEST_READY transition ready=%d reason=%s softap=%d sta_started=%d sta_connected=%d sta_got_ip=%d",
             g_net_ready ? 1 : 0,
             reason != NULL ? reason : "unknown",
             s_softap_ready ? 1 : 0,
             s_sta_started ? 1 : 0,
             s_sta_connected ? 1 : 0,
             s_sta_got_ip ? 1 : 0);
}

static void post_network_event(network_worker_event_t event,
                               network_worker_event_source_t source,
                               uint32_t ip_addr,
                               uint8_t disconnect_reason)
{
    esp_err_t ret = network_worker_post_event(event, source, ip_addr, disconnect_reason);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "network event post failed event=%d source=%d ret=%s",
                 (int)event,
                 (int)source,
                 esp_err_to_name(ret));
    }
}

static void post_ap_station_event(network_worker_event_t event,
                                  network_worker_event_source_t source,
                                  const uint8_t mac[6],
                                  uint8_t aid)
{
    esp_err_t ret = network_worker_post_ap_station_event(event, source, 0U, mac, aid);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "AP station event post failed event=%d source=%d aid=%u ret=%s",
                 (int)event,
                 (int)source,
                 (unsigned int)aid,
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
                           0U,
                           0U);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        s_softap_ready = false;
        s_ap_sta_connected_count = 0U;
        post_network_event(NETWORK_WORKER_EVENT_LINK_DOWN,
                           NETWORK_WORKER_SOURCE_SOFTAP_STOP,
                           0U,
                           0U);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        log_ap_station_connected(event_data);
        if (event_data != NULL) {
            const wifi_event_ap_staconnected_t *event =
                (const wifi_event_ap_staconnected_t *)event_data;
            post_ap_station_event(NETWORK_WORKER_EVENT_LINK_UP,
                                  NETWORK_WORKER_SOURCE_AP_STA_CONNECTED,
                                  event->mac,
                                  event->aid);
        } else {
            post_network_event(NETWORK_WORKER_EVENT_LINK_UP,
                               NETWORK_WORKER_SOURCE_AP_STA_CONNECTED,
                               0U,
                               0U);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        log_ap_station_disconnected(event_data);
        if (event_data != NULL) {
            const wifi_event_ap_stadisconnected_t *event =
                (const wifi_event_ap_stadisconnected_t *)event_data;
            post_ap_station_event(NETWORK_WORKER_EVENT_LINK_DOWN,
                                  NETWORK_WORKER_SOURCE_AP_STA_DISCONNECTED,
                                  event->mac,
                                  event->aid);
        } else {
            post_network_event(NETWORK_WORKER_EVENT_LINK_DOWN,
                               NETWORK_WORKER_SOURCE_AP_STA_DISCONNECTED,
                               0U,
                               0U);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_sta_started = true;
        s_sta_connected = false;
        s_sta_got_ip = false;
        post_network_event(NETWORK_WORKER_EVENT_LINK_UP,
                           NETWORK_WORKER_SOURCE_STA_START,
                           0U,
                           0U);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        s_sta_connected = true;
        s_sta_got_ip = false;
        post_network_event(NETWORK_WORKER_EVENT_LINK_UP,
                           NETWORK_WORKER_SOURCE_STA_CONNECTED,
                           0U,
                           0U);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)event_data;
        s_sta_connected = false;
        s_sta_got_ip = false;
        post_network_event(NETWORK_WORKER_EVENT_LINK_DOWN,
                           NETWORK_WORKER_SOURCE_STA_DISCONNECTED,
                           0U,
                           event != NULL ? event->reason : WIFI_REASON_UNSPECIFIED);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        post_network_event(NETWORK_WORKER_EVENT_SCAN_DONE,
                           NETWORK_WORKER_SOURCE_STA_SCAN_DONE,
                           0U,
                           0U);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
        s_sta_started = false;
        s_sta_connected = false;
        s_sta_got_ip = false;
        post_network_event(NETWORK_WORKER_EVENT_LINK_DOWN,
                           NETWORK_WORKER_SOURCE_STA_STOP,
                           0U,
                           0U);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_sta_connected = true;
        s_sta_got_ip = true;
        ++s_sta_network_epoch;
        if (s_sta_network_epoch == 0U) {
            s_sta_network_epoch = 1U;
        }
        post_network_event(NETWORK_WORKER_EVENT_IP_READY,
                           NETWORK_WORKER_SOURCE_STA_GOT_IP,
                           event != NULL ? event->ip_info.ip.addr : 0U,
                           0U);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        s_sta_got_ip = false;
        post_network_event(NETWORK_WORKER_EVENT_LINK_DOWN,
                           NETWORK_WORKER_SOURCE_STA_LOST_IP,
                           0U,
                           0U);
    }
}

bool gateway_wifi_get_ap_client_mac(const char *peer_ip, uint8_t out_mac[6])
{
    if (out_mac == NULL) {
        return false;
    }
    memset(out_mac, 0, 6U);
    if (peer_ip == NULL || peer_ip[0] == '\0' || s_ap_netif == NULL || !s_softap_ready) {
        return false;
    }

    ip4_addr_t target_ip = {0};
    if (!ip4addr_aton(peer_ip, &target_ip)) {
        return false;
    }

    wifi_sta_list_t station_list = {0};
    if (esp_wifi_ap_get_sta_list(&station_list) != ESP_OK || station_list.num <= 0) {
        return false;
    }

    wifi_sta_mac_ip_list_t client_list = {0};
    if (esp_wifi_ap_get_sta_list_with_ip(&station_list, &client_list) != ESP_OK) {
        return false;
    }

    for (int i = 0; i < client_list.num; ++i) {
        if (client_list.sta[i].ip.addr == target_ip.addr &&
            station_mac_is_valid(client_list.sta[i].mac)) {
            memcpy(out_mac, client_list.sta[i].mac, 6U);
            return true;
        }
    }
    return false;
}

bool gateway_wifi_get_ap_client_ip(const uint8_t mac[6], char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return false;
    }
    out[0] = '\0';
    if (!station_mac_is_valid(mac) || s_ap_netif == NULL || !s_softap_ready) {
        return false;
    }

    esp_netif_pair_mac_ip_t client = {0};
    memcpy(client.mac, mac, sizeof(client.mac));
    if (esp_netif_dhcps_get_clients_by_mac(s_ap_netif, 1, &client) != ESP_OK ||
        client.ip.addr == 0U) {
        return false;
    }

    return esp_ip4addr_ntoa(&client.ip, out, (int)out_size) != NULL;
}

esp_err_t gateway_wifi_get_ap_station_count(size_t *out_count)
{
    if (out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0U;
    if (!s_softap_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_sta_list_t station_list = {0};
    esp_err_t ret = esp_wifi_ap_get_sta_list(&station_list);
    if (ret != ESP_OK) {
        return ret;
    }

    *out_count = (size_t)station_list.num;
    return ESP_OK;
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

static bool ap_ssid_matches_credential(const wifi_ap_record_t *ap,
                                       const gateway_wifi_credential_t *credential)
{
    if (ap == NULL || !sta_credential_configured(credential)) {
        return false;
    }

    const size_t ssid_len = strnlen((const char *)ap->ssid, sizeof(ap->ssid));
    const size_t credential_len = strlen(credential->ssid);
    return ssid_len == credential_len &&
           memcmp(ap->ssid, credential->ssid, ssid_len) == 0;
}

static void format_bssid(const uint8_t bssid[6], char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    if (bssid == NULL) {
        strlcpy(out, "<unknown>", out_size);
        return;
    }
    snprintf(out,
             out_size,
             "%02x:%02x:%02x:%02x:%02x:%02x",
             bssid[0],
             bssid[1],
             bssid[2],
             bssid[3],
             bssid[4],
             bssid[5]);
}

static int find_scan_candidate_for_credential(size_t credential_index)
{
    for (size_t i = 0; i < s_sta_scan_candidate_count; ++i) {
        if (s_sta_scan_candidates[i].credential_index == credential_index) {
            return (int)i;
        }
    }
    return -1;
}

static int compare_candidates_by_rssi(const void *left, const void *right)
{
    const sta_scan_candidate_t *a = (const sta_scan_candidate_t *)left;
    const sta_scan_candidate_t *b = (const sta_scan_candidate_t *)right;
    return (int)b->ap.rssi - (int)a->ap.rssi;
}

static esp_err_t configure_sta_candidate(const sta_scan_candidate_t *candidate)
{
    if (candidate == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const gateway_runtime_config_t *config = gateway_config_get();
    if (candidate->credential_index >= config->sta_credentials_count) {
        return ESP_ERR_INVALID_ARG;
    }
    const gateway_wifi_credential_t *credential =
        &config->sta_credentials[candidate->credential_index];
    if (!sta_credential_configured(credential)) {
        return ESP_ERR_NOT_FOUND;
    }

    /* A candidate change never restarts APSTA. Disconnect only if STA is still associated. */
    if (s_sta_connected) {
        esp_err_t disconnect_ret = esp_wifi_disconnect();
        if (disconnect_ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "STA disconnect before candidate change failed ret=%s",
                     esp_err_to_name(disconnect_ret));
            return disconnect_ret;
        }
    }

    wifi_config_t sta_config = {0};
    memcpy(sta_config.sta.ssid, candidate->ap.ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password,
            credential->password != NULL ? credential->password : "",
            sizeof(sta_config.sta.password));
    memcpy(sta_config.sta.bssid, candidate->ap.bssid, sizeof(sta_config.sta.bssid));
    sta_config.sta.bssid_set = true;
    sta_config.sta.channel = candidate->ap.primary;
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (ret != ESP_OK) {
        return ret;
    }

    s_sta_credential_index = candidate->credential_index;
    s_sta_selected_candidate = *candidate;
    s_sta_selected_candidate_valid = true;
    return ESP_OK;
}

esp_err_t gateway_wifi_start_sta_scan(void)
{
    wifi_scan_config_t scan_config = {0};
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 40U;
    scan_config.scan_time.active.max = 100U;
    scan_config.home_chan_dwell_time = 30U;

    ESP_LOGI(TAG, "WIFI_SCAN_START mode=async dwell_ms=100");
    esp_err_t ret = esp_wifi_scan_start(&scan_config, false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WIFI_SCAN_START failed ret=%s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t gateway_wifi_collect_sta_scan_candidates(size_t *out_scan_count)
{
    if (out_scan_count != NULL) {
        *out_scan_count = 0U;
    }

    uint16_t ap_count = 0U;
    esp_err_t ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WIFI_SCAN_DONE count=0 ret=%s", esp_err_to_name(ret));
        return ret;
    }
    if (out_scan_count != NULL) {
        *out_scan_count = ap_count;
    }

    s_sta_scan_candidate_count = 0U;
    if (ap_count == 0U) {
        ESP_LOGI(TAG, "WIFI_SCAN_DONE count=0 known=0");
        return ESP_ERR_NOT_FOUND;
    }

    wifi_ap_record_t *records = calloc(ap_count, sizeof(*records));
    if (records == NULL) {
        ESP_LOGW(TAG, "WIFI_SCAN_DONE count=%u ret=ESP_ERR_NO_MEM", (unsigned int)ap_count);
        return ESP_ERR_NO_MEM;
    }

    uint16_t record_count = ap_count;
    ret = esp_wifi_scan_get_ap_records(&record_count, records);
    if (ret != ESP_OK) {
        free(records);
        ESP_LOGW(TAG, "WIFI_SCAN_DONE count=%u ret=%s", (unsigned int)ap_count, esp_err_to_name(ret));
        return ret;
    }

    const gateway_runtime_config_t *config = gateway_config_get();
    for (uint16_t record_index = 0U; record_index < record_count; ++record_index) {
        const wifi_ap_record_t *ap = &records[record_index];
        size_t credential_index = config->sta_credentials_count;
        for (size_t index = 0U; index < config->sta_credentials_count; ++index) {
            if (ap_ssid_matches_credential(ap, &config->sta_credentials[index])) {
                credential_index = index;
                break;
            }
        }

        const size_t ssid_len = strnlen((const char *)ap->ssid, sizeof(ap->ssid));
        char bssid[18] = {0};
        format_bssid(ap->bssid, bssid, sizeof(bssid));
        ESP_LOGI(TAG,
                 "WIFI_SCAN_RESULT ssid=%.*s rssi=%d bssid=%s channel=%u authmode=%u known=%d",
                 (int)ssid_len,
                 (const char *)ap->ssid,
                 (int)ap->rssi,
                 bssid,
                 (unsigned int)ap->primary,
                 (unsigned int)ap->authmode,
                 credential_index < config->sta_credentials_count ? 1 : 0);
        if (credential_index >= config->sta_credentials_count ||
            s_sta_scan_candidates == NULL) {
            continue;
        }

        const int candidate_index = find_scan_candidate_for_credential(credential_index);
        if (candidate_index >= 0) {
            if (ap->rssi > s_sta_scan_candidates[candidate_index].ap.rssi) {
                s_sta_scan_candidates[candidate_index].ap = *ap;
            }
        } else if (s_sta_scan_candidate_count < s_sta_scan_candidate_slots) {
            s_sta_scan_candidates[s_sta_scan_candidate_count++] =
                (sta_scan_candidate_t){.ap = *ap, .credential_index = credential_index};
        }
    }
    free(records);

    if (s_sta_scan_candidate_count > 1U) {
        qsort(s_sta_scan_candidates,
              s_sta_scan_candidate_count,
              sizeof(*s_sta_scan_candidates),
              compare_candidates_by_rssi);
    }
    for (size_t index = 0U; index < s_sta_scan_candidate_count; ++index) {
        const sta_scan_candidate_t *candidate = &s_sta_scan_candidates[index];
        const size_t ssid_len = strnlen((const char *)candidate->ap.ssid,
                                        sizeof(candidate->ap.ssid));
        char bssid[18] = {0};
        format_bssid(candidate->ap.bssid, bssid, sizeof(bssid));
        ESP_LOGI(TAG,
                 "WIFI_KNOWN_AP_LIST rank=%u index=%u ssid=%.*s rssi=%d bssid=%s channel=%u authmode=%u",
                 (unsigned int)index,
                 (unsigned int)candidate->credential_index,
                 (int)ssid_len,
                 (const char *)candidate->ap.ssid,
                 (int)candidate->ap.rssi,
                 bssid,
                 (unsigned int)candidate->ap.primary,
                 (unsigned int)candidate->ap.authmode);
    }
    ESP_LOGI(TAG,
             "WIFI_SCAN_DONE count=%u known=%u",
             (unsigned int)record_count,
             (unsigned int)s_sta_scan_candidate_count);
    return s_sta_scan_candidate_count > 0U ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t gateway_wifi_select_sta_candidate(bool avoid_current)
{
    if (s_sta_scan_candidate_count == 0U || s_sta_scan_candidates == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t selected_index = 0U;
    if (avoid_current && s_sta_selected_candidate_valid) {
        for (size_t index = 0U; index < s_sta_scan_candidate_count; ++index) {
            const sta_scan_candidate_t *candidate = &s_sta_scan_candidates[index];
            if (candidate->credential_index != s_sta_selected_candidate.credential_index ||
                memcmp(candidate->ap.bssid,
                       s_sta_selected_candidate.ap.bssid,
                       sizeof(candidate->ap.bssid)) != 0) {
                selected_index = index;
                break;
            }
        }
    }

    const sta_scan_candidate_t *candidate = &s_sta_scan_candidates[selected_index];
    esp_err_t ret = configure_sta_candidate(candidate);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WIFI_CANDIDATE_SELECTED failed ret=%s", esp_err_to_name(ret));
        return ret;
    }

    const size_t ssid_len = strnlen((const char *)candidate->ap.ssid, sizeof(candidate->ap.ssid));
    char bssid[18] = {0};
    format_bssid(candidate->ap.bssid, bssid, sizeof(bssid));
    ESP_LOGI(TAG,
             "WIFI_CANDIDATE_SELECTED ssid=%.*s rssi=%d index=%u bssid=%s",
             (int)ssid_len,
             (const char *)candidate->ap.ssid,
             (int)candidate->ap.rssi,
             (unsigned int)candidate->credential_index,
             bssid);
    ESP_LOGI(TAG,
             "WIFI_SELECTED_AP ssid=%.*s rssi=%d bssid=%s channel=%u authmode=%u",
             (int)ssid_len,
             (const char *)candidate->ap.ssid,
             (int)candidate->ap.rssi,
             bssid,
             (unsigned int)candidate->ap.primary,
             (unsigned int)candidate->ap.authmode);
    return ESP_OK;
}

esp_err_t gateway_wifi_cancel_sta_scan(void)
{
    return esp_wifi_scan_stop();
}

bool gateway_wifi_has_sta_candidate(void)
{
    return s_sta_selected_candidate_valid;
}

void gateway_wifi_log_sta_candidate_fallback(const char *reason)
{
    if (!s_sta_selected_candidate_valid) {
        ESP_LOGW(TAG,
                 "WIFI_CANDIDATE_FALLBACK ssid=<none> reason=%s",
                 reason != NULL ? reason : "connect_failed");
        return;
    }
    const size_t ssid_len = strnlen((const char *)s_sta_selected_candidate.ap.ssid,
                                    sizeof(s_sta_selected_candidate.ap.ssid));
    ESP_LOGW(TAG,
             "WIFI_CANDIDATE_FALLBACK ssid=%.*s reason=%s",
             (int)ssid_len,
             (const char *)s_sta_selected_candidate.ap.ssid,
             reason != NULL ? reason : "connect_failed");
}

esp_err_t gateway_wifi_connect_sta_current(void)
{
    const gateway_wifi_credential_t *credential = current_sta_credential();
    if (credential == NULL || !s_sta_selected_candidate_valid) {
        ESP_LOGW(TAG, "STA connect skipped: no selected scan candidate");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = esp_wifi_connect();
    const size_t ssid_len = strnlen((const char *)s_sta_selected_candidate.ap.ssid,
                                    sizeof(s_sta_selected_candidate.ap.ssid));
    char bssid[18] = {0};
    format_bssid(s_sta_selected_candidate.ap.bssid, bssid, sizeof(bssid));
    ESP_LOGI(TAG,
             "WIFI_CONNECT_RESULT ssid=%.*s bssid=%s status=request ret=%s",
             (int)ssid_len,
             (const char *)s_sta_selected_candidate.ap.ssid,
             bssid,
             esp_err_to_name(ret));
    return ret;
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
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_LOST_IP,
                                                            wifi_event_handler,
                                                            NULL,
                                                            NULL),
                        TAG,
                        "register lost ip handler failed");

    const gateway_runtime_config_t *config = gateway_config_get();
    if (config->sta_credentials_count > 0U && s_sta_credential_failures == NULL) {
        s_sta_credential_failures = calloc(config->sta_credentials_count,
                                           sizeof(*s_sta_credential_failures));
        if (s_sta_credential_failures == NULL) {
            ESP_LOGW(TAG,
                     "STA credential failure budget unavailable candidates=%u",
                     (unsigned int)config->sta_credentials_count);
        } else {
            s_sta_credential_failure_slots = config->sta_credentials_count;
        }
    }
    if (config->sta_credentials_count > 0U && s_sta_scan_candidates == NULL) {
        s_sta_scan_candidates = calloc(config->sta_credentials_count,
                                       sizeof(*s_sta_scan_candidates));
        if (s_sta_scan_candidates == NULL) {
            ESP_LOGW(TAG,
                     "STA scan candidate storage unavailable credentials=%u",
                     (unsigned int)config->sta_credentials_count);
        } else {
            s_sta_scan_candidate_slots = config->sta_credentials_count;
        }
    }
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

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    s_started = true;
    ESP_LOGI(TAG,
             "APSTA initialized softap_ssid=%s softap_ip=%s sta_configured=%d sta_selection=scan",
             config->softap_ssid,
             config->softap_ip,
             gateway_config_sta_credentials_configured() ? 1 : 0);
    return ESP_OK;
}

bool gateway_wifi_is_softap_ready(void)
{
    return s_softap_ready;
}

bool gateway_wifi_is_local_ingest_ready(void)
{
    return g_net_ready;
}

bool gateway_wifi_is_sta_started(void)
{
    return s_sta_started;
}

bool gateway_wifi_is_sta_connected(void)
{
    return s_sta_connected;
}

bool gateway_wifi_is_sta_ip_ready(void)
{
    return s_sta_connected && s_sta_got_ip && s_sta_network_epoch != 0U;
}

uint32_t gateway_wifi_get_sta_network_epoch(void)
{
    return s_sta_network_epoch;
}

size_t gateway_wifi_get_sta_credential_index(void)
{
    return s_sta_credential_index;
}

uint32_t gateway_wifi_record_sta_credential_failure(void)
{
    if (s_sta_credential_failures == NULL ||
        s_sta_credential_index >= s_sta_credential_failure_slots) {
        if (s_sta_untracked_failure_count < UINT8_MAX) {
            ++s_sta_untracked_failure_count;
        }
        return s_sta_untracked_failure_count;
    }

    uint8_t *count = &s_sta_credential_failures[s_sta_credential_index];
    if (*count < UINT8_MAX) {
        ++(*count);
    }
    return *count;
}

void gateway_wifi_reset_sta_credential_failures(void)
{
    s_sta_untracked_failure_count = 0U;
    if (s_sta_credential_failures != NULL && s_sta_credential_failure_slots > 0U) {
        memset(s_sta_credential_failures, 0, s_sta_credential_failure_slots);
    }
}

bool gateway_wifi_is_net_ready(void)
{
    return gateway_wifi_is_local_ingest_ready();
}

/**
 * @file wifi_manager.c
 * @brief C5 终端连接 S3 SoftAP 的 WiFi 状态机。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），负责 STA 模式初始化、连接
 * terminal_config 指定的 ESPS3 SoftAP、断线重连和稳定性判断。本文件不扫描家庭 WiFi、
 * 不连接公网 Server，也不处理 /local/v1 HTTP；HTTP 请求由 server_comm_http 发起。
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "gateway_link.h"
#include "nvs_flash.h"
#include "terminal_config.h"
#include "wifi_manager.h"

static const char *TAG = "wifi_manager";

static EventGroupHandle_t s_wifi_event_group;
static TaskHandle_t s_wifi_reconnect_task_handle = NULL;
static TickType_t s_wifi_connected_tick;

enum {
    WIFI_CONNECTED_BIT = BIT0,
    WIFI_RECONNECT_BIT = BIT1,
    WIFI_DISCONNECTED_BIT = BIT2,
};

static esp_err_t connect_gateway_softap(void)
{
    const terminal_runtime_config_t *config = terminal_config_get();
    if (config->gateway_ssid[0] == '\0') {
        ESP_LOGE(TAG, "gateway SoftAP SSID is empty");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    if (config->gateway_password[0] == '\0') {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    strlcpy((char *)wifi_config.sta.ssid, config->gateway_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password,
            config->gateway_password,
            sizeof(wifi_config.sta.password));

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set gateway Wi-Fi config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT);
    ESP_LOGI(TAG, "Connecting to S3 gateway SoftAP: %s", config->gateway_ssid);

    ret = esp_wifi_connect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "Start gateway Wi-Fi connect failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

static void wifi_reconnect_task(void *arg)
{
    (void)arg;

    while (1) {
        xEventGroupWaitBits(s_wifi_event_group,
                            WIFI_RECONNECT_BIT,
                            pdTRUE,
                            pdFALSE,
                            portMAX_DELAY);

        while (!wifi_is_connected()) {
            xEventGroupClearBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);

            esp_err_t ret = connect_gateway_softap();
            if (ret != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(WIFI_RESCAN_DELAY_MS));
                continue;
            }

            EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                                   WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT,
                                                   pdFALSE,
                                                   pdFALSE,
                                                   pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

            if (bits & WIFI_CONNECTED_BIT) {
                ESP_LOGI(TAG, "S3 gateway SoftAP connected");
                break;
            }

            if (bits & WIFI_DISCONNECTED_BIT) {
                ESP_LOGI(TAG,
                         "S3 gateway SoftAP connection failed, retry in %d ms",
                         WIFI_RESCAN_DELAY_MS);
            } else {
                ESP_LOGI(TAG,
                         "S3 gateway SoftAP connection timeout, retry in %d ms",
                         WIFI_RESCAN_DELAY_MS);
                esp_wifi_disconnect();
            }

            vTaskDelay(pdMS_TO_TICKS(WIFI_RESCAN_DELAY_MS));
        }
    }
}

static void event_handler(void *arg,
                          esp_event_base_t event_base,
                          int32_t event_id,
                          void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi STA started for S3 gateway SoftAP");
        xEventGroupSetBits(s_wifi_event_group, WIFI_RECONNECT_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected =
            (const wifi_event_sta_disconnected_t *)event_data;
        int reason = disconnected != NULL ? disconnected->reason : -1;

        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_RECONNECT_BIT | WIFI_DISCONNECTED_BIT);
        s_wifi_connected_tick = 0;
        gateway_link_notify_wifi_down();
        ESP_LOGI(TAG, "S3 gateway SoftAP disconnected: reason=%d", reason);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Gateway SoftAP IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupClearBits(s_wifi_event_group, WIFI_RECONNECT_BIT | WIFI_DISCONNECTED_BIT);
        s_wifi_connected_tick = xTaskGetTickCount();
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        gateway_link_notify_wifi_got_ip();
    }
}

esp_err_t wifi_manager_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    (void)terminal_config_load();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Create Wi-Fi event group failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Wi-Fi manager initialized for gateway SoftAP-only STA");
    return ESP_OK;
}

esp_err_t wifi_connect_to_ap(void)
{
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_wifi_reconnect_task_handle == NULL) {
        BaseType_t task_created = xTaskCreate(wifi_reconnect_task,
                                             "wifi_reconnect",
                                             WIFI_RECONNECT_TASK_STACK,
                                             NULL,
                                             WIFI_RECONNECT_TASK_PRIORITY,
                                             &s_wifi_reconnect_task_handle);
        if (task_created != pdPASS) {
            s_wifi_reconnect_task_handle = NULL;
            ESP_LOGE(TAG, "Create Wi-Fi reconnect task failed");
            return ESP_ERR_NO_MEM;
        }
    }

    xEventGroupSetBits(s_wifi_event_group, WIFI_RECONNECT_BIT);
    ESP_LOGI(TAG, "Waiting for S3 gateway SoftAP connection");

    xEventGroupWaitBits(s_wifi_event_group,
                        WIFI_CONNECTED_BIT,
                        pdFALSE,
                        pdTRUE,
                        portMAX_DELAY);

    return ESP_OK;
}

bool wifi_is_connected(void)
{
    if (s_wifi_event_group == NULL) {
        return false;
    }

    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_is_stable(void)
{
    if (!wifi_is_connected() || s_wifi_connected_tick == 0) {
        return false;
    }

    TickType_t connected_ticks = xTaskGetTickCount() - s_wifi_connected_tick;
    return connected_ticks >= pdMS_TO_TICKS(WIFI_STABLE_REQUIRED_MS);
}

bool wifi_get_connected_ssid(char *ssid, size_t ssid_len)
{
    if (ssid == NULL || ssid_len == 0 || !wifi_is_connected()) {
        return false;
    }

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return false;
    }

    strlcpy(ssid, (const char *)ap_info.ssid, ssid_len);
    return true;
}

/**
 * @file wifi_manager.c
 * @brief C5 终端连接 S3 SoftAP 的 WiFi 状态机。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），负责 STA 模式初始化、连接
 * terminal_config 指定的 ESPS3 SoftAP、断线重连和稳定性判断。本文件不扫描家庭 WiFi、
 * 不连接公网 Server，也不处理 /local/v1 HTTP；HTTP 请求由 server_comm_http 发起。
 */

#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
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
static esp_netif_t *s_wifi_sta_netif;
static TickType_t s_wifi_connected_tick;
static TickType_t s_wifi_disconnected_tick;
static bool s_wifi_driver_ready;
static bool s_wifi_handlers_registered;

typedef enum {
    WIFI_MANAGER_STATE_UNINITIALIZED = 0,
    WIFI_MANAGER_STATE_INIT_RETRY_PENDING,
    WIFI_MANAGER_STATE_STARTED,
    WIFI_MANAGER_STATE_CONNECTING,
    WIFI_MANAGER_STATE_CONNECTED,
} wifi_manager_state_t;

static wifi_manager_state_t s_wifi_state = WIFI_MANAGER_STATE_UNINITIALIZED;

enum {
    WIFI_CONNECTED_BIT = BIT0,
    WIFI_RECONNECT_BIT = BIT1,
    WIFI_DISCONNECTED_BIT = BIT2,
};

static void event_handler(void *arg,
                          esp_event_base_t event_base,
                          int32_t event_id,
                          void *event_data);

static const char *wifi_manager_state_name(wifi_manager_state_t state)
{
    switch (state) {
    case WIFI_MANAGER_STATE_UNINITIALIZED: return "UNINITIALIZED";
    case WIFI_MANAGER_STATE_INIT_RETRY_PENDING: return "INIT_RETRY_PENDING";
    case WIFI_MANAGER_STATE_STARTED: return "STARTED";
    case WIFI_MANAGER_STATE_CONNECTING: return "CONNECTING";
    case WIFI_MANAGER_STATE_CONNECTED: return "CONNECTED";
    default: return "UNKNOWN";
    }
}

static void wifi_manager_set_state(wifi_manager_state_t next, const char *reason)
{
    const wifi_manager_state_t previous = s_wifi_state;
    s_wifi_state = next;
    if (previous != next) {
        ESP_LOGI(TAG,
                 "WIFI_STATE_CHANGE old=%s new=%s reason=%s",
                 wifi_manager_state_name(previous),
                 wifi_manager_state_name(next),
                 reason != NULL ? reason : "none");
    }
}

static void wifi_manager_log_memory(const char *stage)
{
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t dma_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

    ESP_LOGI(TAG,
             "WIFI_MEM stage=%s internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u "
             "psram_free=%u psram_largest=%u",
             stage != NULL ? stage : "none",
             (unsigned int)heap_caps_get_free_size(internal_caps),
             (unsigned int)heap_caps_get_largest_free_block(internal_caps),
             (unsigned int)heap_caps_get_free_size(dma_caps),
             (unsigned int)heap_caps_get_largest_free_block(dma_caps),
             (unsigned int)heap_caps_get_free_size(psram_caps),
             (unsigned int)heap_caps_get_largest_free_block(psram_caps));
}

static bool wifi_manager_nonfatal_init_result(esp_err_t ret)
{
    return ret == ESP_OK || ret == ESP_ERR_INVALID_STATE;
}

static esp_err_t wifi_manager_prepare_platform(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ret = nvs_flash_erase();
        if (ret == ESP_OK) {
            ret = nvs_flash_init();
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi NVS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_netif_init();
    if (!wifi_manager_nonfatal_init_result(ret)) {
        ESP_LOGE(TAG, "Wi-Fi netif init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (!wifi_manager_nonfatal_init_result(ret)) {
        ESP_LOGE(TAG, "Wi-Fi event loop init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (s_wifi_sta_netif == NULL) {
        s_wifi_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_wifi_sta_netif == NULL) {
            ESP_LOGE(TAG, "Create default Wi-Fi STA netif failed");
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static esp_err_t wifi_manager_start_driver(void)
{
    if (s_wifi_driver_ready) {
        return ESP_OK;
    }

    wifi_manager_log_memory("before_driver_init");
    esp_err_t ret = wifi_manager_prepare_platform();
    if (ret != ESP_OK) {
        wifi_manager_log_memory("platform_init_failed");
        return ret;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Wi-Fi driver init deferred: %s", esp_err_to_name(ret));
        wifi_manager_log_memory("driver_init_failed");
        return ret;
    }

    if (!s_wifi_handlers_registered) {
        ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
        const bool wifi_handler_registered = ret == ESP_OK;
        if (ret == ESP_OK) {
            ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL);
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Register Wi-Fi event handlers failed: %s", esp_err_to_name(ret));
            if (wifi_handler_registered) {
                (void)esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler);
            }
            (void)esp_wifi_deinit();
            return ret;
        }
        s_wifi_handlers_registered = true;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret == ESP_OK) {
        ret = esp_wifi_start();
    }
    if (ret == ESP_ERR_WIFI_INIT_STATE) {
        ret = ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = esp_wifi_set_ps(WIFI_PS_NONE);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi driver start deferred: %s", esp_err_to_name(ret));
        wifi_manager_log_memory("driver_start_failed");
        return ret;
    }

    s_wifi_driver_ready = true;
    wifi_manager_set_state(WIFI_MANAGER_STATE_STARTED, "driver_started");
    wifi_manager_log_memory("driver_started");
    ESP_LOGI(TAG, "Wi-Fi manager driver ready for gateway SoftAP-only STA");
    return ESP_OK;
}

static esp_err_t connect_gateway_softap(void)
{
    if (!s_wifi_driver_ready) {
        return ESP_ERR_INVALID_STATE;
    }

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
    wifi_manager_set_state(WIFI_MANAGER_STATE_CONNECTING, "connect_requested");
    ESP_LOGI(TAG, "Connecting to S3 gateway SoftAP: %s", config->gateway_ssid);

    ret = esp_wifi_connect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "Start gateway Wi-Fi connect failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

static uint32_t wifi_reconnect_backoff_ms(uint32_t failures)
{
    uint32_t delay_ms = WIFI_RECONNECT_BACKOFF_MIN_MS;
    if (failures > 0U) {
        delay_ms += failures * WIFI_RECONNECT_BACKOFF_STEP_MS;
    }
    if (delay_ms > WIFI_RECONNECT_BACKOFF_MAX_MS) {
        delay_ms = WIFI_RECONNECT_BACKOFF_MAX_MS;
    }
    return delay_ms;
}

static void wifi_reconnect_task(void *arg)
{
    (void)arg;
    uint32_t consecutive_failures = 0;

    while (1) {
        xEventGroupWaitBits(s_wifi_event_group,
                            WIFI_RECONNECT_BIT,
                            pdTRUE,
                            pdFALSE,
                            portMAX_DELAY);

        while (!wifi_is_connected()) {
            uint32_t backoff_ms = wifi_reconnect_backoff_ms(consecutive_failures);
            ESP_LOGI(TAG,
                     "WIFI_RETRY_START phase=%s failures=%u backoff_ms=%u",
                     s_wifi_driver_ready ? "connect" : "driver_init",
                     (unsigned int)consecutive_failures,
                     (unsigned int)backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            if (wifi_is_connected()) {
                break;
            }

            if (!s_wifi_driver_ready) {
                esp_err_t init_ret = wifi_manager_start_driver();
                if (init_ret != ESP_OK) {
                    wifi_manager_set_state(WIFI_MANAGER_STATE_INIT_RETRY_PENDING,
                                           "driver_init_failed");
                    if (consecutive_failures < UINT32_MAX) {
                        consecutive_failures++;
                    }
                    continue;
                }
            }

            xEventGroupClearBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);

            esp_err_t ret = connect_gateway_softap();
            if (ret != ESP_OK) {
                if (consecutive_failures < UINT32_MAX) {
                    consecutive_failures++;
                }
                continue;
            }

            EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                                   WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT,
                                                   pdFALSE,
                                                   pdFALSE,
                                                   pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

            if (bits & WIFI_CONNECTED_BIT) {
                ESP_LOGI(TAG, "S3 gateway SoftAP connected");
                consecutive_failures = 0;
                break;
            }

            if (consecutive_failures < UINT32_MAX) {
                consecutive_failures++;
            }
            if (bits & WIFI_DISCONNECTED_BIT) {
                ESP_LOGI(TAG,
                         "S3 gateway SoftAP connection failed, failures=%u",
                         (unsigned int)consecutive_failures);
            } else {
                ESP_LOGI(TAG,
                         "S3 gateway SoftAP connection timeout, failures=%u",
                         (unsigned int)consecutive_failures);
                esp_wifi_disconnect();
            }
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
        wifi_manager_set_state(WIFI_MANAGER_STATE_STARTED, "sta_started");
        ESP_LOGI(TAG, "WIFI_STATE_CHANGE event=STA_START action=reconnect");
        xEventGroupSetBits(s_wifi_event_group, WIFI_RECONNECT_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected =
            (const wifi_event_sta_disconnected_t *)event_data;
        int reason = disconnected != NULL ? disconnected->reason : -1;

        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_RECONNECT_BIT | WIFI_DISCONNECTED_BIT);
        s_wifi_connected_tick = 0;
        s_wifi_disconnected_tick = xTaskGetTickCount();
        gateway_link_notify_wifi_down();
        wifi_manager_set_state(WIFI_MANAGER_STATE_STARTED, "sta_disconnected");
        ESP_LOGW(TAG, "WIFI_LOST reason=%d", reason);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Gateway SoftAP IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupClearBits(s_wifi_event_group, WIFI_RECONNECT_BIT | WIFI_DISCONNECTED_BIT);
        s_wifi_connected_tick = xTaskGetTickCount();
        s_wifi_disconnected_tick = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        gateway_link_notify_wifi_got_ip();
        wifi_manager_set_state(WIFI_MANAGER_STATE_CONNECTED, "got_ip");
        ESP_LOGI(TAG, "WIFI_CONNECTED event=GOT_IP");
        ESP_LOGI(TAG, "WIFI_READY_FOR_VOICE gateway_link_reconnect_requested");
    }
}

esp_err_t wifi_manager_init(void)
{
    (void)terminal_config_load();

    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL) {
            ESP_LOGE(TAG, "Create Wi-Fi event group failed");
            return ESP_ERR_NO_MEM;
        }
    }

    const esp_err_t ret = wifi_manager_start_driver();
    if (ret != ESP_OK) {
        /* The reconnect task owns future driver-init attempts.  Returning OK here
         * keeps the local runtime and gateway recovery state machines alive. */
        wifi_manager_set_state(WIFI_MANAGER_STATE_INIT_RETRY_PENDING, "initial_driver_init_failed");
        ESP_LOGW(TAG,
                 "Wi-Fi manager initialized with deferred driver retry: %s",
                 esp_err_to_name(ret));
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Wi-Fi manager initialized for gateway SoftAP-only STA");
    return ESP_OK;
}

esp_err_t wifi_connect_to_ap(void)
{
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_wifi_reconnect_task_handle == NULL) {
        ESP_LOGI(TAG,
                 "MEM_ALLOC_PLAN owner=wifi_reconnect caps=internal_control size=%u",
                 (unsigned int)WIFI_RECONNECT_TASK_STACK);
        BaseType_t task_created = xTaskCreateWithCaps(wifi_reconnect_task,
                                                      "wifi_reconnect",
                                                      WIFI_RECONNECT_TASK_STACK,
                                                      NULL,
                                                      WIFI_RECONNECT_TASK_PRIORITY,
                                                      &s_wifi_reconnect_task_handle,
                                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (task_created != pdPASS) {
            s_wifi_reconnect_task_handle = NULL;
            ESP_LOGE(TAG, "Create Wi-Fi reconnect task failed");
            return ESP_ERR_NO_MEM;
        }
    }

    xEventGroupSetBits(s_wifi_event_group, WIFI_RECONNECT_BIT);
    ESP_LOGI(TAG, "Waiting for S3 gateway SoftAP connection");

    const EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                                 WIFI_CONNECTED_BIT,
                                                 pdFALSE,
                                                 pdTRUE,
                                                 pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if ((bits & WIFI_CONNECTED_BIT) == 0U) {
        ESP_LOGW(TAG, "S3 gateway SoftAP connection timed out; reconnect task remains active");
        return ESP_ERR_TIMEOUT;
    }
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

bool wifi_is_down_stable(void)
{
    if (wifi_is_connected() || s_wifi_disconnected_tick == 0) {
        return false;
    }

    TickType_t disconnected_ticks = xTaskGetTickCount() - s_wifi_disconnected_tick;
    return disconnected_ticks >= pdMS_TO_TICKS(WIFI_DOWN_STABLE_REQUIRED_MS);
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

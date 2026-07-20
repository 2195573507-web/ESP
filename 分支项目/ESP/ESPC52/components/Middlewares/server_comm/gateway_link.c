/**
 * @file gateway_link.c
 * @brief C5 终端到 ESPS3 本地网关的恢复优先级状态机。
 *
 * 本文件只处理 C5 <-> S3 链路健康、health probe、child register 和普通业务 gate。
 * C5 与 S3 断联时，继续语音/传感器/命令只会反复创建失败 socket 并挤占 S3 恢复窗口；
 * 所以 reconnect mode 只保留 WiFi reconnect、health probe、register 和低频日志。
 */

#include "gateway_link.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "server_comm_config.h"
#include "server_comm_errors.h"
#include "server_comm_http.h"
#include "terminal_config.h"

static const char *TAG = "gateway_link";

#define GATEWAY_LINK_READY_BIT BIT0
#define GATEWAY_LINK_RECONNECT_BIT BIT1

#ifndef GATEWAY_LINK_TASK_STACK
#define GATEWAY_LINK_TASK_STACK 8192U
#endif

#ifndef GATEWAY_LINK_TASK_PRIORITY
#define GATEWAY_LINK_TASK_PRIORITY 6U
#endif

#ifndef GATEWAY_LINK_HEALTH_TIMEOUT_MS
#define GATEWAY_LINK_HEALTH_TIMEOUT_MS 2000U
#endif

#ifndef GATEWAY_LINK_REGISTER_TIMEOUT_MS
#define GATEWAY_LINK_REGISTER_TIMEOUT_MS 5000U
#endif

#ifndef GATEWAY_LINK_FAILURE_THRESHOLD
#define GATEWAY_LINK_FAILURE_THRESHOLD 3U
#endif

#ifndef GATEWAY_LINK_RETRY_FAST_MS
#define GATEWAY_LINK_RETRY_FAST_MS 1000U
#endif

#ifndef GATEWAY_LINK_RETRY_STEP1_MS
#define GATEWAY_LINK_RETRY_STEP1_MS 3000U
#endif

#ifndef GATEWAY_LINK_RETRY_STEP2_MS
#define GATEWAY_LINK_RETRY_STEP2_MS 10000U
#endif

#ifndef GATEWAY_LINK_RETRY_MAX_MS
#define GATEWAY_LINK_RETRY_MAX_MS 30000U
#endif

#ifndef GATEWAY_LINK_WIFI_STABILITY_POLL_MS
#define GATEWAY_LINK_WIFI_STABILITY_POLL_MS 500U
#endif

#ifndef GATEWAY_LINK_WIFI_UP_STABLE_REQUIRED_MS
#define GATEWAY_LINK_WIFI_UP_STABLE_REQUIRED_MS 3000U
#endif

#ifndef GATEWAY_LINK_WIFI_DOWN_STABLE_REQUIRED_MS
#define GATEWAY_LINK_WIFI_DOWN_STABLE_REQUIRED_MS 1000U
#endif

#ifndef GATEWAY_LINK_HEALTH_BODY_SIZE
#define GATEWAY_LINK_HEALTH_BODY_SIZE 256U
#endif

#ifndef GATEWAY_LINK_REGISTER_BODY_SIZE
#define GATEWAY_LINK_REGISTER_BODY_SIZE 1024U
#endif

typedef struct {
    gateway_link_state_t state;
    EventGroupHandle_t events;
    TaskHandle_t task;
    TaskHandle_t reconnect_request_task;
    gateway_link_voice_abort_cb_t voice_abort_cb;
    uint32_t consecutive_failures;
    uint32_t reconnect_failures;
    int64_t last_skip_log_ms;
    int64_t last_voice_skip_log_ms;
    TickType_t wifi_got_ip_tick;
    TickType_t wifi_down_tick;
    bool wifi_has_ip;
    bool ever_ready;
} gateway_link_context_t;

static gateway_link_context_t s_link = {
    .state = LINK_DOWN,
};
static portMUX_TYPE s_link_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_seq_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned long long s_reconnect_seq;

static unsigned long long gateway_link_next_seq(void)
{
    unsigned long long seq = 0;

    portENTER_CRITICAL(&s_seq_lock);
    s_reconnect_seq++;
    seq = s_reconnect_seq;
    portEXIT_CRITICAL(&s_seq_lock);

    return seq;
}

const char *gateway_link_state_name(gateway_link_state_t state)
{
    switch (state) {
    case LINK_DOWN:
        return "LINK_DOWN";
    case LINK_WIFI_CONNECTED:
        return "LINK_WIFI_CONNECTED";
    case LINK_REGISTERING:
        return "LINK_REGISTERING";
    case LINK_READY:
        return "LINK_READY";
    case LINK_DEGRADED:
        return "LINK_DEGRADED";
    case LINK_LOST:
        return "LINK_LOST";
    default:
        return "LINK_UNKNOWN";
    }
}

static gateway_link_voice_abort_cb_t gateway_link_snapshot_abort_cb(void)
{
    gateway_link_voice_abort_cb_t callback = NULL;

    portENTER_CRITICAL(&s_link_lock);
    callback = s_link.voice_abort_cb;
    portEXIT_CRITICAL(&s_link_lock);
    return callback;
}

static void gateway_link_request_reconnect(void)
{
    EventGroupHandle_t events = NULL;

    portENTER_CRITICAL(&s_link_lock);
    events = s_link.events;
    portEXIT_CRITICAL(&s_link_lock);

    if (events != NULL) {
        xEventGroupSetBits(events, GATEWAY_LINK_RECONNECT_BIT);
    }
}

static void gateway_link_set_state(gateway_link_state_t state, const char *reason)
{
    gateway_link_state_t old_state = LINK_DOWN;
    bool changed = false;
    bool left_ready = false;
    EventGroupHandle_t events = NULL;

    portENTER_CRITICAL(&s_link_lock);
    old_state = s_link.state;
    if (s_link.state != state) {
        s_link.state = state;
        changed = true;
        left_ready = old_state == LINK_READY && state != LINK_READY;
    }
    if (state == LINK_READY) {
        s_link.ever_ready = true;
        s_link.consecutive_failures = 0;
        s_link.reconnect_failures = 0;
    }
    events = s_link.events;
    portEXIT_CRITICAL(&s_link_lock);

    if (events != NULL) {
        if (state == LINK_READY) {
            xEventGroupSetBits(events, GATEWAY_LINK_READY_BIT);
        } else {
            xEventGroupClearBits(events, GATEWAY_LINK_READY_BIT);
        }
    }

    if (changed) {
        ESP_LOGI(TAG,
                 "gateway_transport_state=%s->%s%s%s",
                 gateway_link_state_name(old_state),
                 gateway_link_state_name(state),
                 reason != NULL && reason[0] != '\0' ? " reason=" : "",
                 reason != NULL && reason[0] != '\0' ? reason : "");
    }

    if (left_ready) {
        gateway_link_voice_abort_cb_t callback = gateway_link_snapshot_abort_cb();
        if (callback != NULL) {
            callback(reason != NULL && reason[0] != '\0' ? reason : "gateway_link_lost");
        }
    }
}

void gateway_link_set_voice_abort_callback(gateway_link_voice_abort_cb_t callback)
{
    portENTER_CRITICAL(&s_link_lock);
    s_link.voice_abort_cb = callback;
    portEXIT_CRITICAL(&s_link_lock);
}

gateway_link_state_t gateway_link_get_state(void)
{
    gateway_link_state_t state;

    portENTER_CRITICAL(&s_link_lock);
    state = s_link.state;
    portEXIT_CRITICAL(&s_link_lock);

    return state;
}

bool gateway_link_is_ready(void)
{
    return gateway_link_get_state() == LINK_READY;
}

bool gateway_link_in_reconnect_mode(void)
{
    return !gateway_link_is_ready();
}

void gateway_link_notify_wifi_down(void)
{
    portENTER_CRITICAL(&s_link_lock);
    s_link.consecutive_failures = 0;
    s_link.wifi_has_ip = false;
    s_link.wifi_got_ip_tick = 0;
    s_link.wifi_down_tick = xTaskGetTickCount();
    portEXIT_CRITICAL(&s_link_lock);
    gateway_link_set_state(LINK_DOWN, "wifi_down");
    gateway_link_request_reconnect();
}

void gateway_link_notify_wifi_got_ip(void)
{
    portENTER_CRITICAL(&s_link_lock);
    s_link.wifi_has_ip = true;
    s_link.wifi_got_ip_tick = xTaskGetTickCount();
    s_link.wifi_down_tick = 0;
    portEXIT_CRITICAL(&s_link_lock);
    gateway_link_set_state(LINK_WIFI_CONNECTED, "wifi_got_ip");
    gateway_link_request_reconnect();
}

bool gateway_link_wifi_is_stable(void)
{
    bool wifi_has_ip = false;
    TickType_t got_ip_tick = 0;

    portENTER_CRITICAL(&s_link_lock);
    wifi_has_ip = s_link.wifi_has_ip;
    got_ip_tick = s_link.wifi_got_ip_tick;
    portEXIT_CRITICAL(&s_link_lock);

    if (!wifi_has_ip || got_ip_tick == 0) {
        return false;
    }

    TickType_t connected_ticks = xTaskGetTickCount() - got_ip_tick;
    return connected_ticks >= pdMS_TO_TICKS(GATEWAY_LINK_WIFI_UP_STABLE_REQUIRED_MS);
}

bool gateway_link_wifi_is_down_stable(void)
{
    bool wifi_has_ip = false;
    TickType_t down_tick = 0;

    portENTER_CRITICAL(&s_link_lock);
    wifi_has_ip = s_link.wifi_has_ip;
    down_tick = s_link.wifi_down_tick;
    portEXIT_CRITICAL(&s_link_lock);

    if (wifi_has_ip || down_tick == 0) {
        return false;
    }

    TickType_t disconnected_ticks = xTaskGetTickCount() - down_tick;
    return disconnected_ticks >= pdMS_TO_TICKS(GATEWAY_LINK_WIFI_DOWN_STABLE_REQUIRED_MS);
}

static bool gateway_link_should_log(int64_t *last_log_ms)
{
    if (last_log_ms == NULL) {
        return true;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    bool should_log = false;

    portENTER_CRITICAL(&s_link_lock);
    if (*last_log_ms == 0 ||
        now_ms - *last_log_ms >= (int64_t)GATEWAY_LINK_RECONNECT_LOG_INTERVAL_MS) {
        *last_log_ms = now_ms;
        should_log = true;
    }
    portEXIT_CRITICAL(&s_link_lock);

    return should_log;
}

bool gateway_link_can_run_non_voice_task(const char *task_name)
{
    if (gateway_link_is_ready()) {
        return true;
    }

    if (gateway_link_should_log(&s_link.last_skip_log_ms)) {
        ESP_LOGI(TAG,
                 "gateway link down, reconnecting, skip non-voice task%s%s",
                 task_name != NULL && task_name[0] != '\0' ? ": " : "",
                 task_name != NULL && task_name[0] != '\0' ? task_name : "");
    }
    return false;
}

bool gateway_link_can_start_voice_turn(void)
{
    if (gateway_link_is_ready()) {
        return true;
    }

    if (gateway_link_should_log(&s_link.last_voice_skip_log_ms)) {
        ESP_LOGI(TAG,
                 "gateway offline, skip voice turn state=%s",
                 gateway_link_state_name(gateway_link_get_state()));
    }
    return false;
}

void gateway_link_set_reconnect_request_active(bool active)
{
    portENTER_CRITICAL(&s_link_lock);
    s_link.reconnect_request_task = active ? xTaskGetCurrentTaskHandle() : NULL;
    portEXIT_CRITICAL(&s_link_lock);
}

bool gateway_link_reconnect_request_is_active_for_current_task(void)
{
    TaskHandle_t task = NULL;

    portENTER_CRITICAL(&s_link_lock);
    task = s_link.reconnect_request_task;
    portEXIT_CRITICAL(&s_link_lock);

    return task != NULL && task == xTaskGetCurrentTaskHandle();
}

bool gateway_link_http_error_is_link_failure(esp_err_t ret)
{
    switch (ret) {
    case ESP_ERR_HTTP_CONNECT:
    case ESP_ERR_HTTP_FETCH_HEADER:
    case ESP_ERR_HTTP_INVALID_TRANSPORT:
    case ESP_ERR_HTTP_CONNECTION_CLOSED:
    case ESP_ERR_HTTP_EAGAIN:
    case ESP_ERR_TIMEOUT:
    case ESP_FAIL:
    case SERVER_COMM_ERR_WIFI_NOT_READY:
        return true;
    default:
        return false;
    }
}

void gateway_link_record_http_result(esp_err_t ret, bool voice_request, bool reconnect_request)
{
    if (reconnect_request) {
        return;
    }
    if (ret == ESP_OK) {
        bool degraded_exit = false;
        portENTER_CRITICAL(&s_link_lock);
        degraded_exit = s_link.state == LINK_DEGRADED;
        s_link.consecutive_failures = 0;
        portEXIT_CRITICAL(&s_link_lock);
        if (degraded_exit) {
            ESP_LOGI(TAG, "degraded_exit reason=http_success");
            gateway_link_set_state(LINK_READY, "http_success");
        }
        return;
    }

    /*
     * 语音播放/等待响应期间 ESP_ERR_HTTP_EAGAIN 代表短暂无数据，不作为断联证据；
     * 真正的 CONNECT/TIMEOUT/FETCH_HEADER 等连续失败才进入 LINK_LOST。
     */
    if (voice_request && ret == ESP_ERR_HTTP_EAGAIN) {
        return;
    }
    if (!gateway_link_http_error_is_link_failure(ret)) {
        return;
    }

    bool enter_degraded = false;
    uint32_t failure_count = 0;
    portENTER_CRITICAL(&s_link_lock);
    if (s_link.state == LINK_READY || s_link.state == LINK_DEGRADED || s_link.state == LINK_LOST) {
        s_link.consecutive_failures++;
        failure_count = s_link.consecutive_failures;
        enter_degraded = s_link.state == LINK_READY &&
                         failure_count >= GATEWAY_LINK_FAILURE_THRESHOLD;
    }
    portEXIT_CRITICAL(&s_link_lock);

    ESP_LOGW(TAG,
             "http_failure_count=%lu ret=%s voice=%d reconnect=%d",
             (unsigned long)failure_count,
             esp_err_to_name(ret),
             voice_request ? 1 : 0,
             reconnect_request ? 1 : 0);
    if (enter_degraded) {
        ESP_LOGW(TAG, "degraded_enter failures=%lu reason=%s", (unsigned long)failure_count,
                 esp_err_to_name(ret));
        gateway_link_set_state(LINK_DEGRADED, esp_err_to_name(ret));
        gateway_link_request_reconnect();
    }
}

static uint32_t gateway_link_retry_delay_ms(void)
{
    uint32_t failures = 0;

    portENTER_CRITICAL(&s_link_lock);
    failures = s_link.reconnect_failures;
    portEXIT_CRITICAL(&s_link_lock);

    if (failures < 3U) {
        return GATEWAY_LINK_RETRY_FAST_MS;
    }
    if (failures < 6U) {
        return GATEWAY_LINK_RETRY_STEP1_MS;
    }
    if (failures < 10U) {
        return GATEWAY_LINK_RETRY_STEP2_MS;
    }
    return GATEWAY_LINK_RETRY_MAX_MS;
}

static void gateway_link_note_reconnect_failure(void)
{
    portENTER_CRITICAL(&s_link_lock);
    if (s_link.reconnect_failures < UINT32_MAX) {
        s_link.reconnect_failures++;
    }
    portEXIT_CRITICAL(&s_link_lock);
}

static esp_err_t gateway_link_health_probe(void)
{
    char response_body[GATEWAY_LINK_HEALTH_BODY_SIZE] = {0};
    server_comm_http_response_t response = {0};

    gateway_link_set_reconnect_request_active(true);
    esp_err_t ret = server_comm_http_get_json(ESP111_PROTOCOL_ROUTE_HEALTH,
                                              GATEWAY_LINK_HEALTH_TIMEOUT_MS,
                                              response_body,
                                              sizeof(response_body),
                                              &response);
    gateway_link_set_reconnect_request_active(false);
    return ret;
}

static esp_err_t gateway_link_build_register_json(char *json_body, size_t json_body_size)
{
    if (json_body == NULL || json_body_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_ap_record_t ap = {0};
    char rssi_json[12];
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(rssi_json, sizeof(rssi_json), "%d", (int)ap.rssi);
    } else {
        strlcpy(rssi_json, "null", sizeof(rssi_json));
    }

    uint32_t free_heap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
    uint32_t min_free_heap = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    int64_t uptime_ms = esp_timer_get_time() / 1000;
    unsigned long long seq = gateway_link_next_seq();

    int written = snprintf(json_body,
                           json_body_size,
                           "{\"" ESP111_PROTOCOL_LOCAL_JSON_PROTOCOL_VERSION "\":%u,"
                           "\"" ESP111_PROTOCOL_LOCAL_JSON_ID "\":%u,"
                           "\"" ESP111_PROTOCOL_LOCAL_JSON_TYPE "\":%u,"
                           "\"" ESP111_PROTOCOL_LOCAL_JSON_PAYLOAD_TYPE "\":\"%s\","
                           "\"" ESP111_PROTOCOL_LOCAL_JSON_HEALTH_SUBTYPE "\":%u,"
                           "\"" ESP111_PROTOCOL_LOCAL_JSON_UPTIME_MS "\":%lld,"
                           "\"" ESP111_PROTOCOL_LOCAL_JSON_SEQ "\":%llu,"
                           "\"" ESP111_PROTOCOL_LOCAL_JSON_TIME_SYNCED "\":false,"
                           "\"" ESP111_PROTOCOL_LOCAL_JSON_ROOM_ID "\":\"%s\","
                           "\"" ESP111_PROTOCOL_LOCAL_JSON_WIFI_RSSI "\":%s,"
                           "\"" ESP111_PROTOCOL_LOCAL_JSON_VALUES "\":[1,1,1,%lu,%lu]}",
                           ESP111_PROTOCOL_LOCAL_SCHEMA_VERSION,
                           (unsigned int)terminal_config_get_local_id(),
                           ESP111_PROTOCOL_LOCAL_PACKET_HEALTH,
                           ESP111_PROTOCOL_MSG_REGISTER,
                           ESP111_PROTOCOL_LOCAL_HEALTH_REGISTER,
                           (long long)uptime_ms,
                           seq,
                           terminal_config_get_room_id(),
                           rssi_json,
                           (unsigned long)free_heap,
                           (unsigned long)min_free_heap);
    return written > 0 && written < (int)json_body_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t gateway_link_register_child(void)
{
    char json_body[GATEWAY_LINK_REGISTER_BODY_SIZE] = {0};

    esp_err_t ret = gateway_link_build_register_json(json_body, sizeof(json_body));
    if (ret != ESP_OK) {
        return ret;
    }

    server_comm_http_response_t response = {0};
    gateway_link_set_reconnect_request_active(true);
    ret = server_comm_http_post_json(ESP111_PROTOCOL_ROUTE_REGISTER,
                                     json_body,
                                     GATEWAY_LINK_REGISTER_TIMEOUT_MS,
                                     NULL,
                                     0,
                                     &response);
    gateway_link_set_reconnect_request_active(false);
    return ret;
}

static bool gateway_link_defer_reconnect_for_voice(esp_err_t ret)
{
    if (ret != SERVER_COMM_ERR_BLOCKED_BY_VOICE_BUSY) {
        return false;
    }

    if (gateway_link_should_log(&s_link.last_voice_skip_log_ms)) {
        ESP_LOGI(TAG,
                 "reconnect health/register deferred while voice lease owns normal HTTP");
    }
    vTaskDelay(pdMS_TO_TICKS(GATEWAY_LINK_RETRY_FAST_MS));
    gateway_link_request_reconnect();
    return true;
}

static void gateway_link_reconnect_task(void *arg)
{
    (void)arg;

    while (1) {
        if (s_link.events != NULL) {
            (void)xEventGroupWaitBits(s_link.events,
                                      GATEWAY_LINK_RECONNECT_BIT,
                                      pdTRUE,
                                      pdFALSE,
                                      pdMS_TO_TICKS(gateway_link_retry_delay_ms()));
        }

        if (!gateway_link_wifi_is_stable()) {
            if (gateway_link_wifi_is_down_stable()) {
                gateway_link_set_state(LINK_DOWN, "wifi_down_stable");
                gateway_link_note_reconnect_failure();
            }
            vTaskDelay(pdMS_TO_TICKS(GATEWAY_LINK_WIFI_STABILITY_POLL_MS));
            gateway_link_request_reconnect();
            continue;
        }

        gateway_link_state_t state = gateway_link_get_state();
        if (state == LINK_READY) {
            continue;
        }

        if (state == LINK_DOWN || state == LINK_LOST || state == LINK_DEGRADED) {
            gateway_link_set_state(LINK_WIFI_CONNECTED, "wifi_stable");
        }

        esp_err_t ret = gateway_link_health_probe();
        if (ret != ESP_OK) {
            if (gateway_link_defer_reconnect_for_voice(ret)) {
                continue;
            }
            gateway_link_note_reconnect_failure();
            vTaskDelay(pdMS_TO_TICKS(gateway_link_retry_delay_ms()));
            continue;
        }

        gateway_link_set_state(LINK_REGISTERING, "health_ok");
        ret = gateway_link_register_child();
        if (ret != ESP_OK) {
            if (gateway_link_defer_reconnect_for_voice(ret)) {
                continue;
            }
            gateway_link_note_reconnect_failure();
            vTaskDelay(pdMS_TO_TICKS(gateway_link_retry_delay_ms()));
            continue;
        }

        gateway_link_set_state(LINK_READY, "register_ok");
    }
}

esp_err_t gateway_link_start(void)
{
    if (s_link.events == NULL) {
        EventGroupHandle_t events = xEventGroupCreate();
        if (events == NULL) {
            return ESP_ERR_NO_MEM;
        }
        portENTER_CRITICAL(&s_link_lock);
        if (s_link.events == NULL) {
            s_link.events = events;
        } else {
            vEventGroupDelete(events);
        }
        portEXIT_CRITICAL(&s_link_lock);
    }

    if (s_link.task == NULL) {
        BaseType_t created = xTaskCreate(gateway_link_reconnect_task,
                                         "gateway_link",
                                         GATEWAY_LINK_TASK_STACK,
                                         NULL,
                                         GATEWAY_LINK_TASK_PRIORITY,
                                         &s_link.task);
        if (created != pdPASS) {
            s_link.task = NULL;
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "gateway link reconnect task started");
    }

    gateway_link_request_reconnect();
    return ESP_OK;
}

esp_err_t gateway_link_wait_ready(uint32_t timeout_ms)
{
    if (gateway_link_is_ready()) {
        return ESP_OK;
    }
    if (s_link.events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t ticks = timeout_ms == GATEWAY_LINK_WAIT_FOREVER_MS ?
                           portMAX_DELAY :
                           pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_link.events,
                                           GATEWAY_LINK_READY_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           ticks);
    return (bits & GATEWAY_LINK_READY_BIT) != 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

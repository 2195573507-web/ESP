/**
 * @file network_worker.c
 * @brief ESPS3 网络状态 worker 和 Server 上云 gate。
 *
 * 本文件属于 ESPS3 网关。WiFi callback 只投递轻量事件；network_worker 负责
 * STA 连接/重连、稳定窗口门控和 scheduler 网络状态发布。upload/command worker
 * 只在 LINK_STABLE 后访问 ESP-server，避免 C5 本地链路刚恢复时被上云请求挤占。
 */

#include "network_worker.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "app_stack_monitor.h"
#include "cJSON.h"
#include "child_registry.h"
#include "command_router.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gateway_config.h"
#include "gateway_event_reporter.h"
#include "gateway_wifi.h"
#include "offline_policy.h"
#include "s3_scheduler.h"
#include "sensor_aggregator.h"
#include "server_client.h"
#include "smart_home_gateway.h"

static const char *TAG = "network_worker";

#ifndef NETWORK_WORKER_QUEUE_DEPTH
#define NETWORK_WORKER_QUEUE_DEPTH 16U
#endif

#ifndef NETWORK_WORKER_WORK_QUEUE_DEPTH
#define NETWORK_WORKER_WORK_QUEUE_DEPTH 16U
#endif

#ifndef NETWORK_WORKER_TASK_STACK
#define NETWORK_WORKER_TASK_STACK 6144U
#endif

#ifndef NETWORK_WORKER_UPLOAD_TASK_STACK
#define NETWORK_WORKER_UPLOAD_TASK_STACK 12288U
#endif

#ifndef NETWORK_WORKER_COMMAND_TASK_STACK
#define NETWORK_WORKER_COMMAND_TASK_STACK 16384U
#endif

#ifndef NETWORK_WORKER_TASK_PRIORITY
#define NETWORK_WORKER_TASK_PRIORITY 5U
#endif

#ifndef NETWORK_WORKER_UPLOAD_TASK_PRIORITY
#define NETWORK_WORKER_UPLOAD_TASK_PRIORITY 4U
#endif

#ifndef NETWORK_WORKER_COMMAND_TASK_PRIORITY
#define NETWORK_WORKER_COMMAND_TASK_PRIORITY 4U
#endif

#ifndef NETWORK_WORKER_STABLE_GATE_MS
#define NETWORK_WORKER_STABLE_GATE_MS 3000U
#endif

#ifndef NETWORK_WORKER_POLL_MS
#define NETWORK_WORKER_POLL_MS 250U
#endif

#ifndef NETWORK_WORKER_STA_RECONNECT_DELAY_MS
#define NETWORK_WORKER_STA_RECONNECT_DELAY_MS 3000U
#endif

typedef struct {
    network_worker_event_t event;
    network_worker_event_source_t source;
    uint32_t ip_addr;
} network_worker_item_t;

typedef enum {
    NETWORK_WORKER_WORK_UPLOAD_JSON = 0,
    NETWORK_WORKER_WORK_UPLOAD_SNAPSHOT,
    NETWORK_WORKER_WORK_COMMAND_PULL,
    NETWORK_WORKER_WORK_COMMAND_ACK,
    NETWORK_WORKER_WORK_SMART_HOME_POLL,
} network_worker_work_type_t;

typedef struct {
    network_worker_work_type_t work_type;
    network_worker_server_json_type_t json_type;
    char *json_body;
    char command_id[48];
    char source[24];
} network_worker_work_item_t;

static QueueHandle_t s_event_queue;
static QueueHandle_t s_work_queue;
static QueueHandle_t s_command_queue;
static TaskHandle_t s_worker_task;
static TaskHandle_t s_upload_task;
static TaskHandle_t s_command_task;
static network_worker_link_state_t s_link_state = NETWORK_WORKER_LINK_DOWN;
static s3_scheduler_network_state_t s_last_scheduler_state = S3_SCHEDULER_NET_NOT_READY;
static int64_t s_last_network_change_ms;
static int64_t s_last_worker_stack_log_ms;
static int64_t s_last_worker_heap_log_ms;
static int64_t s_last_upload_stack_log_ms;
static int64_t s_last_upload_heap_log_ms;
static int64_t s_last_command_stack_log_ms;
static int64_t s_last_command_heap_log_ms;
static bool s_sta_connect_pending;
static bool s_sta_connect_use_next;
static int64_t s_next_sta_connect_ms;
static char s_command_ack_response[SERVER_CLIENT_SMALL_BODY_BYTES];

/* 网络门控只看本机 uptime，避免 Server 时间未同步时影响重连/稳定窗口。 */
static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

const char *network_worker_link_state_name(network_worker_link_state_t state)
{
    switch (state) {
    case NETWORK_WORKER_LINK_DOWN:
        return "LINK_DOWN";
    case NETWORK_WORKER_LINK_UP:
        return "LINK_UP";
    case NETWORK_WORKER_LINK_IP_READY:
        return "IP_READY";
    case NETWORK_WORKER_LINK_STABLE:
        return "LINK_STABLE";
    default:
        return "UNKNOWN";
    }
}

static const char *event_name(network_worker_event_t event)
{
    switch (event) {
    case NETWORK_WORKER_EVENT_LINK_UP:
        return "LINK_UP";
    case NETWORK_WORKER_EVENT_LINK_DOWN:
        return "LINK_DOWN";
    case NETWORK_WORKER_EVENT_IP_READY:
        return "IP_READY";
    default:
        return "UNKNOWN";
    }
}

static const char *source_name(network_worker_event_source_t source)
{
    switch (source) {
    case NETWORK_WORKER_SOURCE_SOFTAP_START:
        return "softap_start";
    case NETWORK_WORKER_SOURCE_SOFTAP_STOP:
        return "softap_stop";
    case NETWORK_WORKER_SOURCE_AP_STA_CONNECTED:
        return "ap_sta_connected";
    case NETWORK_WORKER_SOURCE_AP_STA_DISCONNECTED:
        return "ap_sta_disconnected";
    case NETWORK_WORKER_SOURCE_STA_START:
        return "sta_start";
    case NETWORK_WORKER_SOURCE_STA_STOP:
        return "sta_stop";
    case NETWORK_WORKER_SOURCE_STA_DISCONNECTED:
        return "sta_disconnected";
    case NETWORK_WORKER_SOURCE_STA_GOT_IP:
        return "sta_got_ip";
    case NETWORK_WORKER_SOURCE_UNKNOWN:
    default:
        return "unknown";
    }
}

static const char *work_name(network_worker_work_type_t work_type)
{
    switch (work_type) {
    case NETWORK_WORKER_WORK_UPLOAD_JSON:
        return "upload_json";
    case NETWORK_WORKER_WORK_UPLOAD_SNAPSHOT:
        return "upload_snapshot";
    case NETWORK_WORKER_WORK_COMMAND_PULL:
        return "command_pull";
    case NETWORK_WORKER_WORK_COMMAND_ACK:
        return "command_ack";
    case NETWORK_WORKER_WORK_SMART_HOME_POLL:
        return "smart_home_poll";
    default:
        return "unknown";
    }
}

static const char *json_type_name(network_worker_server_json_type_t type)
{
    switch (type) {
    case NETWORK_WORKER_SERVER_JSON_INGEST:
        return "ingest";
    case NETWORK_WORKER_SERVER_JSON_CSI_EVENT:
        return "csi_event";
    case NETWORK_WORKER_SERVER_JSON_GATEWAY_STATE:
        return "gateway_state";
    case NETWORK_WORKER_SERVER_JSON_SYSTEM_LOG:
        return "system_log";
    case NETWORK_WORKER_SERVER_JSON_ALARM:
        return "alarm";
    default:
        return "unknown";
    }
}

static bool server_link_stable(void)
{
    return gateway_wifi_is_sta_connected() && s3_scheduler_is_server_upload_allowed();
}

static void set_gateway_link_state(network_worker_link_state_t state, const char *reason)
{
    if (s_link_state == state) {
        return;
    }

    network_worker_link_state_t old_state = s_link_state;
    s_link_state = state;
    ESP_LOGI(TAG,
             "gateway_link transition %s -> %s reason=%s softap=%d sta_started=%d sta_connected=%d",
             network_worker_link_state_name(old_state),
             network_worker_link_state_name(state),
             reason != NULL ? reason : "unknown",
             gateway_wifi_is_softap_ready() ? 1 : 0,
             gateway_wifi_is_sta_started() ? 1 : 0,
             gateway_wifi_is_sta_connected() ? 1 : 0);
}

static void publish_scheduler_state(s3_scheduler_network_state_t state, const char *reason)
{
    if (s_last_scheduler_state == state) {
        return;
    }
    s_last_scheduler_state = state;

    esp_err_t ret = s3_scheduler_enqueue_network_state(state);
    if (ret != ESP_OK) {
        /* scheduler 队列满时仍直接更新状态，避免全局网络 gate 长时间停在旧值。 */
        ESP_LOGW(TAG,
                 "scheduler network state enqueue failed state=%s ret=%s",
                 s3_scheduler_network_state_name(state),
                 esp_err_to_name(ret));
        s3_scheduler_set_network_state(state);
    }
}

static void schedule_sta_connect(bool use_next, uint32_t delay_ms)
{
    if (!gateway_config_sta_credentials_configured()) {
        s_sta_connect_pending = false;
        return;
    }
    /* 只记录意图，真正调用 WiFi connect 在 worker task 中执行。 */
    s_sta_connect_pending = true;
    s_sta_connect_use_next = use_next;
    s_next_sta_connect_ms = now_ms() + (int64_t)delay_ms;
}

static void service_sta_connect(void)
{
    if (!s_sta_connect_pending || now_ms() < s_next_sta_connect_ms ||
        gateway_wifi_is_sta_connected()) {
        return;
    }

    s_sta_connect_pending = false;
    esp_err_t ret = s_sta_connect_use_next ? gateway_wifi_reconnect_sta_next() :
                                             gateway_wifi_connect_sta_current();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "STA connect request failed mode=%s ret=%s",
                 s_sta_connect_use_next ? "next" : "current",
                 esp_err_to_name(ret));
        schedule_sta_connect(true, NETWORK_WORKER_STA_RECONNECT_DELAY_MS);
    }
}

static void evaluate_state(const char *reason)
{
    service_sta_connect();

    /*
     * SoftAP ready 代表 C5 本地链路可开始稳定计时；STA connected/link stable 才允许
     * 访问 ESP-server。net_ready_gate 在稳定窗口内保持 false，避免刚获 IP 就发包。
     */
    const bool local_link_ready = gateway_wifi_is_softap_ready() || gateway_wifi_is_sta_connected();
    if (!local_link_ready) {
        gateway_wifi_set_net_ready_gate(false, reason);
        publish_scheduler_state(S3_SCHEDULER_NET_NOT_READY, reason);
        set_gateway_link_state(NETWORK_WORKER_LINK_DOWN, reason);
        return;
    }

    if (s_last_network_change_ms <= 0) {
        s_last_network_change_ms = now_ms();
    }

    const int64_t stable_for_ms = now_ms() - s_last_network_change_ms;
    if (stable_for_ms < (int64_t)NETWORK_WORKER_STABLE_GATE_MS) {
        gateway_wifi_set_net_ready_gate(false, reason);
        if (gateway_wifi_is_sta_connected()) {
            publish_scheduler_state(S3_SCHEDULER_IP_READY, reason);
        } else if (gateway_wifi_is_sta_started()) {
            publish_scheduler_state(S3_SCHEDULER_STA_CONNECTED, reason);
        } else {
            publish_scheduler_state(S3_SCHEDULER_NET_NOT_READY, reason);
        }
        set_gateway_link_state(gateway_wifi_is_sta_connected() ?
                                   NETWORK_WORKER_LINK_IP_READY :
                                   NETWORK_WORKER_LINK_UP,
                               reason);
        return;
    }

    gateway_wifi_set_net_ready_gate(true, reason);
    publish_scheduler_state(gateway_wifi_is_sta_connected() ?
                                S3_SCHEDULER_LINK_STABLE :
                                S3_SCHEDULER_NET_NOT_READY,
                            reason);
    set_gateway_link_state(NETWORK_WORKER_LINK_STABLE, reason);
}

static void handle_network_event(const network_worker_item_t *item)
{
    if (item == NULL) {
        return;
    }

    const char *reason = source_name(item->source);
    s_last_network_change_ms = now_ms();

    ESP_LOGI(TAG,
             "network event=%s source=%s ip=0x%08lx",
             event_name(item->event),
             reason,
             (unsigned long)item->ip_addr);

    switch (item->source) {
    case NETWORK_WORKER_SOURCE_STA_START:
        schedule_sta_connect(false, 0U);
        break;
    case NETWORK_WORKER_SOURCE_STA_DISCONNECTED:
        schedule_sta_connect(true, NETWORK_WORKER_STA_RECONNECT_DELAY_MS);
        break;
    case NETWORK_WORKER_SOURCE_AP_STA_DISCONNECTED:
        /* AP 断开只标记 C5 link_lost；child_registry 会给 C5 重连留宽限期。 */
        child_registry_mark_all_link_lost("ap_sta_disconnected");
        break;
    default:
        break;
    }

    evaluate_state(reason);
}

static esp_err_t enqueue_to_queue(QueueHandle_t queue, const network_worker_work_item_t *item)
{
    if (item == NULL || queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(queue, item, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t enqueue_upload_work_item(const network_worker_work_item_t *item)
{
    return enqueue_to_queue(s_work_queue, item);
}

static esp_err_t enqueue_command_work_item(const network_worker_work_item_t *item)
{
    return enqueue_to_queue(s_command_queue, item);
}

static void release_work_item(network_worker_work_item_t *item)
{
    if (item == NULL) {
        return;
    }
    if (item->json_body != NULL) {
        cJSON_free(item->json_body);
        item->json_body = NULL;
    }
}

static void requeue_or_drop_work(QueueHandle_t queue,
                                 network_worker_work_item_t *item,
                                 esp_err_t reason)
{
    if (item == NULL) {
        return;
    }
    if (queue != NULL && xQueueSendToFront(queue, item, 0) == pdTRUE) {
        return;
    }

    /* 队列已满时宁可丢弃本次上云工作；Server 是事实中心，S3 不在 RAM 中堆积离线队列。 */
    ESP_LOGW(TAG,
             "offline work drop type=%s json_type=%s source=%s ret=%s",
             work_name(item->work_type),
             json_type_name(item->json_type),
             item->source,
             esp_err_to_name(reason));
    release_work_item(item);
}

static esp_err_t perform_server_json(network_worker_work_item_t *item)
{
    if (item == NULL || item->json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char response[SERVER_CLIENT_SMALL_BODY_BYTES];
    int status = 0;
    esp_err_t ret = ESP_ERR_INVALID_ARG;
    switch (item->json_type) {
    case NETWORK_WORKER_SERVER_JSON_INGEST:
        ret = server_client_post_ingest_json(item->json_body, response, sizeof(response), &status);
        break;
    case NETWORK_WORKER_SERVER_JSON_CSI_EVENT:
        ret = server_client_post_csi_event_json(item->json_body, response, sizeof(response), &status);
        break;
    case NETWORK_WORKER_SERVER_JSON_GATEWAY_STATE:
        ret = server_client_post_gateway_state_json(item->json_body, response, sizeof(response), &status);
        break;
    case NETWORK_WORKER_SERVER_JSON_SYSTEM_LOG:
        ret = server_client_post_system_log_json(item->json_body, response, sizeof(response), &status);
        break;
    case NETWORK_WORKER_SERVER_JSON_ALARM:
        ret = server_client_post_alarm_json(item->json_body, response, sizeof(response), &status);
        break;
    default:
        ret = ESP_ERR_INVALID_ARG;
        break;
    }

    if (ret == ESP_ERR_INVALID_STATE && !server_link_stable()) {
        return ret;
    }

    /* offline_policy 只记录最近可用性和错误码，不改变 C5 本地请求是否成功。 */
    offline_policy_record_server_result(ret, status);
    if (item->json_type == NETWORK_WORKER_SERVER_JSON_GATEWAY_STATE) {
        gateway_event_reporter_record_server_state(ret == ESP_OK && status >= 200 && status < 300);
    }
    if (ret != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG,
                 "server JSON upload failed type=%s source=%s status=%d ret=%s",
                 json_type_name(item->json_type),
                 item->source,
                 status,
                 esp_err_to_name(ret));
    }
    return ret;
}

static void process_upload_work_item(network_worker_work_item_t *item)
{
    if (item == NULL) {
        return;
    }
    if (!server_link_stable()) {
        requeue_or_drop_work(s_work_queue, item, ESP_ERR_INVALID_STATE);
        return;
    }

    /* upload worker 只处理 Server-facing 工作；C5 本地 HTTP 响应早已在 S3 ingress 路径完成。 */
    esp_err_t ret = ESP_OK;
    switch (item->work_type) {
    case NETWORK_WORKER_WORK_UPLOAD_JSON:
        ret = perform_server_json(item);
        if (ret == ESP_ERR_INVALID_STATE && !server_link_stable()) {
            requeue_or_drop_work(s_work_queue, item, ret);
            return;
        }
        break;
    case NETWORK_WORKER_WORK_UPLOAD_SNAPSHOT:
        sensor_aggregator_upload_snapshot_now();
        break;
    default:
        ret = ESP_ERR_INVALID_ARG;
        break;
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "network worker work failed type=%s source=%s ret=%s",
                 work_name(item->work_type),
                 item->source,
                 esp_err_to_name(ret));
    }
    release_work_item(item);
}

static esp_err_t perform_command_ack(network_worker_work_item_t *item)
{
    if (item == NULL || item->command_id[0] == '\0' || item->json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int status = 0;
    esp_err_t ret = server_client_ack_command(item->command_id,
                                             item->json_body,
                                             s_command_ack_response,
                                             sizeof(s_command_ack_response),
                                             &status);
    if (ret == ESP_ERR_INVALID_STATE && !server_link_stable()) {
        return ret;
    }
    /* command ack 属于 Server contract，失败只记录并交给上层下一轮命令生命周期处理。 */
    offline_policy_record_server_result(ret, status);
    if (ret != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG,
                 "command ack upload failed id=%s status=%d ret=%s",
                 item->command_id,
                 status,
                 esp_err_to_name(ret));
    }
    return ret;
}

static void process_command_work_item(network_worker_work_item_t *item)
{
    if (item == NULL) {
        return;
    }
    if (!server_link_stable()) {
        requeue_or_drop_work(s_command_queue, item, ESP_ERR_INVALID_STATE);
        return;
    }

    /* command/smart-home 与普通 snapshot 分开队列，避免大 JSON 上传阻塞命令 ack。 */
    esp_err_t ret = ESP_OK;
    switch (item->work_type) {
    case NETWORK_WORKER_WORK_COMMAND_PULL:
        command_router_poll_server_pending();
        break;
    case NETWORK_WORKER_WORK_COMMAND_ACK:
        ret = perform_command_ack(item);
        if (ret == ESP_ERR_INVALID_STATE && !server_link_stable()) {
            requeue_or_drop_work(s_command_queue, item, ret);
            return;
        }
        break;
    case NETWORK_WORKER_WORK_SMART_HOME_POLL:
        smart_home_gateway_poll_once();
        break;
    default:
        ret = ESP_ERR_INVALID_ARG;
        break;
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "network worker work failed type=%s source=%s ret=%s",
                 work_name(item->work_type),
                 item->source,
                 esp_err_to_name(ret));
    }
    release_work_item(item);
}

static void network_worker_task(void *arg)
{
    (void)arg;
    const bool wdt_registered = app_task_wdt_add_current(TAG, "network_worker");
    ESP_LOGI(TAG,
             "network worker started queue_depth=%u stable_gate_ms=%u",
             (unsigned int)NETWORK_WORKER_QUEUE_DEPTH,
             (unsigned int)NETWORK_WORKER_STABLE_GATE_MS);
    app_stack_monitor_log(TAG, "network_worker", "entry");

    while (1) {
        network_worker_item_t item = {0};
        if (xQueueReceive(s_event_queue, &item, pdMS_TO_TICKS(NETWORK_WORKER_POLL_MS)) ==
            pdTRUE) {
            handle_network_event(&item);
        } else {
            /* 周期评估用于稳定窗口到期、STA reconnect 延迟到期等无事件状态推进。 */
            evaluate_state("periodic");
        }
        app_stack_monitor_log_periodic(TAG,
                                       "network_worker",
                                       &s_last_worker_stack_log_ms,
                                       APP_STACK_MONITOR_INTERVAL_MS);
        app_heap_monitor_log_periodic(TAG,
                                      &s_last_worker_heap_log_ms,
                                      APP_HEAP_MONITOR_INTERVAL_MS);
        app_task_wdt_reset_current(wdt_registered);
    }
}

static void upload_worker_task(void *arg)
{
    (void)arg;
    const bool wdt_registered = app_task_wdt_add_current(TAG, "upload_worker");
    ESP_LOGI(TAG,
             "upload worker started queue_depth=%u server_gate=LINK_STABLE",
             (unsigned int)NETWORK_WORKER_WORK_QUEUE_DEPTH);
    app_stack_monitor_log(TAG, "upload_worker", "entry");

    while (1) {
        if (!server_link_stable()) {
            /* Server 未 ready 时不从队列取工作，保留队列头的时序。 */
            app_stack_monitor_log_periodic(TAG,
                                           "upload_worker",
                                           &s_last_upload_stack_log_ms,
                                           APP_STACK_MONITOR_INTERVAL_MS);
            app_heap_monitor_log_periodic(TAG,
                                          &s_last_upload_heap_log_ms,
                                          APP_HEAP_MONITOR_INTERVAL_MS);
            app_task_wdt_delay_ms(wdt_registered, NETWORK_WORKER_POLL_MS);
            continue;
        }

        network_worker_work_item_t item = {0};
        if (xQueueReceive(s_work_queue, &item, pdMS_TO_TICKS(NETWORK_WORKER_POLL_MS)) ==
            pdTRUE) {
            process_upload_work_item(&item);
        }
        app_stack_monitor_log_periodic(TAG,
                                       "upload_worker",
                                       &s_last_upload_stack_log_ms,
                                       APP_STACK_MONITOR_INTERVAL_MS);
        app_heap_monitor_log_periodic(TAG,
                                      &s_last_upload_heap_log_ms,
                                      APP_HEAP_MONITOR_INTERVAL_MS);
        app_task_wdt_reset_current(wdt_registered);
    }
}

static void command_worker_task(void *arg)
{
    (void)arg;
    const bool wdt_registered = app_task_wdt_add_current(TAG, "command_worker");
    ESP_LOGI(TAG,
             "command worker started queue_depth=%u server_gate=LINK_STABLE",
             (unsigned int)NETWORK_WORKER_WORK_QUEUE_DEPTH);
    app_stack_monitor_log(TAG, "command_worker", "entry");

    while (1) {
        if (!server_link_stable()) {
            /* command 队列同样等 LINK_STABLE，避免 ack 在 STA 抖动时反复失败。 */
            app_stack_monitor_log_periodic(TAG,
                                           "command_worker",
                                           &s_last_command_stack_log_ms,
                                           APP_STACK_MONITOR_INTERVAL_MS);
            app_heap_monitor_log_periodic(TAG,
                                          &s_last_command_heap_log_ms,
                                          APP_HEAP_MONITOR_INTERVAL_MS);
            app_task_wdt_delay_ms(wdt_registered, NETWORK_WORKER_POLL_MS);
            continue;
        }

        network_worker_work_item_t item = {0};
        if (xQueueReceive(s_command_queue, &item, pdMS_TO_TICKS(NETWORK_WORKER_POLL_MS)) ==
            pdTRUE) {
            process_command_work_item(&item);
        }
        app_stack_monitor_log_periodic(TAG,
                                       "command_worker",
                                       &s_last_command_stack_log_ms,
                                       APP_STACK_MONITOR_INTERVAL_MS);
        app_heap_monitor_log_periodic(TAG,
                                      &s_last_command_heap_log_ms,
                                      APP_HEAP_MONITOR_INTERVAL_MS);
        app_task_wdt_reset_current(wdt_registered);
    }
}

esp_err_t network_worker_init(void)
{
    if (s_event_queue == NULL) {
        s_event_queue = xQueueCreate(NETWORK_WORKER_QUEUE_DEPTH, sizeof(network_worker_item_t));
        if (s_event_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_work_queue == NULL) {
        s_work_queue =
            xQueueCreate(NETWORK_WORKER_WORK_QUEUE_DEPTH, sizeof(network_worker_work_item_t));
        if (s_work_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_command_queue == NULL) {
        s_command_queue =
            xQueueCreate(NETWORK_WORKER_WORK_QUEUE_DEPTH, sizeof(network_worker_work_item_t));
        if (s_command_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_worker_task == NULL) {
        BaseType_t created = xTaskCreate(network_worker_task,
                                         "network_worker",
                                         NETWORK_WORKER_TASK_STACK,
                                         NULL,
                                         NETWORK_WORKER_TASK_PRIORITY,
                                         &s_worker_task);
        if (created != pdPASS) {
            s_worker_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_upload_task == NULL) {
        BaseType_t created = xTaskCreate(upload_worker_task,
                                         "upload_worker",
                                         NETWORK_WORKER_UPLOAD_TASK_STACK,
                                         NULL,
                                         NETWORK_WORKER_UPLOAD_TASK_PRIORITY,
                                         &s_upload_task);
        if (created != pdPASS) {
            s_upload_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_command_task == NULL) {
        BaseType_t created = xTaskCreate(command_worker_task,
                                         "command_worker",
                                         NETWORK_WORKER_COMMAND_TASK_STACK,
                                         NULL,
                                         NETWORK_WORKER_COMMAND_TASK_PRIORITY,
                                         &s_command_task);
        if (created != pdPASS) {
            s_command_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t network_worker_post_event(network_worker_event_t event,
                                    network_worker_event_source_t source,
                                    uint32_t ip_addr)
{
    if (s_event_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    network_worker_item_t item = {
        .event = event,
        .source = source,
        .ip_addr = ip_addr,
    };
    if (xQueueSend(s_event_queue, &item, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

network_worker_link_state_t network_worker_get_link_state(void)
{
    return s_link_state;
}

esp_err_t network_worker_submit_server_json(network_worker_server_json_type_t type,
                                            char *json_body,
                                            const char *source)
{
    if (json_body == NULL || type > NETWORK_WORKER_SERVER_JSON_ALARM) {
        return ESP_ERR_INVALID_ARG;
    }

    network_worker_work_item_t item = {
        .work_type = NETWORK_WORKER_WORK_UPLOAD_JSON,
        .json_type = type,
        .json_body = json_body,
    };
    strlcpy(item.source, source != NULL ? source : "server_json", sizeof(item.source));
    /* 入队成功后 json_body 生命周期转交给 upload_worker。 */
    return enqueue_upload_work_item(&item);
}

esp_err_t network_worker_enqueue_snapshot_upload(void)
{
    network_worker_work_item_t item = {
        .work_type = NETWORK_WORKER_WORK_UPLOAD_SNAPSHOT,
    };
    strlcpy(item.source, "scheduler", sizeof(item.source));
    return enqueue_upload_work_item(&item);
}

esp_err_t network_worker_enqueue_command_pull(void)
{
    network_worker_work_item_t item = {
        .work_type = NETWORK_WORKER_WORK_COMMAND_PULL,
    };
    strlcpy(item.source, "scheduler", sizeof(item.source));
    return enqueue_command_work_item(&item);
}

esp_err_t network_worker_enqueue_command_ack(const char *command_id, const char *ack_json)
{
    if (command_id == NULL || command_id[0] == '\0' || ack_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ack_len = strlen(ack_json);
    /* ack_json 可能来自调用方栈/临时 buffer，这里复制后再交给异步 command worker。 */
    char *owned_ack = cJSON_malloc(ack_len + 1U);
    if (owned_ack == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(owned_ack, ack_json, ack_len + 1U);

    network_worker_work_item_t item = {
        .work_type = NETWORK_WORKER_WORK_COMMAND_ACK,
        .json_body = owned_ack,
    };
    strlcpy(item.command_id, command_id, sizeof(item.command_id));
    strlcpy(item.source, "command_ack", sizeof(item.source));

    esp_err_t ret = enqueue_command_work_item(&item);
    if (ret != ESP_OK) {
        cJSON_free(owned_ack);
    }
    return ret;
}

esp_err_t network_worker_enqueue_smart_home_poll(void)
{
    network_worker_work_item_t item = {
        .work_type = NETWORK_WORKER_WORK_SMART_HOME_POLL,
    };
    strlcpy(item.source, "scheduler", sizeof(item.source));
    return enqueue_command_work_item(&item);
}

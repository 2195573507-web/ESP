#include "habit_event_reporter.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "app_stack_monitor.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "habit_rule_engine.h"
#include "server_client.h"

#define HABIT_EVENT_REPORTER_POLL_MS 250U
#define HABIT_EVENT_REPORTER_STACK 3072U
#define HABIT_EVENT_REPORTER_PRIORITY (tskIDLE_PRIORITY + 1U)

static const char *TAG = "habit_event_reporter";
static TaskHandle_t s_task;
typedef struct {
    char json[768];
    char response[SERVER_CLIENT_SMALL_BODY_BYTES];
} habit_event_reporter_workspace_t;

/* Kept outside the 3 KiB reporter stack for the task's full lifetime. */
static habit_event_reporter_workspace_t *s_workspace;
static habit_event_t s_pending;
static bool s_has_pending;
static uint32_t s_retry_count;
static uint64_t s_next_attempt_ms;
static habit_event_reporter_stats_t s_stats;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static uint32_t retry_delay_ms(uint32_t retry_count)
{
    const uint32_t delays[] = {1000U, 2000U, 5000U, 10000U, 30000U};
    const size_t last = (sizeof(delays) / sizeof(delays[0])) - 1U;
    return delays[retry_count < last ? retry_count : last];
}

static esp_err_t serialize_event(const habit_event_t *event, char *out, size_t out_size)
{
    if (event == NULL || out == NULL || out_size == 0U) return ESP_ERR_INVALID_ARG;
    cJSON *root = cJSON_CreateObject();
    cJSON *payload = cJSON_CreateObject();
    if (root == NULL || payload == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(payload);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "event_id", event->event_id);
    cJSON_AddStringToObject(root, "rule_id", event->rule_id);
    cJSON_AddStringToObject(root, "rule_type", event->event_type);
    cJSON_AddStringToObject(root, "room", event->room);
    cJSON_AddStringToObject(root, "source", event->source);
    cJSON_AddStringToObject(root, "timestamp", event->timestamp);
    cJSON_AddNumberToObject(root, "sequence", event->sequence);
    cJSON_AddNumberToObject(payload, "person_count", event->person_count);
    cJSON_AddStringToObject(payload, "reason", event->reason);
    cJSON_AddItemToObject(root, "payload", payload);
    const bool written = cJSON_PrintPreallocated(root, out, (int)out_size, false) != 0;
    cJSON_Delete(root);
    return written ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void habit_event_reporter_task(void *arg)
{
    habit_event_reporter_workspace_t *workspace = (habit_event_reporter_workspace_t *)arg;
    if (workspace == NULL) {
        ESP_LOGE(TAG, "habit event reporter workspace unavailable");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        if (!s_has_pending) {
            s_has_pending = habit_rule_engine_runtime_pop_event(&s_pending);
            if (s_has_pending) {
                s_retry_count = 0U;
                s_next_attempt_ms = 0U;
            }
        }
        if (s_has_pending && now_ms() >= s_next_attempt_ms) {
            int http_status = 0;
            const esp_err_t json_ret = serialize_event(&s_pending,
                                                       workspace->json,
                                                       sizeof(workspace->json));
            const esp_err_t ret = json_ret == ESP_OK
                ? server_client_post_habit_event_json(workspace->json,
                                                      workspace->response,
                                                      sizeof(workspace->response),
                                                      &http_status)
                : json_ret;
            if (ret == ESP_OK && http_status >= 200 && http_status < 300) {
                ++s_stats.sent_events;
                ESP_LOGI(TAG, "HABIT_EVENT_REPORT event_id=%s sequence=%lu result=sent status=%d",
                         s_pending.event_id, (unsigned long)s_pending.sequence, http_status);
                s_has_pending = false;
            } else {
                ++s_stats.retry_attempts;
                s_next_attempt_ms = now_ms() + retry_delay_ms(s_retry_count++);
                ESP_LOGW(TAG, "HABIT_EVENT_REPORT event_id=%s sequence=%lu result=retry status=%d ret=%s next_ms=%llu",
                         s_pending.event_id, (unsigned long)s_pending.sequence, http_status,
                         esp_err_to_name(ret), (unsigned long long)s_next_attempt_ms);
            }
        }
        habit_rule_diagnostics_t diagnostics = {0};
        habit_rule_engine_runtime_get_diagnostics(&diagnostics);
        s_stats.dropped_events = diagnostics.dropped_events;
        s_stats.pending_events = s_has_pending ? 1U : 0U;
        vTaskDelay(pdMS_TO_TICKS(HABIT_EVENT_REPORTER_POLL_MS));
    }
}

esp_err_t habit_event_reporter_start(void)
{
    if (s_task != NULL) return ESP_OK;
    if (s_workspace == NULL) {
        s_workspace = heap_caps_calloc(1U,
                                       sizeof(*s_workspace),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_workspace == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xTaskCreateWithCaps(habit_event_reporter_task,
                            "habit_event_reporter",
                            HABIT_EVENT_REPORTER_STACK,
                            s_workspace,
                            HABIT_EVENT_REPORTER_PRIORITY,
                            &s_task,
                            APP_TASK_STACK_CAPS_PSRAM) != pdPASS) {
        s_task = NULL;
        heap_caps_free(s_workspace);
        s_workspace = NULL;
        return ESP_ERR_NO_MEM;
    }
    app_stack_monitor_log_task_created(TAG,
                                       "habit_event_reporter",
                                       s_task,
                                       HABIT_EVENT_REPORTER_STACK);
    return ESP_OK;
}

void habit_event_reporter_get_stats(habit_event_reporter_stats_t *out)
{
    if (out != NULL) *out = s_stats;
}

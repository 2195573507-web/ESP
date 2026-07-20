/**
 * @file network_replay_worker.c
 * @brief Replays cached BME690 Server ingest JSON after LINK_STABLE.
 */

#include "network_replay_worker.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "bme_cache_manager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "gateway_config.h"
#include "gateway_event_reporter.h"
#include "home_ai_history_store.h"
#include "network_worker.h"
#include "offline_policy.h"
#include "resource_manager.h"
#include "sensor_aggregator.h"
#include "server_client.h"

static const char *TAG = "network_replay_worker";

#ifndef NETWORK_REPLAY_WORKER_TASK_STACK
#define NETWORK_REPLAY_WORKER_TASK_STACK 16384U
#endif

#ifndef NETWORK_REPLAY_WORKER_TASK_PRIORITY
#define NETWORK_REPLAY_WORKER_TASK_PRIORITY 4U
#endif

#ifndef NETWORK_REPLAY_WORKER_IDLE_MS
#define NETWORK_REPLAY_WORKER_IDLE_MS 500U
#endif

#ifndef NETWORK_REPLAY_WORKER_RATE_DELAY_MS
#define NETWORK_REPLAY_WORKER_RATE_DELAY_MS 100U
#endif

#ifndef NETWORK_REPLAY_WORKER_FAILURE_DELAY_MS
#define NETWORK_REPLAY_WORKER_FAILURE_DELAY_MS 1000U
#endif

#ifndef NETWORK_REPLAY_PROGRESS_LOG_EVERY
#define NETWORK_REPLAY_PROGRESS_LOG_EVERY 10U
#endif

#define NETWORK_REPLAY_HOME_AI_BODY_BYTES 1600U
#define NETWORK_REPLAY_HOME_AI_FLUSH_BATCH 4U

static TaskHandle_t s_replay_task;

static bool json_escape(const char *input, char *out, size_t out_size)
{
    if (input == NULL || out == NULL || out_size == 0U) return false;
    size_t used = 0U;
    for (size_t index = 0U; input[index] != '\0'; ++index) {
        const unsigned char value = (unsigned char)input[index];
        const char *escape = NULL;
        char escaped[7] = {0};
        if (value == '"') escape = "\\\"";
        else if (value == '\\') escape = "\\\\";
        else if (value == '\n') escape = "\\n";
        else if (value == '\r') escape = "\\r";
        else if (value == '\t') escape = "\\t";
        else if (value < 0x20U) {
            (void)snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned int)value);
            escape = escaped;
        }
        if (escape != NULL) {
            const size_t length = strlen(escape);
            if (used + length + 1U > out_size) return false;
            memcpy(out + used, escape, length);
            used += length;
        } else {
            if (used + 2U > out_size) return false;
            out[used++] = (char)value;
        }
    }
    out[used] = '\0';
    return true;
}

static bool build_home_ai_replay_json(const home_ai_history_event_t *event,
                                      char *out,
                                      size_t out_size)
{
    if (event == NULL || out == NULL || out_size == 0U ||
        event->payload_len == 0U || event->payload[0] != '{') return false;
    char event_id[HOME_AI_HISTORY_EVENT_ID_LEN * 2U];
    char room_id[HOME_AI_HISTORY_ROOM_ID_LEN * 2U];
    char event_type[HOME_AI_HISTORY_EVENT_TYPE_LEN * 2U];
    char gateway_id[128];
    if (!json_escape(event->event_id, event_id, sizeof(event_id)) ||
        !json_escape(event->room_id, room_id, sizeof(room_id)) ||
        !json_escape(event->event_type, event_type, sizeof(event_type)) ||
        !json_escape(gateway_config_get()->gateway_id, gateway_id, sizeof(gateway_id))) return false;
    const int written = snprintf(
        out,
        out_size,
        "{\"gateway_id\":\"%s\",\"events\":[{\"event_id\":\"%s\","
        "\"event_type\":\"%s\",\"room_id\":\"%s\",\"priority\":%u,"
        "\"occurred_at_ms\":%llu,\"source_device_id\":\"%s\","
        "\"sequence_no\":%lu,\"schema_version\":1,\"payload\":%s}]}",
        gateway_id,
        event_id,
        event_type,
        room_id,
        (unsigned int)event->priority,
        (unsigned long long)event->occurred_at_ms,
        gateway_id,
        (unsigned long)event->sequence,
        event->payload);
    return written > 0 && written < (int)out_size;
}

static bool replay_one_home_ai_event(void)
{
    home_ai_history_event_t event = {0};
    esp_err_t ret = home_ai_history_peek_unuploaded(&event);
    if (ret == ESP_ERR_NOT_FOUND) return false;
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "HOME_AI_REPLAY status=peek_failed ret=%s", esp_err_to_name(ret));
        return true;
    }
    char request[NETWORK_REPLAY_HOME_AI_BODY_BYTES];
    if (!build_home_ai_replay_json(&event, request, sizeof(request))) {
        ESP_LOGW(TAG,
                 "HOME_AI_REPLAY status=encode_failed seq=%lu event=%s",
                 (unsigned long)event.sequence,
                 event.event_id);
        return true;
    }
    char response[SERVER_CLIENT_SMALL_BODY_BYTES];
    int status = 0;
    ret = server_client_post_home_ai_history_replay(request,
                                                    response,
                                                    sizeof(response),
                                                    &status);
    const bool ok = ret == ESP_OK && status >= 200 && status < 300;
    offline_policy_record_server_result(ret, status);
    gateway_event_reporter_record_server_state(ok);
    if (ok) {
        esp_err_t mark_ret = home_ai_history_mark_uploaded(event.sequence);
        ESP_LOGI(TAG,
                 "HOME_AI_REPLAY status=uploaded seq=%lu remaining=%u mark_ret=%s",
                 (unsigned long)event.sequence,
                 (unsigned int)home_ai_history_pending_count(),
                 esp_err_to_name(mark_ret));
    } else {
        ESP_LOGW(TAG,
                 "HOME_AI_REPLAY status=upload_failed seq=%lu http_status=%d ret=%s",
                 (unsigned long)event.sequence,
                 status,
                 esp_err_to_name(ret));
    }
    return true;
}

typedef struct {
    const char *device_id;
    bool cancelled;
} replay_cancel_ctx_t;

static bool link_stable(void)
{
    return network_worker_get_link_state() == NETWORK_WORKER_LINK_STABLE &&
           network_worker_is_server_ready();
}

static bool replay_cancelled(void *ctx)
{
    replay_cancel_ctx_t *cancel = (replay_cancel_ctx_t *)ctx;
    if (cancel == NULL || cancel->device_id == NULL ||
        !sensor_aggregator_peer_active(cancel->device_id) ||
        !resource_manager_is_live(cancel->device_id)) {
        if (cancel != NULL) {
            cancel->cancelled = true;
        }
        return true;
    }
    return false;
}

static esp_err_t peek_oldest_live_record(bme_cache_record_t *out_record)
{
    if (out_record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_record, 0, sizeof(*out_record));

    char live_ids[GATEWAY_CONFIG_MAX_CHILDREN][RESOURCE_MANAGER_DEVICE_ID_LEN] = {{0}};
    const size_t live_count =
        resource_manager_snapshot_live(live_ids, GATEWAY_CONFIG_MAX_CHILDREN);
    if (live_count == 0U) {
        return bme_cache_manager_size() == 0U ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    }

    bme_cache_record_t selected = {0};
    bool found = false;
    for (size_t i = 0; i < live_count; ++i) {
        bme_cache_record_t candidate = {0};
        esp_err_t ret = bme_cache_manager_peek_oldest_for_device(live_ids[i], &candidate);
        if (ret == ESP_ERR_NOT_FOUND || ret == ESP_ERR_INVALID_STATE) {
            continue;
        }
        if (ret != ESP_OK) {
            bme_cache_manager_release_record(&selected);
            return ret;
        }

        if (!found || candidate.sequence < selected.sequence) {
            bme_cache_manager_release_record(&selected);
            selected = candidate;
            memset(&candidate, 0, sizeof(candidate));
            found = true;
        }
        bme_cache_manager_release_record(&candidate);
    }

    if (!found) {
        return bme_cache_manager_size() == 0U ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    }
    *out_record = selected;
    return ESP_OK;
}

static void replay_worker_task(void *arg)
{
    (void)arg;
    bool replay_active = false;
    bool prefer_home_ai = true;
    uint32_t replayed_in_run = 0U;

    ESP_LOGI(TAG,
             "network replay worker started rate_limit=10/s cache_capacity=%u task_stack_internal=%u",
             (unsigned int)BME_CACHE_MANAGER_CAPACITY,
             (unsigned int)NETWORK_REPLAY_WORKER_TASK_STACK);

    while (1) {
        (void)home_ai_history_flush(NETWORK_REPLAY_HOME_AI_FLUSH_BATCH);
        if (!link_stable()) {
            if (replay_active) {
                ESP_LOGI(TAG,
                         "BME_REPLAY_DONE reason=link_not_stable uploaded=%lu remaining=%u",
                         (unsigned long)replayed_in_run,
                         (unsigned int)bme_cache_manager_size());
                replay_active = false;
                replayed_in_run = 0U;
            }
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(NETWORK_REPLAY_WORKER_IDLE_MS));
            continue;
        }

        if (prefer_home_ai && replay_one_home_ai_event()) {
            prefer_home_ai = false;
            vTaskDelay(pdMS_TO_TICKS(NETWORK_REPLAY_WORKER_RATE_DELAY_MS));
            continue;
        }
        prefer_home_ai = true;

        bme_cache_record_t record = {0};
        esp_err_t ret = peek_oldest_live_record(&record);
        if (ret == ESP_ERR_NOT_FOUND) {
            if (replay_active) {
                ESP_LOGI(TAG,
                         "BME_REPLAY_DONE reason=cache_empty uploaded=%lu remaining=0",
                         (unsigned long)replayed_in_run);
                replay_active = false;
                replayed_in_run = 0U;
            }
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(NETWORK_REPLAY_WORKER_IDLE_MS));
            continue;
        }
        if (ret != ESP_OK) {
            if (ret == ESP_ERR_INVALID_STATE) {
                if (replay_active) {
                    ESP_LOGI(TAG,
                             "BME_REPLAY_DONE reason=no_active_record uploaded=%lu remaining=%u",
                             (unsigned long)replayed_in_run,
                             (unsigned int)bme_cache_manager_size());
                    replay_active = false;
                    replayed_in_run = 0U;
                }
                (void)ulTaskNotifyTake(pdTRUE,
                                       pdMS_TO_TICKS(NETWORK_REPLAY_WORKER_IDLE_MS));
            } else {
                ESP_LOGW(TAG,
                         "BME_REPLAY_PROGRESS status=peek_failed ret=%s",
                         esp_err_to_name(ret));
                vTaskDelay(pdMS_TO_TICKS(NETWORK_REPLAY_WORKER_FAILURE_DELAY_MS));
            }
            continue;
        }

        if (!replay_active) {
            replay_active = true;
            replayed_in_run = 0U;
            ESP_LOGI(TAG,
                     "BME_REPLAY_START pending=%u first_seq=%lu",
                     (unsigned int)bme_cache_manager_size(),
                     (unsigned long)record.sequence);
        }

        ret = bme_cache_manager_mark_in_flight(record.sequence, true);
        if (ret != ESP_OK) {
            bme_cache_manager_release_record(&record);
            vTaskDelay(pdMS_TO_TICKS(NETWORK_REPLAY_WORKER_FAILURE_DELAY_MS));
            continue;
        }

        char response[SERVER_CLIENT_SMALL_BODY_BYTES];
        int status = 0;
        replay_cancel_ctx_t cancel_ctx = {
            .device_id = record.device_id,
        };
        ret = server_client_post_ingest_json_cancellable_for_device(
            record.device_id,
            record.server_json,
            response,
            sizeof(response),
            &status,
            replay_cancelled,
            &cancel_ctx);
        if (cancel_ctx.cancelled) {
            (void)bme_cache_manager_mark_in_flight(record.sequence, false);
            bme_cache_manager_release_record(&record);
            (void)ulTaskNotifyTake(pdTRUE,
                                   pdMS_TO_TICKS(NETWORK_REPLAY_WORKER_IDLE_MS));
            continue;
        }
        const bool ok = ret == ESP_OK && status >= 200 && status < 300;
        offline_policy_record_server_result(ret, status);
        gateway_event_reporter_record_server_state(ok);

        if (ok) {
            esp_err_t delete_ret = bme_cache_manager_delete_sequence(record.sequence);
            if (delete_ret != ESP_OK) {
                (void)bme_cache_manager_mark_in_flight(record.sequence, false);
            }
            ++replayed_in_run;
            const size_t remaining = bme_cache_manager_size();
            if (delete_ret != ESP_OK ||
                replayed_in_run == 1U ||
                replayed_in_run % NETWORK_REPLAY_PROGRESS_LOG_EVERY == 0U ||
                remaining == 0U) {
                ESP_LOGI(TAG,
                         "BME_REPLAY_PROGRESS uploaded=%lu seq=%lu remaining=%u delete_ret=%s",
                         (unsigned long)replayed_in_run,
                         (unsigned long)record.sequence,
                         (unsigned int)remaining,
                         esp_err_to_name(delete_ret));
            }
            bme_cache_manager_release_record(&record);
            vTaskDelay(pdMS_TO_TICKS(NETWORK_REPLAY_WORKER_RATE_DELAY_MS));
            continue;
        }

        ESP_LOGW(TAG,
                 "BME_REPLAY_PROGRESS status=upload_failed seq=%lu http_status=%d ret=%s remaining=%u",
                 (unsigned long)record.sequence,
                 status,
                 esp_err_to_name(ret),
                 (unsigned int)bme_cache_manager_size());
        (void)bme_cache_manager_mark_in_flight(record.sequence, false);
        bme_cache_manager_release_record(&record);
        vTaskDelay(pdMS_TO_TICKS(NETWORK_REPLAY_WORKER_FAILURE_DELAY_MS));
    }
}

esp_err_t network_replay_worker_init(void)
{
    if (s_replay_task != NULL) {
        return ESP_OK;
    }

    BaseType_t created = xTaskCreateWithCaps(replay_worker_task,
                                             "bme_replay_worker",
                                             NETWORK_REPLAY_WORKER_TASK_STACK,
                                             NULL,
                                             NETWORK_REPLAY_WORKER_TASK_PRIORITY,
                                             &s_replay_task,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        s_replay_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void network_replay_worker_request_bme_replay(void)
{
    if (s_replay_task != NULL) {
        xTaskNotifyGive(s_replay_task);
    }
}

void network_replay_worker_request_home_ai_replay(void)
{
    if (s_replay_task != NULL) {
        xTaskNotifyGive(s_replay_task);
    }
}

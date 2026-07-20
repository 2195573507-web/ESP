/**
 * @file network_replay_worker.c
 * @brief Replays cached BME690 Server ingest JSON after LINK_STABLE.
 */

#include "network_replay_worker.h"

#include <stdbool.h>
#include <string.h>

#include "bme_cache_manager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "gateway_config.h"
#include "gateway_event_reporter.h"
#include "network_worker.h"
#include "offline_policy.h"
#include "resource_manager.h"
#include "sensor_aggregator.h"
#include "server_client.h"

static const char *TAG = "network_replay_worker";

#ifndef NETWORK_REPLAY_WORKER_TASK_STACK
#define NETWORK_REPLAY_WORKER_TASK_STACK 8192U
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

static TaskHandle_t s_replay_task;

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
    uint32_t replayed_in_run = 0U;

    ESP_LOGI(TAG,
             "network replay worker started rate_limit=10/s cache_capacity=%u",
             (unsigned int)BME_CACHE_MANAGER_CAPACITY);

    while (1) {
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
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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

/**
 * @file gateway_event_reporter.c
 * @brief S3 网关 system log / alarm 上报实现。
 */

#include "gateway_event_reporter.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp111_protocol_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gateway_config.h"
#include "network_worker.h"
#include "offline_policy.h"

static const char *TAG = "gateway_event";

#define GATEWAY_EVENT_MIN_INTERVAL_MS 10000

static int64_t s_last_event_ms;
static bool s_had_server_state;
static bool s_last_server_available;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void gateway_event_reporter_init(void)
{
    s_last_event_ms = 0;
    s_had_server_state = false;
    s_last_server_available = false;
}

static bool event_rate_limited(void)
{
    int64_t timestamp_ms = now_ms();
    if (s_last_event_ms > 0 &&
        timestamp_ms - s_last_event_ms < GATEWAY_EVENT_MIN_INTERVAL_MS) {
        return true;
    }
    s_last_event_ms = timestamp_ms;
    return false;
}

static esp_err_t post_json(bool alarm, char *json)
{
    esp_err_t ret = network_worker_submit_server_json(alarm ?
                                                          NETWORK_WORKER_SERVER_JSON_ALARM :
                                                          NETWORK_WORKER_SERVER_JSON_SYSTEM_LOG,
                                                      json,
                                                      alarm ? "gateway_alarm" : "gateway_event");
    if (ret != ESP_OK) {
        offline_policy_record_server_result(ret, 0);
        ESP_LOGW(TAG,
                 "%s upload enqueue failed ret=%s",
                 alarm ? "alarm" : "system log",
                 esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t gateway_event_reporter_system(const char *device_id,
                                        const char *level,
                                        const char *message,
                                        const char *reason)
{
    if (event_rate_limited()) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root,
                            "device_id",
                            device_id != NULL && device_id[0] != '\0' ?
                                device_id :
                                gateway_config_get()->gateway_id);
    cJSON_AddStringToObject(root, "level", level != NULL && level[0] != '\0' ? level : "info");
    cJSON_AddStringToObject(root,
                            "message",
                            message != NULL && message[0] != '\0' ? message : "gateway event");
    cJSON *payload = cJSON_AddObjectToObject(root, "payload");
    cJSON_AddStringToObject(payload, "reason", reason != NULL ? reason : "");
    cJSON_AddStringToObject(payload, "gateway_id", gateway_config_get()->gateway_id);
    cJSON_AddStringToObject(root, "source", "s3_gateway");

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = post_json(false, json);
    if (ret != ESP_OK) {
        cJSON_free(json);
    }
    return ret;
}

esp_err_t gateway_event_reporter_alarm(const char *device_id,
                                       const char *level,
                                       const char *title,
                                       const char *message,
                                       const char *reason)
{
    if (event_rate_limited()) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root,
                            "device_id",
                            device_id != NULL && device_id[0] != '\0' ?
                                device_id :
                                gateway_config_get()->gateway_id);
    cJSON_AddStringToObject(root, "level", level != NULL && level[0] != '\0' ? level : "warning");
    cJSON_AddStringToObject(root,
                            "title",
                            title != NULL && title[0] != '\0' ? title : "gateway alarm");
    cJSON_AddStringToObject(root,
                            "message",
                            message != NULL && message[0] != '\0' ? message : "gateway alarm");
    cJSON_AddBoolToObject(root, "acknowledged", false);
    cJSON *payload = cJSON_AddObjectToObject(root, "payload");
    cJSON_AddStringToObject(payload, "reason", reason != NULL ? reason : "");
    cJSON_AddStringToObject(payload, "gateway_id", gateway_config_get()->gateway_id);
    cJSON_AddStringToObject(root, "source", "s3_gateway");

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = post_json(true, json);
    if (ret != ESP_OK) {
        cJSON_free(json);
    }
    return ret;
}

void gateway_event_reporter_record_server_state(bool available)
{
    if (!s_had_server_state) {
        s_had_server_state = true;
        s_last_server_available = available;
        return;
    }
    if (s_last_server_available == available) {
        return;
    }
    s_last_server_available = available;
    (void)gateway_event_reporter_system(gateway_config_get()->gateway_id,
                                        available ? "info" : "warning",
                                        available ? "server uplink recovered" :
                                                    "server uplink unavailable",
                                        offline_policy_last_error_code());
}

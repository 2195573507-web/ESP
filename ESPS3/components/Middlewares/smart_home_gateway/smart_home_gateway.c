/**
 * @file smart_home_gateway.c
 * @brief S3 智能家居 pending/ack 网关实现。
 */

#include "smart_home_gateway.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp111_protocol_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gateway_config.h"
#include "gateway_event_reporter.h"
#include "offline_policy.h"
#include "sensor_aggregator.h"
#include "server_client.h"
#include "voice_proxy.h"

static const char *TAG = "smart_home_gateway";

esp_err_t smart_home_gateway_init(void)
{
    ESP_LOGI(TAG, "smart-home gateway initialized real_device_attached=0");
    return ESP_OK;
}

static cJSON *read_commands_array(cJSON *root)
{
    if (root == NULL) {
        return NULL;
    }

    cJSON *commands = cJSON_GetObjectItemCaseSensitive(root, "commands");
    if (cJSON_IsArray(commands)) {
        return commands;
    }

    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    commands = cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "commands") : NULL;
    return cJSON_IsArray(commands) ? commands : NULL;
}

static const char *json_string(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring != NULL ? item->valuestring : "";
}

static esp_err_t ack_failed_command(const char *command_id, const char *target_id, const char *action)
{
    if (command_id == NULL || command_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t executed_at_ms = esp_timer_get_time() / 1000;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "status", "failed");
    cJSON_AddNumberToObject(root, "executed_at_ms", (double)executed_at_ms);
    cJSON *result = cJSON_AddObjectToObject(root, "result");
    cJSON_AddBoolToObject(result, "applied", false);
    cJSON_AddStringToObject(result, "gateway_id", gateway_config_get()->gateway_id);
    cJSON_AddStringToObject(result, "reason", "no_real_smart_home_device_attached");
    cJSON_AddStringToObject(root, "error_message", "no real smart-home device attached");

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char response[SERVER_CLIENT_SMALL_BODY_BYTES];
    int status = 0;
    esp_err_t ret = server_client_ack_smart_home_command(command_id,
                                                         body,
                                                         response,
                                                         sizeof(response),
                                                         &status);
    cJSON_free(body);
    offline_policy_record_server_result(ret, status);
    sensor_aggregator_record_command_ack(target_id != NULL && target_id[0] != '\0' ?
                                             target_id :
                                             gateway_config_get()->gateway_id,
                                         command_id,
                                         0,
                                         false);
    (void)gateway_event_reporter_alarm(target_id,
                                       "warning",
                                       "Smart-home command failed",
                                       action != NULL && action[0] != '\0' ?
                                           action :
                                           "smart-home command failed",
                                       "no real smart-home device attached");
    ESP_LOGI(TAG,
             "smart-home command ack failed id=%s target=%s status=%d ret=%s",
             command_id,
             target_id != NULL ? target_id : "",
             status,
             esp_err_to_name(ret));
    return ret;
}

void smart_home_gateway_poll_once(void)
{
    if (voice_proxy_should_skip_non_voice_task("smart-home pending poll")) {
        return;
    }

    char body[SERVER_CLIENT_SMALL_BODY_BYTES];
    int status = 0;
    esp_err_t ret = server_client_get_smart_home_pending(body, sizeof(body), &status);
    offline_policy_record_server_result(ret, status);
    gateway_event_reporter_record_server_state(ret == ESP_OK && status >= 200 && status < 300);
    if (ret != ESP_OK || status < 200 || status >= 300 || body[0] == '\0') {
        ESP_LOGD(TAG, "smart-home pending unavailable status=%d ret=%s", status, esp_err_to_name(ret));
        return;
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        ESP_LOGW(TAG, "smart-home pending response parse failed");
        return;
    }

    cJSON *commands = read_commands_array(root);
    if (commands == NULL) {
        cJSON_Delete(root);
        return;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, commands) {
        const char *command_id = json_string(item, "command_id");
        const char *target_id = json_string(item, "target_id");
        const char *action = json_string(item, "action");
        if (command_id[0] == '\0') {
            continue;
        }
        (void)ack_failed_command(command_id, target_id, action);
    }

    cJSON_Delete(root);
}

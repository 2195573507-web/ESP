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
#include "home_ai_event_reporter.h"
#include "home_ai_user_override.h"
#include "home_ai_virtual_device.h"
#include "home_ai_rule_engine.h"
#include "offline_policy.h"
#include "sensor_aggregator.h"
#include "server_client.h"

static const char *TAG = "smart_home_gateway";

static esp_err_t ack_failed_command(const char *command_id,
                                    const char *target_id,
                                    const char *action);

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
    if (root == NULL || key == NULL) return "";
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring != NULL ? item->valuestring : "";
}

static cJSON *command_params(cJSON *root)
{
    cJSON *params = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, "params") : NULL;
    return cJSON_IsObject(params) ? params : NULL;
}

static const char *command_string(cJSON *root, const char *key)
{
    const char *value = json_string(root, key);
    if (value[0] != '\0') return value;
    return json_string(command_params(root), key);
}

static uint64_t command_u64(cJSON *root, const char *key)
{
    cJSON *value = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, key) : NULL;
    if (!cJSON_IsNumber(value)) {
        cJSON *params = command_params(root);
        value = params != NULL ? cJSON_GetObjectItemCaseSensitive(params, key) : NULL;
    }
    if (!cJSON_IsNumber(value) || value->valuedouble < 1.0 || value->valuedouble > 9007199254740991.0) {
        return 0U;
    }
    return (uint64_t)value->valuedouble;
}

static bool infer_device_type(const char *device_id, home_ai_rule_device_type_t *out)
{
    if (device_id == NULL || out == NULL) return false;
    if (strstr(device_id, "_air_conditioner") != NULL) {
        *out = HOME_AI_RULE_DEVICE_AIR_CONDITIONER;
        return true;
    }
    if (strstr(device_id, "_fan") != NULL) {
        *out = HOME_AI_RULE_DEVICE_FAN;
        return true;
    }
    if (strstr(device_id, "_light") != NULL) {
        *out = HOME_AI_RULE_DEVICE_LIGHT;
        return true;
    }
    return false;
}

static bool parse_device_type(const char *text,
                              const char *device_id,
                              home_ai_rule_device_type_t *out)
{
    if (text != NULL && strcmp(text, "light") == 0) {
        *out = HOME_AI_RULE_DEVICE_LIGHT;
        return true;
    }
    if (text != NULL && strcmp(text, "air_conditioner") == 0) {
        *out = HOME_AI_RULE_DEVICE_AIR_CONDITIONER;
        return true;
    }
    if (text != NULL && strcmp(text, "fan") == 0) {
        *out = HOME_AI_RULE_DEVICE_FAN;
        return true;
    }
    return infer_device_type(device_id, out);
}

static bool infer_room_id(const char *device_id, char *out, size_t out_size)
{
    if (device_id == NULL || out == NULL || out_size < 2U) return false;
    const char *suffix = strstr(device_id, "_air_conditioner");
    if (suffix == NULL) suffix = strstr(device_id, "_light");
    if (suffix == NULL) suffix = strstr(device_id, "_fan");
    if (suffix == NULL || suffix == device_id) return false;
    const size_t length = (size_t)(suffix - device_id);
    if (length + 1U > out_size) return false;
    memcpy(out, device_id, length);
    out[length] = '\0';
    return true;
}

static bool is_virtual_command(cJSON *item)
{
    const char *action = command_string(item, "action");
    if (strcmp(action, "turn_on") != 0 && strcmp(action, "turn_off") != 0) return false;
    const char *target_id = command_string(item, "target_id");
    home_ai_rule_device_type_t type = HOME_AI_RULE_DEVICE_LIGHT;
    return target_id[0] != '\0' && parse_device_type(command_string(item, "device_type"), target_id, &type);
}

static esp_err_t ack_virtual_command(cJSON *item)
{
    const char *command_id = json_string(item, "command_id");
    const char *target_id = command_string(item, "target_id");
    const char *action = command_string(item, "action");
    if (command_id[0] == '\0' || target_id[0] == '\0' || action[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char room_id[HOME_AI_RULE_ROOM_ID_LEN] = {0};
    const char *configured_room = command_string(item, "room_id");
    if (configured_room[0] != '\0') {
        strlcpy(room_id, configured_room, sizeof(room_id));
    } else if (!infer_room_id(target_id, room_id, sizeof(room_id))) {
        return ack_failed_command(command_id, target_id, action);
    }
    home_ai_rule_device_type_t device_type = HOME_AI_RULE_DEVICE_LIGHT;
    if (!parse_device_type(command_string(item, "device_type"), target_id, &device_type)) {
        return ack_failed_command(command_id, target_id, action);
    }

    const uint64_t executed_at_ms = (uint64_t)(esp_timer_get_time() / 1000);
    home_ai_rule_decision_t decision = {0};
    strlcpy(decision.decision_id, command_id, sizeof(decision.decision_id));
    strlcpy(decision.rule_id, "user_command", sizeof(decision.rule_id));
    strlcpy(decision.room_id, room_id, sizeof(decision.room_id));
    strlcpy(decision.device_id, target_id, sizeof(decision.device_id));
    decision.device_type = device_type;
    decision.action = strcmp(action, "turn_on") == 0 ? HOME_AI_RULE_ACTION_TURN_ON :
                                                          HOME_AI_RULE_ACTION_TURN_OFF;
    decision.state = HOME_AI_RULE_DECISION_EXECUTE;
    decision.priority = 900U;

    home_ai_virtual_device_execution_t execution = {0};
    esp_err_t execute_ret = home_ai_virtual_device_execute(&decision,
                                                           executed_at_ms,
                                                           true,
                                                           &execution);
    const bool applied = execute_ret == ESP_OK &&
                         execution.result != HOME_AI_VIRTUAL_EXECUTION_REJECTED &&
                         execution.result != HOME_AI_VIRTUAL_EXECUTION_DEFERRED_MINIMUM;
    if (execution.reason[0] == '\0') {
        strlcpy(execution.reason,
                execute_ret == ESP_OK ? "virtual_state_written" : "virtual_execution_failed",
                sizeof(execution.reason));
    }
    if (applied) {
        home_ai_user_override_t override = {0};
        (void)snprintf(override.override_id,
                       sizeof(override.override_id),
                       "cmd_%s",
                       command_id);
        strlcpy(override.room_id, room_id, sizeof(override.room_id));
        strlcpy(override.device_id, target_id, sizeof(override.device_id));
        override.action = decision.action == HOME_AI_RULE_ACTION_TURN_ON ?
                              HOME_AI_OVERRIDE_KEEP_ON : HOME_AI_OVERRIDE_KEEP_OFF;
        override.priority = 900U;
        override.created_at_ms = executed_at_ms;
        override.expires_at_ms = command_u64(item, "expires_at_ms");
        if (override.expires_at_ms == 0U) {
            override.expires_at_ms = executed_at_ms + 600000U;
        }
        override.allow_safety_override = true;
        const esp_err_t override_ret = home_ai_user_override_upsert(&override);
        if (override_ret == ESP_OK) {
            home_ai_event_reporter_record_user_override(&override, target_id, executed_at_ms);
        } else {
            ESP_LOGW(TAG,
                     "virtual command override unavailable command_id=%s ret=%s",
                     command_id,
                     esp_err_to_name(override_ret));
        }
    }
    home_ai_event_reporter_record_decision(&decision, &execution, executed_at_ms);
    if (execute_ret == ESP_OK && execution.state.valid) {
        home_ai_event_reporter_record_virtual_state(&execution.state, executed_at_ms);
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "status", applied ? "succeeded" : "failed");
    cJSON_AddNumberToObject(root, "executed_at_ms", (double)executed_at_ms);
    cJSON *result = cJSON_AddObjectToObject(root, "result");
    cJSON_AddBoolToObject(result, "applied", applied);
    cJSON_AddStringToObject(result, "gateway_id", gateway_config_get()->gateway_id);
    cJSON_AddStringToObject(result, "device_id", target_id);
    cJSON_AddStringToObject(result, "room_id", room_id);
    cJSON_AddStringToObject(result, "device_type", home_ai_rule_device_type_name(device_type));
    cJSON_AddStringToObject(result, "action", action);
    cJSON_AddStringToObject(result, "power", home_ai_virtual_power_name(execution.state.power));
    cJSON_AddStringToObject(result, "execution_mode", "virtual");
    cJSON_AddBoolToObject(result,
                         "verified",
                         execution.state.valid && execution.state.verified);
    cJSON_AddStringToObject(result, "reason", execution.reason);
    if (!applied) cJSON_AddStringToObject(root, "error_message", execution.reason);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) return ESP_ERR_NO_MEM;

    char response[SERVER_CLIENT_SMALL_BODY_BYTES];
    int status = 0;
    esp_err_t ret = server_client_ack_smart_home_command(command_id,
                                                         body,
                                                         response,
                                                         sizeof(response),
                                                         &status);
    cJSON_free(body);
    offline_policy_record_server_result(ret, status);
    sensor_aggregator_record_command_ack(target_id,
                                         command_id,
                                         0U,
                                         applied);
    if (applied) {
        (void)gateway_event_reporter_system(target_id,
                                             "info",
                                             "virtual smart-home command applied",
                                             action);
    } else {
        (void)gateway_event_reporter_alarm(target_id,
                                            "warning",
                                            "Virtual smart-home command failed",
                                            action,
                                            execution.reason);
    }
    ESP_LOGI(TAG,
             "virtual smart-home command id=%s target=%s action=%s applied=%d status=%d ret=%s",
             command_id,
             target_id,
             action,
             applied ? 1 : 0,
             status,
             esp_err_to_name(ret));
    return ret;
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
        if (is_virtual_command(item)) {
            (void)ack_virtual_command(item);
        } else {
            (void)ack_failed_command(command_id, target_id, action);
        }
    }

    cJSON_Delete(root);
}

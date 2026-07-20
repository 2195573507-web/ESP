#include "home_ai_local_handler.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp111_protocol_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "home_ai_local_voice_command.h"
#include "home_ai_room_state.h"
#include "home_ai_voice_router.h"
#include "home_ai_voice_session.h"
#include "protocol_adapter.h"

static const char *TAG = "home_ai_http";

/* No heap allocation is permitted in this handler. Keep the protocol payload small. */
#define HOME_AI_VOICE_HTTP_MAX_BODY 384U
#define HOME_AI_VOICE_HTTP_RESPONSE_BYTES 320U
#define HOME_AI_VOICE_TEMPORARY_AWAKE_MS 600000U

static bool room_for_voice_terminal(const char *device_id, char *out, size_t out_size);

static const char *skip_ws(const char *cursor)
{
    while (cursor != NULL && *cursor != '\0' && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    return cursor;
}

static const char *find_value(const char *json, const char *key)
{
    if (json == NULL || key == NULL || key[0] == '\0') {
        return NULL;
    }

    char needle[64];
    int written = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (written <= 0 || written >= (int)sizeof(needle)) {
        return NULL;
    }

    const char *cursor = json;
    while ((cursor = strstr(cursor, needle)) != NULL) {
        cursor = skip_ws(cursor + strlen(needle));
        if (cursor != NULL && *cursor == ':') {
            return skip_ws(cursor + 1);
        }
    }
    return NULL;
}

static bool json_string(const char *json, const char *key, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return false;
    }
    out[0] = '\0';
    const char *cursor = find_value(json, key);
    if (cursor == NULL || *cursor != '\"') {
        return false;
    }

    cursor++;
    size_t length = 0U;
    while (*cursor != '\0' && *cursor != '\"') {
        unsigned char value = (unsigned char)*cursor++;
        if (value < 0x20U || value == '\\' || length + 1U >= out_size) {
            return false;
        }
        out[length++] = (char)value;
    }
    if (*cursor != '\"' || length == 0U) {
        return false;
    }
    out[length] = '\0';
    return true;
}

static bool json_uint(const char *json, const char *key, uint32_t *out)
{
    if (out == NULL) {
        return false;
    }
    const char *cursor = find_value(json, key);
    if (cursor == NULL || !isdigit((unsigned char)*cursor)) {
        return false;
    }

    uint32_t value = 0U;
    do {
        const uint32_t digit = (uint32_t)(*cursor - '0');
        if (value > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
        cursor++;
    } while (isdigit((unsigned char)*cursor));
    *out = value;
    return true;
}

static bool local_device_id(const char *json, char *out, size_t out_size)
{
    char supplied[HOME_AI_VOICE_OWNER_ID_LEN] = {0};
    uint32_t local_id = 0U;

    if (json_string(json, ESP111_PROTOCOL_LOCAL_JSON_ID, supplied, sizeof(supplied)) ||
        json_string(json, ESP111_PROTOCOL_LOCAL_JSON_LOCAL_ID, supplied, sizeof(supplied))) {
        local_id = protocol_adapter_device_id_to_local_id(supplied);
    } else if (json_uint(json, ESP111_PROTOCOL_LOCAL_JSON_ID, &local_id) ||
               json_uint(json, ESP111_PROTOCOL_LOCAL_JSON_LOCAL_ID, &local_id)) {
        /* Numeric local ID is supported for older C5 control clients. */
    } else {
        return false;
    }

    const char *device_id = protocol_adapter_local_device_id_to_device_id((uint8_t)local_id);
    if (device_id == NULL || device_id[0] == '\0' || strlen(device_id) >= out_size) {
        return false;
    }
    strlcpy(out, device_id, out_size);
    return true;
}

static esp_err_t respond_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status);
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t respond_error(httpd_req_t *req, const char *status, const char *code)
{
    char body[HOME_AI_VOICE_HTTP_RESPONSE_BYTES];
    int written = snprintf(body,
                           sizeof(body),
                           "{\"ok\":false,\"e\":\"%s\"}",
                           code != NULL ? code : ESP111_PROTOCOL_ERROR_INTERNAL);
    if (written <= 0 || written >= (int)sizeof(body)) {
        return ESP_FAIL;
    }
    return respond_json(req, status, body);
}

static esp_err_t respond_lease(httpd_req_t *req,
                               const char *status,
                               bool ok,
                               const char *code,
                               const home_ai_voice_session_lease_t *lease)
{
    char body[HOME_AI_VOICE_HTTP_RESPONSE_BYTES];
    int written = 0;
    if (ok && lease != NULL) {
        written = snprintf(body,
                           sizeof(body),
                           "{\"ok\":true,\"voice_session_id\":\"%s\",\"owner_device_id\":\"%s\",\"generation\":%lu,\"lease_expires_at_ms\":%lld,\"state\":\"%s\"}",
                           lease->voice_session_id,
                           lease->owner_device_id,
                           (unsigned long)lease->generation,
                           (long long)lease->lease_expires_at_ms,
                           home_ai_voice_session_state_name(lease->state));
    } else {
        written = snprintf(body,
                           sizeof(body),
                           "{\"ok\":false,\"e\":\"%s\",\"owner_device_id\":\"%s\",\"generation\":%lu}",
                           code != NULL ? code : ESP111_PROTOCOL_ERROR_VOICE_SESSION_INVALID,
                           lease != NULL ? lease->owner_device_id : "",
                           (unsigned long)(lease != NULL ? lease->generation : 0U));
    }
    if (written <= 0 || written >= (int)sizeof(body)) {
        return respond_error(req, "500 Internal Server Error", ESP111_PROTOCOL_ERROR_INTERNAL);
    }
    return respond_json(req, status, body);
}

static esp_err_t read_body(httpd_req_t *req, char *body, size_t body_size)
{
    if (req == NULL || body == NULL || body_size == 0U || req->content_len <= 0 ||
        (size_t)req->content_len >= body_size || (size_t)req->content_len > HOME_AI_VOICE_HTTP_MAX_BODY) {
        return ESP_ERR_INVALID_SIZE;
    }

    int received = 0;
    while (received < req->content_len) {
        const int read = httpd_req_recv(req, body + received, req->content_len - received);
        if (read <= 0) {
            return read == HTTPD_SOCK_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
        received += read;
    }
    body[received] = '\0';
    return ESP_OK;
}

esp_err_t home_ai_voice_session_handler(httpd_req_t *req)
{
    char body[HOME_AI_VOICE_HTTP_MAX_BODY + 1U] = {0};
    esp_err_t ret = read_body(req, body, sizeof(body));
    if (ret != ESP_OK) {
        return respond_error(req,
                             ret == ESP_ERR_TIMEOUT ? "408 Request Timeout" : "400 Bad Request",
                             ret == ESP_ERR_TIMEOUT ? ESP111_PROTOCOL_ERROR_TIMEOUT :
                                                       ESP111_PROTOCOL_ERROR_INVALID_COMMAND_PAYLOAD);
    }

    char device_id[HOME_AI_VOICE_OWNER_ID_LEN] = {0};
    char action[32] = {0};
    if (!local_device_id(body, device_id, sizeof(device_id)) ||
        !json_string(body, ESP111_PROTOCOL_LOCAL_JSON_VOICE_ACTION, action, sizeof(action))) {
        return respond_error(req, "400 Bad Request", ESP111_PROTOCOL_ERROR_INVALID_DEVICE_ID);
    }

    home_ai_voice_session_lease_t lease = {0};
    if (strcmp(action, ESP111_PROTOCOL_LOCAL_VOICE_ACTION_ACQUIRE) == 0) {
        ret = home_ai_voice_session_acquire(device_id, &lease);
        if (ret == ESP_OK) {
            char room_id[HOME_AI_ROOM_STATE_ROOM_ID_LEN] = {0};
            if (room_for_voice_terminal(device_id, room_id, sizeof(room_id))) {
                (void)home_ai_voice_router_set_temporary_awake(
                    room_id,
                    HOME_AI_VOICE_TEMPORARY_AWAKE_MS,
                    (uint64_t)(esp_timer_get_time() / 1000));
            }
            ESP_LOGI(TAG, "voice session acquired owner=%s generation=%lu",
                     device_id, (unsigned long)lease.generation);
            return respond_lease(req, "200 OK", true, NULL, &lease);
        }
        if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGI(TAG, "voice session busy requester=%s owner=%s", device_id, lease.owner_device_id);
            return respond_lease(req,
                                 "409 Conflict",
                                 false,
                                 ESP111_PROTOCOL_ERROR_VOICE_SESSION_BUSY,
                                 &lease);
        }
        return respond_error(req, "503 Service Unavailable", ESP111_PROTOCOL_ERROR_TIMEOUT);
    }

    char session_id[HOME_AI_VOICE_SESSION_ID_LEN] = {0};
    uint32_t generation = 0U;
    if (!json_string(body,
                     ESP111_PROTOCOL_LOCAL_JSON_VOICE_SESSION_ID,
                     session_id,
                     sizeof(session_id)) ||
        !json_uint(body, ESP111_PROTOCOL_LOCAL_JSON_VOICE_GENERATION, &generation) || generation == 0U) {
        return respond_error(req, "400 Bad Request", ESP111_PROTOCOL_ERROR_VOICE_SESSION_INVALID);
    }

    if (strcmp(action, ESP111_PROTOCOL_LOCAL_VOICE_ACTION_RENEW) == 0) {
        ret = home_ai_voice_session_renew(device_id, session_id, generation, &lease);
        return ret == ESP_OK ? respond_lease(req, "200 OK", true, NULL, &lease) :
                               respond_error(req, "409 Conflict", ESP111_PROTOCOL_ERROR_VOICE_SESSION_EXPIRED);
    }
    if (strcmp(action, ESP111_PROTOCOL_LOCAL_VOICE_ACTION_STATE) == 0) {
        char state_name[24] = {0};
        home_ai_voice_session_state_t next_state = HOME_AI_VOICE_SESSION_IDLE;
        if (!json_string(body,
                         ESP111_PROTOCOL_LOCAL_JSON_VOICE_STATE,
                         state_name,
                         sizeof(state_name)) ||
            !home_ai_voice_session_state_from_name(state_name, &next_state)) {
            return respond_error(req, "400 Bad Request", ESP111_PROTOCOL_ERROR_INVALID_COMMAND_PAYLOAD);
        }
        ret = home_ai_voice_session_transition(device_id,
                                               session_id,
                                               generation,
                                               next_state,
                                               &lease);
        return ret == ESP_OK ? respond_lease(req, "200 OK", true, NULL, &lease) :
                               respond_error(req, "409 Conflict", ESP111_PROTOCOL_ERROR_VOICE_SESSION_INVALID);
    }
    if (strcmp(action, ESP111_PROTOCOL_LOCAL_VOICE_ACTION_RELEASE) == 0) {
        ret = home_ai_voice_session_release(device_id, session_id, generation);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "voice session released owner=%s generation=%lu",
                     device_id, (unsigned long)generation);
            return respond_json(req, "200 OK", "{\"ok\":true}");
        }
        return respond_error(req, "409 Conflict", ESP111_PROTOCOL_ERROR_VOICE_SESSION_INVALID);
    }

    return respond_error(req, "400 Bad Request", ESP111_PROTOCOL_ERROR_UNSUPPORTED_COMMAND);
}

static bool room_for_voice_terminal(const char *device_id, char *out, size_t out_size)
{
    if (device_id == NULL || out == NULL || out_size == 0U) return false;
    out[0] = '\0';
    for (radar_source_id_t source = RADAR_SOURCE_S3_LOCAL;
         source < RADAR_SOURCE_COUNT;
         source = (radar_source_id_t)(source + 1)) {
        home_ai_room_state_config_t config = {0};
        if (home_ai_room_state_get_config(source, &config) &&
            strcmp(config.voice_terminal_device_id, device_id) == 0 &&
            strlen(config.room_id) < out_size) {
            strlcpy(out, config.room_id, out_size);
            return true;
        }
    }
    return false;
}

esp_err_t home_ai_offline_voice_command_handler(httpd_req_t *req)
{
    char body[HOME_AI_VOICE_HTTP_MAX_BODY + 1U] = {0};
    esp_err_t ret = read_body(req, body, sizeof(body));
    if (ret != ESP_OK) {
        return respond_error(req,
                             ret == ESP_ERR_TIMEOUT ? "408 Request Timeout" : "400 Bad Request",
                             ret == ESP_ERR_TIMEOUT ? ESP111_PROTOCOL_ERROR_TIMEOUT :
                                                       ESP111_PROTOCOL_ERROR_INVALID_COMMAND_PAYLOAD);
    }
    char device_id[HOME_AI_VOICE_OWNER_ID_LEN] = {0};
    char command_text[HOME_AI_LOCAL_VOICE_COMMAND_TEXT_LEN] = {0};
    char room_id[HOME_AI_ROOM_STATE_ROOM_ID_LEN] = {0};
    if (!local_device_id(body, device_id, sizeof(device_id)) ||
        !json_string(body,
                     ESP111_PROTOCOL_LOCAL_JSON_VOICE_COMMAND_TEXT,
                     command_text,
                     sizeof(command_text))) {
        return respond_error(req, "400 Bad Request", ESP111_PROTOCOL_ERROR_INVALID_COMMAND_PAYLOAD);
    }
    if (!room_for_voice_terminal(device_id, room_id, sizeof(room_id))) {
        return respond_error(req, "409 Conflict", ESP111_PROTOCOL_ERROR_INVALID_DEVICE_ID);
    }

    home_ai_local_voice_command_t command = {0};
    if (!home_ai_local_voice_command_parse(command_text, room_id, &command)) {
        return respond_error(req, "400 Bad Request", ESP111_PROTOCOL_ERROR_UNSUPPORTED_COMMAND);
    }
    home_ai_local_voice_command_result_t result = {0};
    ret = home_ai_local_voice_command_execute(&command,
                                              (uint64_t)(esp_timer_get_time() / 1000),
                                              &result);
    if (ret != ESP_OK) {
        return respond_error(req,
                             ret == ESP_ERR_NO_MEM ? "503 Service Unavailable" : "409 Conflict",
                             ret == ESP_ERR_NO_MEM ? ESP111_PROTOCOL_ERROR_TIMEOUT :
                                                     ESP111_PROTOCOL_ERROR_COMMAND_FAILED);
    }
    char response[HOME_AI_VOICE_HTTP_RESPONSE_BYTES];
    const int written = snprintf(response,
                                 sizeof(response),
                                 "{\"ok\":true,\"command\":\"%s\",\"room_id\":\"%s\","
                                 "\"device_id\":\"%s\",\"applied\":%s,\"reason\":\"%s\"}",
                                 home_ai_local_voice_command_name(result.command),
                                 result.room_id,
                                 result.device_id,
                                 result.applied ? "true" : "false",
                                 result.reason);
    if (written <= 0 || written >= (int)sizeof(response)) {
        return respond_error(req, "500 Internal Server Error", ESP111_PROTOCOL_ERROR_INTERNAL);
    }
    ESP_LOGI(TAG,
             "offline voice command applied device=%s room=%s command=%s",
             device_id,
             result.room_id,
             home_ai_local_voice_command_name(result.command));
    return respond_json(req, "200 OK", response);
}

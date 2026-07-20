#include "c5_voice_session_client.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "device_protocol_metadata.h"
#include "esp111_protocol_common.h"
#include "esp_log.h"
#include "server_comm_config.h"
#include "server_comm_http.h"

static const char *TAG = "c5_voice_session";

/* Fixed protocol buffers keep this small control request out of the PCM resource budget. */
#define C5_VOICE_SESSION_REQUEST_BYTES 320U
#define C5_VOICE_SESSION_RESPONSE_BYTES 384U
#define C5_VOICE_SESSION_TIMEOUT_MS 2500U

static bool s_initialized;

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
        const unsigned char value = (unsigned char)*cursor++;
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

static bool json_uint32(const char *json, const char *key, uint32_t *out)
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

static bool json_int64(const char *json, const char *key, int64_t *out)
{
    if (out == NULL) {
        return false;
    }
    const char *cursor = find_value(json, key);
    if (cursor == NULL || !isdigit((unsigned char)*cursor)) {
        return false;
    }
    uint64_t value = 0U;
    do {
        const uint64_t digit = (uint64_t)(*cursor - '0');
        if (value > ((uint64_t)INT64_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
        cursor++;
    } while (isdigit((unsigned char)*cursor));
    *out = (int64_t)value;
    return true;
}

static const char *state_name(c5_voice_session_state_t state)
{
    switch (state) {
    case C5_VOICE_SESSION_STATE_LOCKED: return ESP111_PROTOCOL_LOCAL_VOICE_STATE_LOCKED;
    case C5_VOICE_SESSION_STATE_RECORDING: return ESP111_PROTOCOL_LOCAL_VOICE_STATE_RECORDING;
    case C5_VOICE_SESSION_STATE_WAITING_SERVER: return ESP111_PROTOCOL_LOCAL_VOICE_STATE_WAITING_SERVER;
    case C5_VOICE_SESSION_STATE_PLAYING: return ESP111_PROTOCOL_LOCAL_VOICE_STATE_PLAYING;
    case C5_VOICE_SESSION_STATE_ENDING: return ESP111_PROTOCOL_LOCAL_VOICE_STATE_ENDING;
    default: return NULL;
    }
}

static c5_voice_session_state_t state_from_name(const char *name)
{
    if (name == NULL) return C5_VOICE_SESSION_STATE_INVALID;
    if (strcmp(name, ESP111_PROTOCOL_LOCAL_VOICE_STATE_LOCKED) == 0) return C5_VOICE_SESSION_STATE_LOCKED;
    if (strcmp(name, ESP111_PROTOCOL_LOCAL_VOICE_STATE_RECORDING) == 0) return C5_VOICE_SESSION_STATE_RECORDING;
    if (strcmp(name, ESP111_PROTOCOL_LOCAL_VOICE_STATE_WAITING_SERVER) == 0) return C5_VOICE_SESSION_STATE_WAITING_SERVER;
    if (strcmp(name, ESP111_PROTOCOL_LOCAL_VOICE_STATE_PLAYING) == 0) return C5_VOICE_SESSION_STATE_PLAYING;
    if (strcmp(name, ESP111_PROTOCOL_LOCAL_VOICE_STATE_ENDING) == 0) return C5_VOICE_SESSION_STATE_ENDING;
    return C5_VOICE_SESSION_STATE_INVALID;
}

bool c5_voice_session_client_is_valid(const c5_voice_session_lease_t *lease)
{
    return lease != NULL && lease->voice_session_id[0] != '\0' &&
           lease->owner_device_id[0] != '\0' && lease->generation != 0U &&
           lease->lease_expires_at_ms > 0;
}

static bool parse_lease(const char *json, c5_voice_session_lease_t *out_lease)
{
    c5_voice_session_lease_t parsed = {0};
    char parsed_state[24] = {0};
    if (!json_string(json,
                     ESP111_PROTOCOL_LOCAL_JSON_VOICE_SESSION_ID,
                     parsed.voice_session_id,
                     sizeof(parsed.voice_session_id)) ||
        !json_string(json,
                     ESP111_PROTOCOL_LOCAL_JSON_VOICE_OWNER_DEVICE_ID,
                     parsed.owner_device_id,
                     sizeof(parsed.owner_device_id)) ||
        !json_uint32(json, ESP111_PROTOCOL_LOCAL_JSON_VOICE_GENERATION, &parsed.generation) ||
        !json_int64(json,
                    ESP111_PROTOCOL_LOCAL_JSON_VOICE_LEASE_EXPIRES_AT_MS,
                    &parsed.lease_expires_at_ms) ||
        !json_string(json,
                     ESP111_PROTOCOL_LOCAL_JSON_VOICE_STATE,
                     parsed_state,
                     sizeof(parsed_state)) ||
        (parsed.state = state_from_name(parsed_state)) == C5_VOICE_SESSION_STATE_INVALID ||
        !c5_voice_session_client_is_valid(&parsed)) {
        return false;
    }
    if (strcmp(parsed.owner_device_id, server_comm_get_device_id()) != 0) {
        return false;
    }
    *out_lease = parsed;
    return true;
}

static esp_err_t session_request(const char *action,
                                 const c5_voice_session_lease_t *current,
                                 c5_voice_session_state_t next_state,
                                 c5_voice_session_lease_t *out_lease)
{
    if (!s_initialized || action == NULL || action[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    const char *device_id = server_comm_get_device_id();
    if (device_id == NULL || device_id[0] == '\0' || strchr(device_id, '\"') != NULL ||
        strchr(device_id, '\\') != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char request[C5_VOICE_SESSION_REQUEST_BYTES] = {0};
    int written = 0;
    if (current == NULL) {
        written = snprintf(request,
                           sizeof(request),
                           "{\"schema_version\":%u,\"id\":\"%s\",\"voice_action\":\"%s\"}",
                           ESP111_PROTOCOL_HOME_AI_SCHEMA_VERSION,
                           device_id,
                           action);
    } else if (next_state == C5_VOICE_SESSION_STATE_INVALID) {
        written = snprintf(request,
                           sizeof(request),
                           "{\"schema_version\":%u,\"id\":\"%s\",\"voice_action\":\"%s\",\"voice_session_id\":\"%s\",\"generation\":%lu}",
                           ESP111_PROTOCOL_HOME_AI_SCHEMA_VERSION,
                           device_id,
                           action,
                           current->voice_session_id,
                           (unsigned long)current->generation);
    } else {
        const char *next_state_name = state_name(next_state);
        if (next_state_name == NULL) return ESP_ERR_INVALID_ARG;
        written = snprintf(request,
                           sizeof(request),
                           "{\"schema_version\":%u,\"id\":\"%s\",\"voice_action\":\"%s\",\"voice_session_id\":\"%s\",\"generation\":%lu,\"voice_state\":\"%s\"}",
                           ESP111_PROTOCOL_HOME_AI_SCHEMA_VERSION,
                           device_id,
                           action,
                           current->voice_session_id,
                           (unsigned long)current->generation,
                           next_state_name);
    }
    if (written <= 0 || written >= (int)sizeof(request)) {
        return ESP_ERR_INVALID_SIZE;
    }

    device_protocol_metadata_t metadata = {0};
    device_protocol_prepare_metadata(&metadata, "home_ai_voice_session");
    char response_body[C5_VOICE_SESSION_RESPONSE_BYTES] = {0};
    server_comm_http_response_t response = {0};
    esp_err_t ret = server_comm_http_post_json_with_headers(ESP111_PROTOCOL_ROUTE_VOICE_SESSION,
                                                             request,
                                                             metadata.headers,
                                                             metadata.header_count,
                                                             C5_VOICE_SESSION_TIMEOUT_MS,
                                                             response_body,
                                                             sizeof(response_body),
                                                             &response);
    if (ret != ESP_OK) {
        char error_code[48] = {0};
        (void)json_string(response_body, ESP111_PROTOCOL_LOCAL_JSON_ERROR, error_code, sizeof(error_code));
        if (response.status_code == 409 &&
            strcmp(error_code, ESP111_PROTOCOL_ERROR_VOICE_SESSION_BUSY) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGW(TAG,
                 "voice session request failed action=%s status=%d ret=%s code=%s",
                 action,
                 response.status_code,
                 esp_err_to_name(ret),
                 error_code[0] != '\0' ? error_code : "-");
        return ret;
    }

    if (out_lease != NULL &&
        (strcmp(action, ESP111_PROTOCOL_LOCAL_VOICE_ACTION_ACQUIRE) == 0 ||
         strcmp(action, ESP111_PROTOCOL_LOCAL_VOICE_ACTION_RENEW) == 0 ||
         strcmp(action, ESP111_PROTOCOL_LOCAL_VOICE_ACTION_STATE) == 0) &&
        !parse_lease(response_body, out_lease)) {
        ESP_LOGW(TAG, "voice session response rejected action=%s", action);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t c5_voice_session_client_init(void)
{
    s_initialized = true;
    return ESP_OK;
}

esp_err_t c5_voice_session_client_acquire(c5_voice_session_lease_t *out_lease)
{
    if (out_lease == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_lease, 0, sizeof(*out_lease));
    return session_request(ESP111_PROTOCOL_LOCAL_VOICE_ACTION_ACQUIRE,
                           NULL,
                           C5_VOICE_SESSION_STATE_INVALID,
                           out_lease);
}

esp_err_t c5_voice_session_client_renew(c5_voice_session_lease_t *in_out_lease)
{
    if (!c5_voice_session_client_is_valid(in_out_lease)) {
        return ESP_ERR_INVALID_ARG;
    }
    c5_voice_session_lease_t renewed = {0};
    esp_err_t ret = session_request(ESP111_PROTOCOL_LOCAL_VOICE_ACTION_RENEW,
                                    in_out_lease,
                                    C5_VOICE_SESSION_STATE_INVALID,
                                    &renewed);
    if (ret == ESP_OK) {
        *in_out_lease = renewed;
    }
    return ret;
}

esp_err_t c5_voice_session_client_transition(c5_voice_session_lease_t *in_out_lease,
                                             c5_voice_session_state_t next_state)
{
    if (!c5_voice_session_client_is_valid(in_out_lease) || state_name(next_state) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    c5_voice_session_lease_t transitioned = {0};
    esp_err_t ret = session_request(ESP111_PROTOCOL_LOCAL_VOICE_ACTION_STATE,
                                    in_out_lease,
                                    next_state,
                                    &transitioned);
    if (ret == ESP_OK) *in_out_lease = transitioned;
    return ret;
}

esp_err_t c5_voice_session_client_release(c5_voice_session_lease_t *lease)
{
    if (!c5_voice_session_client_is_valid(lease)) {
        return ESP_OK;
    }
    (void)c5_voice_session_client_transition(lease, C5_VOICE_SESSION_STATE_ENDING);
    esp_err_t ret = session_request(ESP111_PROTOCOL_LOCAL_VOICE_ACTION_RELEASE,
                                    lease,
                                    C5_VOICE_SESSION_STATE_INVALID,
                                    NULL);
    /* A local lease is never reused after release attempt; S3 expiry is the fallback on disconnect. */
    memset(lease, 0, sizeof(*lease));
    return ret;
}

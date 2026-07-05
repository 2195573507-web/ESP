#include "system_server_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "device_protocol_metadata.h"
#include "screen_service.h"
#include "server_comm_config.h"
#include "server_comm_http.h"

static const char *TAG = "system_server_client";

#define SYSTEM_COMMAND_PROTOCOL_VERSION "agent-command-v1"
#define SYSTEM_COMMAND_CAPABILITIES_ENDPOINT "/api/devices/capabilities"
#define SYSTEM_COMMAND_PENDING_ENDPOINT "/api/commands/pending"
#define SYSTEM_COMMAND_ACK_ENDPOINT_PREFIX "/api/commands/"
#define SYSTEM_COMMAND_ACK_ENDPOINT_SUFFIX "/ack"
#define SYSTEM_COMMAND_RESPONSE_BUFFER_SIZE 2048U
#define SYSTEM_COMMAND_JSON_BUFFER_SIZE 1024U
#define SYSTEM_COMMAND_ENDPOINT_BUFFER_SIZE 512U
#define SYSTEM_COMMAND_ID_BUFFER_SIZE 80U
#define SYSTEM_COMMAND_NAME_BUFFER_SIZE 80U
#define SYSTEM_COMMAND_TEXT_BUFFER_SIZE 512U
#define SYSTEM_COMMAND_ESCAPED_SMALL_BUFFER_SIZE 160U
#define SYSTEM_COMMAND_ESCAPED_TEXT_BUFFER_SIZE 360U
#define SYSTEM_COMMAND_QUERY_VALUE_BUFFER_SIZE 384U
#define SYSTEM_COMMAND_DEFAULT_TTL_MS 5000
#define SYSTEM_COMMAND_MIN_TTL_MS 1000
#define SYSTEM_COMMAND_MAX_TTL_MS 60000
#define SYSTEM_COMMAND_HTTP_TIMEOUT_MS 5000U

typedef struct {
    char command_id[SYSTEM_COMMAND_ID_BUFFER_SIZE];
    char name[SYSTEM_COMMAND_NAME_BUFFER_SIZE];
    char text[SYSTEM_COMMAND_TEXT_BUFFER_SIZE];
    int ttl_ms;
} system_server_command_t;

typedef struct {
    system_server_command_t command;
    char response_body[SYSTEM_COMMAND_RESPONSE_BUFFER_SIZE];
    char json_body[SYSTEM_COMMAND_JSON_BUFFER_SIZE];
    char object[SYSTEM_COMMAND_JSON_BUFFER_SIZE];
    char payload[SYSTEM_COMMAND_JSON_BUFFER_SIZE];
    char endpoint[SYSTEM_COMMAND_ENDPOINT_BUFFER_SIZE];
    char encoded_device_id[SYSTEM_COMMAND_QUERY_VALUE_BUFFER_SIZE];
    char escaped_error_code[SYSTEM_COMMAND_ESCAPED_SMALL_BUFFER_SIZE];
    char escaped_error_message[SYSTEM_COMMAND_ESCAPED_TEXT_BUFFER_SIZE];
    char escaped_device_id[SYSTEM_COMMAND_ESCAPED_SMALL_BUFFER_SIZE];
} system_server_client_scratch_t;

static bool s_capabilities_registered;
static uint32_t s_capabilities_register_error_count;
static SemaphoreHandle_t s_scratch_lock;
static system_server_client_scratch_t *s_scratch;

static esp_err_t system_server_client_ensure_scratch(void)
{
    if (s_scratch_lock == NULL) {
        SemaphoreHandle_t lock = xSemaphoreCreateMutex();
        if (lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
        if (s_scratch_lock == NULL) {
            s_scratch_lock = lock;
        } else {
            vSemaphoreDelete(lock);
        }
    }

    if (s_scratch == NULL) {
        system_server_client_scratch_t *scratch =
            (system_server_client_scratch_t *)heap_caps_calloc(1,
                                                               sizeof(*scratch),
                                                               MALLOC_CAP_8BIT);
        if (scratch == NULL) {
            ESP_LOGW(TAG,
                     "system command scratch alloc failed bytes=%u",
                     (unsigned int)sizeof(*scratch));
            return ESP_ERR_NO_MEM;
        }
        if (s_scratch == NULL) {
            s_scratch = scratch;
        } else {
            heap_caps_free(scratch);
        }
    }

    return ESP_OK;
}

static esp_err_t system_server_client_take_scratch(system_server_client_scratch_t **out_scratch)
{
    if (out_scratch == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_scratch = NULL;
    esp_err_t ret = system_server_client_ensure_scratch();
    if (ret != ESP_OK) {
        return ret;
    }

    if (xSemaphoreTake(s_scratch_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    memset(s_scratch, 0, sizeof(*s_scratch));
    *out_scratch = s_scratch;
    return ESP_OK;
}

static void system_server_client_give_scratch(void)
{
    if (s_scratch_lock != NULL) {
        xSemaphoreGive(s_scratch_lock);
    }
}

static bool system_server_client_should_log_periodic(uint32_t *counter)
{
    if (counter == NULL) {
        return true;
    }

    *counter += 1U;
    return *counter == 1U || (*counter % 12U) == 0U;
}

static int system_server_client_normalize_ttl_ms(int ttl_ms)
{
    if (ttl_ms <= 0) {
        return SYSTEM_COMMAND_DEFAULT_TTL_MS;
    }
    if (ttl_ms < SYSTEM_COMMAND_MIN_TTL_MS) {
        return SYSTEM_COMMAND_MIN_TTL_MS;
    }
    if (ttl_ms > SYSTEM_COMMAND_MAX_TTL_MS) {
        return SYSTEM_COMMAND_MAX_TTL_MS;
    }
    return ttl_ms;
}

static bool system_json_append_char(char *out, size_t out_size, size_t *out_len, char ch)
{
    if (out == NULL || out_size == 0 || out_len == NULL) {
        return false;
    }

    if (*out_len + 1U >= out_size) {
        out[out_size - 1U] = '\0';
        return false;
    }

    out[*out_len] = ch;
    *out_len += 1U;
    out[*out_len] = '\0';
    return true;
}

static bool system_json_append_text(char *out, size_t out_size, size_t *out_len, const char *text)
{
    if (text == NULL) {
        return true;
    }

    bool complete = true;
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        if (!system_json_append_char(out, out_size, out_len, *cursor)) {
            complete = false;
            break;
        }
    }
    return complete;
}

static bool system_json_escape_string(const char *input, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return false;
    }

    out[0] = '\0';
    if (input == NULL) {
        return true;
    }

    bool complete = true;
    size_t out_len = 0;
    for (const unsigned char *cursor = (const unsigned char *)input; *cursor != '\0'; cursor++) {
        const char *escape = NULL;
        char unicode_escape[7];

        switch (*cursor) {
        case '\"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\b':
            escape = "\\b";
            break;
        case '\f':
            escape = "\\f";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            if (*cursor < 0x20U) {
                int written = snprintf(unicode_escape, sizeof(unicode_escape), "\\u%04x", *cursor);
                if (written < 0 || written >= (int)sizeof(unicode_escape)) {
                    complete = false;
                    continue;
                }
                escape = unicode_escape;
            }
            break;
        }

        if (escape != NULL) {
            if (!system_json_append_text(out, out_size, &out_len, escape)) {
                complete = false;
                break;
            }
        } else if (!system_json_append_char(out, out_size, &out_len, (char)*cursor)) {
            complete = false;
            break;
        }
    }

    return complete;
}

static bool system_url_is_unreserved(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

static bool system_url_encode_component(const char *input, char *out, size_t out_size)
{
    static const char hex[] = "0123456789ABCDEF";

    if (out == NULL || out_size == 0) {
        return false;
    }

    out[0] = '\0';
    if (input == NULL) {
        return true;
    }

    size_t out_len = 0;
    for (const unsigned char *cursor = (const unsigned char *)input; *cursor != '\0'; cursor++) {
        if (system_url_is_unreserved(*cursor)) {
            if (!system_json_append_char(out, out_size, &out_len, (char)*cursor)) {
                return false;
            }
            continue;
        }

        if (out_len + 3U >= out_size) {
            out[out_size - 1U] = '\0';
            return false;
        }
        out[out_len++] = '%';
        out[out_len++] = hex[*cursor >> 4];
        out[out_len++] = hex[*cursor & 0x0FU];
        out[out_len] = '\0';
    }

    return true;
}

static const char *system_json_skip_ws(const char *value)
{
    while (value != NULL &&
           (*value == ' ' || *value == '\n' || *value == '\r' || *value == '\t')) {
        value++;
    }
    return value;
}

static const char *system_json_find_key(const char *json, const char *key)
{
    if (json == NULL || key == NULL || key[0] == '\0') {
        return NULL;
    }

    char pattern[64];
    int written = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (written < 0 || written >= (int)sizeof(pattern)) {
        return NULL;
    }

    const char *key_pos = strstr(json, pattern);
    if (key_pos == NULL) {
        return NULL;
    }

    const char *colon = strchr(key_pos + written, ':');
    if (colon == NULL) {
        return NULL;
    }

    return system_json_skip_ws(colon + 1);
}

static const char *system_json_find_matching(const char *start, char open_ch, char close_ch)
{
    if (start == NULL || *start != open_ch) {
        return NULL;
    }

    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    for (const char *cursor = start; *cursor != '\0'; cursor++) {
        char ch = *cursor;
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }

        if (ch == '"') {
            in_string = true;
        } else if (ch == open_ch) {
            depth++;
        } else if (ch == close_ch) {
            depth--;
            if (depth == 0) {
                return cursor;
            }
        }
    }

    return NULL;
}

static bool system_json_copy_string_value(const char *value, char *out, size_t out_size)
{
    if (value == NULL || out == NULL || out_size == 0) {
        return false;
    }

    value = system_json_skip_ws(value);
    if (value == NULL || *value != '"') {
        return false;
    }
    value++;

    size_t out_len = 0;
    bool escaped = false;
    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        char ch = *cursor;
        if (escaped) {
            switch (ch) {
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            default:
                break;
            }
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
            continue;
        } else if (ch == '"') {
            out[out_len] = '\0';
            return true;
        }

        if (out_len + 1U < out_size) {
            out[out_len++] = ch;
        }
    }

    out[out_len] = '\0';
    return false;
}

static bool system_json_get_string(const char *json,
                                   const char *key,
                                   char *out,
                                   size_t out_size)
{
    const char *value = system_json_find_key(json, key);
    return system_json_copy_string_value(value, out, out_size);
}

static bool system_json_get_int(const char *json, const char *key, int *out)
{
    const char *value = system_json_find_key(json, key);
    if (value == NULL || out == NULL) {
        return false;
    }

    int parsed = 0;
    int consumed = 0;
    if (sscanf(value, "%d%n", &parsed, &consumed) != 1 || consumed <= 0) {
        return false;
    }

    *out = parsed;
    return true;
}

static bool system_json_copy_object_for_key(const char *json,
                                            const char *key,
                                            char *out,
                                            size_t out_size)
{
    const char *value = system_json_find_key(json, key);
    if (value == NULL || out == NULL || out_size == 0 || *value != '{') {
        return false;
    }

    const char *end = system_json_find_matching(value, '{', '}');
    if (end == NULL) {
        return false;
    }

    size_t len = (size_t)(end - value + 1);
    if (len >= out_size) {
        len = out_size - 1U;
    }
    memcpy(out, value, len);
    out[len] = '\0';
    return true;
}

static esp_err_t system_server_parse_first_command(const char *json,
                                                   system_server_client_scratch_t *scratch)
{
    if (json == NULL || scratch == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    system_server_command_t *command = &scratch->command;
    memset(command, 0, sizeof(*command));
    command->ttl_ms = SYSTEM_COMMAND_DEFAULT_TTL_MS;

    const char *commands = system_json_find_key(json, "commands");
    if (commands == NULL || *commands != '[') {
        return ESP_ERR_NOT_FOUND;
    }

    const char *cursor = system_json_skip_ws(commands + 1);
    if (cursor == NULL || *cursor == ']') {
        return ESP_ERR_NOT_FOUND;
    }
    if (*cursor != '{') {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *end = system_json_find_matching(cursor, '{', '}');
    if (end == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t len = (size_t)(end - cursor + 1);
    if (len >= sizeof(scratch->object)) {
        len = sizeof(scratch->object) - 1U;
    }
    memcpy(scratch->object, cursor, len);
    scratch->object[len] = '\0';

    if (!system_json_get_string(scratch->object,
                                "command_id",
                                command->command_id,
                                sizeof(command->command_id)) ||
        command->command_id[0] == '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!system_json_get_string(scratch->object, "name", command->name, sizeof(command->name)) ||
        command->name[0] == '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (system_json_copy_object_for_key(scratch->object,
                                        "payload",
                                        scratch->payload,
                                        sizeof(scratch->payload))) {
        (void)system_json_get_string(scratch->payload,
                                     "text",
                                     command->text,
                                     sizeof(command->text));
        (void)system_json_get_int(scratch->payload, "ttl_ms", &command->ttl_ms);
    }

    return ESP_OK;
}

static esp_err_t system_server_client_post_ack(system_server_client_scratch_t *scratch,
                                               const char *command_id,
                                               bool completed,
                                               const char *error_code,
                                               const char *error_message)
{
    if (scratch == NULL || command_id == NULL || command_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    int endpoint_len = snprintf(scratch->endpoint,
                                sizeof(scratch->endpoint),
                                "%s%s%s",
                                SYSTEM_COMMAND_ACK_ENDPOINT_PREFIX,
                                command_id,
                                SYSTEM_COMMAND_ACK_ENDPOINT_SUFFIX);
    if (endpoint_len < 0 || endpoint_len >= (int)sizeof(scratch->endpoint)) {
        return ESP_ERR_INVALID_SIZE;
    }

    int json_len = 0;
    if (completed) {
        json_len = snprintf(scratch->json_body,
                            sizeof(scratch->json_body),
                            "{\"status\":\"completed\",\"result\":{\"applied\":true}}");
    } else {
        bool error_code_complete =
            system_json_escape_string(error_code != NULL ? error_code : "COMMAND_FAILED",
                                      scratch->escaped_error_code,
                                      sizeof(scratch->escaped_error_code));
        bool error_message_complete =
            system_json_escape_string(error_message != NULL ? error_message : "command failed",
                                      scratch->escaped_error_message,
                                      sizeof(scratch->escaped_error_message));
        if (!error_code_complete || !error_message_complete) {
            ESP_LOGW(TAG,
                     "command ack error text truncated command_id=%s code_complete=%d message_complete=%d",
                     command_id,
                     error_code_complete ? 1 : 0,
                     error_message_complete ? 1 : 0);
        }

        json_len = snprintf(scratch->json_body,
                            sizeof(scratch->json_body),
                            "{\"status\":\"failed\",\"error_code\":\"%s\","
                            "\"error_message\":\"%s\",\"result\":{\"applied\":false}}",
                            scratch->escaped_error_code,
                            scratch->escaped_error_message);
    }
    if (json_len < 0 || json_len >= (int)sizeof(scratch->json_body)) {
        return ESP_ERR_INVALID_SIZE;
    }

    device_protocol_metadata_t metadata = {0};
    device_protocol_prepare_metadata(&metadata, "command.ack");
    server_comm_http_response_t response = {0};
    esp_err_t ret = server_comm_http_post_json_with_headers(scratch->endpoint,
                                                            scratch->json_body,
                                                            metadata.headers,
                                                            metadata.header_count,
                                                            SYSTEM_COMMAND_HTTP_TIMEOUT_MS,
                                                            NULL,
                                                            0,
                                                            &response);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "command ack failed command_id=%s ret=%s status=%d",
                 command_id,
                 esp_err_to_name(ret),
                 response.status_code);
    }
    return ret;
}

static esp_err_t system_server_client_execute_command(system_server_client_scratch_t *scratch,
                                                      const system_server_command_t *command)
{
    if (scratch == NULL || command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(command->name, "device.noop") == 0) {
        ESP_LOGI(TAG, "command noop id=%s", command->command_id);
        return system_server_client_post_ack(scratch, command->command_id, true, NULL, NULL);
    }

    if (strcmp(command->name, "display.show_text") == 0) {
        if (command->text[0] == '\0') {
            return system_server_client_post_ack(scratch,
                                                command->command_id,
                                                false,
                                                "DISPLAY_TEXT_REQUIRED",
                                                "display.show_text payload.text is required");
        }

        int ttl_ms = system_server_client_normalize_ttl_ms(command->ttl_ms);
        esp_err_t ret = screen_service_show_text("Server", command->text, ttl_ms);
        if (ret != ESP_OK) {
            return system_server_client_post_ack(scratch,
                                                command->command_id,
                                                false,
                                                "DISPLAY_COMMAND_FAILED",
                                                esp_err_to_name(ret));
        }

        ESP_LOGI(TAG,
                 "display command applied id=%s text_len=%u ttl_ms=%d",
                 command->command_id,
                 (unsigned int)strlen(command->text),
                 ttl_ms);
        return system_server_client_post_ack(scratch, command->command_id, true, NULL, NULL);
    }

    ESP_LOGW(TAG, "unsupported command from server id=%s name=%s", command->command_id, command->name);
    return system_server_client_post_ack(scratch,
                                        command->command_id,
                                        false,
                                        "COMMAND_UNSUPPORTED_BY_FIRMWARE",
                                        "firmware does not support this command");
}

esp_err_t system_server_client_init(void)
{
    const char *device_id = server_comm_get_device_id();
    if (s_capabilities_registered) {
        return ESP_OK;
    }

    system_server_client_scratch_t *scratch = NULL;
    esp_err_t ret = system_server_client_take_scratch(&scratch);
    if (ret != ESP_OK) {
        return ret;
    }

    bool device_id_complete =
        system_json_escape_string(device_id,
                                  scratch->escaped_device_id,
                                  sizeof(scratch->escaped_device_id));
    if (!device_id_complete) {
        ESP_LOGW(TAG, "command capability device_id truncated while escaping json");
    }

    int json_len = snprintf(scratch->json_body,
                            sizeof(scratch->json_body),
                            "{\"device_id\":\"%s\",\"protocol_version\":\"%s\","
                            "\"capabilities\":{\"commands\":[\"device.noop\",\"display.show_text\"]}}",
                            scratch->escaped_device_id,
                            SYSTEM_COMMAND_PROTOCOL_VERSION);
    if (json_len < 0 || json_len >= (int)sizeof(scratch->json_body)) {
        system_server_client_give_scratch();
        return ESP_ERR_INVALID_SIZE;
    }

    device_protocol_metadata_t metadata = {0};
    device_protocol_prepare_metadata(&metadata, "command.capabilities");
    server_comm_http_response_t response = {0};
    ret = server_comm_http_post_json_with_headers(SYSTEM_COMMAND_CAPABILITIES_ENDPOINT,
                                                  scratch->json_body,
                                                  metadata.headers,
                                                  metadata.header_count,
                                                  SYSTEM_COMMAND_HTTP_TIMEOUT_MS,
                                                  NULL,
                                                  0,
                                                  &response);
    if (ret == ESP_OK) {
        s_capabilities_registered = true;
        s_capabilities_register_error_count = 0;
        ESP_LOGI(TAG,
                 "registered command capabilities status=%d body_len=%u",
                 response.status_code,
                 (unsigned int)json_len);
    } else if (system_server_client_should_log_periodic(&s_capabilities_register_error_count)) {
        ESP_LOGW(TAG,
                 "register command capabilities failed ret=%s status=%d",
                 esp_err_to_name(ret),
                 response.status_code);
    }
    system_server_client_give_scratch();
    return ret;
}

esp_err_t system_server_client_send_heartbeat(const char *device_id)
{
    (void)device_id;
    ESP_LOGD(TAG, "system heartbeat reserved");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t system_server_client_poll_commands(const char *device_id)
{
    const char *target_device_id =
        (device_id != NULL && device_id[0] != '\0') ? device_id : server_comm_get_device_id();

    if (!s_capabilities_registered) {
        esp_err_t register_ret = system_server_client_init();
        if (register_ret != ESP_OK) {
            return register_ret;
        }
    }

    system_server_client_scratch_t *scratch = NULL;
    esp_err_t ret = system_server_client_take_scratch(&scratch);
    if (ret != ESP_OK) {
        return ret;
    }

    if (!system_url_encode_component(target_device_id,
                                     scratch->encoded_device_id,
                                     sizeof(scratch->encoded_device_id))) {
        ESP_LOGW(TAG, "command poll device_id too long after url encoding");
        system_server_client_give_scratch();
        return ESP_ERR_INVALID_SIZE;
    }

    int endpoint_len = snprintf(scratch->endpoint,
                                sizeof(scratch->endpoint),
                                "%s?device_id=%s&limit=1",
                                SYSTEM_COMMAND_PENDING_ENDPOINT,
                                scratch->encoded_device_id);
    if (endpoint_len < 0 || endpoint_len >= (int)sizeof(scratch->endpoint)) {
        system_server_client_give_scratch();
        return ESP_ERR_INVALID_SIZE;
    }

    device_protocol_metadata_t metadata = {0};
    device_protocol_prepare_metadata(&metadata, "command.poll");
    server_comm_http_response_t response = {0};
    ret = server_comm_http_get_json_with_headers(scratch->endpoint,
                                                 metadata.headers,
                                                 metadata.header_count,
                                                 SYSTEM_COMMAND_HTTP_TIMEOUT_MS,
                                                 scratch->response_body,
                                                 sizeof(scratch->response_body),
                                                 &response);
    if (ret != ESP_OK) {
        system_server_client_give_scratch();
        return ret;
    }

    ret = system_server_parse_first_command(scratch->response_body, scratch);
    if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGD(TAG, "no pending command device_id=%s", target_device_id);
        system_server_client_give_scratch();
        return ret;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "pending command response parse failed ret=%s status=%d response_len=%u overflow=%d",
                 esp_err_to_name(ret),
                 response.status_code,
                 (unsigned int)response.body_len,
                 response.body_overflow ? 1 : 0);
        system_server_client_give_scratch();
        return ret;
    }

    ESP_LOGI(TAG,
             "pending command id=%s name=%s status=%d response_len=%u",
             scratch->command.command_id,
             scratch->command.name,
             response.status_code,
             (unsigned int)response.body_len);
    ret = system_server_client_execute_command(scratch, &scratch->command);
    system_server_client_give_scratch();
    return ret;
}

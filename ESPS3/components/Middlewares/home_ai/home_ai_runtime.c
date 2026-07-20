#include "home_ai_runtime.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp111_protocol_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gateway_config.h"
#include "home_ai_config_store.h"
#include "home_ai_event_reporter.h"
#include "home_ai_emergency_coordinator.h"
#include "home_ai_rule_engine.h"
#include "home_ai_room_state.h"
#include "home_ai_user_override.h"
#include "home_ai_virtual_device.h"
#include "home_ai_voice_router.h"
#include "home_ai_weather_context.h"
#include "mbedtls/sha256.h"
#include "network_worker.h"
#include "offline_policy.h"
#include "sensor_aggregator.h"
#include "server_client.h"

static const char *TAG = "home_ai_runtime";

#define HOME_AI_RUNTIME_EVAL_INTERVAL_MS 500U
#define HOME_AI_RUNTIME_ENV_FRESHNESS_MS 120000U
#define HOME_AI_RUNTIME_RULE_RESPONSE_BYTES 16384U
#define HOME_AI_RUNTIME_RULE_FILE_BYTES (HOME_AI_RULE_PACKAGE_BYTES + 1U)
#define HOME_AI_RUNTIME_RULE_ACTIVE_PATH "/home_ai/rules_active.json"
#define HOME_AI_RUNTIME_RULE_PREVIOUS_PATH "/home_ai/rules_previous.json"
#define HOME_AI_RUNTIME_RULE_ACTIVE_TMP_PATH "/home_ai/rules_active.tmp"
#define HOME_AI_RUNTIME_RULE_PREVIOUS_TMP_PATH "/home_ai/rules_previous.tmp"
#define HOME_AI_RUNTIME_FILE_COPY_BYTES 512U
#define HOME_AI_RUNTIME_DEPLOYMENT_BYTES 4096U
#define HOME_AI_RUNTIME_CONFIG_CHECKSUM_LEN 65U

static bool s_initialized;
static int64_t s_last_eval_ms;
static char *s_rule_response;
static char *s_active_payload;
static StaticSemaphore_t s_sync_lock_storage;
static SemaphoreHandle_t s_sync_lock;
static home_ai_room_state_t *s_rooms;
static home_ai_room_state_t *s_reported_rooms;
static bool s_reported_room_valid[HOME_AI_ROOM_STATE_COUNT];
static home_ai_rule_environment_t *s_environment;
static sensor_aggregator_home_ai_environment_t *s_sensor_environment;
static home_ai_rule_decision_t *s_decisions;
static home_ai_room_state_config_t *s_config_rooms;
static home_ai_user_override_t *s_config_overrides;
static home_ai_emergency_acknowledgement_t s_config_acknowledgements[HOME_AI_EMERGENCY_ACKNOWLEDGEMENT_MAX];
static char s_config_checksum[HOME_AI_RUNTIME_CONFIG_CHECKSUM_LEN];
static uint64_t s_server_epoch_at_sync_ms;
static uint64_t s_local_uptime_at_sync_ms;
static int32_t s_server_timezone_offset_minutes;
static bool s_weather_available;
static bool s_weather_dark;
static uint64_t s_weather_expires_at_uptime_ms;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *temporary_path_for(const char *destination_path)
{
    if (destination_path == NULL) return NULL;
    if (strcmp(destination_path, HOME_AI_RUNTIME_RULE_ACTIVE_PATH) == 0) {
        return HOME_AI_RUNTIME_RULE_ACTIVE_TMP_PATH;
    }
    if (strcmp(destination_path, HOME_AI_RUNTIME_RULE_PREVIOUS_PATH) == 0) {
        return HOME_AI_RUNTIME_RULE_PREVIOUS_TMP_PATH;
    }
    return NULL;
}

static bool commit_temporary_file(const char *temporary_path, const char *destination_path)
{
    if (temporary_path == NULL || destination_path == NULL) return false;
    (void)remove(destination_path);
    if (rename(temporary_path, destination_path) == 0) return true;
    (void)remove(temporary_path);
    return false;
}

static bool copy_file(const char *source_path, const char *destination_path)
{
    if (source_path == NULL || destination_path == NULL) return false;
    const char *temporary_path = temporary_path_for(destination_path);
    if (temporary_path == NULL) return false;
    FILE *source = fopen(source_path, "rb");
    if (source == NULL) return false;
    FILE *destination = fopen(temporary_path, "wb");
    if (destination == NULL) {
        fclose(source);
        return false;
    }
    uint8_t buffer[HOME_AI_RUNTIME_FILE_COPY_BYTES];
    bool ok = true;
    size_t read_count = 0U;
    while ((read_count = fread(buffer, 1U, sizeof(buffer), source)) > 0U) {
        if (fwrite(buffer, 1U, read_count, destination) != read_count) {
            ok = false;
            break;
        }
    }
    if (ferror(source) != 0) ok = false;
    if (fflush(destination) != 0) ok = false;
    fclose(destination);
    fclose(source);
    if (!ok) {
        (void)remove(temporary_path);
        return false;
    }
    return commit_temporary_file(temporary_path, destination_path);
}

static bool read_payload_file(const char *path, char *out, size_t capacity, size_t *out_len)
{
    if (path == NULL || out == NULL || capacity < 2U || out_len == NULL) return false;
    *out_len = 0U;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    const size_t count = fread(out, 1U, capacity - 1U, file);
    const bool too_large = fgetc(file) != EOF;
    const bool read_error = ferror(file) != 0;
    fclose(file);
    if (too_large || read_error || count == 0U) return false;
    out[count] = '\0';
    *out_len = count;
    return true;
}

static bool write_payload_file(const char *path, const char *payload, size_t payload_len)
{
    if (path == NULL || payload == NULL || payload_len == 0U ||
        payload_len > HOME_AI_RULE_PACKAGE_BYTES) return false;
    const char *temporary_path = temporary_path_for(path);
    if (temporary_path == NULL) return false;
    FILE *file = fopen(temporary_path, "wb");
    if (file == NULL) return false;
    const bool ok = fwrite(payload, 1U, payload_len, file) == payload_len && fflush(file) == 0;
    fclose(file);
    if (!ok) {
        (void)remove(temporary_path);
        return false;
    }
    return commit_temporary_file(temporary_path, path);
}

static bool json_escape(const char *input, char *out, size_t out_size)
{
    if (input == NULL || out == NULL || out_size == 0U) return false;
    size_t used = 0U;
    for (size_t index = 0U; input[index] != '\0'; ++index) {
        const unsigned char value = (unsigned char)input[index];
        const char *escape = NULL;
        char unicode[7] = {0};
        if (value == '"') escape = "\\\"";
        else if (value == '\\') escape = "\\\\";
        else if (value == '\n') escape = "\\n";
        else if (value == '\r') escape = "\\r";
        else if (value == '\t') escape = "\\t";
        else if (value < 0x20U) {
            (void)snprintf(unicode, sizeof(unicode), "\\u%04x", (unsigned int)value);
            escape = unicode;
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

static bool verify_checksum(const char *payload,
                            size_t payload_len,
                            const char *expected_checksum)
{
    if (payload == NULL || expected_checksum == NULL || strlen(expected_checksum) != 64U) {
        return false;
    }
    unsigned char digest[32];
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    const bool ok = mbedtls_sha256_starts(&context, 0) == 0 &&
                    mbedtls_sha256_update(&context,
                                          (const unsigned char *)payload,
                                          payload_len) == 0 &&
                    mbedtls_sha256_finish(&context, digest) == 0;
    mbedtls_sha256_free(&context);
    if (!ok) {
        return false;
    }
    char calculated[65];
    for (size_t index = 0U; index < sizeof(digest); ++index) {
        (void)snprintf(&calculated[index * 2U], 3U, "%02x", digest[index]);
    }
    calculated[64] = '\0';
    return strcmp(calculated, expected_checksum) == 0;
}

static bool json_uint64(cJSON *object, const char *name, uint64_t *out)
{
    cJSON *item = cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, name) : NULL;
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0 ||
        item->valuedouble > 9007199254740991.0) return false;
    const uint64_t value = (uint64_t)item->valuedouble;
    if ((double)value != item->valuedouble) return false;
    *out = value;
    return true;
}

static bool json_uint32_range(cJSON *object,
                              const char *name,
                              uint32_t minimum,
                              uint32_t maximum,
                              uint32_t *out)
{
    uint64_t value = 0U;
    if (!json_uint64(object, name, &value) || value < minimum || value > maximum) return false;
    *out = (uint32_t)value;
    return true;
}

static bool bounded_json_string(cJSON *item, size_t capacity, bool allow_empty)
{
    return cJSON_IsString(item) && item->valuestring != NULL &&
           (allow_empty || item->valuestring[0] != '\0') && strlen(item->valuestring) < capacity;
}

static bool source_from_name(const char *name, radar_source_id_t *out)
{
    if (name == NULL || out == NULL) return false;
    if (strcmp(name, "s3_local") == 0) *out = RADAR_SOURCE_S3_LOCAL;
    else if (strcmp(name, "sensair_shuttle_01") == 0) *out = RADAR_SOURCE_C51;
    else if (strcmp(name, "sensair_shuttle_02") == 0) *out = RADAR_SOURCE_C52;
    else return false;
    return true;
}

static int voice_terminal_slot(cJSON *item)
{
    if (item == NULL) {
        return -1;
    }
    if (!bounded_json_string(item, HOME_AI_ROOM_STATE_VOICE_TERMINAL_ID_LEN, true)) {
        return -2;
    }
    if (item->valuestring[0] == '\0') {
        return -1;
    }
    if (strcmp(item->valuestring, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51) == 0) {
        return 0;
    }
    if (strcmp(item->valuestring, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52) == 0) {
        return 1;
    }
    return -2;
}

static bool parse_hhmm(cJSON *item, uint16_t *out_minute)
{
    if (!bounded_json_string(item, 6U, false) || strlen(item->valuestring) != 5U ||
        item->valuestring[2] != ':' || item->valuestring[0] < '0' || item->valuestring[0] > '2' ||
        item->valuestring[1] < '0' || item->valuestring[1] > '9' ||
        item->valuestring[3] < '0' || item->valuestring[3] > '5' ||
        item->valuestring[4] < '0' || item->valuestring[4] > '9') return false;
    const unsigned int hour = (unsigned int)(item->valuestring[0] - '0') * 10U +
                              (unsigned int)(item->valuestring[1] - '0');
    const unsigned int minute = (unsigned int)(item->valuestring[3] - '0') * 10U +
                                (unsigned int)(item->valuestring[4] - '0');
    if (hour > 23U) return false;
    *out_minute = (uint16_t)(hour * 60U + minute);
    return true;
}

static bool parse_config_rooms(cJSON *payload_root)
{
    cJSON *rooms = cJSON_IsObject(payload_root) ?
        cJSON_GetObjectItemCaseSensitive(payload_root, "rooms") : NULL;
    const int room_count = cJSON_IsArray(rooms) ? cJSON_GetArraySize(rooms) : 0;
    if (room_count < 1 || room_count > (int)HOME_AI_ROOM_STATE_COUNT) return false;
    bool seen[HOME_AI_ROOM_STATE_COUNT] = {false};
    bool terminal_seen[2] = {false};
    memset(s_config_rooms, 0, HOME_AI_ROOM_STATE_COUNT * sizeof(*s_config_rooms));
    for (int room_index = 0; room_index < room_count; ++room_index) {
        cJSON *room = cJSON_GetArrayItem(rooms, room_index);
        cJSON *room_id = cJSON_IsObject(room) ? cJSON_GetObjectItemCaseSensitive(room, "room_id") : NULL;
        cJSON *sources = cJSON_IsObject(room) ?
            cJSON_GetObjectItemCaseSensitive(room, "sensing_sources") : NULL;
        cJSON *voice_terminal = cJSON_IsObject(room) ?
            cJSON_GetObjectItemCaseSensitive(
                room,
                ESP111_PROTOCOL_HOME_AI_JSON_VOICE_TERMINAL_DEVICE_ID) : NULL;
        const int terminal_slot = voice_terminal_slot(voice_terminal);
        uint32_t occupied_ms = 0U;
        uint32_t vacant_ms = 0U;
        uint32_t multiple_ms = 0U;
        uint32_t single_ms = 0U;
        uint16_t quiet_start = 0U;
        uint16_t quiet_end = 0U;
        if (!bounded_json_string(room_id, HOME_AI_ROOM_STATE_ROOM_ID_LEN, false) ||
            terminal_slot < -1 ||
            (terminal_slot >= 0 && terminal_seen[terminal_slot]) ||
            !cJSON_IsArray(sources) || cJSON_GetArraySize(sources) < 1 ||
            !json_uint32_range(room, "presence_confirm_ms", 1U, 600000U, &occupied_ms) ||
            !json_uint32_range(room, "vacant_confirm_ms", 1U, 3600000U, &vacant_ms) ||
            !json_uint32_range(room, "multiple_confirm_ms", 1U, 600000U, &multiple_ms) ||
            !json_uint32_range(room, "single_confirm_ms", 1U, 600000U, &single_ms) ||
            !parse_hhmm(cJSON_GetObjectItemCaseSensitive(room, "quiet_start"), &quiet_start) ||
            !parse_hhmm(cJSON_GetObjectItemCaseSensitive(room, "quiet_end"), &quiet_end) ||
            quiet_start == quiet_end) return false;
        if (terminal_slot >= 0) {
            terminal_seen[terminal_slot] = true;
        }
        const int source_count = cJSON_GetArraySize(sources);
        for (int source_index = 0; source_index < source_count; ++source_index) {
            cJSON *source_item = cJSON_GetArrayItem(sources, source_index);
            radar_source_id_t source = RADAR_SOURCE_COUNT;
            if (!bounded_json_string(source_item, 32U, false) ||
                !source_from_name(source_item->valuestring, &source) || seen[source]) return false;
            seen[source] = true;
            home_ai_room_state_config_t *config = &s_config_rooms[source];
            config->source = source;
            strlcpy(config->room_id, room_id->valuestring, sizeof(config->room_id));
            strlcpy(config->voice_terminal_device_id,
                    voice_terminal != NULL ? voice_terminal->valuestring : "",
                    sizeof(config->voice_terminal_device_id));
            config->occupied_confirm_ms = occupied_ms;
            config->vacant_confirm_ms = vacant_ms;
            config->multiple_confirm_ms = multiple_ms;
            config->single_confirm_ms = single_ms;
            config->quiet_start_minute = quiet_start;
            config->quiet_end_minute = quiet_end;
        }
    }
    for (size_t index = 0U; index < HOME_AI_ROOM_STATE_COUNT; ++index) {
        if (!seen[index]) return false;
    }
    return true;
}

static bool parse_override_action(const char *name, home_ai_override_action_t *out)
{
    if (name == NULL || out == NULL) return false;
    if (strcmp(name, "keep_on") == 0) *out = HOME_AI_OVERRIDE_KEEP_ON;
    else if (strcmp(name, "keep_off") == 0) *out = HOME_AI_OVERRIDE_KEEP_OFF;
    else if (strcmp(name, "pause_automation") == 0) *out = HOME_AI_OVERRIDE_PAUSE_AUTOMATION;
    else if (strcmp(name, "mute") == 0) *out = HOME_AI_OVERRIDE_MUTE;
    else return false;
    return true;
}

static bool parse_config_overrides(cJSON *payload_root,
                                   uint64_t server_time_ms,
                                   uint64_t local_time_ms,
                                   size_t *out_count)
{
    cJSON *overrides = cJSON_IsObject(payload_root) ?
        cJSON_GetObjectItemCaseSensitive(payload_root, "overrides") : NULL;
    const int count = cJSON_IsArray(overrides) ? cJSON_GetArraySize(overrides) : -1;
    if (count < 0 || count > (int)HOME_AI_MAX_USER_OVERRIDES || out_count == NULL) return false;
    memset(s_config_overrides, 0, HOME_AI_MAX_USER_OVERRIDES * sizeof(*s_config_overrides));
    size_t parsed_count = 0U;
    for (int index = 0; index < count; ++index) {
        cJSON *item = cJSON_GetArrayItem(overrides, index);
        cJSON *override_id = cJSON_IsObject(item) ?
            cJSON_GetObjectItemCaseSensitive(item, "override_id") : NULL;
        cJSON *scope = cJSON_IsObject(item) ? cJSON_GetObjectItemCaseSensitive(item, "scope") : NULL;
        cJSON *room_id = cJSON_IsObject(scope) ? cJSON_GetObjectItemCaseSensitive(scope, "room_id") : NULL;
        cJSON *device_id = cJSON_IsObject(scope) ? cJSON_GetObjectItemCaseSensitive(scope, "device_id") : NULL;
        cJSON *action = cJSON_IsObject(item) ? cJSON_GetObjectItemCaseSensitive(item, "action") : NULL;
        cJSON *until = cJSON_IsObject(item) ? cJSON_GetObjectItemCaseSensitive(item, "until_condition") : NULL;
        cJSON *allow_safety = cJSON_IsObject(item) ?
            cJSON_GetObjectItemCaseSensitive(item, "allow_safety_override") : NULL;
        uint32_t priority = 0U;
        uint64_t created_at_ms = 0U;
        if (!bounded_json_string(override_id, HOME_AI_OVERRIDE_ID_LEN, false) ||
            !cJSON_IsObject(scope) ||
            !(bounded_json_string(room_id, HOME_AI_OVERRIDE_ROOM_ID_LEN, false) ||
              bounded_json_string(device_id, HOME_AI_OVERRIDE_DEVICE_ID_LEN, false)) ||
            !bounded_json_string(action, 32U, false) ||
            !(cJSON_IsNull(device_id) || bounded_json_string(device_id, HOME_AI_OVERRIDE_DEVICE_ID_LEN, true)) ||
            !bounded_json_string(until, HOME_AI_OVERRIDE_UNTIL_CONDITION_LEN, true) ||
            !cJSON_IsBool(allow_safety) ||
            !json_uint32_range(item, "priority", 800U, 999U, &priority) ||
            !json_uint64(item, "created_at_ms", &created_at_ms)) return false;
        cJSON *expires = cJSON_GetObjectItemCaseSensitive(item, "expires_at_ms");
        uint64_t expires_at_ms = 0U;
        if (!cJSON_IsNull(expires) && !json_uint64(item, "expires_at_ms", &expires_at_ms)) return false;
        if (expires_at_ms != 0U && expires_at_ms <= server_time_ms) continue;
        home_ai_user_override_t *parsed = &s_config_overrides[parsed_count];
        strlcpy(parsed->override_id, override_id->valuestring, sizeof(parsed->override_id));
        if (cJSON_IsString(room_id)) strlcpy(parsed->room_id, room_id->valuestring, sizeof(parsed->room_id));
        if (cJSON_IsString(device_id)) strlcpy(parsed->device_id, device_id->valuestring, sizeof(parsed->device_id));
        if (!parse_override_action(action->valuestring, &parsed->action)) return false;
        parsed->priority = (uint16_t)priority;
        const uint64_t age_ms = server_time_ms >= created_at_ms ? server_time_ms - created_at_ms : 0U;
        parsed->created_at_ms = age_ms < local_time_ms ? local_time_ms - age_ms : 1U;
        if (expires_at_ms != 0U) {
            const uint64_t ttl_ms = expires_at_ms - server_time_ms;
            parsed->expires_at_ms = ttl_ms <= UINT64_MAX - local_time_ms ?
                                        local_time_ms + ttl_ms : UINT64_MAX;
        }
        strlcpy(parsed->until_condition, until->valuestring, sizeof(parsed->until_condition));
        parsed->allow_safety_override = cJSON_IsTrue(allow_safety);
        ++parsed_count;
    }
    *out_count = parsed_count;
    return true;
}

static bool parse_config_emergency_acknowledgements(cJSON *payload_root,
                                                    size_t *out_count)
{
    if (out_count == NULL) return false;
    memset(s_config_acknowledgements, 0, sizeof(s_config_acknowledgements));
    cJSON *items = cJSON_IsObject(payload_root) ?
        cJSON_GetObjectItemCaseSensitive(payload_root,
                                         ESP111_PROTOCOL_HOME_AI_JSON_EMERGENCY_ACKNOWLEDGEMENTS) : NULL;
    if (items == NULL) {
        *out_count = 0U;
        return true;
    }
    if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) > (int)HOME_AI_EMERGENCY_ACKNOWLEDGEMENT_MAX) {
        return false;
    }
    const int count = cJSON_GetArraySize(items);
    for (int index = 0; index < count; ++index) {
        cJSON *item = cJSON_GetArrayItem(items, index);
        cJSON *event_id = cJSON_IsObject(item) ?
            cJSON_GetObjectItemCaseSensitive(item, ESP111_PROTOCOL_HOME_AI_JSON_EMERGENCY_EVENT_ID) : NULL;
        uint64_t acknowledged_at_ms = 0U;
        if (!bounded_json_string(event_id, HOME_AI_EMERGENCY_EVENT_ID_LEN, false) ||
            !json_uint64(item,
                         ESP111_PROTOCOL_HOME_AI_JSON_EMERGENCY_ACKNOWLEDGED_AT_MS,
                         &acknowledged_at_ms) ||
            acknowledged_at_ms == 0U) {
            return false;
        }
        strlcpy(s_config_acknowledgements[index].event_id,
                event_id->valuestring,
                sizeof(s_config_acknowledgements[index].event_id));
        s_config_acknowledgements[index].acknowledged_at_ms = acknowledged_at_ms;
    }
    *out_count = (size_t)count;
    return true;
}

static esp_err_t apply_config_response(const char *response, uint64_t local_time_ms)
{
    cJSON *root = cJSON_ParseWithLength(response, strlen(response));
    cJSON *data = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, "data") : NULL;
    cJSON *schema = cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "schema_version") : NULL;
    cJSON *payload = cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "payload") : NULL;
    cJSON *checksum = cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "checksum") : NULL;
    uint64_t server_time_ms = 0U;
    cJSON *timezone = cJSON_IsObject(data) ?
        cJSON_GetObjectItemCaseSensitive(data, "timezone_offset_minutes") : NULL;
    const bool timezone_valid = cJSON_IsNumber(timezone) && timezone->valuedouble >= -840.0 &&
                                timezone->valuedouble <= 840.0 &&
                                timezone->valuedouble == (double)(int32_t)timezone->valuedouble;
    if (!cJSON_IsObject(data) || !cJSON_IsNumber(schema) || schema->valueint != 1 ||
        !bounded_json_string(payload, HOME_AI_RUNTIME_RULE_RESPONSE_BYTES, false) ||
        !bounded_json_string(checksum, HOME_AI_RUNTIME_CONFIG_CHECKSUM_LEN, false) ||
        strlen(checksum->valuestring) != 64U ||
        !json_uint64(data, "server_time_ms", &server_time_ms) || !timezone_valid ||
        !verify_checksum(payload->valuestring, strlen(payload->valuestring), checksum->valuestring)) {
        if (root != NULL) cJSON_Delete(root);
        return ESP_ERR_INVALID_CRC;
    }
    s_server_epoch_at_sync_ms = server_time_ms;
    s_local_uptime_at_sync_ms = local_time_ms;
    s_server_timezone_offset_minutes = (int32_t)timezone->valuedouble;
    if (strcmp(s_config_checksum, checksum->valuestring) == 0) {
        cJSON_Delete(root);
        return ESP_OK;
    }
    cJSON *payload_root = cJSON_ParseWithLength(payload->valuestring, strlen(payload->valuestring));
    cJSON *payload_schema = payload_root != NULL ?
        cJSON_GetObjectItemCaseSensitive(payload_root, "schema_version") : NULL;
    size_t override_count = 0U;
    size_t emergency_ack_count = 0U;
    bool weather_available = false;
    bool weather_dark = false;
    uint64_t weather_expires_at_uptime_ms = 0U;
    const bool valid = cJSON_IsNumber(payload_schema) && payload_schema->valueint == 1 &&
                       parse_config_rooms(payload_root) &&
                       parse_config_overrides(payload_root, server_time_ms, local_time_ms, &override_count) &&
                       parse_config_emergency_acknowledgements(payload_root, &emergency_ack_count) &&
                       home_ai_weather_context_parse(payload_root,
                                                     server_time_ms,
                                                     local_time_ms,
                                                     &weather_available,
                                                     &weather_dark,
                                                     &weather_expires_at_uptime_ms);
    if (!valid) {
        if (payload_root != NULL) cJSON_Delete(payload_root);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    esp_err_t ret = ESP_OK;
    if (!home_ai_config_store_save(s_config_rooms, HOME_AI_ROOM_STATE_COUNT)) {
        ESP_LOGW(TAG, "config snapshot persistence failed");
        ret = ESP_FAIL;
    }
    if (ret == ESP_OK && !home_ai_room_state_set_config(s_config_rooms,
                                                        HOME_AI_ROOM_STATE_COUNT)) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK) {
        ret = home_ai_user_override_replace_synced(s_config_overrides,
                                                   override_count,
                                                   local_time_ms);
    }
    if (ret == ESP_OK) {
        ret = home_ai_emergency_coordinator_replace_acknowledgements(
            s_config_acknowledgements,
            emergency_ack_count,
            local_time_ms) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK) {
        s_weather_available = weather_available;
        s_weather_dark = weather_dark;
        s_weather_expires_at_uptime_ms = weather_expires_at_uptime_ms;
        strlcpy(s_config_checksum, checksum->valuestring, sizeof(s_config_checksum));
        memset(s_reported_room_valid, 0, sizeof(s_reported_room_valid));
        ESP_LOGI(TAG,
                 "config sync applied rooms=%u overrides=%u weather=%s checksum=%.12s",
                 (unsigned int)HOME_AI_ROOM_STATE_COUNT,
                 (unsigned int)override_count,
                 s_weather_available ? (s_weather_dark ? "dark" : "light") : "unavailable",
                 s_config_checksum);
    }
    cJSON_Delete(payload_root);
    cJSON_Delete(root);
    return ret;
}

static void refresh_quiet_schedule(uint64_t timestamp_ms)
{
    if (s_server_epoch_at_sync_ms == 0U || timestamp_ms < s_local_uptime_at_sync_ms) return;
    const uint64_t epoch_ms = s_server_epoch_at_sync_ms +
                              (timestamp_ms - s_local_uptime_at_sync_ms);
    int64_t local_minutes = (int64_t)(epoch_ms / 60000U) +
                            (int64_t)s_server_timezone_offset_minutes;
    local_minutes %= 1440;
    if (local_minutes < 0) local_minutes += 1440;
    for (radar_source_id_t source = RADAR_SOURCE_S3_LOCAL;
         source < RADAR_SOURCE_COUNT;
         source = (radar_source_id_t)(source + 1)) {
        home_ai_room_state_config_t config = {0};
        if (!home_ai_room_state_get_config(source, &config)) continue;
        const bool scheduled = config.quiet_start_minute < config.quiet_end_minute ?
            ((uint16_t)local_minutes >= config.quiet_start_minute &&
             (uint16_t)local_minutes < config.quiet_end_minute) :
            ((uint16_t)local_minutes >= config.quiet_start_minute ||
             (uint16_t)local_minutes < config.quiet_end_minute);
        (void)home_ai_room_state_apply_quiet_schedule(source, scheduled, timestamp_ms);
    }
}

static bool source_for_sensor(const char *device_id, radar_source_id_t *out_source)
{
    if (device_id == NULL || out_source == NULL) return false;
    if (strcmp(device_id, "sensair_shuttle_01") == 0) {
        *out_source = RADAR_SOURCE_C51;
        return true;
    }
    if (strcmp(device_id, "sensair_shuttle_02") == 0) {
        *out_source = RADAR_SOURCE_C52;
        return true;
    }
    return false;
}

static void refresh_environment(uint64_t timestamp_ms)
{
    memset(s_environment, 0, HOME_AI_ROOM_STATE_COUNT * sizeof(*s_environment));
    for (size_t index = 0U; index < HOME_AI_ROOM_STATE_COUNT; ++index) {
        (void)home_ai_room_state_set_environment_fresh(s_rooms[index].source, false, timestamp_ms);
    }
    const size_t count = sensor_aggregator_get_home_ai_environment_snapshot(
        s_sensor_environment, HOME_AI_ROOM_STATE_COUNT);
    for (size_t index = 0U; index < count; ++index) {
        if (!s_sensor_environment[index].valid || s_sensor_environment[index].sampled_at_ms <= 0) {
            continue;
        }
        radar_source_id_t source = RADAR_SOURCE_COUNT;
        if (!source_for_sensor(s_sensor_environment[index].device_id, &source)) continue;
        for (size_t room_index = 0U; room_index < HOME_AI_ROOM_STATE_COUNT; ++room_index) {
            if (s_rooms[room_index].source != source) continue;
            const uint64_t sampled_at = (uint64_t)s_sensor_environment[index].sampled_at_ms;
            const bool fresh = timestamp_ms >= sampled_at &&
                               timestamp_ms - sampled_at <= HOME_AI_RUNTIME_ENV_FRESHNESS_MS;
            s_environment[room_index].valid = fresh;
            s_environment[room_index].temperature_c =
                (float)s_sensor_environment[index].temperature_c;
            s_environment[room_index].humidity_percent =
                (float)s_sensor_environment[index].humidity_percent;
            s_environment[room_index].air_quality_score =
                (float)s_sensor_environment[index].air_quality_score;
            (void)home_ai_room_state_set_environment_fresh(s_rooms[room_index].source,
                                                             fresh,
                                                             timestamp_ms);
            break;
        }
    }
}

static void report_room_changes(uint64_t timestamp_ms)
{
    for (size_t index = 0U; index < HOME_AI_ROOM_STATE_COUNT; ++index) {
        if (!s_reported_room_valid[index] ||
            memcmp(&s_reported_rooms[index], &s_rooms[index], sizeof(s_rooms[index])) != 0) {
            home_ai_event_reporter_record_room_state(&s_rooms[index], timestamp_ms);
            s_reported_rooms[index] = s_rooms[index];
            s_reported_room_valid[index] = true;
        }
    }
}

static void evaluate_rules(uint64_t timestamp_ms)
{
    const bool weather_available = s_weather_available &&
                                   s_weather_expires_at_uptime_ms != 0U &&
                                   timestamp_ms < s_weather_expires_at_uptime_ms;
    const home_ai_rule_evaluation_context_t context = {
        .rooms = s_rooms,
        .room_count = HOME_AI_ROOM_STATE_COUNT,
        .environment = s_environment,
        .environment_count = HOME_AI_ROOM_STATE_COUNT,
        .time_window = NULL,
        .server_online = network_worker_is_server_ready(),
        .weather_available = weather_available,
        .weather_dark = weather_available && s_weather_dark,
        .now_ms = timestamp_ms,
    };
    const size_t decision_count = home_ai_rule_engine_evaluate(&context,
                                                                s_decisions,
                                                                HOME_AI_MAX_PENDING_DECISIONS);
    for (size_t index = 0U; index < decision_count; ++index) {
        home_ai_rule_decision_t *decision = &s_decisions[index];
        home_ai_virtual_device_execution_t execution = {0};
        execution.result = HOME_AI_VIRTUAL_EXECUTION_REJECTED;
        (void)snprintf(execution.reason,
                       sizeof(execution.reason),
                       "%s",
                       "action_not_supported_yet");
        home_ai_virtual_device_execution_t *execution_ptr = NULL;
        esp_err_t execute_ret = ESP_OK;
        if (decision->state == HOME_AI_RULE_DECISION_EXECUTE &&
            (decision->action == HOME_AI_RULE_ACTION_TURN_ON ||
             decision->action == HOME_AI_RULE_ACTION_TURN_OFF)) {
            execute_ret = home_ai_virtual_device_execute(decision, timestamp_ms, false, &execution);
            execution_ptr = &execution;
            home_ai_rule_engine_note_action_result(decision, execute_ret == ESP_OK);
            if (execute_ret == ESP_OK && execution.state.valid) {
                home_ai_event_reporter_record_virtual_state(&execution.state, timestamp_ms);
            }
        } else if (decision->state == HOME_AI_RULE_DECISION_EXECUTE &&
                   decision->action == HOME_AI_RULE_ACTION_PLAY_PROMPT) {
            home_ai_voice_route_result_t route = {0};
            execute_ret = home_ai_voice_router_request_prompt(decision->room_id,
                                                              decision->prompt,
                                                              decision->decision_id,
                                                              decision->safety_action,
                                                              timestamp_ms,
                                                              &route);
            if (execute_ret != ESP_OK || route.status != HOME_AI_VOICE_ROUTE_QUEUED) {
                ESP_LOGW(TAG,
                         "voice prompt route decision=%s status=%s ret=%s",
                         decision->decision_id,
                         home_ai_voice_route_status_name(route.status),
                         esp_err_to_name(execute_ret));
            }
        }
        home_ai_event_reporter_record_decision(decision,
                                               execution_ptr,
                                               timestamp_ms);
    }
}

static bool parse_transport(cJSON *root,
                            const char **payload,
                            size_t *payload_len,
                            const char **checksum,
                            uint32_t *version)
{
    if (!cJSON_IsObject(root) || payload == NULL || payload_len == NULL ||
        checksum == NULL || version == NULL) return false;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *package = cJSON_IsObject(data) ?
        cJSON_GetObjectItemCaseSensitive(data, "rule_package") : NULL;
    if (!cJSON_IsObject(package)) return false;
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(package, "schema_version");
    cJSON *version_item = cJSON_GetObjectItemCaseSensitive(package, "version");
    cJSON *payload_item = cJSON_GetObjectItemCaseSensitive(package, "payload");
    cJSON *checksum_item = cJSON_GetObjectItemCaseSensitive(package, "checksum");
    if (!cJSON_IsNumber(schema) || schema->valueint != HOME_AI_RULE_SCHEMA_VERSION ||
        !cJSON_IsNumber(version_item) || version_item->valuedouble < 1.0 ||
        version_item->valuedouble != (double)version_item->valueint ||
        !cJSON_IsString(payload_item) || payload_item->valuestring == NULL ||
        !cJSON_IsString(checksum_item) || checksum_item->valuestring == NULL) return false;
    *payload = payload_item->valuestring;
    *payload_len = strlen(payload_item->valuestring);
    *checksum = checksum_item->valuestring;
    *version = (uint32_t)version_item->valueint;
    return *payload_len > 0U && *payload_len <= HOME_AI_RULE_PACKAGE_BYTES;
}

static void report_deployment(const home_ai_rule_activation_result_t *activation,
                              const char *state_override)
{
    if (activation == NULL) return;
    char body[HOME_AI_RUNTIME_DEPLOYMENT_BYTES];
    int written = snprintf(body,
                           sizeof(body),
                           "{\"gateway_id\":\"%s\",\"package_version\":%lu,"
                           "\"state\":\"%s\",\"result\":{\"accepted_count\":%lu,"
                           "\"rejected_count\":%lu,\"active_rule_count\":%lu,\"items\":[",
                           gateway_config_get()->gateway_id,
                           (unsigned long)activation->package_version,
                           state_override != NULL ? state_override :
                                                    home_ai_rule_activation_state_name(activation->state),
                           (unsigned long)activation->accepted_count,
                           (unsigned long)activation->rejected_count,
                           (unsigned long)activation->active_rule_count);
    if (written <= 0 || written >= (int)sizeof(body)) return;
    size_t used = (size_t)written;
    for (size_t index = 0U; index < activation->item_count; ++index) {
        char rule_id[HOME_AI_RULE_ID_LEN * 2U];
        char code[64];
        if (!json_escape(activation->items[index].rule_id, rule_id, sizeof(rule_id)) ||
            !json_escape(activation->items[index].code, code, sizeof(code))) return;
        const int item_written = snprintf(body + used,
                                          sizeof(body) - used,
                                          "%s{\"rule_id\":\"%s\",\"accepted\":%s,"
                                          "\"retained_previous\":%s,\"code\":\"%s\"}",
                                          index == 0U ? "" : ",",
                                          rule_id,
                                          activation->items[index].accepted ? "true" : "false",
                                          activation->items[index].retained_previous ? "true" : "false",
                                          code);
        if (item_written <= 0 || (size_t)item_written >= sizeof(body) - used) return;
        used += (size_t)item_written;
    }
    written = snprintf(body + used,
                       sizeof(body) - used,
                       "],\"error_code\":\"%s\"}}}",
                       activation->error_code);
    if (written <= 0 || (size_t)written >= sizeof(body) - used) return;
    char *owned = cJSON_malloc(strlen(body) + 1U);
    if (owned == NULL) return;
    memcpy(owned, body, strlen(body) + 1U);
    if (network_worker_submit_home_ai_deployment(owned, "home_ai_rule_sync") != ESP_OK) {
        cJSON_free(owned);
    }
}

static void apply_loaded_payload(void)
{
    size_t payload_len = 0U;
    home_ai_rule_activation_result_t activation = {0};
    if (read_payload_file(HOME_AI_RUNTIME_RULE_PREVIOUS_PATH,
                          s_active_payload,
                          HOME_AI_RUNTIME_RULE_FILE_BYTES,
                          &payload_len)) {
        if (home_ai_rule_engine_apply_payload(s_active_payload, payload_len, &activation) == ESP_OK) {
            ESP_LOGI(TAG,
                     "rules previous restored version=%lu active=%lu",
                     (unsigned long)activation.package_version,
                     (unsigned long)activation.active_rule_count);
        } else {
            ESP_LOGW(TAG, "rules previous restore rejected code=%s", activation.error_code);
        }
    }
    payload_len = 0U;
    if (!read_payload_file(HOME_AI_RUNTIME_RULE_ACTIVE_PATH,
                           s_active_payload,
                           HOME_AI_RUNTIME_RULE_FILE_BYTES,
                           &payload_len)) return;
    memset(&activation, 0, sizeof(activation));
    if (home_ai_rule_engine_apply_payload(s_active_payload, payload_len, &activation) == ESP_OK) {
        home_ai_event_reporter_record_rule_activation(&activation, (uint64_t)now_ms());
        ESP_LOGI(TAG,
                 "rules restored version=%lu state=%s active=%lu",
                 (unsigned long)activation.package_version,
                 home_ai_rule_activation_state_name(activation.state),
                 (unsigned long)activation.active_rule_count);
    } else {
        ESP_LOGW(TAG, "rules restore rejected code=%s", activation.error_code);
    }
}

static esp_err_t sync_config_now_locked(void)
{
    if (s_rule_response == NULL) return ESP_ERR_INVALID_STATE;
    int status = 0;
    memset(s_rule_response, 0, HOME_AI_RUNTIME_RULE_RESPONSE_BYTES);
    esp_err_t ret = server_client_get_home_ai_config(s_rule_response,
                                                       HOME_AI_RUNTIME_RULE_RESPONSE_BYTES,
                                                       &status);
    offline_policy_record_server_result(ret, status);
    if (ret != ESP_OK || status < 200 || status >= 300) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_RESPONSE;
    }
    return apply_config_response(s_rule_response, (uint64_t)now_ms());
}

bool home_ai_runtime_init(void)
{
    if (s_initialized) return true;
    s_rule_response = heap_caps_calloc(1U,
                                       HOME_AI_RUNTIME_RULE_RESPONSE_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_active_payload = heap_caps_calloc(1U,
                                        HOME_AI_RUNTIME_RULE_FILE_BYTES,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_rooms = heap_caps_calloc(HOME_AI_ROOM_STATE_COUNT,
                               sizeof(*s_rooms),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_reported_rooms = heap_caps_calloc(HOME_AI_ROOM_STATE_COUNT,
                                        sizeof(*s_reported_rooms),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_environment = heap_caps_calloc(HOME_AI_ROOM_STATE_COUNT,
                                     sizeof(*s_environment),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_sensor_environment = heap_caps_calloc(HOME_AI_ROOM_STATE_COUNT,
                                            sizeof(*s_sensor_environment),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_decisions = heap_caps_calloc(HOME_AI_MAX_PENDING_DECISIONS,
                                   sizeof(*s_decisions),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_config_rooms = heap_caps_calloc(HOME_AI_ROOM_STATE_COUNT,
                                      sizeof(*s_config_rooms),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_config_overrides = heap_caps_calloc(HOME_AI_MAX_USER_OVERRIDES,
                                          sizeof(*s_config_overrides),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_sync_lock = xSemaphoreCreateMutexStatic(&s_sync_lock_storage);
    if (s_rule_response == NULL || s_active_payload == NULL || s_rooms == NULL ||
        s_reported_rooms == NULL || s_environment == NULL || s_sensor_environment == NULL ||
        s_decisions == NULL || s_config_rooms == NULL || s_config_overrides == NULL ||
        s_sync_lock == NULL) {
        heap_caps_free(s_rule_response);
        heap_caps_free(s_active_payload);
        heap_caps_free(s_rooms);
        heap_caps_free(s_reported_rooms);
        heap_caps_free(s_environment);
        heap_caps_free(s_sensor_environment);
        heap_caps_free(s_decisions);
        heap_caps_free(s_config_rooms);
        heap_caps_free(s_config_overrides);
        s_rule_response = NULL;
        s_active_payload = NULL;
        s_rooms = NULL;
        s_reported_rooms = NULL;
        s_environment = NULL;
        s_sensor_environment = NULL;
        s_decisions = NULL;
        s_config_rooms = NULL;
        s_config_overrides = NULL;
        return false;
    }
    memset(s_reported_room_valid, 0, sizeof(s_reported_room_valid));
    memset(s_rooms, 0, HOME_AI_ROOM_STATE_COUNT * sizeof(*s_rooms));
    memset(s_reported_rooms, 0, HOME_AI_ROOM_STATE_COUNT * sizeof(*s_reported_rooms));
    memset(s_environment, 0, HOME_AI_ROOM_STATE_COUNT * sizeof(*s_environment));
    memset(s_sensor_environment, 0, HOME_AI_ROOM_STATE_COUNT * sizeof(*s_sensor_environment));
    memset(s_decisions, 0, HOME_AI_MAX_PENDING_DECISIONS * sizeof(*s_decisions));
    memset(s_config_rooms, 0, HOME_AI_ROOM_STATE_COUNT * sizeof(*s_config_rooms));
    memset(s_config_overrides, 0, HOME_AI_MAX_USER_OVERRIDES * sizeof(*s_config_overrides));
    s_last_eval_ms = 0;
    s_config_checksum[0] = '\0';
    s_server_epoch_at_sync_ms = 0U;
    s_local_uptime_at_sync_ms = 0U;
    s_server_timezone_offset_minutes = 0;
    s_weather_available = false;
    s_weather_dark = false;
    s_weather_expires_at_uptime_ms = 0U;
    s_initialized = true;
    if (home_ai_config_store_load(s_config_rooms, HOME_AI_ROOM_STATE_COUNT) &&
        home_ai_room_state_set_config(s_config_rooms, HOME_AI_ROOM_STATE_COUNT)) {
        ESP_LOGI(TAG, "room config snapshot restored; weather and overrides reset");
    } else {
        memset(s_config_rooms, 0, HOME_AI_ROOM_STATE_COUNT * sizeof(*s_config_rooms));
    }
    apply_loaded_payload();
    return true;
}

void home_ai_runtime_tick(uint64_t timestamp_ms)
{
    if (!s_initialized || timestamp_ms == 0U ||
        (s_last_eval_ms != 0 && timestamp_ms - (uint64_t)s_last_eval_ms <
         HOME_AI_RUNTIME_EVAL_INTERVAL_MS)) return;
    if (xSemaphoreTake(s_sync_lock, 0U) != pdTRUE) return;
    s_last_eval_ms = (int64_t)timestamp_ms;
    refresh_quiet_schedule(timestamp_ms);
    const size_t count = home_ai_room_state_snapshot(s_rooms, HOME_AI_ROOM_STATE_COUNT);
    if (count != HOME_AI_ROOM_STATE_COUNT) {
        xSemaphoreGive(s_sync_lock);
        return;
    }
    refresh_environment(timestamp_ms);
    const size_t refreshed_count = home_ai_room_state_snapshot(s_rooms, HOME_AI_ROOM_STATE_COUNT);
    if (refreshed_count != HOME_AI_ROOM_STATE_COUNT) {
        xSemaphoreGive(s_sync_lock);
        return;
    }
    report_room_changes(timestamp_ms);
    home_ai_event_reporter_tick(timestamp_ms);
    home_ai_voice_router_tick(timestamp_ms);
    home_ai_emergency_coordinator_tick(timestamp_ms);
    evaluate_rules(timestamp_ms);
    xSemaphoreGive(s_sync_lock);
}

esp_err_t home_ai_runtime_sync_rules_now(void)
{
    if (!s_initialized || s_rule_response == NULL || s_sync_lock == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_sync_lock, pdMS_TO_TICKS(1000U)) != pdTRUE) return ESP_ERR_TIMEOUT;
    const esp_err_t config_ret = sync_config_now_locked();
    int status = 0;
    memset(s_rule_response, 0, HOME_AI_RUNTIME_RULE_RESPONSE_BYTES);
    esp_err_t ret = server_client_get_home_ai_rule_package(s_rule_response,
                                                            HOME_AI_RUNTIME_RULE_RESPONSE_BYTES,
                                                            &status);
    offline_policy_record_server_result(ret, status);
    if (ret != ESP_OK || status < 200 || status >= 300) {
        xSemaphoreGive(s_sync_lock);
        return ret != ESP_OK ? ret : (config_ret != ESP_OK ? config_ret : ESP_ERR_INVALID_RESPONSE);
    }
    cJSON *root = cJSON_ParseWithLength(s_rule_response, strlen(s_rule_response));
    cJSON *transport_data = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, "data") : NULL;
    cJSON *transport_package = cJSON_IsObject(transport_data) ?
        cJSON_GetObjectItemCaseSensitive(transport_data, "rule_package") : NULL;
    if (root != NULL && cJSON_IsNull(transport_package)) {
        cJSON_Delete(root);
        xSemaphoreGive(s_sync_lock);
        return config_ret;
    }
    const char *payload = NULL;
    const char *checksum = NULL;
    size_t payload_len = 0U;
    uint32_t package_version = 0U;
    if (root == NULL || !parse_transport(root, &payload, &payload_len, &checksum, &package_version) ||
        !verify_checksum(payload, payload_len, checksum)) {
        if (root != NULL) cJSON_Delete(root);
        xSemaphoreGive(s_sync_lock);
        ESP_LOGW(TAG, "rule sync rejected transport checksum_or_shape_invalid");
        return config_ret != ESP_OK ? config_ret : ESP_ERR_INVALID_CRC;
    }
    home_ai_rule_activation_result_t current = {0};
    (void)home_ai_rule_engine_get_activation(&current);
    if (current.package_version >= package_version && current.package_version != 0U) {
        cJSON_Delete(root);
        xSemaphoreGive(s_sync_lock);
        return config_ret;
    }
    const bool had_active_file = copy_file(HOME_AI_RUNTIME_RULE_ACTIVE_PATH,
                                           HOME_AI_RUNTIME_RULE_PREVIOUS_PATH);
    home_ai_rule_activation_result_t activation = {0};
    ret = home_ai_rule_engine_apply_payload(payload, payload_len, &activation);
    if (ret == ESP_OK && !write_payload_file(HOME_AI_RUNTIME_RULE_ACTIVE_PATH, payload, payload_len)) {
        (void)home_ai_rule_engine_rollback(&activation);
        ret = ESP_FAIL;
        if (!had_active_file) remove(HOME_AI_RUNTIME_RULE_PREVIOUS_PATH);
    }
    if (ret == ESP_OK) {
        home_ai_event_reporter_record_rule_activation(&activation, (uint64_t)now_ms());
        report_deployment(&activation, NULL);
        ESP_LOGI(TAG,
                 "rule sync applied version=%lu state=%s active=%lu accepted=%lu rejected=%lu",
                 (unsigned long)activation.package_version,
                 home_ai_rule_activation_state_name(activation.state),
                 (unsigned long)activation.active_rule_count,
                 (unsigned long)activation.accepted_count,
                 (unsigned long)activation.rejected_count);
    }
    cJSON_Delete(root);
    xSemaphoreGive(s_sync_lock);
    return ret != ESP_OK ? ret : config_ret;
}

esp_err_t home_ai_runtime_check_rule_notification(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    char response[768];
    int status = 0;
    const uint32_t known_version = home_ai_runtime_active_rule_version();
    esp_err_t ret = server_client_get_home_ai_rule_notification(known_version,
                                                                s_config_checksum,
                                                                response,
                                                                sizeof(response),
                                                                &status);
    offline_policy_record_server_result(ret, status);
    if (ret != ESP_OK || status < 200 || status >= 300) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *root = cJSON_ParseWithLength(response, strlen(response));
    cJSON *data = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, "data") : NULL;
    cJSON *available = cJSON_IsObject(data) ?
        cJSON_GetObjectItemCaseSensitive(data, "update_available") : NULL;
    cJSON *version = cJSON_IsObject(data) ?
        cJSON_GetObjectItemCaseSensitive(data, "package_version") : NULL;
    cJSON *config_available = cJSON_IsObject(data) ?
        cJSON_GetObjectItemCaseSensitive(data, "config_update_available") : NULL;
    cJSON *config_checksum = cJSON_IsObject(data) ?
        cJSON_GetObjectItemCaseSensitive(data, "config_checksum") : NULL;
    const bool valid = cJSON_IsBool(available) && cJSON_IsNumber(version) &&
                       version->valuedouble >= 0.0 &&
                       version->valuedouble == (double)version->valueint &&
                       cJSON_IsBool(config_available) && cJSON_IsString(config_checksum) &&
                       config_checksum->valuestring != NULL && strlen(config_checksum->valuestring) == 64U;
    const bool update_available = valid && cJSON_IsTrue(available) &&
                                  (uint32_t)version->valueint > known_version;
    const bool config_update = valid && cJSON_IsTrue(config_available) &&
                               strcmp(config_checksum->valuestring, s_config_checksum) != 0;
    if (root != NULL) cJSON_Delete(root);
    if (!valid) return ESP_ERR_INVALID_RESPONSE;
    return (update_available || config_update) ?
               home_ai_runtime_sync_rules_now() : ESP_OK;
}

bool home_ai_runtime_rollback_rules(void)
{
    if (!s_initialized || s_sync_lock == NULL ||
        xSemaphoreTake(s_sync_lock, pdMS_TO_TICKS(1000U)) != pdTRUE) return false;
    size_t active_len = 0U;
    size_t previous_len = 0U;
    const bool files_ready = read_payload_file(HOME_AI_RUNTIME_RULE_ACTIVE_PATH,
                                                s_rule_response,
                                                HOME_AI_RUNTIME_RULE_RESPONSE_BYTES,
                                                &active_len) &&
                             read_payload_file(HOME_AI_RUNTIME_RULE_PREVIOUS_PATH,
                                               s_active_payload,
                                               HOME_AI_RUNTIME_RULE_FILE_BYTES,
                                               &previous_len);
    home_ai_rule_activation_result_t activation = {0};
    bool rolled_back = files_ready && home_ai_rule_engine_rollback(&activation);
    bool active_persisted = rolled_back && write_payload_file(HOME_AI_RUNTIME_RULE_ACTIVE_PATH,
                                                               s_active_payload,
                                                               previous_len);
    if (rolled_back && !active_persisted) {
        home_ai_rule_activation_result_t restored = {0};
        (void)home_ai_rule_engine_rollback(&restored);
        rolled_back = false;
    }
    const bool previous_persisted = active_persisted &&
                                    write_payload_file(HOME_AI_RUNTIME_RULE_PREVIOUS_PATH,
                                                       s_rule_response,
                                                       active_len);
    if (active_persisted) {
        home_ai_event_reporter_record_rule_activation(&activation, (uint64_t)now_ms());
        report_deployment(&activation, "ROLLED_BACK");
        if (!previous_persisted) {
            ESP_LOGW(TAG, "rule rollback active persisted but previous swap failed");
        }
    }
    xSemaphoreGive(s_sync_lock);
    return rolled_back && active_persisted;
}

uint32_t home_ai_runtime_active_rule_version(void)
{
    home_ai_rule_activation_result_t activation = {0};
    return home_ai_rule_engine_get_activation(&activation) ? activation.package_version : 0U;
}

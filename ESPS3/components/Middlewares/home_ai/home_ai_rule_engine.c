#include "home_ai_rule_engine.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#ifndef HOME_AI_RULE_ENGINE_HOST_TEST
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

#define HOME_AI_RULE_STORE_COUNT 3U
#define HOME_AI_RULE_STRING_NONE UINT16_MAX
#define HOME_AI_RULE_MAX_TEXT_VALUE_LEN 48U

typedef enum {
    HOME_AI_FIELD_PRESENCE_STATE = 0,
    HOME_AI_FIELD_STABLE_TARGET_COUNT,
    HOME_AI_FIELD_OCCUPANCY_MODE,
    HOME_AI_FIELD_ENVIRONMENT_FRESH,
    HOME_AI_FIELD_RADAR_FRESH,
    HOME_AI_FIELD_QUIET_STATE,
    HOME_AI_FIELD_TIME_WINDOW,
    HOME_AI_FIELD_TEMPERATURE_C,
    HOME_AI_FIELD_HUMIDITY_PERCENT,
    HOME_AI_FIELD_AIR_QUALITY_SCORE,
    HOME_AI_FIELD_WEATHER_DARK,
} home_ai_rule_field_t;

typedef enum {
    HOME_AI_OPERATOR_EQ = 0,
    HOME_AI_OPERATOR_NEQ,
    HOME_AI_OPERATOR_GT,
    HOME_AI_OPERATOR_GTE,
    HOME_AI_OPERATOR_LT,
    HOME_AI_OPERATOR_LTE,
    HOME_AI_OPERATOR_IN,
    HOME_AI_OPERATOR_RANGE,
} home_ai_rule_operator_t;

typedef enum {
    HOME_AI_VALUE_TEXT = 0,
    HOME_AI_VALUE_NUMBER,
    HOME_AI_VALUE_BOOL,
} home_ai_rule_value_kind_t;

typedef enum {
    HOME_AI_RULE_TYPE_BASIC = 0,
    HOME_AI_RULE_TYPE_HABIT,
    HOME_AI_RULE_TYPE_SAFETY,
    HOME_AI_RULE_TYPE_MANUAL,
} home_ai_rule_type_t;

typedef enum {
    HOME_AI_OFFLINE_CONTINUE = 0,
    HOME_AI_OFFLINE_PAUSE,
    HOME_AI_OFFLINE_REQUIRE_SERVER,
} home_ai_rule_offline_policy_t;

typedef struct {
    home_ai_rule_field_t field;
    home_ai_rule_operator_t operator;
    home_ai_rule_value_kind_t value_kind;
    uint8_t value_count;
    uint32_t duration_ms;
    float number_values[4];
    bool bool_values[4];
    uint16_t text_offsets[4];
} home_ai_rule_condition_t;

typedef struct {
    uint16_t device_id_offset;
    uint16_t prompt_offset;
    home_ai_rule_device_type_t device_type;
    home_ai_rule_action_t action;
    uint32_t minimum_active_seconds;
} home_ai_rule_action_definition_t;

typedef struct {
    bool valid;
    uint16_t rule_id_offset;
    uint16_t room_id_offset;
    uint32_t version;
    home_ai_rule_type_t type;
    bool enabled;
    uint16_t priority;
    uint32_t cooldown_seconds;
    uint32_t minimum_active_seconds;
    uint64_t expires_at_ms;
    home_ai_rule_offline_policy_t offline_policy;
    uint8_t condition_count;
    uint8_t action_count;
    home_ai_rule_condition_t conditions[HOME_AI_MAX_CONDITIONS_PER_RULE];
    home_ai_rule_action_definition_t actions[HOME_AI_MAX_ACTIONS_PER_RULE];
} home_ai_rule_definition_t;

typedef struct {
    uint64_t condition_true_since_ms[HOME_AI_MAX_CONDITIONS_PER_RULE];
    uint64_t last_trigger_ms;
    bool was_eligible;
} home_ai_rule_runtime_t;

typedef struct {
    bool valid;
    uint32_t package_version;
    size_t pool_used;
    home_ai_rule_definition_t rules[HOME_AI_MAX_RULES];
} home_ai_rule_store_t;

static home_ai_rule_store_t *s_stores;
static home_ai_rule_runtime_t *s_runtime;
static home_ai_rule_activation_result_t s_activation;
static char *s_pools[HOME_AI_RULE_STORE_COUNT];
static uint8_t s_active_store;
static uint8_t s_previous_store;
static uint32_t s_decision_sequence;
static bool s_initialized;

#ifdef HOME_AI_RULE_ENGINE_HOST_TEST
static home_ai_rule_store_t s_host_stores[HOME_AI_RULE_STORE_COUNT];
static home_ai_rule_runtime_t s_host_runtime[HOME_AI_MAX_RULES];
static char s_host_pools[HOME_AI_RULE_STORE_COUNT][HOME_AI_RULE_STRING_POOL_BYTES];
#else
static StaticSemaphore_t s_rule_lock_storage;
static SemaphoreHandle_t s_rule_lock;
#endif

static bool engine_lock(void)
{
#ifdef HOME_AI_RULE_ENGINE_HOST_TEST
    return true;
#else
    return s_rule_lock != NULL && xSemaphoreTake(s_rule_lock, portMAX_DELAY) == pdTRUE;
#endif
}

static void engine_unlock(void)
{
#ifndef HOME_AI_RULE_ENGINE_HOST_TEST
    xSemaphoreGive(s_rule_lock);
#endif
}

static void copy_text(char *out, size_t out_size, const char *value)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    size_t length = 0U;
    if (value != NULL) {
        while (length + 1U < out_size && value[length] != '\0') {
            ++length;
        }
        memcpy(out, value, length);
    }
    out[length] = '\0';
}

static bool text_fits(const char *value, size_t max_size)
{
    if (value == NULL || value[0] == '\0' || max_size < 2U) {
        return false;
    }
    for (size_t index = 0U; index < max_size; ++index) {
        if (value[index] == '\0') {
            return true;
        }
    }
    return false;
}

static const char *store_string(uint8_t store_index, uint16_t offset)
{
    if (store_index >= HOME_AI_RULE_STORE_COUNT || offset == HOME_AI_RULE_STRING_NONE ||
        s_pools[store_index] == NULL || offset >= HOME_AI_RULE_STRING_POOL_BYTES) {
        return "";
    }
    return &s_pools[store_index][offset];
}

static bool store_add_string(uint8_t store_index,
                             const char *value,
                             size_t max_len,
                             uint16_t *out_offset)
{
    if (out_offset == NULL || store_index >= HOME_AI_RULE_STORE_COUNT ||
        s_pools[store_index] == NULL) {
        return false;
    }
    *out_offset = HOME_AI_RULE_STRING_NONE;
    if (value == NULL || value[0] == '\0') {
        return true;
    }
    size_t length = strlen(value);
    if (length >= max_len || length > HOME_AI_RULE_STRING_POOL_BYTES - 1U) {
        return false;
    }
    home_ai_rule_store_t *store = &s_stores[store_index];
    if (store->pool_used + length + 1U > HOME_AI_RULE_STRING_POOL_BYTES ||
        store->pool_used > UINT16_MAX) {
        return false;
    }
    *out_offset = (uint16_t)store->pool_used;
    memcpy(s_pools[store_index] + store->pool_used, value, length + 1U);
    store->pool_used += length + 1U;
    return true;
}

static void store_clear(uint8_t store_index)
{
    if (store_index >= HOME_AI_RULE_STORE_COUNT) {
        return;
    }
    memset(&s_stores[store_index], 0, sizeof(s_stores[store_index]));
    if (s_pools[store_index] != NULL) {
        memset(s_pools[store_index], 0, HOME_AI_RULE_STRING_POOL_BYTES);
    }
}

static uint8_t staging_store_index(void)
{
    for (uint8_t index = 0U; index < HOME_AI_RULE_STORE_COUNT; ++index) {
        if (index != s_active_store && index != s_previous_store) {
            return index;
        }
    }
    return (uint8_t)((s_active_store + 1U) % HOME_AI_RULE_STORE_COUNT);
}

static bool parse_field(const char *text, home_ai_rule_field_t *out)
{
    if (text == NULL || out == NULL) return false;
    if (strcmp(text, "presence_state") == 0) *out = HOME_AI_FIELD_PRESENCE_STATE;
    else if (strcmp(text, "stable_target_count") == 0) *out = HOME_AI_FIELD_STABLE_TARGET_COUNT;
    else if (strcmp(text, "occupancy_mode") == 0) *out = HOME_AI_FIELD_OCCUPANCY_MODE;
    else if (strcmp(text, "environment_fresh") == 0) *out = HOME_AI_FIELD_ENVIRONMENT_FRESH;
    else if (strcmp(text, "radar_fresh") == 0) *out = HOME_AI_FIELD_RADAR_FRESH;
    else if (strcmp(text, "quiet_state") == 0) *out = HOME_AI_FIELD_QUIET_STATE;
    else if (strcmp(text, "time_window") == 0) *out = HOME_AI_FIELD_TIME_WINDOW;
    else if (strcmp(text, "temperature_c") == 0) *out = HOME_AI_FIELD_TEMPERATURE_C;
    else if (strcmp(text, "humidity_percent") == 0) *out = HOME_AI_FIELD_HUMIDITY_PERCENT;
    else if (strcmp(text, "air_quality_score") == 0) *out = HOME_AI_FIELD_AIR_QUALITY_SCORE;
    else if (strcmp(text, "weather_dark") == 0) *out = HOME_AI_FIELD_WEATHER_DARK;
    else return false;
    return true;
}

static bool parse_operator(const char *text, home_ai_rule_operator_t *out)
{
    if (text == NULL || out == NULL) return false;
    if (strcmp(text, "eq") == 0) *out = HOME_AI_OPERATOR_EQ;
    else if (strcmp(text, "neq") == 0) *out = HOME_AI_OPERATOR_NEQ;
    else if (strcmp(text, "gt") == 0) *out = HOME_AI_OPERATOR_GT;
    else if (strcmp(text, "gte") == 0) *out = HOME_AI_OPERATOR_GTE;
    else if (strcmp(text, "lt") == 0) *out = HOME_AI_OPERATOR_LT;
    else if (strcmp(text, "lte") == 0) *out = HOME_AI_OPERATOR_LTE;
    else if (strcmp(text, "in") == 0) *out = HOME_AI_OPERATOR_IN;
    else if (strcmp(text, "range") == 0) *out = HOME_AI_OPERATOR_RANGE;
    else return false;
    return true;
}

static bool field_value_kind(home_ai_rule_field_t field, home_ai_rule_value_kind_t *out)
{
    if (out == NULL) return false;
    switch (field) {
    case HOME_AI_FIELD_PRESENCE_STATE:
    case HOME_AI_FIELD_OCCUPANCY_MODE:
    case HOME_AI_FIELD_QUIET_STATE:
    case HOME_AI_FIELD_TIME_WINDOW:
        *out = HOME_AI_VALUE_TEXT;
        return true;
    case HOME_AI_FIELD_STABLE_TARGET_COUNT:
    case HOME_AI_FIELD_TEMPERATURE_C:
    case HOME_AI_FIELD_HUMIDITY_PERCENT:
    case HOME_AI_FIELD_AIR_QUALITY_SCORE:
        *out = HOME_AI_VALUE_NUMBER;
        return true;
    case HOME_AI_FIELD_ENVIRONMENT_FRESH:
    case HOME_AI_FIELD_RADAR_FRESH:
    case HOME_AI_FIELD_WEATHER_DARK:
        *out = HOME_AI_VALUE_BOOL;
        return true;
    default:
        return false;
    }
}

static bool operator_allowed(home_ai_rule_value_kind_t kind, home_ai_rule_operator_t operator)
{
    if (kind == HOME_AI_VALUE_NUMBER) {
        return true;
    }
    return operator == HOME_AI_OPERATOR_EQ || operator == HOME_AI_OPERATOR_NEQ ||
           operator == HOME_AI_OPERATOR_IN;
}

static bool cjson_uint32(cJSON *item, uint32_t max, uint32_t *out)
{
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0 ||
        item->valuedouble > (double)max || item->valuedouble != (double)item->valueint) {
        return false;
    }
    *out = (uint32_t)item->valueint;
    return true;
}

static bool parse_scalar_value(uint8_t store_index,
                               home_ai_rule_value_kind_t kind,
                               cJSON *item,
                               home_ai_rule_condition_t *condition,
                               uint8_t index)
{
    if (item == NULL || condition == NULL || index >= 4U) {
        return false;
    }
    switch (kind) {
    case HOME_AI_VALUE_TEXT:
        return cJSON_IsString(item) && text_fits(item->valuestring, HOME_AI_RULE_MAX_TEXT_VALUE_LEN) &&
               store_add_string(store_index,
                                item->valuestring,
                                HOME_AI_RULE_MAX_TEXT_VALUE_LEN,
                                &condition->text_offsets[index]);
    case HOME_AI_VALUE_NUMBER:
        if (!cJSON_IsNumber(item) || item->valuedouble < -1000000.0 ||
            item->valuedouble > 1000000.0) {
            return false;
        }
        condition->number_values[index] = (float)item->valuedouble;
        return true;
    case HOME_AI_VALUE_BOOL:
        if (!cJSON_IsBool(item)) return false;
        condition->bool_values[index] = cJSON_IsTrue(item);
        return true;
    default:
        return false;
    }
}

static bool parse_condition(uint8_t store_index, cJSON *input, home_ai_rule_condition_t *out)
{
    if (!cJSON_IsObject(input) || out == NULL) return false;
    memset(out, 0, sizeof(*out));
    for (size_t index = 0U; index < 4U; ++index) out->text_offsets[index] = HOME_AI_RULE_STRING_NONE;

    cJSON *field = cJSON_GetObjectItemCaseSensitive(input, "field");
    cJSON *operator = cJSON_GetObjectItemCaseSensitive(input, "operator");
    cJSON *value = cJSON_GetObjectItemCaseSensitive(input, "value");
    cJSON *duration = cJSON_GetObjectItemCaseSensitive(input, "duration_ms");
    if (!cJSON_IsString(field) || !cJSON_IsString(operator) || value == NULL ||
        !parse_field(field->valuestring, &out->field) ||
        !parse_operator(operator->valuestring, &out->operator) ||
        !field_value_kind(out->field, &out->value_kind) ||
        !operator_allowed(out->value_kind, out->operator)) {
        return false;
    }
    if (duration != NULL && !cjson_uint32(duration, 3600000U, &out->duration_ms)) return false;

    if (out->operator == HOME_AI_OPERATOR_RANGE) {
        if (out->value_kind != HOME_AI_VALUE_NUMBER || !cJSON_IsArray(value) ||
            cJSON_GetArraySize(value) != 2 ||
            !parse_scalar_value(store_index, out->value_kind, cJSON_GetArrayItem(value, 0), out, 0U) ||
            !parse_scalar_value(store_index, out->value_kind, cJSON_GetArrayItem(value, 1), out, 1U) ||
            out->number_values[0] > out->number_values[1]) {
            return false;
        }
        out->value_count = 2U;
        return true;
    }
    if (out->operator == HOME_AI_OPERATOR_IN) {
        if (!cJSON_IsArray(value)) return false;
        const int count = cJSON_GetArraySize(value);
        if (count < 1 || count > 4) return false;
        for (int index = 0; index < count; ++index) {
            if (!parse_scalar_value(store_index,
                                    out->value_kind,
                                    cJSON_GetArrayItem(value, index),
                                    out,
                                    (uint8_t)index)) {
                return false;
            }
        }
        out->value_count = (uint8_t)count;
        return true;
    }
    if (!parse_scalar_value(store_index, out->value_kind, value, out, 0U)) return false;
    out->value_count = 1U;
    return true;
}

static bool parse_device_type(const char *text, home_ai_rule_device_type_t *out)
{
    if (text == NULL || out == NULL) return false;
    if (strcmp(text, "light") == 0) *out = HOME_AI_RULE_DEVICE_LIGHT;
    else if (strcmp(text, "air_conditioner") == 0) *out = HOME_AI_RULE_DEVICE_AIR_CONDITIONER;
    else if (strcmp(text, "fan") == 0) *out = HOME_AI_RULE_DEVICE_FAN;
    else return false;
    return true;
}

static bool parse_action_name(const char *text, home_ai_rule_action_t *out)
{
    if (text == NULL || out == NULL) return false;
    if (strcmp(text, "turn_on") == 0) *out = HOME_AI_RULE_ACTION_TURN_ON;
    else if (strcmp(text, "turn_off") == 0) *out = HOME_AI_RULE_ACTION_TURN_OFF;
    else if (strcmp(text, "pause_automation") == 0) *out = HOME_AI_RULE_ACTION_PAUSE_AUTOMATION;
    else if (strcmp(text, "resume_automation") == 0) *out = HOME_AI_RULE_ACTION_RESUME_AUTOMATION;
    else if (strcmp(text, "play_prompt") == 0) *out = HOME_AI_RULE_ACTION_PLAY_PROMPT;
    else return false;
    return true;
}

static bool parse_action(uint8_t store_index, cJSON *input, home_ai_rule_action_definition_t *out)
{
    if (!cJSON_IsObject(input) || out == NULL) return false;
    memset(out, 0, sizeof(*out));
    out->device_id_offset = HOME_AI_RULE_STRING_NONE;
    out->prompt_offset = HOME_AI_RULE_STRING_NONE;
    cJSON *action = cJSON_GetObjectItemCaseSensitive(input, "action");
    cJSON *device_id = cJSON_GetObjectItemCaseSensitive(input, "device_id");
    cJSON *device_type = cJSON_GetObjectItemCaseSensitive(input, "device_type");
    cJSON *prompt = cJSON_GetObjectItemCaseSensitive(input, "prompt");
    cJSON *minimum = cJSON_GetObjectItemCaseSensitive(input, "minimum_active_seconds");
    if (!cJSON_IsString(action) || !parse_action_name(action->valuestring, &out->action)) return false;
    if (minimum != NULL && !cjson_uint32(minimum, 86400U, &out->minimum_active_seconds)) return false;
    if (out->action == HOME_AI_RULE_ACTION_TURN_ON || out->action == HOME_AI_RULE_ACTION_TURN_OFF) {
        if (!cJSON_IsString(device_id) || !cJSON_IsString(device_type) ||
            !text_fits(device_id->valuestring, HOME_AI_RULE_DEVICE_ID_LEN) ||
            !parse_device_type(device_type->valuestring, &out->device_type) ||
            !store_add_string(store_index,
                              device_id->valuestring,
                              HOME_AI_RULE_DEVICE_ID_LEN,
                              &out->device_id_offset)) {
            return false;
        }
    }
    if (prompt != NULL) {
        if (!cJSON_IsString(prompt) || !text_fits(prompt->valuestring, HOME_AI_RULE_PROMPT_LEN) ||
            !store_add_string(store_index,
                              prompt->valuestring,
                              HOME_AI_RULE_PROMPT_LEN,
                              &out->prompt_offset)) {
            return false;
        }
    }
    if (out->action == HOME_AI_RULE_ACTION_PLAY_PROMPT && out->prompt_offset == HOME_AI_RULE_STRING_NONE) {
        return false;
    }
    return true;
}

static bool parse_rule_type(const char *text, home_ai_rule_type_t *out)
{
    if (text == NULL || out == NULL) return false;
    if (strcmp(text, "basic_automation") == 0) *out = HOME_AI_RULE_TYPE_BASIC;
    else if (strcmp(text, "habit_learning") == 0) *out = HOME_AI_RULE_TYPE_HABIT;
    else if (strcmp(text, "safety") == 0) *out = HOME_AI_RULE_TYPE_SAFETY;
    else if (strcmp(text, "manual") == 0) *out = HOME_AI_RULE_TYPE_MANUAL;
    else return false;
    return true;
}

static bool parse_offline_policy(const char *text, home_ai_rule_offline_policy_t *out)
{
    if (text == NULL || out == NULL) return false;
    if (strcmp(text, "continue") == 0) *out = HOME_AI_OFFLINE_CONTINUE;
    else if (strcmp(text, "pause") == 0) *out = HOME_AI_OFFLINE_PAUSE;
    else if (strcmp(text, "require_server") == 0) *out = HOME_AI_OFFLINE_REQUIRE_SERVER;
    else return false;
    return true;
}

static bool parse_rule(uint8_t store_index, cJSON *input, home_ai_rule_definition_t *out)
{
    if (!cJSON_IsObject(input) || out == NULL) return false;
    memset(out, 0, sizeof(*out));
    cJSON *rule_id = cJSON_GetObjectItemCaseSensitive(input, "rule_id");
    cJSON *room_id = cJSON_GetObjectItemCaseSensitive(input, "room_id");
    cJSON *version = cJSON_GetObjectItemCaseSensitive(input, "version");
    cJSON *rule_type = cJSON_GetObjectItemCaseSensitive(input, "rule_type");
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(input, "enabled");
    cJSON *priority = cJSON_GetObjectItemCaseSensitive(input, "priority");
    cJSON *conditions = cJSON_GetObjectItemCaseSensitive(input, "conditions");
    cJSON *actions = cJSON_GetObjectItemCaseSensitive(input, "actions");
    cJSON *cooldown = cJSON_GetObjectItemCaseSensitive(input, "cooldown_seconds");
    cJSON *minimum = cJSON_GetObjectItemCaseSensitive(input, "minimum_active_seconds");
    cJSON *offline = cJSON_GetObjectItemCaseSensitive(input, "offline_policy");
    cJSON *expires = cJSON_GetObjectItemCaseSensitive(input, "expires_at_ms");
    if (!cJSON_IsString(rule_id) || !cJSON_IsString(room_id) || !cJSON_IsNumber(version) ||
        !cJSON_IsString(rule_type) || !cJSON_IsArray(conditions) || !cJSON_IsArray(actions) ||
        !text_fits(rule_id->valuestring, HOME_AI_RULE_ID_LEN) ||
        !text_fits(room_id->valuestring, HOME_AI_RULE_ROOM_ID_LEN) ||
        !cjson_uint32(version, INT_MAX, &out->version) ||
        !parse_rule_type(rule_type->valuestring, &out->type)) {
        return false;
    }
    out->enabled = enabled == NULL || cJSON_IsTrue(enabled);
    uint32_t numeric_priority = 500U;
    if (priority != NULL && !cjson_uint32(priority, 1000U, &numeric_priority)) return false;
    out->priority = (uint16_t)numeric_priority;
    if (cooldown != NULL && !cjson_uint32(cooldown, 86400U, &out->cooldown_seconds)) return false;
    if (minimum != NULL && !cjson_uint32(minimum, 86400U, &out->minimum_active_seconds)) return false;
    if (offline != NULL && (!cJSON_IsString(offline) ||
                            !parse_offline_policy(offline->valuestring, &out->offline_policy))) return false;
    if (expires != NULL && !cJSON_IsNull(expires)) {
        if (!cJSON_IsNumber(expires) || expires->valuedouble < 1.0 ||
            expires->valuedouble > 9007199254740991.0) return false;
        out->expires_at_ms = (uint64_t)expires->valuedouble;
    }
    const int condition_count = cJSON_GetArraySize(conditions);
    const int action_count = cJSON_GetArraySize(actions);
    if (condition_count < 1 || condition_count > (int)HOME_AI_MAX_CONDITIONS_PER_RULE ||
        action_count < 1 || action_count > (int)HOME_AI_MAX_ACTIONS_PER_RULE ||
        !store_add_string(store_index, rule_id->valuestring, HOME_AI_RULE_ID_LEN, &out->rule_id_offset) ||
        !store_add_string(store_index, room_id->valuestring, HOME_AI_RULE_ROOM_ID_LEN, &out->room_id_offset)) {
        return false;
    }
    for (int index = 0; index < condition_count; ++index) {
        if (!parse_condition(store_index, cJSON_GetArrayItem(conditions, index), &out->conditions[index])) {
            return false;
        }
    }
    for (int index = 0; index < action_count; ++index) {
        if (!parse_action(store_index, cJSON_GetArrayItem(actions, index), &out->actions[index])) {
            return false;
        }
    }
    out->condition_count = (uint8_t)condition_count;
    out->action_count = (uint8_t)action_count;
    out->valid = true;
    return true;
}

static bool copy_offset(uint8_t destination_store,
                        uint8_t source_store,
                        uint16_t source_offset,
                        size_t max_len,
                        uint16_t *out_offset)
{
    if (source_offset == HOME_AI_RULE_STRING_NONE) {
        *out_offset = HOME_AI_RULE_STRING_NONE;
        return true;
    }
    return store_add_string(destination_store,
                            store_string(source_store, source_offset),
                            max_len,
                            out_offset);
}

static bool copy_rule(uint8_t destination_store,
                      uint8_t source_store,
                      const home_ai_rule_definition_t *source,
                      home_ai_rule_definition_t *out)
{
    if (source == NULL || out == NULL || !source->valid) return false;
    *out = *source;
    if (!copy_offset(destination_store,
                     source_store,
                     source->rule_id_offset,
                     HOME_AI_RULE_ID_LEN,
                     &out->rule_id_offset) ||
        !copy_offset(destination_store,
                     source_store,
                     source->room_id_offset,
                     HOME_AI_RULE_ROOM_ID_LEN,
                     &out->room_id_offset)) {
        return false;
    }
    for (uint8_t condition = 0U; condition < source->condition_count; ++condition) {
        if (source->conditions[condition].value_kind != HOME_AI_VALUE_TEXT) continue;
        for (uint8_t value = 0U; value < source->conditions[condition].value_count; ++value) {
            if (!copy_offset(destination_store,
                             source_store,
                             source->conditions[condition].text_offsets[value],
                             HOME_AI_RULE_MAX_TEXT_VALUE_LEN,
                             &out->conditions[condition].text_offsets[value])) {
                return false;
            }
        }
    }
    for (uint8_t action = 0U; action < source->action_count; ++action) {
        if (!copy_offset(destination_store,
                         source_store,
                         source->actions[action].device_id_offset,
                         HOME_AI_RULE_DEVICE_ID_LEN,
                         &out->actions[action].device_id_offset) ||
            !copy_offset(destination_store,
                         source_store,
                         source->actions[action].prompt_offset,
                         HOME_AI_RULE_PROMPT_LEN,
                         &out->actions[action].prompt_offset)) {
            return false;
        }
    }
    return true;
}

static int find_rule(uint8_t store_index, const char *rule_id)
{
    if (store_index >= HOME_AI_RULE_STORE_COUNT || rule_id == NULL) return -1;
    for (size_t index = 0U; index < HOME_AI_MAX_RULES; ++index) {
        const home_ai_rule_definition_t *rule = &s_stores[store_index].rules[index];
        if (rule->valid && strcmp(store_string(store_index, rule->rule_id_offset), rule_id) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static void result_item(home_ai_rule_activation_result_t *result,
                        const char *rule_id,
                        bool accepted,
                        bool retained_previous,
                        const char *code)
{
    if (result == NULL || result->item_count >= HOME_AI_MAX_RULES) return;
    home_ai_rule_activation_item_t *item = &result->items[result->item_count++];
    copy_text(item->rule_id, sizeof(item->rule_id), rule_id);
    copy_text(item->code, sizeof(item->code), code);
    item->accepted = accepted;
    item->retained_previous = retained_previous;
    if (accepted) ++result->accepted_count;
    else ++result->rejected_count;
}

static bool payload_base_valid(cJSON *root, uint32_t *out_version)
{
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *rooms = cJSON_GetObjectItemCaseSensitive(root, "rooms");
    cJSON *rules = cJSON_GetObjectItemCaseSensitive(root, "rules");
    uint32_t package_version = 0U;
    if (!cJSON_IsObject(root) || !cjson_uint32(schema, HOME_AI_RULE_SCHEMA_VERSION, &package_version) ||
        package_version != HOME_AI_RULE_SCHEMA_VERSION || !cjson_uint32(version, INT_MAX, &package_version) ||
        !cJSON_IsArray(rooms) || !cJSON_IsArray(rules) ||
        cJSON_GetArraySize(rooms) < 1 || cJSON_GetArraySize(rooms) > (int)HOME_AI_ROOM_STATE_COUNT ||
        cJSON_GetArraySize(rules) > (int)HOME_AI_MAX_RULES) {
        return false;
    }
    for (int index = 0; index < cJSON_GetArraySize(rooms); ++index) {
        cJSON *room = cJSON_GetArrayItem(rooms, index);
        cJSON *room_id = cJSON_GetObjectItemCaseSensitive(room, "room_id");
        if (!cJSON_IsObject(room) || !cJSON_IsString(room_id) ||
            !text_fits(room_id->valuestring, HOME_AI_RULE_ROOM_ID_LEN)) {
            return false;
        }
    }
    *out_version = package_version;
    return true;
}

static bool payload_has_duplicate_rule_id(cJSON *rules)
{
    for (int outer = 0; outer < cJSON_GetArraySize(rules); ++outer) {
        cJSON *left = cJSON_GetArrayItem(rules, outer);
        cJSON *left_id = cJSON_GetObjectItemCaseSensitive(left, "rule_id");
        if (!cJSON_IsString(left_id) || !text_fits(left_id->valuestring, HOME_AI_RULE_ID_LEN)) {
            continue;
        }
        for (int inner = outer + 1; inner < cJSON_GetArraySize(rules); ++inner) {
            cJSON *right = cJSON_GetArrayItem(rules, inner);
            cJSON *right_id = cJSON_GetObjectItemCaseSensitive(right, "rule_id");
            if (cJSON_IsString(right_id) && strcmp(left_id->valuestring, right_id->valuestring) == 0) {
                return true;
            }
        }
    }
    return false;
}

bool home_ai_rule_engine_init(void)
{
    if (s_initialized) {
        if (!engine_lock()) return false;
        for (uint8_t index = 0U; index < HOME_AI_RULE_STORE_COUNT; ++index) store_clear(index);
        memset(s_runtime, 0, HOME_AI_MAX_RULES * sizeof(*s_runtime));
        memset(&s_activation, 0, sizeof(s_activation));
        s_activation.state = HOME_AI_RULE_ACTIVATION_EMPTY;
        s_active_store = 0U;
        s_previous_store = 1U;
        s_decision_sequence = 0U;
        engine_unlock();
        return true;
    }
#ifdef HOME_AI_RULE_ENGINE_HOST_TEST
    s_stores = s_host_stores;
    s_runtime = s_host_runtime;
    for (size_t index = 0U; index < HOME_AI_RULE_STORE_COUNT; ++index) {
        s_pools[index] = s_host_pools[index];
    }
#else
    s_stores = heap_caps_calloc(HOME_AI_RULE_STORE_COUNT,
                                 sizeof(*s_stores),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_runtime = heap_caps_calloc(HOME_AI_MAX_RULES,
                                  sizeof(*s_runtime),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_stores == NULL || s_runtime == NULL) {
        heap_caps_free(s_stores);
        heap_caps_free(s_runtime);
        s_stores = NULL;
        s_runtime = NULL;
        return false;
    }
    s_rule_lock = xSemaphoreCreateMutexStatic(&s_rule_lock_storage);
    if (s_rule_lock == NULL) {
        heap_caps_free(s_stores);
        heap_caps_free(s_runtime);
        s_stores = NULL;
        s_runtime = NULL;
        return false;
    }
    for (size_t index = 0U; index < HOME_AI_RULE_STORE_COUNT; ++index) {
        s_pools[index] = heap_caps_calloc(1,
                                          HOME_AI_RULE_STRING_POOL_BYTES,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_pools[index] == NULL) {
            for (size_t release = 0U; release < index; ++release) {
                heap_caps_free(s_pools[release]);
                s_pools[release] = NULL;
            }
            heap_caps_free(s_stores);
            heap_caps_free(s_runtime);
            s_stores = NULL;
            s_runtime = NULL;
            return false;
        }
    }
#endif
    for (uint8_t index = 0U; index < HOME_AI_RULE_STORE_COUNT; ++index) store_clear(index);
    memset(s_runtime, 0, HOME_AI_MAX_RULES * sizeof(*s_runtime));
    memset(&s_activation, 0, sizeof(s_activation));
    s_activation.state = HOME_AI_RULE_ACTIVATION_EMPTY;
    s_active_store = 0U;
    s_previous_store = 1U;
    s_decision_sequence = 0U;
    s_initialized = true;
    return true;
}

void home_ai_rule_engine_reset(void)
{
    if (!s_initialized || !engine_lock()) return;
    for (uint8_t index = 0U; index < HOME_AI_RULE_STORE_COUNT; ++index) store_clear(index);
    memset(s_runtime, 0, HOME_AI_MAX_RULES * sizeof(*s_runtime));
    memset(&s_activation, 0, sizeof(s_activation));
    s_activation.state = HOME_AI_RULE_ACTIVATION_EMPTY;
    s_active_store = 0U;
    s_previous_store = 1U;
    engine_unlock();
}

esp_err_t home_ai_rule_engine_apply_payload(const char *payload,
                                            size_t payload_len,
                                            home_ai_rule_activation_result_t *out_result)
{
    home_ai_rule_activation_result_t result = {0};
    result.state = HOME_AI_RULE_ACTIVATION_REJECTED;
    if (!s_initialized || payload == NULL || payload_len == 0U ||
        payload_len > HOME_AI_RULE_PACKAGE_BYTES) {
        copy_text(result.error_code, sizeof(result.error_code), "package_size_invalid");
        if (out_result != NULL) *out_result = result;
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_ParseWithLength(payload, payload_len);
    uint32_t package_version = 0U;
    if (root == NULL || !payload_base_valid(root, &package_version)) {
        if (root != NULL) cJSON_Delete(root);
        copy_text(result.error_code, sizeof(result.error_code), "package_structure_invalid");
        if (out_result != NULL) *out_result = result;
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *rules = cJSON_GetObjectItemCaseSensitive(root, "rules");
    if (payload_has_duplicate_rule_id(rules)) {
        cJSON_Delete(root);
        copy_text(result.error_code, sizeof(result.error_code), "rule_id_duplicate");
        if (out_result != NULL) *out_result = result;
        return ESP_ERR_INVALID_ARG;
    }

    if (!engine_lock()) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_stores[s_active_store].valid && package_version < s_stores[s_active_store].package_version) {
        engine_unlock();
        cJSON_Delete(root);
        copy_text(result.error_code, sizeof(result.error_code), "package_version_stale");
        if (out_result != NULL) *out_result = result;
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t staging = staging_store_index();
    store_clear(staging);
    for (int index = 0; index < cJSON_GetArraySize(rules); ++index) {
        cJSON *raw_rule = cJSON_GetArrayItem(rules, index);
        cJSON *raw_id = cJSON_GetObjectItemCaseSensitive(raw_rule, "rule_id");
        const char *rule_id = cJSON_IsString(raw_id) ? raw_id->valuestring : "";
        const size_t pool_before = s_stores[staging].pool_used;
        home_ai_rule_definition_t parsed = {0};
        const bool parsed_ok = parse_rule(staging, raw_rule, &parsed);
        if (parsed_ok) {
            size_t slot = 0U;
            while (slot < HOME_AI_MAX_RULES && s_stores[staging].rules[slot].valid) ++slot;
            if (slot >= HOME_AI_MAX_RULES) {
                s_stores[staging].pool_used = pool_before;
                result_item(&result, rule_id, false, false, "resource_limit");
                continue;
            }
            s_stores[staging].rules[slot] = parsed;
            result_item(&result, rule_id, true, false, "accepted");
            continue;
        }
        s_stores[staging].pool_used = pool_before;
        const int previous_rule = text_fits(rule_id, HOME_AI_RULE_ID_LEN) ?
                                      find_rule(s_active_store, rule_id) : -1;
        bool retained = false;
        if (previous_rule >= 0) {
            size_t slot = 0U;
            while (slot < HOME_AI_MAX_RULES && s_stores[staging].rules[slot].valid) ++slot;
            if (slot < HOME_AI_MAX_RULES) {
                const size_t retained_pool_before = s_stores[staging].pool_used;
                retained = copy_rule(staging,
                                     s_active_store,
                                     &s_stores[s_active_store].rules[previous_rule],
                                     &s_stores[staging].rules[slot]);
                if (!retained) {
                    s_stores[staging].pool_used = retained_pool_before;
                    memset(&s_stores[staging].rules[slot], 0, sizeof(s_stores[staging].rules[slot]));
                }
            }
        }
        result_item(&result, rule_id, false, retained, retained ? "retained_previous" : "rule_invalid");
    }
    size_t active_count = 0U;
    for (size_t index = 0U; index < HOME_AI_MAX_RULES; ++index) {
        if (s_stores[staging].rules[index].valid) ++active_count;
    }
    s_stores[staging].valid = true;
    s_stores[staging].package_version = package_version;
    const uint8_t old_active = s_active_store;
    s_active_store = staging;
    s_previous_store = old_active;
    memset(s_runtime, 0, HOME_AI_MAX_RULES * sizeof(*s_runtime));
    result.package_version = package_version;
    result.active_rule_count = (uint32_t)active_count;
    result.state = result.rejected_count == 0U ? HOME_AI_RULE_ACTIVATION_ACTIVE :
                                                 HOME_AI_RULE_ACTIVATION_ACTIVE_PARTIAL;
    s_activation = result;
    engine_unlock();
    cJSON_Delete(root);
    if (out_result != NULL) *out_result = result;
    return ESP_OK;
}

bool home_ai_rule_engine_rollback(home_ai_rule_activation_result_t *out_result)
{
    if (!s_initialized || !engine_lock()) return false;
    if (!s_stores[s_previous_store].valid) {
        engine_unlock();
        return false;
    }
    const uint8_t current = s_active_store;
    s_active_store = s_previous_store;
    s_previous_store = current;
    memset(s_runtime, 0, HOME_AI_MAX_RULES * sizeof(*s_runtime));
    memset(&s_activation, 0, sizeof(s_activation));
    s_activation.state = HOME_AI_RULE_ACTIVATION_ACTIVE;
    s_activation.package_version = s_stores[s_active_store].package_version;
    for (size_t index = 0U; index < HOME_AI_MAX_RULES; ++index) {
        if (s_stores[s_active_store].rules[index].valid) ++s_activation.active_rule_count;
    }
    s_activation.accepted_count = s_activation.active_rule_count;
    if (out_result != NULL) *out_result = s_activation;
    engine_unlock();
    return true;
}

static const home_ai_room_state_t *room_for_rule(const home_ai_rule_evaluation_context_t *context,
                                                  uint8_t store_index,
                                                  const home_ai_rule_definition_t *rule,
                                                  size_t *out_room_index)
{
    if (context == NULL || context->rooms == NULL || rule == NULL) return NULL;
    const char *room_id = store_string(store_index, rule->room_id_offset);
    for (size_t index = 0U; index < context->room_count; ++index) {
        if (strcmp(context->rooms[index].room_id, room_id) == 0) {
            if (out_room_index != NULL) *out_room_index = index;
            return &context->rooms[index];
        }
    }
    return NULL;
}

static bool compare_text(const home_ai_rule_condition_t *condition,
                         uint8_t store_index,
                         const char *actual)
{
    const bool equal = strcmp(actual != NULL ? actual : "",
                              store_string(store_index, condition->text_offsets[0])) == 0;
    switch (condition->operator) {
    case HOME_AI_OPERATOR_EQ: return equal;
    case HOME_AI_OPERATOR_NEQ: return !equal;
    case HOME_AI_OPERATOR_IN:
        for (uint8_t index = 0U; index < condition->value_count; ++index) {
            if (strcmp(actual != NULL ? actual : "",
                       store_string(store_index, condition->text_offsets[index])) == 0) return true;
        }
        return false;
    default: return false;
    }
}

static bool compare_bool(const home_ai_rule_condition_t *condition, bool actual)
{
    const bool equal = actual == condition->bool_values[0];
    switch (condition->operator) {
    case HOME_AI_OPERATOR_EQ: return equal;
    case HOME_AI_OPERATOR_NEQ: return !equal;
    case HOME_AI_OPERATOR_IN:
        for (uint8_t index = 0U; index < condition->value_count; ++index) {
            if (actual == condition->bool_values[index]) return true;
        }
        return false;
    default: return false;
    }
}

static bool compare_number(const home_ai_rule_condition_t *condition, float actual)
{
    const float value = condition->number_values[0];
    switch (condition->operator) {
    case HOME_AI_OPERATOR_EQ: return actual == value;
    case HOME_AI_OPERATOR_NEQ: return actual != value;
    case HOME_AI_OPERATOR_GT: return actual > value;
    case HOME_AI_OPERATOR_GTE: return actual >= value;
    case HOME_AI_OPERATOR_LT: return actual < value;
    case HOME_AI_OPERATOR_LTE: return actual <= value;
    case HOME_AI_OPERATOR_RANGE:
        return actual >= condition->number_values[0] && actual <= condition->number_values[1];
    case HOME_AI_OPERATOR_IN:
        for (uint8_t index = 0U; index < condition->value_count; ++index) {
            if (actual == condition->number_values[index]) return true;
        }
        return false;
    default: return false;
    }
}

static bool evaluate_condition(const home_ai_rule_evaluation_context_t *context,
                               uint8_t store_index,
                               const home_ai_rule_condition_t *condition,
                               const home_ai_room_state_t *room,
                               const home_ai_rule_environment_t *environment)
{
    if (context == NULL || condition == NULL || room == NULL) return false;
    switch (condition->field) {
    case HOME_AI_FIELD_PRESENCE_STATE:
        return compare_text(condition, store_index,
                            home_ai_room_presence_state_name(room->presence_state));
    case HOME_AI_FIELD_STABLE_TARGET_COUNT:
        return compare_number(condition, (float)room->stable_target_count);
    case HOME_AI_FIELD_OCCUPANCY_MODE:
        return compare_text(condition, store_index,
                            home_ai_room_occupancy_mode_name(room->occupancy_mode));
    case HOME_AI_FIELD_ENVIRONMENT_FRESH:
        return compare_bool(condition, room->environment_fresh);
    case HOME_AI_FIELD_RADAR_FRESH:
        return compare_bool(condition, room->radar_fresh);
    case HOME_AI_FIELD_QUIET_STATE:
        switch (room->quiet_state) {
        case HOME_AI_ROOM_QUIET_SCHEDULED: return compare_text(condition, store_index, "scheduled");
        case HOME_AI_ROOM_QUIET_SLEEP_CONFIRMED: return compare_text(condition, store_index, "sleep_confirmed");
        case HOME_AI_ROOM_QUIET_TEMPORARY_AWAKE: return compare_text(condition, store_index, "temporary_awake");
        case HOME_AI_ROOM_QUIET_NORMAL:
        default: return compare_text(condition, store_index, "normal");
        }
    case HOME_AI_FIELD_TIME_WINDOW:
        return context->time_window != NULL && compare_text(condition, store_index, context->time_window);
    case HOME_AI_FIELD_TEMPERATURE_C:
        return environment != NULL && environment->valid && compare_number(condition, environment->temperature_c);
    case HOME_AI_FIELD_HUMIDITY_PERCENT:
        return environment != NULL && environment->valid && compare_number(condition, environment->humidity_percent);
    case HOME_AI_FIELD_AIR_QUALITY_SCORE:
        return environment != NULL && environment->valid && compare_number(condition, environment->air_quality_score);
    case HOME_AI_FIELD_WEATHER_DARK:
        return context->weather_available && compare_bool(condition, context->weather_dark);
    default:
        return false;
    }
}

static bool rule_eligible(uint8_t store_index,
                          uint8_t rule_slot,
                          const home_ai_rule_evaluation_context_t *context)
{
    const home_ai_rule_definition_t *rule = &s_stores[store_index].rules[rule_slot];
    home_ai_rule_runtime_t *runtime = &s_runtime[rule_slot];
    size_t room_index = 0U;
    const home_ai_room_state_t *room = room_for_rule(context, store_index, rule, &room_index);
    const home_ai_rule_environment_t *environment =
        context->environment != NULL && room_index < context->environment_count ?
            &context->environment[room_index] : NULL;
    bool conditions_met = rule->valid && rule->enabled && room != NULL &&
                          (rule->expires_at_ms == 0U || context->now_ms < rule->expires_at_ms) &&
                          !(rule->offline_policy != HOME_AI_OFFLINE_CONTINUE && !context->server_online);
    for (uint8_t index = 0U; conditions_met && index < rule->condition_count; ++index) {
        const bool condition_met = evaluate_condition(context,
                                                      store_index,
                                                      &rule->conditions[index],
                                                      room,
                                                      environment);
        if (!condition_met) {
            runtime->condition_true_since_ms[index] = 0U;
            conditions_met = false;
            break;
        }
        if (runtime->condition_true_since_ms[index] == 0U) {
            runtime->condition_true_since_ms[index] = context->now_ms;
        }
        if (context->now_ms < runtime->condition_true_since_ms[index] ||
            context->now_ms - runtime->condition_true_since_ms[index] <
                rule->conditions[index].duration_ms) {
            conditions_met = false;
            break;
        }
    }
    if (!conditions_met) {
        runtime->was_eligible = false;
        return false;
    }
    if (runtime->last_trigger_ms != 0U) {
        const uint64_t cooldown_ms = (uint64_t)rule->cooldown_seconds * 1000U;
        if (cooldown_ms == 0U) {
            if (runtime->was_eligible) return false;
        } else if (context->now_ms >= runtime->last_trigger_ms &&
                   context->now_ms - runtime->last_trigger_ms < cooldown_ms) {
            runtime->was_eligible = true;
            return false;
        }
    }
    runtime->was_eligible = true;
    return true;
}

static uint16_t effective_priority(const home_ai_rule_definition_t *rule)
{
    return rule->type == HOME_AI_RULE_TYPE_SAFETY ? 1000U : rule->priority;
}

static bool device_claimed(char claimed[][HOME_AI_RULE_DEVICE_ID_LEN],
                           size_t claim_count,
                           const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') return false;
    for (size_t index = 0U; index < claim_count; ++index) {
        if (strcmp(claimed[index], device_id) == 0) return true;
    }
    return false;
}

size_t home_ai_rule_engine_evaluate(const home_ai_rule_evaluation_context_t *context,
                                    home_ai_rule_decision_t *out,
                                    size_t capacity)
{
    if (!s_initialized || context == NULL || context->now_ms == 0U || out == NULL || capacity == 0U ||
        !engine_lock()) return 0U;
    const uint8_t store_index = s_active_store;
    if (!s_stores[store_index].valid) {
        engine_unlock();
        return 0U;
    }
    bool eligible[HOME_AI_MAX_RULES] = {false};
    bool considered[HOME_AI_MAX_RULES] = {false};
    for (uint8_t index = 0U; index < HOME_AI_MAX_RULES; ++index) {
        eligible[index] = rule_eligible(store_index, index, context);
    }
    char claimed[HOME_AI_MAX_PENDING_DECISIONS][HOME_AI_RULE_DEVICE_ID_LEN] = {{0}};
    size_t claim_count = 0U;
    size_t emitted = 0U;
    for (size_t selection = 0U; selection < HOME_AI_MAX_RULES && emitted < capacity; ++selection) {
        int best = -1;
        for (uint8_t index = 0U; index < HOME_AI_MAX_RULES; ++index) {
            if (!eligible[index] || considered[index]) continue;
            if (best < 0 || effective_priority(&s_stores[store_index].rules[index]) >
                                effective_priority(&s_stores[store_index].rules[best])) {
                best = index;
            }
        }
        if (best < 0) break;
        considered[best] = true;
        const home_ai_rule_definition_t *rule = &s_stores[store_index].rules[best];
        const home_ai_room_state_t *room = room_for_rule(context, store_index, rule, NULL);
        bool any_action = false;
        for (uint8_t action_index = 0U; action_index < rule->action_count && emitted < capacity; ++action_index) {
            const home_ai_rule_action_definition_t *action = &rule->actions[action_index];
            home_ai_rule_decision_t *decision = &out[emitted];
            memset(decision, 0, sizeof(*decision));
            ++s_decision_sequence;
            if (s_decision_sequence == 0U) ++s_decision_sequence;
            snprintf(decision->decision_id,
                     sizeof(decision->decision_id),
                     "ha_%08lx_%02u_%02u",
                     (unsigned long)s_decision_sequence,
                     (unsigned int)best,
                     (unsigned int)action_index);
            copy_text(decision->rule_id,
                      sizeof(decision->rule_id),
                      store_string(store_index, rule->rule_id_offset));
            copy_text(decision->room_id,
                      sizeof(decision->room_id),
                      store_string(store_index, rule->room_id_offset));
            copy_text(decision->device_id,
                      sizeof(decision->device_id),
                      store_string(store_index, action->device_id_offset));
            copy_text(decision->prompt,
                      sizeof(decision->prompt),
                      store_string(store_index, action->prompt_offset));
            decision->device_type = action->device_type;
            decision->action = action->action;
            decision->priority = effective_priority(rule);
            decision->minimum_active_seconds = action->minimum_active_seconds > 0U ?
                                                   action->minimum_active_seconds :
                                                   rule->minimum_active_seconds;
            decision->safety_action = rule->type == HOME_AI_RULE_TYPE_SAFETY;
            decision->rule_slot = (uint8_t)best;
            home_ai_user_override_t match = {0};
            const home_ai_override_decision_t override_decision =
                home_ai_user_override_evaluate(decision->room_id,
                                                decision->device_id,
                                                home_ai_rule_action_name(decision->action),
                                                decision->safety_action,
                                                context->now_ms,
                                                &match);
            if (override_decision == HOME_AI_OVERRIDE_DECISION_SUPPRESS ||
                (override_decision == HOME_AI_OVERRIDE_DECISION_MUTE &&
                 decision->action == HOME_AI_RULE_ACTION_PLAY_PROMPT)) {
                decision->state = override_decision == HOME_AI_OVERRIDE_DECISION_MUTE ?
                                      HOME_AI_RULE_DECISION_SUPPRESSED_MUTE :
                                      HOME_AI_RULE_DECISION_SUPPRESSED_OVERRIDE;
                copy_text(decision->suppression_override_id,
                          sizeof(decision->suppression_override_id),
                          match.override_id);
            } else if (!decision->safety_action && room != NULL &&
                       room->presence_state == HOME_AI_ROOM_PRESENCE_UNKNOWN &&
                       decision->device_type == HOME_AI_RULE_DEVICE_LIGHT &&
                       decision->action == HOME_AI_RULE_ACTION_TURN_OFF) {
                decision->state = HOME_AI_RULE_DECISION_SUPPRESSED_UNKNOWN_PRESENCE;
            } else if (device_claimed(claimed, claim_count, decision->device_id)) {
                decision->state = HOME_AI_RULE_DECISION_SUPPRESSED_PRIORITY;
            } else {
                decision->state = HOME_AI_RULE_DECISION_EXECUTE;
                if (decision->device_id[0] != '\0' && claim_count < HOME_AI_MAX_PENDING_DECISIONS) {
                    copy_text(claimed[claim_count++], sizeof(claimed[0]), decision->device_id);
                }
            }
            any_action = true;
            ++emitted;
        }
        if (any_action) s_runtime[best].last_trigger_ms = context->now_ms;
    }
    engine_unlock();
    return emitted;
}

void home_ai_rule_engine_note_action_result(const home_ai_rule_decision_t *decision, bool success)
{
    (void)success;
    if (!s_initialized || decision == NULL || decision->rule_slot >= HOME_AI_MAX_RULES) return;
    /* Cooldown is committed at decision creation so failures cannot create a tight retry loop. */
}

bool home_ai_rule_engine_get_activation(home_ai_rule_activation_result_t *out_result)
{
    if (!s_initialized || out_result == NULL || !engine_lock()) return false;
    *out_result = s_activation;
    engine_unlock();
    return true;
}

const char *home_ai_rule_action_name(home_ai_rule_action_t action)
{
    switch (action) {
    case HOME_AI_RULE_ACTION_TURN_ON: return "turn_on";
    case HOME_AI_RULE_ACTION_TURN_OFF: return "turn_off";
    case HOME_AI_RULE_ACTION_PAUSE_AUTOMATION: return "pause_automation";
    case HOME_AI_RULE_ACTION_RESUME_AUTOMATION: return "resume_automation";
    case HOME_AI_RULE_ACTION_PLAY_PROMPT: return "play_prompt";
    default: return "unknown";
    }
}

const char *home_ai_rule_device_type_name(home_ai_rule_device_type_t type)
{
    switch (type) {
    case HOME_AI_RULE_DEVICE_LIGHT: return "light";
    case HOME_AI_RULE_DEVICE_AIR_CONDITIONER: return "air_conditioner";
    case HOME_AI_RULE_DEVICE_FAN: return "fan";
    default: return "unknown";
    }
}

const char *home_ai_rule_activation_state_name(home_ai_rule_activation_state_t state)
{
    switch (state) {
    case HOME_AI_RULE_ACTIVATION_ACTIVE: return "ACTIVE";
    case HOME_AI_RULE_ACTIVATION_ACTIVE_PARTIAL: return "ACTIVE_PARTIAL";
    case HOME_AI_RULE_ACTIVATION_REJECTED: return "REJECTED";
    case HOME_AI_RULE_ACTIVATION_EMPTY:
    default: return "EMPTY";
    }
}

const char *home_ai_rule_decision_state_name(home_ai_rule_decision_state_t state)
{
    switch (state) {
    case HOME_AI_RULE_DECISION_EXECUTE: return "execute";
    case HOME_AI_RULE_DECISION_SUPPRESSED_OVERRIDE: return "suppressed_override";
    case HOME_AI_RULE_DECISION_SUPPRESSED_PRIORITY: return "suppressed_priority";
    case HOME_AI_RULE_DECISION_SUPPRESSED_MUTE: return "suppressed_mute";
    case HOME_AI_RULE_DECISION_SUPPRESSED_UNKNOWN_PRESENCE:
        return "suppressed_unknown_presence";
    default: return "unknown";
    }
}

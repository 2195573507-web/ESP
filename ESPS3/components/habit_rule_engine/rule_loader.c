#include "rule_loader.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

static int rule_index(const char *id)
{
    static const char *const names[HABIT_RULE_COUNT] = {
        "person_enter_room", "person_leave_room", "long_stay", "empty_timeout",
        "night_activity", "long_occupancy",
    };
    if (id == NULL) return -1;
    for (size_t i = 0; i < HABIT_RULE_COUNT; ++i) {
        if (strcmp(id, names[i]) == 0) return (int)i;
    }
    return -1;
}

static int rule_type_index(const char *type)
{
    static const char *const types[HABIT_RULE_COUNT] = {
        "PERSON_ENTER_ROOM", "PERSON_LEAVE_ROOM", "PERSON_LONG_STAY",
        "ROOM_EMPTY_TIMEOUT", "NIGHT_ACTIVITY", "LONG_OCCUPANCY",
    };
    if (type == NULL) return -1;
    for (size_t i = 0; i < HABIT_RULE_COUNT; ++i) {
        if (strcmp(type, types[i]) == 0) return (int)i;
    }
    return -1;
}

static void set_rule_identity(habit_rule_engine_t *engine, size_t index,
                              const char *id, const char *scope_type, const char *scope_id)
{
    (void)snprintf(engine->rule_ids[index], sizeof(engine->rule_ids[index]), "%s", id);
    (void)snprintf(engine->rule_scope_type[index], sizeof(engine->rule_scope_type[index]), "%s",
                   scope_type);
    (void)snprintf(engine->rule_scope_id[index], sizeof(engine->rule_scope_id[index]), "%s",
                   scope_id != NULL ? scope_id : "");
}

void rule_loader_load_defaults(habit_rule_engine_t *engine)
{
    static const char *const ids[HABIT_RULE_COUNT] = {
        "person_enter_room", "person_leave_room", "person_long_stay",
        "room_empty_timeout", "night_activity", "long_occupancy",
    };
    if (engine == NULL) return;
    engine->version = 1U;
    for (size_t i = 0; i < HABIT_RULE_COUNT; ++i) {
        engine->rule_enabled[i] = true;
        set_rule_identity(engine, i, ids[i], "home", "");
    }
    engine->long_stay_ms = 30U * 60U * 1000U;
    engine->empty_timeout_ms = 10U * 60U * 1000U;
    engine->long_occupancy_ms = 120U * 60U * 1000U;
    engine->night_start_hour = 22U;
    engine->night_end_hour = 6U;
}

static void apply_parameters(habit_rule_engine_t *engine, int rule, const cJSON *parameters)
{
    if (!cJSON_IsObject(parameters)) return;
    const cJSON *minutes = cJSON_GetObjectItemCaseSensitive(parameters, "minutes");
    if (!cJSON_IsNumber(minutes)) minutes = cJSON_GetObjectItemCaseSensitive(parameters, "threshold_minutes");
    if (!cJSON_IsNumber(minutes)) minutes = cJSON_GetObjectItemCaseSensitive(parameters, "duration_minutes");
    const cJSON *start = cJSON_GetObjectItemCaseSensitive(parameters, "night_start");
    const cJSON *end = cJSON_GetObjectItemCaseSensitive(parameters, "night_end");
    if (cJSON_IsNumber(minutes) && minutes->valuedouble > 0.0 && minutes->valuedouble <= 1440.0) {
        const uint32_t value = (uint32_t)minutes->valuedouble * 60U * 1000U;
        if (rule == HABIT_RULE_PERSON_LONG_STAY) engine->long_stay_ms = value;
        if (rule == HABIT_RULE_ROOM_EMPTY_TIMEOUT) engine->empty_timeout_ms = value;
        if (rule == HABIT_RULE_LONG_OCCUPANCY) engine->long_occupancy_ms = value;
    }
    if (rule == HABIT_RULE_NIGHT_ACTIVITY && cJSON_IsNumber(start) && cJSON_IsNumber(end) &&
        start->valueint >= 0 && start->valueint < 24 && end->valueint >= 0 && end->valueint < 24) {
        engine->night_start_hour = (uint8_t)start->valueint;
        engine->night_end_hour = (uint8_t)end->valueint;
    }
}

static bool read_bundle_scope(const cJSON *scope, const char **out_type, const char **out_id)
{
    if (!cJSON_IsObject(scope) || out_type == NULL || out_id == NULL) return false;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(scope, "type");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(scope, "id");
    if (!cJSON_IsString(type) || type->valuestring == NULL) return false;
    if (strcmp(type->valuestring, "home") == 0) {
        *out_type = "home";
        *out_id = "";
        return true;
    }
    if ((strcmp(type->valuestring, "room") != 0 && strcmp(type->valuestring, "zone") != 0) ||
        !cJSON_IsString(id) || id->valuestring == NULL || id->valuestring[0] == '\0') {
        return false;
    }
    *out_type = type->valuestring;
    *out_id = id->valuestring;
    return true;
}

static esp_err_t load_bundle(habit_rule_engine_t *engine, const cJSON *root)
{
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    const cJSON *bundle_version = cJSON_GetObjectItemCaseSensitive(root, "bundle_version");
    const cJSON *checksum = cJSON_GetObjectItemCaseSensitive(root, "checksum");
    const cJSON *created_at = cJSON_GetObjectItemCaseSensitive(root, "created_at");
    const cJSON *rules = cJSON_GetObjectItemCaseSensitive(root, "rules");
    if (!cJSON_IsString(schema) || strcmp(schema->valuestring, "habit-rule-bundle-v1") != 0 ||
        !cJSON_IsString(bundle_version) || !cJSON_IsString(checksum) || !cJSON_IsString(created_at) ||
        !cJSON_IsArray(rules)) return ESP_ERR_INVALID_ARG;

    habit_rule_engine_t candidate = *engine;
    bool seen[HABIT_RULE_COUNT] = {0};
    const cJSON *rule = NULL;
    cJSON_ArrayForEach(rule, rules) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(rule, "id");
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(rule, "enabled");
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(rule, "type");
        const cJSON *parameters = cJSON_GetObjectItemCaseSensitive(rule, "parameters");
        const char *scope_type = NULL;
        const char *scope_id = NULL;
        const int index = cJSON_IsString(type) ? rule_type_index(type->valuestring) : -1;
        if (!cJSON_IsString(id) || id->valuestring == NULL || id->valuestring[0] == '\0' ||
            !cJSON_IsBool(enabled) || index < 0 || seen[index] ||
            !read_bundle_scope(cJSON_GetObjectItemCaseSensitive(rule, "scope"), &scope_type, &scope_id) ||
            !cJSON_IsObject(parameters)) return ESP_ERR_INVALID_ARG;
        seen[index] = true;
        candidate.rule_enabled[index] = cJSON_IsTrue(enabled);
        set_rule_identity(&candidate, (size_t)index, id->valuestring, scope_type, scope_id);
        apply_parameters(&candidate, index, parameters);
    }
    *engine = candidate;
    return ESP_OK;
}

esp_err_t rule_loader_load_json(habit_rule_engine_t *engine, const char *json)
{
    if (engine == NULL || json == NULL) return ESP_ERR_INVALID_ARG;
    cJSON *root = cJSON_Parse(json);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (cJSON_IsString(schema)) {
        const esp_err_t ret = load_bundle(engine, root);
        cJSON_Delete(root);
        return ret;
    }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *rules = cJSON_GetObjectItemCaseSensitive(root, "rules");
    if (!cJSON_IsNumber(version) || version->valueint <= 0 || !cJSON_IsArray(rules)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    habit_rule_engine_t candidate = *engine;
    candidate.version = (uint32_t)version->valueint;
    const cJSON *rule = NULL;
    cJSON_ArrayForEach(rule, rules) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(rule, "id");
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(rule, "enabled");
        const int index = cJSON_IsString(id) ? rule_index(id->valuestring) : -1;
        if (index < 0 || !cJSON_IsBool(enabled)) continue;
        candidate.rule_enabled[index] = cJSON_IsTrue(enabled);
        apply_parameters(&candidate, index, cJSON_GetObjectItemCaseSensitive(rule, "parameters"));
    }
    *engine = candidate;
    return ESP_OK;
}

esp_err_t rule_loader_load_remote_rule(habit_rule_engine_t *engine)
{
    (void)engine;
    return ESP_ERR_NOT_SUPPORTED;
}

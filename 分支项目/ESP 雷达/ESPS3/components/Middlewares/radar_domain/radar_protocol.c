#include "radar_protocol.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#include "cJSON.h"

/*
 * 远端雷达 JSON 使用严格的封闭 schema：键必须完整且唯一，数值必须是范围内
 * 的整数。拒绝未知字段可防止协议漂移或部分解析造成错误的空间状态。
 */

static bool object_has_exact_keys(cJSON *object,
                                  const char *const *keys,
                                  size_t key_count)
{
    if (!cJSON_IsObject(object) || keys == NULL || key_count == 0U || key_count > 32U) {
        return false;
    }

    uint32_t seen = 0U;
    for (cJSON *item = object->child; item != NULL; item = item->next) {
        if (item->string == NULL) {
            return false;
        }
        size_t index = 0U;
        while (index < key_count && strcmp(item->string, keys[index]) != 0) {
            ++index;
        }
        if (index == key_count || (seen & (1UL << index)) != 0U) {
            return false;
        }
        seen |= 1UL << index;
    }

    const uint32_t expected = key_count == 32U ? UINT32_MAX : ((1UL << key_count) - 1UL);
    return seen == expected;
}

static bool json_integer_in_range(cJSON *item, double min, double max, double *out)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < min || item->valuedouble > max ||
        floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    if (out != NULL) {
        *out = item->valuedouble;
    }
    return true;
}

static bool parse_state(cJSON *item, radar_presence_state_t *out)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL || out == NULL) {
        return false;
    }
    if (strcmp(item->valuestring, "unknown") == 0) {
        *out = RADAR_STATE_UNKNOWN;
    } else if (strcmp(item->valuestring, "vacant_inferred") == 0) {
        *out = RADAR_STATE_VACANT_INFERRED;
    } else if (strcmp(item->valuestring, "hold") == 0) {
        *out = RADAR_STATE_HOLD;
    } else if (strcmp(item->valuestring, "motion") == 0) {
        *out = RADAR_STATE_MOTION;
    } else {
        return false;
    }
    return true;
}

static bool parse_target(cJSON *item, radar_target_t *out)
{
    static const char *const keys[] = {
        "x_mm",
        "y_mm",
        "speed_cm_s",
        "resolution_mm",
    };
    if (out == NULL || !object_has_exact_keys(item, keys, sizeof(keys) / sizeof(keys[0]))) {
        return false;
    }

    double x = 0.0;
    double y = 0.0;
    double speed = 0.0;
    double resolution = 0.0;
    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "x_mm"),
                               INT16_MIN,
                               INT16_MAX,
                               &x) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "y_mm"),
                               INT16_MIN,
                               INT16_MAX,
                               &y) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "speed_cm_s"),
                               INT16_MIN,
                               INT16_MAX,
                               &speed) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "resolution_mm"),
                               0.0,
                               UINT16_MAX,
                               &resolution)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->x_mm = (int16_t)x;
    out->y_mm = (int16_t)y;
    out->speed_cm_s = (int16_t)speed;
    out->resolution_mm = (uint16_t)resolution;
    return true;
}

radar_protocol_result_t radar_protocol_parse_json(const char *json,
                                                  size_t json_len,
                                                  radar_protocol_payload_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (json == NULL || out == NULL || json_len == 0U) {
        return RADAR_PROTOCOL_INVALID_ARGUMENT;
    }
    if (json_len > RADAR_PROTOCOL_MAX_BODY_BYTES) {
        return RADAR_PROTOCOL_TOO_LARGE;
    }

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, json_len, &parse_end, false);
    if (root == NULL) {
        return RADAR_PROTOCOL_INVALID_JSON;
    }
    while (parse_end != NULL && parse_end < json + json_len &&
           isspace((unsigned char)*parse_end)) {
        ++parse_end;
    }
    if (parse_end == NULL || parse_end != json + json_len) {
        cJSON_Delete(root);
        return RADAR_PROTOCOL_INVALID_JSON;
    }

    static const char *const keys[] = {
        "schema_version",
        "local_id",
        "sequence",
        "uptime_ms",
        "state",
        "target_count",
        "uart_online",
        "frame_fresh",
        "last_motion_age_ms",
        "targets",
    };
    if (!object_has_exact_keys(root, keys, sizeof(keys) / sizeof(keys[0]))) {
        cJSON_Delete(root);
        return RADAR_PROTOCOL_INVALID_SCHEMA;
    }

    double number = 0.0;
    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(root, "schema_version"),
                               RADAR_PROTOCOL_SCHEMA_VERSION,
                               RADAR_PROTOCOL_SCHEMA_VERSION,
                               &number)) {
        cJSON_Delete(root);
        return RADAR_PROTOCOL_INVALID_SCHEMA;
    }
    out->schema_version = (uint8_t)number;

    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(root, "local_id"),
                               1.0,
                               2.0,
                               &number)) {
        cJSON_Delete(root);
        return RADAR_PROTOCOL_INVALID_LOCAL_ID;
    }
    out->local_id = (uint8_t)number;

    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(root, "sequence"),
                               1.0,
                               UINT32_MAX,
                               &number)) {
        cJSON_Delete(root);
        return RADAR_PROTOCOL_INVALID_SEQUENCE;
    }
    out->sequence = (uint32_t)number;

    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(root, "uptime_ms"),
                               0.0,
                               9007199254740991.0,
                               &number)) {
        cJSON_Delete(root);
        return RADAR_PROTOCOL_INVALID_SCHEMA;
    }
    out->uptime_ms = (uint64_t)number;

    if (!parse_state(cJSON_GetObjectItemCaseSensitive(root, "state"), &out->state)) {
        cJSON_Delete(root);
        return RADAR_PROTOCOL_INVALID_STATE;
    }

    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(root, "target_count"),
                               0.0,
                               LD2450_MAX_TARGETS,
                               &number)) {
        cJSON_Delete(root);
        return RADAR_PROTOCOL_INVALID_TARGETS;
    }
    out->target_count = (uint8_t)number;

    cJSON *uart_online = cJSON_GetObjectItemCaseSensitive(root, "uart_online");
    cJSON *frame_fresh = cJSON_GetObjectItemCaseSensitive(root, "frame_fresh");
    if (!cJSON_IsBool(uart_online) || !cJSON_IsBool(frame_fresh)) {
        cJSON_Delete(root);
        return RADAR_PROTOCOL_INVALID_SCHEMA;
    }
    out->uart_online = cJSON_IsTrue(uart_online);
    out->frame_fresh = cJSON_IsTrue(frame_fresh);

    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(root, "last_motion_age_ms"),
                               0.0,
                               UINT32_MAX,
                               &number)) {
        cJSON_Delete(root);
        return RADAR_PROTOCOL_INVALID_SCHEMA;
    }
    out->last_motion_age_ms = (uint32_t)number;

    cJSON *targets = cJSON_GetObjectItemCaseSensitive(root, "targets");
    if (!cJSON_IsArray(targets) || cJSON_GetArraySize(targets) != out->target_count) {
        cJSON_Delete(root);
        return RADAR_PROTOCOL_INVALID_TARGETS;
    }
    for (uint8_t i = 0U; i < out->target_count; ++i) {
        if (!parse_target(cJSON_GetArrayItem(targets, i), &out->targets[i])) {
            cJSON_Delete(root);
            return RADAR_PROTOCOL_INVALID_TARGETS;
        }
    }

    cJSON_Delete(root);
    return RADAR_PROTOCOL_OK;
}

static bool target_equal(const radar_target_t *a, const radar_target_t *b)
{
    return a->valid == b->valid &&
           a->x_mm == b->x_mm &&
           a->y_mm == b->y_mm &&
           a->speed_cm_s == b->speed_cm_s &&
           a->resolution_mm == b->resolution_mm;
}

bool radar_protocol_payload_equal(const radar_protocol_payload_t *a,
                                  const radar_protocol_payload_t *b)
{
    if (a == NULL || b == NULL ||
        a->schema_version != b->schema_version ||
        a->local_id != b->local_id ||
        a->sequence != b->sequence ||
        a->uptime_ms != b->uptime_ms ||
        a->state != b->state ||
        a->target_count != b->target_count ||
        a->uart_online != b->uart_online ||
        a->frame_fresh != b->frame_fresh ||
        a->last_motion_age_ms != b->last_motion_age_ms) {
        return false;
    }
    for (size_t i = 0U; i < LD2450_MAX_TARGETS; ++i) {
        if (!target_equal(&a->targets[i], &b->targets[i])) {
            return false;
        }
    }
    return true;
}

const char *radar_protocol_result_name(radar_protocol_result_t result)
{
    switch (result) {
    case RADAR_PROTOCOL_OK:
        return "ok";
    case RADAR_PROTOCOL_INVALID_ARGUMENT:
        return "invalid_argument";
    case RADAR_PROTOCOL_TOO_LARGE:
        return "too_large";
    case RADAR_PROTOCOL_INVALID_JSON:
        return "invalid_json";
    case RADAR_PROTOCOL_INVALID_SCHEMA:
        return "invalid_schema";
    case RADAR_PROTOCOL_INVALID_LOCAL_ID:
        return "invalid_local_id";
    case RADAR_PROTOCOL_INVALID_SEQUENCE:
        return "invalid_sequence";
    case RADAR_PROTOCOL_INVALID_STATE:
        return "invalid_state";
    case RADAR_PROTOCOL_INVALID_TARGETS:
        return "invalid_targets";
    default:
        return "unknown";
    }
}

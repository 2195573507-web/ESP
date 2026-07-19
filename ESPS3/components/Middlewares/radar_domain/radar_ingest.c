#include "radar_ingest.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "cJSON.h"
#include "esp_timer.h"
#include "ld2450_types.h"
#include "radar_gateway_ingest.h"
#include "radar_registry.h"

static uint32_t s_sequences[RADAR_SOURCE_COUNT];

static bool object_has_exact_keys(cJSON *object,
                                  const char *const *keys,
                                  size_t key_count)
{
    if (!cJSON_IsObject(object) || keys == NULL || key_count == 0U || key_count > 32U) {
        return false;
    }
    uint32_t seen = 0U;
    for (cJSON *item = object->child; item != NULL; item = item->next) {
        if (item->string == NULL) return false;
        size_t index = 0U;
        while (index < key_count && strcmp(item->string, keys[index]) != 0) ++index;
        if (index == key_count || (seen & (1UL << index)) != 0U) return false;
        seen |= 1UL << index;
    }
    return seen == (key_count == 32U ? UINT32_MAX : ((1UL << key_count) - 1UL));
}

static bool json_integer_in_range(cJSON *item, double min, double max, double *out)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < min || item->valuedouble > max ||
        floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    if (out != NULL) *out = item->valuedouble;
    return true;
}

static bool parse_target(cJSON *item, radar_gateway_target_t *out)
{
    static const char *const keys[] = {"x", "y", "speed", "distance", "motion"};
    if (out == NULL || !object_has_exact_keys(item, keys, 5U)) return false;
    double x = 0.0, y = 0.0, speed = 0.0, distance = 0.0;
    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "x"),
                               INT16_MIN, INT16_MAX, &x) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "y"),
                               INT16_MIN, INT16_MAX, &y) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "speed"),
                               INT16_MIN, INT16_MAX, &speed) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "distance"),
                               0.0, UINT32_MAX, &distance) ||
        !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(item, "motion"))) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->slot = 0U;
    out->x_mm = (int16_t)x;
    out->y_mm = (int16_t)y;
    out->speed_cm_s = (int16_t)speed;
    out->distance_mm = (uint32_t)distance;
    return true;
}

static radar_ingest_result_t parse_json(const char *json,
                                        size_t json_len,
                                        radar_gateway_sample_t *out,
                                        uint64_t *out_timestamp_ms)
{
    if (out != NULL) memset(out, 0, sizeof(*out));
    if (out_timestamp_ms != NULL) *out_timestamp_ms = 0U;
    if (json == NULL || out == NULL || out_timestamp_ms == NULL || json_len == 0U) {
        return RADAR_INGEST_INVALID_ARGUMENT;
    }
    if (json_len > RADAR_INGEST_MAX_BODY_BYTES) return RADAR_INGEST_TOO_LARGE;

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, json_len, &parse_end, false);
    if (root == NULL) return RADAR_INGEST_INVALID_JSON;
    while (parse_end != NULL && parse_end < json + json_len &&
           isspace((unsigned char)*parse_end)) ++parse_end;
    if (parse_end == NULL || parse_end != json + json_len) {
        cJSON_Delete(root);
        return RADAR_INGEST_INVALID_JSON;
    }

    static const char *const root_keys[] = {
        "device_id", "local_id", "timestamp", "target_count", "targets"
    };
    if (!object_has_exact_keys(root, root_keys, 5U)) {
        cJSON_Delete(root);
        return RADAR_INGEST_INVALID_SCHEMA;
    }

    double number = 0.0;
    cJSON *local_id_item = cJSON_GetObjectItemCaseSensitive(root, "local_id");
    if (!json_integer_in_range(local_id_item, 1.0, 2.0, &number)) {
        cJSON_Delete(root);
        return RADAR_INGEST_INVALID_LOCAL_ID;
    }
    out->local_id = (uint8_t)number;
    cJSON *device_id_item = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    if (!cJSON_IsString(device_id_item) || device_id_item->valuestring == NULL) {
        cJSON_Delete(root);
        return RADAR_INGEST_INVALID_SCHEMA;
    }
    const radar_source_id_t source = radar_registry_source_for_local_id(out->local_id);
    const char *expected_device_id = radar_registry_device_id(source);
    if (source == RADAR_SOURCE_COUNT || expected_device_id == NULL ||
        strcmp(expected_device_id, device_id_item->valuestring) != 0) {
        cJSON_Delete(root);
        return RADAR_INGEST_IDENTITY_MISMATCH;
    }
    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(root, "timestamp"),
                               0.0, 9007199254740991.0, &number)) {
        cJSON_Delete(root);
        return RADAR_INGEST_INVALID_SCHEMA;
    }
    *out_timestamp_ms = (uint64_t)number;
    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(root, "target_count"),
                               0.0, LD2450_MAX_TARGETS, &number)) {
        cJSON_Delete(root);
        return RADAR_INGEST_INVALID_TARGETS;
    }
    out->target_count = (uint8_t)number;
    cJSON *targets = cJSON_GetObjectItemCaseSensitive(root, "targets");
    if (!cJSON_IsArray(targets) || cJSON_GetArraySize(targets) != out->target_count) {
        cJSON_Delete(root);
        return RADAR_INGEST_INVALID_TARGETS;
    }
    out->sample_valid = true;
    out->link_state = 5U;
    for (uint8_t i = 0U; i < out->target_count; ++i) {
        if (!parse_target(cJSON_GetArrayItem(targets, i), &out->targets[i])) {
            cJSON_Delete(root);
            return RADAR_INGEST_INVALID_TARGETS;
        }
        out->targets[i].slot = i;
    }
    cJSON_Delete(root);
    return RADAR_INGEST_ACCEPTED;
}

radar_ingest_result_t radar_ingest_process_json(const char *json,
                                                size_t json_len,
                                                uint64_t received_at_ms)
{
    radar_gateway_sample_t sample = {0};
    uint64_t timestamp_ms = 0U;
    radar_ingest_result_t parsed = parse_json(json, json_len, &sample, &timestamp_ms);
    if (parsed != RADAR_INGEST_ACCEPTED) return parsed;
    if (received_at_ms == 0U) {
        const int64_t now_us = esp_timer_get_time();
        received_at_ms = now_us > 0 ? (uint64_t)(now_us / 1000) : 1U;
    }
    const radar_source_id_t source = radar_registry_source_for_local_id(sample.local_id);
    if (source == RADAR_SOURCE_COUNT) return RADAR_INGEST_INVALID_LOCAL_ID;
    uint32_t sequence = ++s_sequences[source];
    if (sequence == 0U) sequence = ++s_sequences[source];
    sample.request_sequence = sequence;
    sample.request_uptime_ms = timestamp_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)timestamp_ms;
    sample.frame_seq = sequence;
    sample.frame_uptime_ms = sample.request_uptime_ms;
    radar_gateway_output_t output = {0};
    const radar_gateway_ingest_result_t result =
        radar_gateway_ingest_admit(&sample, 1U, received_at_ms, &output);
    switch (result) {
    case RADAR_GATEWAY_INGEST_ACCEPTED:
    case RADAR_GATEWAY_INGEST_DUPLICATE:
        return RADAR_INGEST_ACCEPTED;
    case RADAR_GATEWAY_INGEST_IDENTITY_MISMATCH:
        return RADAR_INGEST_IDENTITY_MISMATCH;
    case RADAR_GATEWAY_INGEST_UNAVAILABLE:
        return RADAR_INGEST_UNAVAILABLE;
    default:
        return RADAR_INGEST_INVALID_SCHEMA;
    }
}

const char *radar_ingest_result_name(radar_ingest_result_t result)
{
    switch (result) {
    case RADAR_INGEST_ACCEPTED: return "accepted";
    case RADAR_INGEST_INVALID_ARGUMENT: return "invalid_argument";
    case RADAR_INGEST_TOO_LARGE: return "too_large";
    case RADAR_INGEST_INVALID_JSON: return "invalid_json";
    case RADAR_INGEST_INVALID_SCHEMA: return "invalid_schema";
    case RADAR_INGEST_INVALID_LOCAL_ID: return "invalid_local_id";
    case RADAR_INGEST_IDENTITY_MISMATCH: return "identity_mismatch";
    case RADAR_INGEST_INVALID_TARGETS: return "invalid_targets";
    case RADAR_INGEST_UNAVAILABLE: return "unavailable";
    default: return "unknown";
    }
}

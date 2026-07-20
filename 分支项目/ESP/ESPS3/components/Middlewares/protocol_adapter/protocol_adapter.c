/**
 * @file protocol_adapter.c
 * @brief S3 网关本地轻量协议到完整 Server 协议的适配器。
 *
 * 本文件属于 ESPS3 网关，负责把 C5<->S3 的短字段 id/t/u/v/cid/c/a/ok/e/cmds
 * 映射为完整 device_id、message_type、payload 和 Server ingest JSON。它不发起 HTTP、
 * 不维护子设备在线状态、不执行命令；这些分别由 server_client、child_registry 和
 * command_router 负责。
 */

#include "protocol_adapter.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp111_protocol_common.h"
#include "esp_timer.h"
#include "gateway_config.h"

typedef struct {
    uint8_t local_id;
    const char *device_id;
    const char *alias;
    const char *room_name;
} protocol_adapter_local_child_t;

typedef struct {
    bool valid;
    double frame_energy;
    double variance;
    double cv;
    double rssi;
    double quality;
} protocol_adapter_csi_metrics_t;

static const protocol_adapter_local_child_t s_local_children[] = {
    {ESP111_PROTOCOL_LOCAL_DEVICE_ID_C51, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51, "SensaiShuttle", "living_room"},
    {ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52, "SensaiShuttle02", "bedroom"},
};

static const char *const s_csi_internal_links[] = {
    "S3_TO_C51",
    "S3_TO_C52",
};

/* CSI fusion may emit the same link map every tick; retain only the last mapping for diagnostics. */
static char s_last_csi_server_link_map[CSI_FUSION_LINK_COUNT][48];
static uint8_t s_last_csi_server_link_count;

static bool copy_json_string(cJSON *root, const char *key, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(value) || value->valuestring == NULL) {
        return false;
    }

    strlcpy(out, value->valuestring, out_size);
    return true;
}

static const char *read_json_string(cJSON *root, const char *key)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(value) && value->valuestring != NULL ? value->valuestring : "";
}

static const char *read_json_string_first(cJSON *root,
                                          const char *primary,
                                          const char *fallback)
{
    const char *value = read_json_string(root, primary);
    return value[0] != '\0' ? value : read_json_string(root, fallback);
}

static bool local_payload_type_matches(const char *local_payload_type, const char *message_type)
{
    if (local_payload_type == NULL || local_payload_type[0] == '\0') {
        return true;
    }
    if (message_type == NULL || message_type[0] == '\0') {
        return false;
    }
    if (strcmp(local_payload_type, message_type) == 0) {
        return true;
    }
    return strcmp(local_payload_type, ESP111_PROTOCOL_MSG_CSI_RESULT) == 0 &&
           strcmp(message_type, ESP111_PROTOCOL_MSG_CSI_MOTION) == 0;
}

static int64_t get_json_i64(cJSON *root, const char *key, int64_t fallback)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(value)) {
        return (int64_t)value->valuedouble;
    }
    return fallback;
}

static int64_t protocol_adapter_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool schema_version_is_csi_v2(cJSON *root)
{
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(root,
                                                     ESP111_PROTOCOL_JSON_SCHEMA_VERSION);
    if (cJSON_IsString(schema) && schema->valuestring != NULL) {
        return strcmp(schema->valuestring, ESP111_PROTOCOL_CSI_EVENT_SCHEMA_VERSION_STRING) == 0 ||
               strcmp(schema->valuestring, "2") == 0;
    }
    return cJSON_IsNumber(schema) && schema->valueint == CSI_FUSION_SCHEMA_VERSION;
}

static bool read_finite_number(cJSON *root, const char *key, double *out)
{
    if (root == NULL || key == NULL || out == NULL) {
        return false;
    }
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble)) {
        return false;
    }
    *out = value->valuedouble;
    return true;
}

static bool protocol_adapter_is_active_csi_link(const char *link_id)
{
    return link_id != NULL &&
           (strcmp(link_id, "S3_TO_C51") == 0 ||
            strcmp(link_id, "S3_TO_C52") == 0);
}

static bool read_finite_array_number(cJSON *array, int index, double *out)
{
    if (!cJSON_IsArray(array) || index < 0 || out == NULL) {
        return false;
    }
    cJSON *value = cJSON_GetArrayItem(array, index);
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble)) {
        return false;
    }
    *out = value->valuedouble;
    return true;
}

static const char *canonical_csi_state_hint(const char *state_hint)
{
    if (state_hint == NULL || state_hint[0] == '\0') {
        return "";
    }
    if (strcasecmp(state_hint, "MOTION") == 0 ||
        strcasecmp(state_hint, "motion") == 0 ||
        strcasecmp(state_hint, "occupied") == 0) {
        return "MOTION";
    }
    if (strcasecmp(state_hint, "HOLD") == 0 ||
        strcasecmp(state_hint, "hold") == 0) {
        return "HOLD";
    }
    if (strcasecmp(state_hint, "IDLE") == 0 ||
        strcasecmp(state_hint, "idle") == 0 ||
        strcasecmp(state_hint, "vacant") == 0 ||
        strcasecmp(state_hint, "no_motion") == 0 ||
        strcasecmp(state_hint, "no-motion") == 0) {
        return "IDLE";
    }
    return "";
}

static bool read_csi_timestamp_ms(cJSON *root, double *out)
{
    if (read_finite_number(root, "timestamp_ms", out) ||
        read_finite_number(root, "timestamp", out) ||
        read_finite_number(root, ESP111_PROTOCOL_DEVICE_STREAM_JSON_TIMESTAMP, out)) {
        return *out > 0.0;
    }
    return false;
}

static const char *read_csi_link_id(cJSON *root)
{
    const char *link_id = read_json_string(root, ESP111_PROTOCOL_DEVICE_STREAM_JSON_LINK_ID);
    return link_id[0] != '\0' ? link_id : read_json_string(root, "link_id");
}

static bool read_csi_edge_fields(cJSON *root,
                                 double *motion_score,
                                 double *confidence,
                                 const char **state)
{
    if (root == NULL || motion_score == NULL || confidence == NULL || state == NULL) {
        return false;
    }
    const char *state_text = read_json_string_first(root, "state", "state_hint");
    const char *canonical_state = canonical_csi_state_hint(state_text);
    if (canonical_state[0] == '\0' ||
        !read_finite_number(root, "motion_score", motion_score) ||
        !read_finite_number(root, "confidence", confidence) ||
        *motion_score < 0.0 || *motion_score > 1.0 ||
        *confidence < 0.0 || *confidence > 1.0) {
        return false;
    }
    *state = canonical_state;
    return true;
}

static double read_csi_number_or(cJSON *root,
                                 cJSON *metrics,
                                 const char *key,
                                 double fallback)
{
    double value = fallback;
    if (read_finite_number(root, key, &value)) {
        return value;
    }
    if (cJSON_IsObject(metrics) && read_finite_number(metrics, key, &value)) {
        return value;
    }
    return fallback;
}

static bool read_csi_feature_metrics(cJSON *root,
                                     cJSON *metrics,
                                     protocol_adapter_csi_metrics_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    double frame_energy = 0.0;
    double variance = 0.0;
    double cv = 0.0;
    double rssi = 0.0;
    double quality = 0.0;
    bool has_energy = read_finite_number(metrics, "frame_energy", &frame_energy) ||
                      read_finite_number(root, "frame_energy", &frame_energy) ||
                      read_finite_number(root, "energy", &frame_energy);
    bool has_variance = read_finite_number(metrics, "variance", &variance) ||
                        read_finite_number(root, "variance", &variance);
    bool has_cv = read_finite_number(metrics, "cv", &cv) ||
                  read_finite_number(root, "cv", &cv);
    bool has_rssi = read_finite_number(metrics, "rssi", &rssi) ||
                    read_finite_number(root, "rssi", &rssi);
    bool has_quality = read_finite_number(metrics, "quality", &quality) ||
                       read_finite_number(root, "quality", &quality);

    if (!has_energy || !has_variance || !has_cv || !has_rssi || !has_quality ||
        frame_energy < 0.0 || variance < 0.0 || cv < 0.0 ||
        quality < 0.0 || quality > 1.0) {
        return false;
    }

    out->valid = true;
    out->frame_energy = frame_energy;
    out->variance = variance;
    out->cv = cv;
    out->rssi = rssi;
    out->quality = quality;
    return true;
}

static bool csi_signal_key_forbidden(const char *key)
{
    return key != NULL &&
           (strcmp(key, "raw_csi") == 0 ||
            strcmp(key, "raw_iq") == 0 ||
            strcmp(key, "iq") == 0 ||
            strcmp(key, "iq_samples") == 0 ||
            strcmp(key, "i_samples") == 0 ||
            strcmp(key, "q_samples") == 0 ||
            strcmp(key, "phase") == 0 ||
            strcmp(key, "phase_data") == 0 ||
            strcmp(key, "subcarrier_data") == 0 ||
            strcmp(key, "selected_subcarriers") == 0 ||
            strcmp(key, "subcarrier_matrix") == 0 ||
            strcmp(key, "subcarriers") == 0 ||
            strcmp(key, "amplitude") == 0 ||
            strcmp(key, "amplitudes") == 0);
}

static bool csi_has_forbidden_signal_fields(cJSON *root)
{
    if (root == NULL) {
        return false;
    }

    for (cJSON *item = root->child; item != NULL; item = item->next) {
        if (csi_signal_key_forbidden(item->string)) {
            return true;
        }
        if ((cJSON_IsObject(item) || cJSON_IsArray(item)) &&
            csi_has_forbidden_signal_fields(item)) {
            return true;
        }
    }
    return false;
}

static esp_err_t add_csi_metrics_payload(cJSON *payload,
                                         const protocol_adapter_csi_metrics_t *metrics)
{
    if (payload == NULL || metrics == NULL || !metrics->valid) {
        return ESP_OK;
    }

    cJSON *payload_metrics = cJSON_AddObjectToObject(payload, "metrics");
    if (payload_metrics == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(payload_metrics, "frame_energy", metrics->frame_energy);
    cJSON_AddNumberToObject(payload_metrics, "variance", metrics->variance);
    cJSON_AddNumberToObject(payload_metrics, "cv", metrics->cv);
    cJSON_AddNumberToObject(payload_metrics, "rssi", metrics->rssi);
    cJSON_AddNumberToObject(payload_metrics, "quality", metrics->quality);
    return ESP_OK;
}

const char *protocol_adapter_local_device_id_to_device_id(uint8_t local_id)
{
    for (size_t i = 0; i < sizeof(s_local_children) / sizeof(s_local_children[0]); i++) {
        if (s_local_children[i].local_id == local_id) {
            return s_local_children[i].device_id;
        }
    }
    return NULL;
}

const char *protocol_adapter_local_device_id_to_alias(uint8_t local_id)
{
    for (size_t i = 0; i < sizeof(s_local_children) / sizeof(s_local_children[0]); i++) {
        if (s_local_children[i].local_id == local_id) {
            return s_local_children[i].alias;
        }
    }
    return "";
}

static const char *protocol_adapter_local_device_id_to_room(uint8_t local_id)
{
    for (size_t i = 0; i < sizeof(s_local_children) / sizeof(s_local_children[0]); i++) {
        if (s_local_children[i].local_id == local_id) {
            return s_local_children[i].room_name;
        }
    }
    return "unassigned";
}

uint8_t protocol_adapter_device_id_to_local_id(const char *device_id)
{
    if (device_id == NULL) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(s_local_children) / sizeof(s_local_children[0]); i++) {
        if (strcmp(s_local_children[i].device_id, device_id) == 0 ||
            strcmp(s_local_children[i].alias, device_id) == 0) {
            return s_local_children[i].local_id;
        }
    }
    if (strcmp(device_id, "C51") == 0) {
        return ESP111_PROTOCOL_LOCAL_DEVICE_ID_C51;
    }
    if (strcmp(device_id, "C52") == 0) {
        return ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52;
    }
    if (strcmp(device_id, "1") == 0) {
        return ESP111_PROTOCOL_LOCAL_DEVICE_ID_C51;
    }
    if (strcmp(device_id, "2") == 0) {
        return ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52;
    }
    return 0;
}

static cJSON *protocol_adapter_add_payload(protocol_adapter_envelope_t *out)
{
    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL ||
        !cJSON_AddItemToObject(out->root, ESP111_PROTOCOL_JSON_PAYLOAD, payload)) {
        cJSON_Delete(payload);
        return NULL;
    }
    out->payload = payload;
    return payload;
}

static esp_err_t protocol_adapter_add_capabilities(protocol_adapter_envelope_t *out)
{
    cJSON *capabilities = cJSON_Parse(ESP111_PROTOCOL_TERMINAL_CAPABILITIES_JSON);
    if (capabilities == NULL ||
        !cJSON_AddItemToObject(out->root, ESP111_PROTOCOL_JSON_CAPABILITIES, capabilities)) {
        cJSON_Delete(capabilities);
        return ESP_ERR_NO_MEM;
    }
    out->capabilities = capabilities;
    return ESP_OK;
}

static const char *protocol_adapter_air_quality_level(int score)
{
    if (score >= 85) {
        return "excellent";
    }
    if (score >= 70) {
        return "good";
    }
    if (score >= 50) {
        return "moderate";
    }
    if (score >= 30) {
        return "poor";
    }
    if (score >= 0) {
        return "bad";
    }
    return "unknown";
}

static bool read_bme_number(cJSON *root,
                            cJSON *values,
                            const char *key,
                            int value_index,
                            double *out)
{
    if (out == NULL) {
        return false;
    }
    if (read_finite_number(root, key, out)) {
        return true;
    }
    return value_index >= 0 && read_finite_array_number(values, value_index, out);
}

static double read_bme_number_or(cJSON *root,
                                 cJSON *values,
                                 const char *key,
                                 int value_index,
                                 double fallback)
{
    double value = fallback;
    (void)read_bme_number(root, values, key, value_index, &value);
    return value;
}

static const char *read_bme_algorithm_version(cJSON *root, bool compact_v2)
{
    const char *algorithm = read_json_string(root, "algorithm_version");
    if (algorithm[0] != '\0') {
        return algorithm;
    }
    algorithm = read_json_string(root, "air_quality_algo_version");
    if (algorithm[0] != '\0') {
        return algorithm;
    }
    return compact_v2 ? "c5_compact_v2" : "c5_compact_v1";
}

static esp_err_t protocol_adapter_copy_bme_item(cJSON *root,
                                                cJSON *payload,
                                                const char *key)
{
    cJSON *source = cJSON_GetObjectItemCaseSensitive(root, key);
    if (source == NULL) {
        return ESP_OK;
    }

    cJSON *copy = cJSON_Duplicate(source, true);
    if (copy == NULL || !cJSON_AddItemToObject(payload, key, copy)) {
        cJSON_Delete(copy);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t protocol_adapter_copy_bme_air_quality(cJSON *root, cJSON *payload)
{
    cJSON *source = cJSON_GetObjectItemCaseSensitive(root, "air_quality");
    if (!cJSON_IsObject(source)) {
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *air_quality = cJSON_Duplicate(source, true);
    if (air_quality == NULL || !cJSON_AddItemToObject(payload, "air_quality", air_quality)) {
        cJSON_Delete(air_quality);
        return ESP_ERR_NO_MEM;
    }
    cJSON *air_quality_json = cJSON_Duplicate(source, true);
    if (air_quality_json == NULL ||
        !cJSON_AddItemToObject(payload, "air_quality_json", air_quality_json)) {
        cJSON_Delete(air_quality_json);
        cJSON_DeleteItemFromObjectCaseSensitive(payload, "air_quality");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t protocol_adapter_fill_bme_payload(cJSON *root,
                                                   cJSON *values,
                                                   cJSON *payload,
                                                   bool compact_v2)
{
    int value_count = cJSON_IsArray(values) ? cJSON_GetArraySize(values) : 0;
    bool named_payload = cJSON_GetObjectItemCaseSensitive(root, "temperature_c") != NULL ||
                         cJSON_GetObjectItemCaseSensitive(root, "humidity_percent") != NULL ||
                         cJSON_GetObjectItemCaseSensitive(root, "air_quality_score") != NULL;
    if (payload == NULL || (!named_payload && value_count != 3 && value_count != 5 &&
                            value_count != 11 && value_count != 12)) {
        return ESP_ERR_INVALID_ARG;
    }

    double temperature = 0.0;
    double humidity = 0.0;
    double score_number = 0.0;
    int score_index = value_count == 3 ? 2 : 4;
    if (!read_bme_number(root, values, "temperature_c", 0, &temperature) ||
        !read_bme_number(root, values, "humidity_percent", 1, &humidity) ||
        !read_bme_number(root, values, "air_quality_score", score_index, &score_number)) {
        return ESP_ERR_INVALID_ARG;
    }

    double pressure = read_bme_number_or(root, values, "pressure_hpa", value_count >= 5 ? 2 : -1, 0.0);
    double gas_resistance =
        read_bme_number_or(root, values, "gas_resistance_ohm", value_count >= 5 ? 3 : -1, 0.0);
    double gas_baseline =
        read_bme_number_or(root, values, "gas_baseline_ohm", value_count >= 11 ? 5 : -1, 0.0);
    double gas_ratio = read_bme_number_or(root, values, "gas_ratio", value_count >= 11 ? 6 : -1, 0.0);
    double gas_score_number =
        read_bme_number_or(root, values, "gas_score", value_count >= 11 ? 7 : -1, score_number);
    double humidity_score_number =
        read_bme_number_or(root, values, "humidity_score", value_count >= 11 ? 8 : -1, 100.0);
    double sample_count_number =
        read_bme_number_or(root,
                           values,
                           "sample_count",
                           value_count == 11 ? 10 : value_count == 12 ? 11 : -1,
                           1.0);
    double flags_number = read_bme_number_or(root, values, "flags", value_count == 11 ? 9 : -1, 0.0);

    int air_quality_score = (int)lround(score_number);
    int gas_score = (int)lround(gas_score_number);
    int humidity_score = (int)lround(humidity_score_number);
    int sample_count = (int)lround(sample_count_number);
    int flags = (int)lround(flags_number);
    bool baseline_ready = false;
    bool warmup_done = false;
    if (value_count == 12) {
        baseline_ready = cJSON_GetArrayItem(values, 9)->valueint != 0;
        warmup_done = cJSON_GetArrayItem(values, 10)->valueint != 0;
    } else {
        baseline_ready = (flags & 0x01) != 0;
        warmup_done = (flags & 0x02) != 0;
    }
    cJSON *baseline_ready_item = cJSON_GetObjectItemCaseSensitive(root, "baseline_ready");
    cJSON *warmup_done_item = cJSON_GetObjectItemCaseSensitive(root, "warmup_done");
    if (cJSON_IsBool(baseline_ready_item)) {
        baseline_ready = cJSON_IsTrue(baseline_ready_item);
    }
    if (cJSON_IsBool(warmup_done_item)) {
        warmup_done = cJSON_IsTrue(warmup_done_item);
    }

    const char *sensor_id = read_json_string(root, "sensor_id");
    const char *level = read_json_string_first(root, "air_quality_level", "level");
    const char *confidence =
        read_json_string_first(root, "air_quality_confidence", "confidence");
    const char *algorithm = read_bme_algorithm_version(root, compact_v2);

    cJSON_AddStringToObject(payload, "sensor_id", sensor_id[0] != '\0' ? sensor_id : "bme690_01");
    cJSON_AddNumberToObject(payload, "temperature_c", temperature);
    cJSON_AddNumberToObject(payload, "humidity_percent", humidity);
    cJSON_AddNumberToObject(payload, "pressure_hpa", pressure);
    cJSON_AddNumberToObject(payload, "gas_resistance_ohm", gas_resistance);
    cJSON_AddNumberToObject(payload, "air_quality_score", air_quality_score);
    cJSON_AddStringToObject(payload,
                            "air_quality_level",
                            level[0] != '\0' ? level :
                                                protocol_adapter_air_quality_level(air_quality_score));
    cJSON_AddStringToObject(payload, "air_quality_confidence", confidence[0] != '\0' ? confidence : "medium");
    cJSON_AddStringToObject(payload,
                            "level",
                            level[0] != '\0' ? level :
                                                protocol_adapter_air_quality_level(air_quality_score));
    cJSON_AddStringToObject(payload, "confidence", confidence[0] != '\0' ? confidence : "medium");
    cJSON_AddStringToObject(payload, "algorithm_version", algorithm);
    cJSON_AddStringToObject(payload, "air_quality_algo_version", algorithm);
    cJSON_AddStringToObject(payload, "air_quality_source", "s3_mapped");
    cJSON_AddNumberToObject(payload, "gas_baseline_ohm", gas_baseline);
    cJSON_AddNumberToObject(payload, "gas_ratio", gas_ratio);
    cJSON_AddNumberToObject(payload, "gas_score", gas_score);
    cJSON_AddNumberToObject(payload, "humidity_score", humidity_score);
    cJSON_AddBoolToObject(payload, "baseline_ready", baseline_ready);
    cJSON_AddBoolToObject(payload, "warmup_done", warmup_done);
    cJSON_AddNumberToObject(payload, "sample_count", sample_count > 0 ? sample_count : 1);

    const char *const passthrough_keys[] = {
        "sample_time_ms",
        "esp_uptime_ms",
        "time_synced",
        "boot_id",
        "request_seq",
        "bme_diag",
    };
    for (size_t i = 0; i < sizeof(passthrough_keys) / sizeof(passthrough_keys[0]); ++i) {
        esp_err_t ret = protocol_adapter_copy_bme_item(root, payload, passthrough_keys[i]);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    esp_err_t air_quality_ret = protocol_adapter_copy_bme_air_quality(root, payload);
    if (air_quality_ret == ESP_OK) {
        return ESP_OK;
    }
    if (air_quality_ret != ESP_ERR_NOT_FOUND) {
        return air_quality_ret;
    }

    /* Legacy C5 frames have no nested air_quality object; retain the prior compatibility shape. */
    cJSON *air_quality_json = cJSON_AddObjectToObject(payload, "air_quality_json");
    if (air_quality_json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(air_quality_json, "algorithm_version", algorithm);
    cJSON_AddNumberToObject(air_quality_json, "gas_baseline_ohm", gas_baseline);
    cJSON_AddNumberToObject(air_quality_json, "gas_ratio", gas_ratio);
    cJSON_AddNumberToObject(air_quality_json, "gas_score", gas_score);
    cJSON_AddNumberToObject(air_quality_json, "humidity_score", humidity_score);
    cJSON_AddNumberToObject(air_quality_json, "sample_count", sample_count > 0 ? sample_count : 1);
    cJSON_AddStringToObject(air_quality_json,
                            "level",
                            level[0] != '\0' ? level :
                                                protocol_adapter_air_quality_level(air_quality_score));
    cJSON_AddStringToObject(air_quality_json,
                            "confidence",
                            confidence[0] != '\0' ? confidence : "medium");
    cJSON_AddNumberToObject(air_quality_json, "air_quality_score", air_quality_score);
    return ESP_OK;
}

static esp_err_t protocol_adapter_fill_csi_v2_payload(protocol_adapter_envelope_t *out)
{
    if (out == NULL || out->root == NULL || csi_has_forbidden_signal_fields(out->root)) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *input_device_id = read_json_string(out->root, "device_id");
    const char *peer_id = read_json_string(out->root, "peer_id");
    const char *link_id = read_csi_link_id(out->root);
    const char *state_input = read_json_string_first(out->root, "state", "state_hint");
    const char *state = canonical_csi_state_hint(state_input);
    cJSON *metrics = cJSON_GetObjectItemCaseSensitive(out->root, "metrics");

    double timestamp_ms = 0.0;
    double rssi = 0.0;
    double quality = 0.0;
    double motion_score = 0.0;
    double confidence = 0.0;
    protocol_adapter_csi_metrics_t csi_metrics = {0};
    const char *edge_state = "";
    bool edge_payload = read_csi_edge_fields(out->root, &motion_score, &confidence, &edge_state);
    bool metrics_payload = read_csi_feature_metrics(out->root, metrics, &csi_metrics);
    if (input_device_id[0] == '\0' || link_id[0] == '\0' ||
        !read_csi_timestamp_ms(out->root, &timestamp_ms)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!protocol_adapter_is_active_csi_link(link_id)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (edge_payload) {
        state = edge_state;
        rssi = metrics_payload ? csi_metrics.rssi :
                                 read_csi_number_or(out->root, metrics, "rssi", 0.0);
        quality = metrics_payload ? csi_metrics.quality :
                                    read_csi_number_or(out->root, metrics, "quality", confidence);
    } else if (metrics_payload) {
        rssi = csi_metrics.rssi;
        quality = csi_metrics.quality;
        confidence = quality;
        if (!read_finite_number(out->root, "motion_score", &motion_score)) {
            motion_score = quality;
        }
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    if (motion_score < 0.0 || motion_score > 1.0 ||
        confidence < 0.0 || confidence > 1.0 ||
        quality < 0.0 || quality > 1.0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t local_id = protocol_adapter_device_id_to_local_id(input_device_id);
    const char *device_id = protocol_adapter_local_device_id_to_device_id(local_id);
    if (device_id == NULL) {
        return ESP_ERR_NOT_ALLOWED;
    }

    out->local_id = local_id;
    out->local_protocol_version = ESP111_PROTOCOL_LOCAL_SCHEMA_VERSION;
    out->local_packet_type = ESP111_PROTOCOL_LOCAL_PACKET_CSI;
    out->seq = (uint32_t)((uint64_t)timestamp_ms & 0xffffffffU);
    out->timestamp_ms = (int64_t)timestamp_ms;
    out->uptime_ms = (int64_t)timestamp_ms;
    strlcpy(out->gateway_id, gateway_config_get()->gateway_id, sizeof(out->gateway_id));
    strlcpy(out->device_id, device_id, sizeof(out->device_id));
    strlcpy(out->room_id,
            protocol_adapter_local_device_id_to_room(local_id),
            sizeof(out->room_id));
    strlcpy(out->alias,
            protocol_adapter_local_device_id_to_alias(local_id),
            sizeof(out->alias));
    strlcpy(out->firmware_version,
            ESP111_PROTOCOL_FIRMWARE_VERSION,
            sizeof(out->firmware_version));
    strlcpy(out->message_type, ESP111_PROTOCOL_MSG_CSI_MOTION, sizeof(out->message_type));

    esp_err_t ret = protocol_adapter_add_capabilities(out);
    if (ret != ESP_OK) {
        return ret;
    }

    cJSON *payload = protocol_adapter_add_payload(out);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(payload, "device_id", out->device_id);
    if (peer_id[0] != '\0') {
        cJSON_AddStringToObject(payload, "peer_id", peer_id);
    }
    cJSON_AddStringToObject(payload, "link_id", link_id);
    cJSON_AddNumberToObject(payload, "timestamp_ms", timestamp_ms);
    if (state_input[0] != '\0') {
        cJSON_AddStringToObject(payload, "state_hint", state_input);
    }
    if (state[0] != '\0') {
        cJSON_AddStringToObject(payload, "state", state);
    }
    /*
     * csi_fusion consumes low-dimensional feature metrics; they remain nested
     * and are never promoted to the server-facing CSI event.
     */
    cJSON_AddNumberToObject(payload, "confidence", confidence);
    cJSON_AddNumberToObject(payload, "motion_score", motion_score);
    cJSON_AddNumberToObject(payload, "quality", quality);
    cJSON_AddNumberToObject(payload, "rssi", rssi);

    return add_csi_metrics_payload(payload, &csi_metrics);
}

static bool protocol_adapter_is_compact_csi_result(cJSON *root)
{
    if (root == NULL) {
        return false;
    }
    return cJSON_GetObjectItemCaseSensitive(root, ESP111_PROTOCOL_DEVICE_STREAM_JSON_LINK_ID) != NULL ||
           cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, ESP111_PROTOCOL_LOCAL_JSON_ID));
}

static esp_err_t protocol_adapter_fill_compact_csi_result_payload(protocol_adapter_envelope_t *out)
{
    if (out == NULL || out->root == NULL || csi_has_forbidden_signal_fields(out->root)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *id = cJSON_GetObjectItemCaseSensitive(out->root, ESP111_PROTOCOL_LOCAL_JSON_ID);
    const char *link_id = read_csi_link_id(out->root);
    cJSON *values = cJSON_GetObjectItemCaseSensitive(out->root,
                                                     ESP111_PROTOCOL_LOCAL_JSON_VALUES);
    double timestamp_ms = 0.0;
    if ((!cJSON_IsString(id) && !cJSON_IsNumber(id)) ||
        !read_csi_timestamp_ms(out->root, &timestamp_ms) ||
        link_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    double rssi = 0.0;
    double quality = 0.0;
    double motion_score = 0.0;
    double confidence = 0.0;
    protocol_adapter_csi_metrics_t csi_metrics = {0};
    const char *state = "";
    bool edge_payload = read_csi_edge_fields(out->root, &motion_score, &confidence, &state);
    bool metrics_payload = false;
    if (!edge_payload) {
        metrics_payload = cJSON_IsArray(values) && cJSON_GetArraySize(values) == 5 &&
                          read_finite_array_number(values, 0, &csi_metrics.frame_energy) &&
                          read_finite_array_number(values, 1, &csi_metrics.variance) &&
                          read_finite_array_number(values, 2, &csi_metrics.cv) &&
                          read_finite_array_number(values, 3, &csi_metrics.rssi) &&
                          read_finite_array_number(values, 4, &csi_metrics.quality);
        if (!metrics_payload ||
            csi_metrics.frame_energy < 0.0 ||
            csi_metrics.variance < 0.0 ||
            csi_metrics.cv < 0.0 ||
            csi_metrics.quality < 0.0 ||
            csi_metrics.quality > 1.0) {
            return ESP_ERR_INVALID_ARG;
        }
        csi_metrics.valid = true;
        rssi = csi_metrics.rssi;
        quality = csi_metrics.quality;
        confidence = quality;
        if (!read_finite_number(out->root, "motion_score", &motion_score)) {
            motion_score = quality;
        }
    } else {
        cJSON *metrics = cJSON_GetObjectItemCaseSensitive(out->root, "metrics");
        metrics_payload = read_csi_feature_metrics(out->root, metrics, &csi_metrics);
        rssi = metrics_payload ? csi_metrics.rssi :
                                 read_csi_number_or(out->root, metrics, "rssi", 0.0);
        quality = metrics_payload ? csi_metrics.quality :
                                    read_csi_number_or(out->root, metrics, "quality", confidence);
    }
    if (!protocol_adapter_is_active_csi_link(link_id)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (motion_score < 0.0 || motion_score > 1.0 ||
        confidence < 0.0 || confidence > 1.0 ||
        quality < 0.0 || quality > 1.0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t local_id = 0;
    if (cJSON_IsNumber(id)) {
        local_id = (uint8_t)id->valueint;
    } else {
        local_id = protocol_adapter_device_id_to_local_id(id->valuestring);
    }
    const char *device_id = protocol_adapter_local_device_id_to_device_id(local_id);
    if (device_id == NULL) {
        return ESP_ERR_NOT_ALLOWED;
    }
    const char *explicit_device_id = read_json_string(out->root, "device_id");
    if (explicit_device_id[0] != '\0' && strcmp(explicit_device_id, device_id) != 0) {
        return ESP_ERR_NOT_ALLOWED;
    }

    out->local_id = local_id;
    out->local_protocol_version = ESP111_PROTOCOL_LOCAL_SCHEMA_VERSION;
    out->local_packet_type = ESP111_PROTOCOL_LOCAL_PACKET_CSI;
    out->seq = (uint32_t)((uint64_t)timestamp_ms & 0xffffffffU);
    out->timestamp_ms = (int64_t)timestamp_ms;
    out->uptime_ms = (int64_t)timestamp_ms;
    strlcpy(out->gateway_id, gateway_config_get()->gateway_id, sizeof(out->gateway_id));
    strlcpy(out->device_id, device_id, sizeof(out->device_id));
    strlcpy(out->room_id,
            protocol_adapter_local_device_id_to_room(local_id),
            sizeof(out->room_id));
    strlcpy(out->alias,
            protocol_adapter_local_device_id_to_alias(local_id),
            sizeof(out->alias));
    strlcpy(out->firmware_version,
            ESP111_PROTOCOL_FIRMWARE_VERSION,
            sizeof(out->firmware_version));
    strlcpy(out->message_type, ESP111_PROTOCOL_MSG_CSI_MOTION, sizeof(out->message_type));

    esp_err_t ret = protocol_adapter_add_capabilities(out);
    if (ret != ESP_OK) {
        return ret;
    }

    cJSON *payload = protocol_adapter_add_payload(out);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(payload, "device_id", out->device_id);
    cJSON_AddStringToObject(payload, "link_id", link_id);
    cJSON_AddNumberToObject(payload, "timestamp_ms", timestamp_ms);
    if (state[0] != '\0') {
        cJSON_AddStringToObject(payload, "state", state);
    }
    cJSON_AddNumberToObject(payload, "confidence", confidence);
    cJSON_AddNumberToObject(payload, "motion_score", motion_score);
    cJSON_AddNumberToObject(payload, "quality", quality);
    cJSON_AddNumberToObject(payload, "rssi", rssi);

    return add_csi_metrics_payload(payload, &csi_metrics);
}

static esp_err_t protocol_adapter_fill_csi_payload(const protocol_adapter_envelope_t *envelope,
                                                   cJSON *payload)
{
    cJSON *values = cJSON_GetObjectItemCaseSensitive(envelope->root,
                                                     ESP111_PROTOCOL_LOCAL_JSON_VALUES);
    int value_count = cJSON_IsArray(values) ? cJSON_GetArraySize(values) : 0;
    if (payload == NULL || csi_has_forbidden_signal_fields(envelope->root)) {
        return ESP_ERR_INVALID_ARG;
    }

    double edge_motion_score = 0.0;
    double edge_confidence = 0.0;
    const char *edge_state = "";
    bool edge_payload =
        read_csi_edge_fields(envelope->root, &edge_motion_score, &edge_confidence, &edge_state);

    if (!edge_payload &&
        (value_count < 5 || value_count > 6 ||
         cJSON_GetObjectItemCaseSensitive(envelope->root, "metrics") != NULL ||
         cJSON_GetObjectItemCaseSensitive(envelope->root, "features") != NULL ||
         cJSON_GetObjectItemCaseSensitive(envelope->root, "device_id") != NULL ||
         cJSON_GetObjectItemCaseSensitive(envelope->root, "frame_energy") != NULL ||
         cJSON_GetObjectItemCaseSensitive(envelope->root, "energy") != NULL ||
         cJSON_GetObjectItemCaseSensitive(envelope->root, "variance") != NULL ||
         cJSON_GetObjectItemCaseSensitive(envelope->root, "cv") != NULL ||
         cJSON_GetObjectItemCaseSensitive(envelope->root, "rssi") != NULL ||
         cJSON_GetObjectItemCaseSensitive(envelope->root, "sample_count") != NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!edge_payload) {
        for (int i = 0; i < value_count; ++i) {
            cJSON *item = cJSON_GetArrayItem(values, i);
            if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    cJSON *link_item = cJSON_GetObjectItemCaseSensitive(envelope->root,
                                                        ESP111_PROTOCOL_DEVICE_STREAM_JSON_LINK_ID);
    if (!cJSON_IsString(link_item) || link_item->valuestring == NULL ||
        link_item->valuestring[0] == '\0') {
        link_item = cJSON_GetObjectItemCaseSensitive(envelope->root, "link_id");
    }
    if (!cJSON_IsString(link_item) || link_item->valuestring == NULL ||
        link_item->valuestring[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!protocol_adapter_is_active_csi_link(link_item->valuestring)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (edge_payload) {
        cJSON *metrics = cJSON_GetObjectItemCaseSensitive(envelope->root, "metrics");
        protocol_adapter_csi_metrics_t csi_metrics = {0};
        bool metrics_payload = read_csi_feature_metrics(envelope->root, metrics, &csi_metrics);
        double timestamp = 0.0;
        double quality = metrics_payload ? csi_metrics.quality :
                                           read_csi_number_or(envelope->root,
                                                              metrics,
                                                              "quality",
                                                              edge_confidence);
        double rssi = metrics_payload ? csi_metrics.rssi :
                                        read_csi_number_or(envelope->root, metrics, "rssi", 0.0);
        double frame_seq = read_csi_number_or(envelope->root, NULL, "frame_seq", 0.0);
        if (!read_csi_timestamp_ms(envelope->root, &timestamp)) {
            timestamp = envelope->timestamp_ms > 0 ? (double)envelope->timestamp_ms : 0.0;
        }
        if (timestamp <= 0.0 || quality < 0.0 || quality > 1.0 || frame_seq < 0.0) {
            return ESP_ERR_INVALID_ARG;
        }

        cJSON_AddStringToObject(payload, "device_id", envelope->device_id);
        cJSON_AddStringToObject(payload, "link_id", link_item->valuestring);
        cJSON_AddStringToObject(payload, "state", edge_state);
        cJSON_AddNumberToObject(payload, "confidence", edge_confidence);
        cJSON_AddNumberToObject(payload, "motion_score", edge_motion_score);
        cJSON_AddNumberToObject(payload, "quality", quality);
        cJSON_AddNumberToObject(payload, "rssi", rssi);
        cJSON_AddNumberToObject(payload, "timestamp", timestamp);
        cJSON_AddNumberToObject(payload, "timestamp_ms", timestamp);
        if (frame_seq > 0.0) {
            cJSON_AddNumberToObject(payload, "frame_seq", frame_seq);
        }
        return add_csi_metrics_payload(payload, &csi_metrics);
    }

    double confidence = cJSON_GetArrayItem(values, 0)->valuedouble;
    double quality = cJSON_GetArrayItem(values, 1)->valuedouble;
    double rssi = cJSON_GetArrayItem(values, 2)->valuedouble;
    double timestamp = cJSON_GetArrayItem(values, 3)->valuedouble;
    double frame_seq = cJSON_GetArrayItem(values, 4)->valuedouble;
    double tick_id = value_count == 6 ? cJSON_GetArrayItem(values, 5)->valuedouble : 0.0;

    if (confidence < 0.0 || confidence > 1.0 || quality < 0.0 || quality > 1.0 ||
        timestamp <= 0.0 || frame_seq < 0.0 || tick_id < 0.0) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_AddStringToObject(payload, "device_id", envelope->device_id);
    cJSON_AddStringToObject(payload, "link_id", link_item->valuestring);
    cJSON_AddNumberToObject(payload, "confidence", confidence);
    cJSON_AddNumberToObject(payload, "motion_score", confidence);
    cJSON_AddNumberToObject(payload, "quality", quality);
    cJSON_AddNumberToObject(payload, "rssi", rssi);
    cJSON_AddNumberToObject(payload, "frame_seq", frame_seq);
    cJSON_AddNumberToObject(payload, "timestamp", timestamp);
    cJSON_AddNumberToObject(payload, "timestamp_ms", timestamp);
    if (tick_id > 0.0) {
        cJSON_AddNumberToObject(payload, "tick_id", tick_id);
    }
    return ESP_OK;
}

static esp_err_t protocol_adapter_fill_local_payload(protocol_adapter_envelope_t *out,
                                                     const char *local_type)
{
    cJSON *payload = protocol_adapter_add_payload(out);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (strcmp(local_type, ESP111_PROTOCOL_LOCAL_TYPE_REGISTER) == 0) {
        cJSON_AddStringToObject(payload,
                                "protocol_version",
                                out->local_protocol_version >= ESP111_PROTOCOL_LOCAL_SCHEMA_VERSION ?
                                    "local-compact-json-v2" :
                                    "local-compact-json-v1");
        cJSON_AddStringToObject(payload, "firmware_role", ESP111_PROTOCOL_TERMINAL_ROLE);
        cJSON_AddBoolToObject(payload, "debug_direct_server_enabled", false);
        cJSON *commands = cJSON_AddArrayToObject(payload, "commands");
        if (commands == NULL) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(commands, cJSON_CreateString("device.noop"));
        cJSON_AddItemToArray(commands, cJSON_CreateString("lcd.show_text"));
        cJSON_AddItemToArray(commands, cJSON_CreateString("display.show_text"));
        return ESP_OK;
    }

    if (strcmp(local_type, ESP111_PROTOCOL_LOCAL_TYPE_HEARTBEAT) == 0) {
        cJSON_AddBoolToObject(payload, "wifi_connected", true);
        cJSON_AddStringToObject(payload, "role", ESP111_PROTOCOL_TERMINAL_ROLE);
        if (out->has_wifi_rssi) {
            cJSON_AddNumberToObject(payload, "wifi_rssi", out->wifi_rssi);
        }
        return ESP_OK;
    }

    if (strcmp(local_type, ESP111_PROTOCOL_LOCAL_TYPE_STATUS) == 0) {
        cJSON_AddStringToObject(payload, "role", ESP111_PROTOCOL_TERMINAL_ROLE);
        cJSON_AddStringToObject(payload, "gateway_ip", ESP111_PROTOCOL_GATEWAY_IP);
        cJSON_AddStringToObject(payload, "voice_client", "local_gateway");
        cJSON_AddStringToObject(payload, "command_poll", "local_gateway");
        if (out->has_wifi_rssi) {
            cJSON_AddNumberToObject(payload, "wifi_rssi", out->wifi_rssi);
        }
        return ESP_OK;
    }

    if (strcmp(local_type, ESP111_PROTOCOL_LOCAL_TYPE_BME690) == 0) {
        cJSON *values = cJSON_GetObjectItemCaseSensitive(out->root,
                                                         ESP111_PROTOCOL_LOCAL_JSON_VALUES);
        return protocol_adapter_fill_bme_payload(out->root,
                                                 values,
                                                 payload,
                                                 out->local_protocol_version >= ESP111_PROTOCOL_LOCAL_SCHEMA_VERSION);
    }

    if (strcmp(local_type, ESP111_PROTOCOL_LOCAL_TYPE_CSI_RESULT) == 0) {
        return protocol_adapter_fill_csi_payload(out, payload);
    }

    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t protocol_adapter_parse_local_envelope(const char *json,
                                                size_t json_len,
                                                protocol_adapter_envelope_t *out)
{
    if (json == NULL || json_len == 0 || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->root = cJSON_ParseWithLength(json, json_len);
    if (out->root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (schema_version_is_csi_v2(out->root)) {
        esp_err_t ret = protocol_adapter_fill_csi_v2_payload(out);
        if (ret != ESP_OK) {
            protocol_adapter_release_envelope(out);
        }
        return ret;
    }

    if (protocol_adapter_is_compact_csi_result(out->root)) {
        esp_err_t ret = protocol_adapter_fill_compact_csi_result_payload(out);
        if (ret != ESP_OK) {
            protocol_adapter_release_envelope(out);
        }
        return ret;
    }

    cJSON *local_id_item =
        cJSON_GetObjectItemCaseSensitive(out->root, ESP111_PROTOCOL_LOCAL_JSON_ID);
    cJSON *local_type_item =
        cJSON_GetObjectItemCaseSensitive(out->root, ESP111_PROTOCOL_LOCAL_JSON_TYPE);
    if (!cJSON_IsNumber(local_id_item) ||
        (!cJSON_IsString(local_type_item) && !cJSON_IsNumber(local_type_item))) {
        protocol_adapter_release_envelope(out);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t local_id = (uint8_t)local_id_item->valueint;
    const char *device_id = protocol_adapter_local_device_id_to_device_id(local_id);
    if (device_id == NULL) {
        protocol_adapter_release_envelope(out);
        return ESP_ERR_NOT_ALLOWED;
    }

    const char *local_type = NULL;
    out->local_id = local_id;
    out->local_protocol_version =
        (uint8_t)get_json_i64(out->root, ESP111_PROTOCOL_LOCAL_JSON_PROTOCOL_VERSION, 1);
    out->seq = (uint32_t)get_json_i64(out->root, ESP111_PROTOCOL_LOCAL_JSON_SEQ, 0);
    out->local_sensor_kind =
        (uint8_t)get_json_i64(out->root, ESP111_PROTOCOL_LOCAL_JSON_SENSOR_KIND, 0);
    out->local_health_subtype =
        (uint8_t)get_json_i64(out->root, ESP111_PROTOCOL_LOCAL_JSON_HEALTH_SUBTYPE, 0);
    cJSON *wifi_rssi = cJSON_GetObjectItemCaseSensitive(out->root,
                                                        ESP111_PROTOCOL_LOCAL_JSON_WIFI_RSSI);
    if (cJSON_IsNumber(wifi_rssi)) {
        out->has_wifi_rssi = true;
        out->wifi_rssi = wifi_rssi->valueint;
    }

    if (cJSON_IsString(local_type_item) && local_type_item->valuestring != NULL) {
        local_type = local_type_item->valuestring;
        if (strcmp(local_type, ESP111_PROTOCOL_LOCAL_TYPE_REGISTER) == 0) {
            out->local_packet_type = ESP111_PROTOCOL_LOCAL_PACKET_HEALTH;
            out->local_health_subtype = ESP111_PROTOCOL_LOCAL_HEALTH_REGISTER;
            strlcpy(out->message_type, ESP111_PROTOCOL_MSG_REGISTER, sizeof(out->message_type));
        } else if (strcmp(local_type, ESP111_PROTOCOL_LOCAL_TYPE_HEARTBEAT) == 0) {
            out->local_packet_type = ESP111_PROTOCOL_LOCAL_PACKET_HEALTH;
            out->local_health_subtype = ESP111_PROTOCOL_LOCAL_HEALTH_HEARTBEAT;
            strlcpy(out->message_type, ESP111_PROTOCOL_MSG_HEARTBEAT, sizeof(out->message_type));
        } else if (strcmp(local_type, ESP111_PROTOCOL_LOCAL_TYPE_STATUS) == 0) {
            out->local_packet_type = ESP111_PROTOCOL_LOCAL_PACKET_HEALTH;
            out->local_health_subtype = ESP111_PROTOCOL_LOCAL_HEALTH_STATUS;
            strlcpy(out->message_type, ESP111_PROTOCOL_MSG_STATUS, sizeof(out->message_type));
        } else if (strcmp(local_type, ESP111_PROTOCOL_LOCAL_TYPE_BME690) == 0) {
            out->local_packet_type = ESP111_PROTOCOL_LOCAL_PACKET_SENSOR;
            out->local_sensor_kind = ESP111_PROTOCOL_LOCAL_SENSOR_KIND_BME690;
            strlcpy(out->message_type, ESP111_PROTOCOL_MSG_SENSOR_BME690, sizeof(out->message_type));
        } else if (strcmp(local_type, ESP111_PROTOCOL_LOCAL_TYPE_CSI_RESULT) == 0) {
            out->local_packet_type = ESP111_PROTOCOL_LOCAL_PACKET_CSI;
            strlcpy(out->message_type, ESP111_PROTOCOL_MSG_CSI_MOTION, sizeof(out->message_type));
        } else {
            protocol_adapter_release_envelope(out);
            return ESP_ERR_NOT_SUPPORTED;
        }
    } else if (cJSON_IsNumber(local_type_item)) {
        out->local_packet_type = (uint8_t)local_type_item->valueint;
        switch (out->local_packet_type) {
        case ESP111_PROTOCOL_LOCAL_PACKET_SENSOR:
            if (out->local_sensor_kind != ESP111_PROTOCOL_LOCAL_SENSOR_KIND_BME690) {
                protocol_adapter_release_envelope(out);
                return ESP_ERR_NOT_SUPPORTED;
            }
            local_type = ESP111_PROTOCOL_LOCAL_TYPE_BME690;
            strlcpy(out->message_type, ESP111_PROTOCOL_MSG_SENSOR_BME690, sizeof(out->message_type));
            break;
        case ESP111_PROTOCOL_LOCAL_PACKET_HEALTH:
            if (out->local_health_subtype == ESP111_PROTOCOL_LOCAL_HEALTH_REGISTER) {
                local_type = ESP111_PROTOCOL_LOCAL_TYPE_REGISTER;
                strlcpy(out->message_type, ESP111_PROTOCOL_MSG_REGISTER, sizeof(out->message_type));
            } else if (out->local_health_subtype == ESP111_PROTOCOL_LOCAL_HEALTH_HEARTBEAT) {
                local_type = ESP111_PROTOCOL_LOCAL_TYPE_HEARTBEAT;
                strlcpy(out->message_type, ESP111_PROTOCOL_MSG_HEARTBEAT, sizeof(out->message_type));
            } else if (out->local_health_subtype == ESP111_PROTOCOL_LOCAL_HEALTH_STATUS) {
                local_type = ESP111_PROTOCOL_LOCAL_TYPE_STATUS;
                strlcpy(out->message_type, ESP111_PROTOCOL_MSG_STATUS, sizeof(out->message_type));
            } else {
                protocol_adapter_release_envelope(out);
                return ESP_ERR_NOT_SUPPORTED;
            }
            break;
        case ESP111_PROTOCOL_LOCAL_PACKET_CSI:
            local_type = ESP111_PROTOCOL_LOCAL_TYPE_CSI_RESULT;
            strlcpy(out->message_type, ESP111_PROTOCOL_MSG_CSI_MOTION, sizeof(out->message_type));
            break;
        case ESP111_PROTOCOL_LOCAL_PACKET_VOICE:
        case ESP111_PROTOCOL_LOCAL_PACKET_CMD_ACK:
        default:
            protocol_adapter_release_envelope(out);
            return ESP_ERR_NOT_SUPPORTED;
        }
    } else {
        protocol_adapter_release_envelope(out);
        return ESP_ERR_NOT_SUPPORTED;
    }

    strlcpy(out->gateway_id, gateway_config_get()->gateway_id, sizeof(out->gateway_id));
    strlcpy(out->device_id, device_id, sizeof(out->device_id));
    strlcpy(out->room_id,
            protocol_adapter_local_device_id_to_room(local_id),
            sizeof(out->room_id));
    const char *room_id = read_json_string(out->root, ESP111_PROTOCOL_LOCAL_JSON_ROOM_ID);
    if (room_id[0] != '\0') {
        strlcpy(out->room_id, room_id, sizeof(out->room_id));
    }
    const char *room_name = read_json_string(out->root, ESP111_PROTOCOL_LOCAL_JSON_ROOM_NAME);
    if (out->room_id[0] == '\0' && room_name[0] != '\0') {
        strlcpy(out->room_id, room_name, sizeof(out->room_id));
    }
    strlcpy(out->alias,
            protocol_adapter_local_device_id_to_alias(local_id),
            sizeof(out->alias));
    strlcpy(out->firmware_version,
            ESP111_PROTOCOL_FIRMWARE_VERSION,
            sizeof(out->firmware_version));
    out->uptime_ms = get_json_i64(out->root, ESP111_PROTOCOL_LOCAL_JSON_UPTIME_MS, 0);
    out->timestamp_ms = get_json_i64(out->root,
                                     ESP111_PROTOCOL_JSON_TIMESTAMP_MS,
                                     out->uptime_ms > 0 ? out->uptime_ms : protocol_adapter_now_ms());

    const char *payload_type = read_json_string(out->root,
                                                ESP111_PROTOCOL_LOCAL_JSON_PAYLOAD_TYPE);
    if (!local_payload_type_matches(payload_type, out->message_type)) {
        protocol_adapter_release_envelope(out);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = protocol_adapter_add_capabilities(out);
    if (ret == ESP_OK) {
        ret = protocol_adapter_fill_local_payload(out, local_type);
    }
    if (ret != ESP_OK) {
        protocol_adapter_release_envelope(out);
    }
    return ret;
}

esp_err_t protocol_adapter_parse_envelope(const char *json,
                                          size_t json_len,
                                          protocol_adapter_envelope_t *out)
{
    if (json == NULL || json_len == 0 || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->root = cJSON_ParseWithLength(json, json_len);
    if (out->root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *schema_version =
        cJSON_GetObjectItemCaseSensitive(out->root, ESP111_PROTOCOL_JSON_SCHEMA_VERSION);
    if (!cJSON_IsNumber(schema_version) ||
        schema_version->valueint != ESP111_PROTOCOL_SCHEMA_VERSION) {
        protocol_adapter_release_envelope(out);
        return ESP_ERR_INVALID_VERSION;
    }

    if (!copy_json_string(out->root,
                          ESP111_PROTOCOL_JSON_MESSAGE_TYPE,
                          out->message_type,
                          sizeof(out->message_type)) ||
        !copy_json_string(out->root,
                          ESP111_PROTOCOL_JSON_GATEWAY_ID,
                          out->gateway_id,
                          sizeof(out->gateway_id)) ||
        !copy_json_string(out->root,
                          ESP111_PROTOCOL_JSON_DEVICE_ID,
                          out->device_id,
                          sizeof(out->device_id))) {
        protocol_adapter_release_envelope(out);
        return ESP_ERR_INVALID_ARG;
    }

    (void)copy_json_string(out->root,
                           ESP111_PROTOCOL_JSON_ROOM_ID,
                           out->room_id,
                           sizeof(out->room_id));
    (void)copy_json_string(out->root,
                           ESP111_PROTOCOL_JSON_ALIAS,
                           out->alias,
                           sizeof(out->alias));
    (void)copy_json_string(out->root,
                           ESP111_PROTOCOL_JSON_FIRMWARE_VERSION,
                           out->firmware_version,
                           sizeof(out->firmware_version));

    out->seq = (uint32_t)get_json_i64(out->root, ESP111_PROTOCOL_JSON_SEQ, 0);
    out->timestamp_ms = get_json_i64(out->root, ESP111_PROTOCOL_JSON_TIMESTAMP_MS, 0);
    out->uptime_ms = get_json_i64(out->root, ESP111_PROTOCOL_JSON_UPTIME_MS, 0);
    out->payload = cJSON_GetObjectItemCaseSensitive(out->root, ESP111_PROTOCOL_JSON_PAYLOAD);
    out->capabilities =
        cJSON_GetObjectItemCaseSensitive(out->root, ESP111_PROTOCOL_JSON_CAPABILITIES);
    return ESP_OK;
}

void protocol_adapter_release_envelope(protocol_adapter_envelope_t *envelope)
{
    if (envelope == NULL) {
        return;
    }

    if (envelope->root != NULL) {
        cJSON_Delete(envelope->root);
    }
    memset(envelope, 0, sizeof(*envelope));
}

protocol_adapter_message_kind_t protocol_adapter_message_kind(const char *message_type)
{
    if (message_type == NULL) {
        return PROTOCOL_ADAPTER_MESSAGE_UNKNOWN;
    }
    if (strcmp(message_type, ESP111_PROTOCOL_MSG_REGISTER) == 0) {
        return PROTOCOL_ADAPTER_MESSAGE_REGISTER;
    }
    if (strcmp(message_type, ESP111_PROTOCOL_MSG_HEARTBEAT) == 0) {
        return PROTOCOL_ADAPTER_MESSAGE_HEARTBEAT;
    }
    if (strcmp(message_type, ESP111_PROTOCOL_MSG_STATUS) == 0) {
        return PROTOCOL_ADAPTER_MESSAGE_STATUS;
    }
    if (strcmp(message_type, ESP111_PROTOCOL_MSG_SENSOR_BME690) == 0) {
        return PROTOCOL_ADAPTER_MESSAGE_SENSOR_BME690;
    }
    if (strcmp(message_type, ESP111_PROTOCOL_MSG_CSI_RESULT) == 0 ||
        strcmp(message_type, ESP111_PROTOCOL_MSG_CSI_MOTION) == 0) {
        return PROTOCOL_ADAPTER_MESSAGE_CSI_RESULT;
    }
    if (strcmp(message_type, ESP111_PROTOCOL_MSG_COMMAND_ACK) == 0) {
        return PROTOCOL_ADAPTER_MESSAGE_COMMAND_ACK;
    }
    return PROTOCOL_ADAPTER_MESSAGE_UNKNOWN;
}

esp_err_t protocol_adapter_validate_local_envelope(const protocol_adapter_envelope_t *envelope)
{
    if (envelope == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(envelope->gateway_id, gateway_config_get()->gateway_id) != 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!gateway_config_child_allowed(envelope->device_id)) {
        return ESP_ERR_NOT_ALLOWED;
    }

    if (protocol_adapter_message_kind(envelope->message_type) ==
        PROTOCOL_ADAPTER_MESSAGE_UNKNOWN) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}

esp_err_t protocol_adapter_build_server_ingest_json(const protocol_adapter_envelope_t *envelope,
                                                    char **out_json)
{
    if (envelope == NULL || out_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_json = NULL;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(root,
                            ESP111_PROTOCOL_JSON_SCHEMA_VERSION,
                            ESP111_PROTOCOL_SCHEMA_VERSION);
    cJSON_AddStringToObject(root, ESP111_PROTOCOL_JSON_PAYLOAD_TYPE, envelope->message_type);
    cJSON_AddStringToObject(root, ESP111_PROTOCOL_JSON_DEVICE_ID, envelope->device_id);
    cJSON_AddStringToObject(root, ESP111_PROTOCOL_JSON_GATEWAY_ID, gateway_config_get()->gateway_id);
    cJSON_AddStringToObject(root, "source", "s3_gateway");
    cJSON_AddNumberToObject(root, ESP111_PROTOCOL_JSON_SEQ, envelope->seq);
    cJSON_AddStringToObject(root, "device_type", ESP111_PROTOCOL_TERMINAL_DEVICE_TYPE);
    if (envelope->room_id[0] != '\0') {
        cJSON_AddStringToObject(root, ESP111_PROTOCOL_JSON_ROOM_ID, envelope->room_id);
        cJSON_AddStringToObject(root, "room_name", envelope->room_id);
    }
    if (envelope->firmware_version[0] != '\0') {
        cJSON_AddStringToObject(root,
                                ESP111_PROTOCOL_JSON_FIRMWARE_VERSION,
                                envelope->firmware_version);
    }
    cJSON_AddBoolToObject(root, ESP111_PROTOCOL_JSON_TIME_SYNCED, false);
    cJSON_AddNumberToObject(root,
                            ESP111_PROTOCOL_JSON_TIMESTAMP_MS,
                            (double)(envelope->timestamp_ms > 0 ? envelope->timestamp_ms :
                                                                 protocol_adapter_now_ms()));
    if (envelope->uptime_ms > 0) {
        cJSON_AddNumberToObject(root,
                                ESP111_PROTOCOL_JSON_UPTIME_MS,
                                (double)envelope->uptime_ms);
    }

    if (envelope->payload != NULL) {
        cJSON *payload = cJSON_Duplicate(envelope->payload, true);
        if (payload == NULL ||
            !cJSON_AddItemToObject(root, ESP111_PROTOCOL_JSON_PAYLOAD, payload)) {
            cJSON_Delete(payload);
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
    } else {
        cJSON_AddObjectToObject(root, ESP111_PROTOCOL_JSON_PAYLOAD);
    }

    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out_json != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool protocol_adapter_float_range(float value, float min, float max)
{
    return isfinite((double)value) && value >= min && value <= max;
}

static esp_err_t protocol_adapter_validate_csi_event_v2(const csi_fusion_fact_t *fact,
                                                        const csi_fusion_telemetry_t *telemetry)
{
    if (fact == NULL || telemetry == NULL || !fact->valid || !telemetry->valid ||
        fact->schema_version != CSI_FUSION_SCHEMA_VERSION ||
        telemetry->schema_version != CSI_FUSION_SCHEMA_VERSION ||
        fact->trace_id[0] == '\0' ||
        strcmp(fact->trace_id, telemetry->trace_id) != 0 ||
        fact->tick_id != telemetry->tick_id ||
        fact->timestamp_ms == 0ULL || fact->timestamp_ms != telemetry->timestamp_ms ||
        telemetry->active_link_count == 0U ||
        fact->active_link_count != telemetry->active_link_count ||
        fact->active_link_count > CSI_FUSION_LINK_COUNT ||
        !protocol_adapter_float_range(fact->motion_score, 0.0f, 1.0f) ||
        !protocol_adapter_float_range(fact->confidence, 0.0f, 1.0f) ||
        !protocol_adapter_float_range(telemetry->motion_score, 0.0f, 1.0f) ||
        !protocol_adapter_float_range(telemetry->confidence, 0.0f, 1.0f) ||
        fact->fused_state != telemetry->fused_state) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint8_t i = 0; i < fact->active_link_count; ++i) {
        if (fact->links[i][0] == '\0' ||
            strcmp(fact->links[i], telemetry->links[i]) != 0) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static bool protocol_adapter_add_csi_link(cJSON *links, const char *link_id)
{
    if (links == NULL || link_id == NULL || link_id[0] == '\0') {
        return false;
    }

    cJSON *item = cJSON_CreateString(link_id);
    if (item == NULL) {
        return false;
    }
    if (!cJSON_AddItemToArray(links, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}

static bool csi_internal_link_is_known(const char *internal)
{
    if (internal == NULL || internal[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < sizeof(s_csi_internal_links) / sizeof(s_csi_internal_links[0]); ++i) {
        if (strcmp(internal, s_csi_internal_links[i]) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t protocol_adapter_add_csi_upload_links(cJSON *links,
                                                       const csi_fusion_fact_t *fact)
{
    if (links == NULL || fact == NULL || fact->active_link_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint8_t i = 0; i < fact->active_link_count && i < CSI_FUSION_LINK_COUNT; ++i) {
        const char *internal_link = fact->links[i];
        if (!csi_internal_link_is_known(internal_link)) {
            char mapping[48];
            snprintf(mapping,
                     sizeof(mapping),
                     "%s->unknown",
                     internal_link != NULL && internal_link[0] != '\0' ? internal_link : "-");
            if (strcmp(s_last_csi_server_link_map[i], mapping) != 0) {
                printf("CSI_SERVER_LINK_MAP internal_link=%s server_link=unknown reason=unknown_link\n",
                       internal_link != NULL && internal_link[0] != '\0' ? internal_link : "-");
                strlcpy(s_last_csi_server_link_map[i],
                        mapping,
                        sizeof(s_last_csi_server_link_map[i]));
            }
            return ESP_ERR_INVALID_ARG;
        }

        char server_link[16];
        int written = snprintf(server_link, sizeof(server_link), "link_%u", (unsigned int)i);
        if (written <= 0 || (size_t)written >= sizeof(server_link)) {
            return ESP_ERR_INVALID_SIZE;
        }
        char mapping[48];
        snprintf(mapping, sizeof(mapping), "%s->%s", internal_link, server_link);
        if (strcmp(s_last_csi_server_link_map[i], mapping) != 0) {
            printf("CSI_SERVER_LINK_MAP internal_link=%s server_link=%s\n",
                   internal_link,
                   server_link);
            strlcpy(s_last_csi_server_link_map[i],
                    mapping,
                    sizeof(s_last_csi_server_link_map[i]));
        }
        if (!protocol_adapter_add_csi_link(links, server_link)) {
            return ESP_ERR_NO_MEM;
        }
    }

    for (uint8_t i = fact->active_link_count;
         i < s_last_csi_server_link_count && i < CSI_FUSION_LINK_COUNT;
         ++i) {
        s_last_csi_server_link_map[i][0] = '\0';
    }
    s_last_csi_server_link_count = fact->active_link_count;

    return ESP_OK;
}

esp_err_t protocol_adapter_build_csi_event_v2_json(const csi_fusion_fact_t *fact,
                                                   const csi_fusion_telemetry_t *telemetry,
                                                   char **out_json)
{
    if (out_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;

    esp_err_t ret = protocol_adapter_validate_csi_event_v2(fact, telemetry);
    if (ret != ESP_OK) {
        return ret;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (cJSON_AddStringToObject(root,
                                ESP111_PROTOCOL_JSON_SCHEMA_VERSION,
                                ESP111_PROTOCOL_CSI_EVENT_SCHEMA_VERSION_STRING) == NULL ||
        cJSON_AddStringToObject(root, "trace_id", fact->trace_id) == NULL ||
        cJSON_AddNumberToObject(root, "tick_id", (double)fact->tick_id) == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    cJSON *links = cJSON_AddArrayToObject(root, "links");
    if (links == NULL ||
        cJSON_AddStringToObject(root,
                                "fused_state",
                                csi_fusion_state_to_string(fact->fused_state)) == NULL ||
        cJSON_AddNumberToObject(root, "confidence", fact->confidence) == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    ret = protocol_adapter_add_csi_upload_links(links, fact);
    if (ret != ESP_OK) {
        cJSON_Delete(root);
        return ret;
    }

    if (cJSON_AddNumberToObject(root, "timestamp_ms", (double)fact->timestamp_ms) == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out_json != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

void protocol_adapter_free_json(char *json)
{
    if (json != NULL) {
        cJSON_free(json);
    }
}

esp_err_t protocol_adapter_build_ok_response(const char *device_id,
                                             uint32_t seq,
                                             char *out,
                                             size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out,
                           out_size,
                           "{\"ok\":true,\"" ESP111_PROTOCOL_JSON_GATEWAY_ID "\":\"%s\",\""
                           ESP111_PROTOCOL_JSON_DEVICE_ID "\":\"%s\",\""
                           ESP111_PROTOCOL_JSON_SEQ "\":%u}",
                           gateway_config_get()->gateway_id,
                           device_id != NULL ? device_id : "",
                           (unsigned int)seq);
    return written > 0 && written < (int)out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t protocol_adapter_build_local_ok_response(uint8_t local_id,
                                                   char *out,
                                                   size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out,
                           out_size,
                           "{\"" ESP111_PROTOCOL_LOCAL_JSON_OK "\":1,"
                           "\"" ESP111_PROTOCOL_LOCAL_JSON_ID "\":%u}",
                           (unsigned int)local_id);
    return written > 0 && written < (int)out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t protocol_adapter_build_local_error_response(unsigned int error_code,
                                                      char *out,
                                                      size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out,
                           out_size,
                           "{\"" ESP111_PROTOCOL_LOCAL_JSON_OK "\":0,"
                           "\"" ESP111_PROTOCOL_LOCAL_JSON_ERROR "\":%u}",
                           error_code);
    return written > 0 && written < (int)out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t protocol_adapter_build_error_response(const char *error_code,
                                                const char *message,
                                                char *out,
                                                size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out,
                           out_size,
                           "{\"ok\":false,\"" ESP111_PROTOCOL_JSON_GATEWAY_ID "\":\"%s\",\""
                           ESP111_PROTOCOL_JSON_ERROR_CODE "\":\"%s\",\""
                           ESP111_PROTOCOL_JSON_MESSAGE "\":\"%s\"}",
                           gateway_config_get()->gateway_id,
                           error_code != NULL ? error_code : ESP111_PROTOCOL_ERROR_UNKNOWN,
                           message != NULL ? message : "");
    return written > 0 && written < (int)out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

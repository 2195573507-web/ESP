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
#include "gateway_config.h"

typedef struct {
    uint8_t local_id;
    const char *device_id;
    const char *alias;
    const char *room_name;
} protocol_adapter_local_child_t;

static const protocol_adapter_local_child_t s_local_children[] = {
    {ESP111_PROTOCOL_LOCAL_DEVICE_ID_C51, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51, "SensaiShuttle", "living_room"},
    {ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52, "SensaiShuttle02", "bedroom"},
};

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
    if (score >= 90) {
        return "excellent";
    }
    if (score >= 75) {
        return "good";
    }
    if (score >= 55) {
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

static esp_err_t protocol_adapter_fill_bme_payload(cJSON *values,
                                                   cJSON *payload,
                                                   bool compact_v2)
{
    int value_count = cJSON_IsArray(values) ? cJSON_GetArraySize(values) : 0;
    if (payload == NULL || value_count < (compact_v2 ? 11 : 12)) {
        return ESP_ERR_INVALID_ARG;
    }

    int air_quality_score = cJSON_GetArrayItem(values, 4)->valueint;
    int flags = compact_v2 ? cJSON_GetArrayItem(values, 9)->valueint : 0;
    cJSON_AddStringToObject(payload, "sensor_id", "bme690_01");
    cJSON_AddNumberToObject(payload, "temperature_c", cJSON_GetArrayItem(values, 0)->valuedouble);
    cJSON_AddNumberToObject(payload, "humidity_percent", cJSON_GetArrayItem(values, 1)->valuedouble);
    cJSON_AddNumberToObject(payload, "pressure_hpa", cJSON_GetArrayItem(values, 2)->valuedouble);
    cJSON_AddNumberToObject(payload, "gas_resistance_ohm", cJSON_GetArrayItem(values, 3)->valuedouble);
    cJSON_AddNumberToObject(payload, "air_quality_score", air_quality_score);
    cJSON_AddStringToObject(payload,
                            "air_quality_level",
                            protocol_adapter_air_quality_level(air_quality_score));
    cJSON_AddStringToObject(payload, "air_quality_confidence", "medium");
    cJSON_AddStringToObject(payload, "air_quality_algo_version", compact_v2 ? "c5_compact_v2" : "c5_compact_v1");
    cJSON_AddStringToObject(payload, "air_quality_source", "s3_mapped");
    cJSON_AddNumberToObject(payload, "gas_baseline_ohm", cJSON_GetArrayItem(values, 5)->valuedouble);
    cJSON_AddNumberToObject(payload, "gas_ratio", cJSON_GetArrayItem(values, 6)->valuedouble);
    cJSON_AddNumberToObject(payload, "gas_score", cJSON_GetArrayItem(values, 7)->valueint);
    cJSON_AddNumberToObject(payload, "humidity_score", cJSON_GetArrayItem(values, 8)->valueint);
    cJSON_AddBoolToObject(payload,
                          "baseline_ready",
                          compact_v2 ? ((flags & 0x01) != 0) : (cJSON_GetArrayItem(values, 9)->valueint != 0));
    cJSON_AddBoolToObject(payload,
                          "warmup_done",
                          compact_v2 ? ((flags & 0x02) != 0) : (cJSON_GetArrayItem(values, 10)->valueint != 0));
    cJSON_AddNumberToObject(payload,
                            "sample_count",
                            cJSON_GetArrayItem(values, compact_v2 ? 10 : 11)->valuedouble);
    return ESP_OK;
}

static esp_err_t protocol_adapter_fill_csi_v2_payload(protocol_adapter_envelope_t *out)
{
    if (out == NULL || out->root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *input_device_id = read_json_string(out->root, "device_id");
    const char *peer_id = read_json_string(out->root, "peer_id");
    const char *link_id = read_json_string(out->root, "link_id");
    const char *state_hint = read_json_string(out->root, "state_hint");
    const char *state = canonical_csi_state_hint(state_hint);
    cJSON *metrics = cJSON_GetObjectItemCaseSensitive(out->root, "metrics");

    double timestamp_ms = 0.0;
    double frame_energy = 0.0;
    double variance = 0.0;
    double cv = 0.0;
    double rssi = 0.0;
    double quality = 0.0;
    if (input_device_id[0] == '\0' || link_id[0] == '\0' || !cJSON_IsObject(metrics) ||
        !read_finite_number(out->root, "timestamp_ms", &timestamp_ms) ||
        !read_finite_number(metrics, "frame_energy", &frame_energy) ||
        !read_finite_number(metrics, "variance", &variance) ||
        !read_finite_number(metrics, "cv", &cv) ||
        !read_finite_number(metrics, "rssi", &rssi) ||
        !read_finite_number(metrics, "quality", &quality) ||
        timestamp_ms <= 0.0 || quality < 0.0 || quality > 1.0) {
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
    if (state_hint[0] != '\0') {
        cJSON_AddStringToObject(payload, "state_hint", state_hint);
    }
    if (state[0] != '\0') {
        cJSON_AddStringToObject(payload, "state", state);
    }
    /*
     * csi_fusion still consumes the established confidence/quality/rssi surface.
     * The v2 metrics remain nested so S3 does not promote raw signal fields.
     */
    cJSON_AddNumberToObject(payload, "confidence", quality);
    cJSON_AddNumberToObject(payload, "quality", quality);
    cJSON_AddNumberToObject(payload, "rssi", rssi);

    cJSON *payload_metrics = cJSON_AddObjectToObject(payload, "metrics");
    if (payload_metrics == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(payload_metrics, "frame_energy", frame_energy);
    cJSON_AddNumberToObject(payload_metrics, "variance", variance);
    cJSON_AddNumberToObject(payload_metrics, "cv", cv);
    cJSON_AddNumberToObject(payload_metrics, "rssi", rssi);
    cJSON_AddNumberToObject(payload_metrics, "quality", quality);
    return ESP_OK;
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
    if (out == NULL || out->root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *id = cJSON_GetObjectItemCaseSensitive(out->root, ESP111_PROTOCOL_LOCAL_JSON_ID);
    cJSON *timestamp = cJSON_GetObjectItemCaseSensitive(out->root,
                                                        ESP111_PROTOCOL_DEVICE_STREAM_JSON_TIMESTAMP);
    cJSON *link_id = cJSON_GetObjectItemCaseSensitive(out->root,
                                                       ESP111_PROTOCOL_DEVICE_STREAM_JSON_LINK_ID);
    cJSON *values = cJSON_GetObjectItemCaseSensitive(out->root,
                                                     ESP111_PROTOCOL_LOCAL_JSON_VALUES);
    if ((!cJSON_IsString(id) && !cJSON_IsNumber(id)) ||
        !cJSON_IsNumber(timestamp) || !isfinite(timestamp->valuedouble) ||
        !cJSON_IsString(link_id) || link_id->valuestring == NULL ||
        link_id->valuestring[0] == '\0' ||
        !cJSON_IsArray(values) || cJSON_GetArraySize(values) != 5) {
        return ESP_ERR_INVALID_ARG;
    }

    double frame_energy = 0.0;
    double variance = 0.0;
    double cv = 0.0;
    double rssi = 0.0;
    double quality = 0.0;
    if (!read_finite_array_number(values, 0, &frame_energy) ||
        !read_finite_array_number(values, 1, &variance) ||
        !read_finite_array_number(values, 2, &cv) ||
        !read_finite_array_number(values, 3, &rssi) ||
        !read_finite_array_number(values, 4, &quality) ||
        timestamp->valuedouble <= 0.0 || quality < 0.0 || quality > 1.0) {
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

    out->local_id = local_id;
    out->local_protocol_version = ESP111_PROTOCOL_LOCAL_SCHEMA_VERSION;
    out->local_packet_type = ESP111_PROTOCOL_LOCAL_PACKET_CSI;
    out->seq = (uint32_t)((uint64_t)timestamp->valuedouble & 0xffffffffU);
    out->timestamp_ms = (int64_t)timestamp->valuedouble;
    out->uptime_ms = (int64_t)timestamp->valuedouble;
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
    cJSON_AddStringToObject(payload, "link_id", link_id->valuestring);
    cJSON_AddNumberToObject(payload, "timestamp_ms", timestamp->valuedouble);
    cJSON_AddNumberToObject(payload, "confidence", quality);
    cJSON_AddNumberToObject(payload, "quality", quality);
    cJSON_AddNumberToObject(payload, "rssi", rssi);

    cJSON *payload_metrics = cJSON_AddObjectToObject(payload, "metrics");
    if (payload_metrics == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(payload_metrics, "frame_energy", frame_energy);
    cJSON_AddNumberToObject(payload_metrics, "variance", variance);
    cJSON_AddNumberToObject(payload_metrics, "cv", cv);
    cJSON_AddNumberToObject(payload_metrics, "rssi", rssi);
    cJSON_AddNumberToObject(payload_metrics, "quality", quality);
    return ESP_OK;
}

static esp_err_t protocol_adapter_fill_csi_payload(const protocol_adapter_envelope_t *envelope,
                                                   cJSON *payload)
{
    cJSON *values = cJSON_GetObjectItemCaseSensitive(envelope->root,
                                                     ESP111_PROTOCOL_LOCAL_JSON_VALUES);
    int value_count = cJSON_IsArray(values) ? cJSON_GetArraySize(values) : 0;
    if (payload == NULL || value_count < 5 || value_count > 6) {
        return ESP_ERR_INVALID_ARG;
    }

    if (cJSON_GetObjectItemCaseSensitive(envelope->root, "raw_csi") != NULL ||
        cJSON_GetObjectItemCaseSensitive(envelope->root, "subcarrier_data") != NULL ||
        cJSON_GetObjectItemCaseSensitive(envelope->root, "selected_subcarriers") != NULL ||
        cJSON_GetObjectItemCaseSensitive(envelope->root, "iq") != NULL ||
        cJSON_GetObjectItemCaseSensitive(envelope->root, "phase") != NULL ||
        cJSON_GetObjectItemCaseSensitive(envelope->root, "metrics") != NULL ||
        cJSON_GetObjectItemCaseSensitive(envelope->root, "features") != NULL ||
        cJSON_GetObjectItemCaseSensitive(envelope->root, "device_id") != NULL ||
        cJSON_GetObjectItemCaseSensitive(envelope->root, "frame_energy") != NULL ||
        cJSON_GetObjectItemCaseSensitive(envelope->root, "energy") != NULL ||
        cJSON_GetObjectItemCaseSensitive(envelope->root, "variance") != NULL ||
        cJSON_GetObjectItemCaseSensitive(envelope->root, "cv") != NULL ||
        cJSON_GetObjectItemCaseSensitive(envelope->root, "rssi") != NULL ||
        cJSON_GetObjectItemCaseSensitive(envelope->root, "motion_score") != NULL ||
        cJSON_GetObjectItemCaseSensitive(envelope->root, "sample_count") != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < value_count; ++i) {
        cJSON *item = cJSON_GetArrayItem(values, i);
        if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) {
            return ESP_ERR_INVALID_ARG;
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
        return protocol_adapter_fill_bme_payload(values,
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
        !protocol_adapter_float_range(fact->confidence, 0.0f, 1.0f) ||
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

    for (uint8_t i = 0; i < fact->active_link_count && i < CSI_FUSION_LINK_COUNT; ++i) {
        const char *internal_link = fact->links[i];
        const char *server_link = NULL;
        if (strcmp(internal_link, "S3_TO_C51") == 0) {
            server_link = "link_0";
        } else if (strcmp(internal_link, "S3_TO_C52") == 0) {
            server_link = "link_1";
        } else if (strcmp(internal_link, "C51_TO_C52") == 0) {
            server_link = "link_2";
        } else if (strcmp(internal_link, "C52_TO_C51") == 0) {
            server_link = "link_3";
        } else {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }

        printf("CSI_SERVER_LINK_MAP internal_link=%s server_link=%s\n",
               internal_link,
               server_link);

        if (!protocol_adapter_add_csi_link(links, server_link)) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
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

/**
 * @file sensor_aggregator.c
 * @brief S3 网关 sensor/status 上云聚合器。
 *
 * 本文件属于 ESPS3 网关，负责把 protocol_adapter 已补全的 status/sensor envelope
 * 构造成 Server ingest JSON 并通过 server_client 转发。它不读取传感器、不修改 BME
 * v 数组顺序，也不缓存离线队列；Server 可用性只交给 offline_policy 记录。
 */

#include "sensor_aggregator.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "child_registry.h"
#include "csi_fusion.h"
#include "esp111_protocol_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gateway_event_reporter.h"
#include "gateway_config.h"
#include "gateway_wifi.h"
#include "offline_policy.h"
#include "protocol_adapter.h"
#include "server_client.h"
#include "voice_proxy.h"

static const char *TAG = "sensor_aggregator";

#define SENSOR_AGGREGATOR_HISTORY_SIZE 8U
#define SENSOR_AGGREGATOR_RECENT_SIZE 4U

typedef struct {
    bool seeded;
    bool online;
    bool has_wifi_rssi;
    bool has_sensor;
    char device_id[CHILD_REGISTRY_DEVICE_ID_LEN];
    char name[64];
    char room_name[48];
    uint8_t local_id;
    int wifi_rssi;
    int64_t timestamp_ms;
    int64_t last_seen_ms;
    int64_t last_uptime_ms;
    bool wifi_connected;
    bool voice_ready;
    bool command_ready;
    uint32_t free_heap;
    uint32_t min_free_heap;
    double temperature;
    double humidity;
    double pressure;
    double gas_resistance;
    int air_quality_score;
    char air_quality_level[16];
    char air_quality_source[24];
} sensor_aggregator_device_t;

typedef struct {
    bool valid;
    char device_id[CHILD_REGISTRY_DEVICE_ID_LEN];
    int64_t timestamp_ms;
    double temperature;
    double humidity;
    double pressure;
    double gas_resistance;
    int air_quality_score;
    char air_quality_level[16];
} sensor_aggregator_history_t;

typedef struct {
    bool valid;
    char device_id[CHILD_REGISTRY_DEVICE_ID_LEN];
    char event[32];
    int64_t timestamp_ms;
    uint32_t duration_ms;
} sensor_aggregator_voice_event_t;

typedef struct {
    bool valid;
    char command_id[48];
    char device_id[CHILD_REGISTRY_DEVICE_ID_LEN];
    unsigned int command_code;
    char status[16];
    int64_t timestamp_ms;
} sensor_aggregator_command_event_t;

static sensor_aggregator_device_t s_devices[GATEWAY_CONFIG_MAX_CHILDREN];
static sensor_aggregator_history_t s_history[SENSOR_AGGREGATOR_HISTORY_SIZE];
static sensor_aggregator_voice_event_t s_voice_events[SENSOR_AGGREGATOR_RECENT_SIZE];
static sensor_aggregator_command_event_t s_command_events[SENSOR_AGGREGATOR_RECENT_SIZE];
static csi_fusion_fact_t s_latest_csi_fact;
static SemaphoreHandle_t s_lock;
static size_t s_history_cursor;
static size_t s_voice_cursor;
static size_t s_command_cursor;
static bool s_has_latest_csi_fact;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *default_room_for_local_id(uint8_t local_id)
{
    return local_id == ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52 ? "bedroom" : "living_room";
}

static void seed_device(sensor_aggregator_device_t *device, uint8_t local_id)
{
    if (device == NULL) {
        return;
    }

    memset(device, 0, sizeof(*device));
    device->seeded = true;
    device->local_id = local_id;
    strlcpy(device->device_id,
            protocol_adapter_local_device_id_to_device_id(local_id),
            sizeof(device->device_id));
    strlcpy(device->name,
            protocol_adapter_local_device_id_to_alias(local_id),
            sizeof(device->name));
    strlcpy(device->room_name,
            default_room_for_local_id(local_id),
            sizeof(device->room_name));
    strlcpy(device->air_quality_level, "unknown", sizeof(device->air_quality_level));
    strlcpy(device->air_quality_source, "s3_mapped", sizeof(device->air_quality_source));
}

static sensor_aggregator_device_t *find_device_locked(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return NULL;
    }

    for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN; i++) {
        if (s_devices[i].device_id[0] != '\0' &&
            strcmp(s_devices[i].device_id, device_id) == 0) {
            return &s_devices[i];
        }
    }
    return NULL;
}

static bool json_array_bool(cJSON *array, int index, bool fallback)
{
    cJSON *value = cJSON_IsArray(array) ? cJSON_GetArrayItem(array, index) : NULL;
    return cJSON_IsNumber(value) ? value->valueint != 0 : fallback;
}

static uint32_t json_array_u32(cJSON *array, int index, uint32_t fallback)
{
    cJSON *value = cJSON_IsArray(array) ? cJSON_GetArrayItem(array, index) : NULL;
    if (!cJSON_IsNumber(value) || value->valuedouble < 0) {
        return fallback;
    }
    return (uint32_t)value->valuedouble;
}

static double json_number(cJSON *root, const char *key, double fallback)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(value) ? value->valuedouble : fallback;
}

static int json_int(cJSON *root, const char *key, int fallback)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(value) ? value->valueint : fallback;
}

static const char *json_string(cJSON *root, const char *key, const char *fallback)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(value) && value->valuestring != NULL ? value->valuestring : fallback;
}

static bool build_csi_fact_server_json(const csi_fusion_fact_t *fact, char **out_json)
{
    if (fact == NULL || out_json == NULL || !fact->valid) {
        return false;
    }

    *out_json = NULL;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return false;
    }
    cJSON_AddNumberToObject(root,
                            ESP111_PROTOCOL_JSON_SCHEMA_VERSION,
                            ESP111_PROTOCOL_SCHEMA_VERSION);
    cJSON_AddStringToObject(root, ESP111_PROTOCOL_JSON_PAYLOAD_TYPE, ESP111_PROTOCOL_MSG_CSI_MOTION);
    cJSON_AddStringToObject(root, ESP111_PROTOCOL_JSON_DEVICE_ID, fact->device_id);
    cJSON_AddStringToObject(root, ESP111_PROTOCOL_JSON_GATEWAY_ID, gateway_config_get()->gateway_id);
    cJSON_AddStringToObject(root, "source", "s3_gateway");
    cJSON_AddStringToObject(root, "device_type", "S3");
    cJSON_AddBoolToObject(root, ESP111_PROTOCOL_JSON_TIME_SYNCED, false);
    cJSON_AddNumberToObject(root, ESP111_PROTOCOL_JSON_TIMESTAMP_MS, (double)fact->timestamp_ms);

    cJSON *payload = cJSON_AddObjectToObject(root, ESP111_PROTOCOL_JSON_PAYLOAD);
    if (payload == NULL) {
        cJSON_Delete(root);
        return false;
    }
    cJSON_AddStringToObject(payload, "device_id", fact->device_id);
    cJSON_AddStringToObject(payload, "link_id", fact->link_id);
    cJSON_AddStringToObject(payload, "state", csi_fusion_state_to_string(fact->state));
    cJSON_AddNumberToObject(payload, "frame_energy", fact->frame_energy);
    cJSON_AddNumberToObject(payload, "variance", fact->variance);
    cJSON_AddNumberToObject(payload, "rssi", fact->rssi);
    cJSON_AddNumberToObject(payload, "motion_score", fact->motion_score);
    cJSON_AddNumberToObject(payload, "timestamp", (double)fact->timestamp_ms);

    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out_json != NULL;
}

static void update_latest_csi_locked(const csi_fusion_fact_t *fact)
{
    if (fact == NULL || !fact->valid) {
        return;
    }

    s_latest_csi_fact = *fact;
    s_has_latest_csi_fact = true;
}

static void add_snapshot_csi_json(cJSON *root)
{
    cJSON *csi = cJSON_AddObjectToObject(root, "csi");
    if (csi == NULL) {
        return;
    }

    if (!s_has_latest_csi_fact || !s_latest_csi_fact.valid) {
        cJSON_AddBoolToObject(csi, "available", false);
        cJSON_AddStringToObject(csi, "device_id", gateway_config_get()->gateway_id);
        cJSON_AddStringToObject(csi, "link_id", "fused");
        cJSON_AddStringToObject(csi, "state", "IDLE");
        cJSON_AddNullToObject(csi, "frame_energy");
        cJSON_AddNullToObject(csi, "variance");
        cJSON_AddNullToObject(csi, "rssi");
        cJSON_AddNumberToObject(csi, "motion_score", 0.0);
        cJSON_AddNullToObject(csi, "timestamp");
        return;
    }

    cJSON_AddBoolToObject(csi, "available", true);
    cJSON_AddStringToObject(csi, "device_id", s_latest_csi_fact.device_id);
    cJSON_AddStringToObject(csi, "link_id", s_latest_csi_fact.link_id);
    cJSON_AddStringToObject(csi, "state", csi_fusion_state_to_string(s_latest_csi_fact.state));
    cJSON_AddNumberToObject(csi, "frame_energy", s_latest_csi_fact.frame_energy);
    cJSON_AddNumberToObject(csi, "variance", s_latest_csi_fact.variance);
    cJSON_AddNumberToObject(csi, "rssi", s_latest_csi_fact.rssi);
    cJSON_AddNumberToObject(csi, "motion_score", s_latest_csi_fact.motion_score);
    cJSON_AddNumberToObject(csi, "timestamp", (double)s_latest_csi_fact.timestamp_ms);
}

static bool device_is_online(const sensor_aggregator_device_t *device, int64_t timestamp_ms)
{
    if (device == NULL || !device->online || device->last_seen_ms <= 0) {
        return false;
    }

    return timestamp_ms - device->last_seen_ms <= (int64_t)gateway_config_get()->heartbeat_timeout_ms;
}

static child_registry_status_t device_registry_status(const sensor_aggregator_device_t *device,
                                                      int64_t timestamp_ms)
{
    if (device == NULL || device->device_id[0] == '\0') {
        return CHILD_REGISTRY_STATUS_OFFLINE;
    }

    child_registry_status_t status = CHILD_REGISTRY_STATUS_OFFLINE;
    if (child_registry_get_status_info(device->device_id, &status)) {
        return status;
    }

    if (device_is_online(device, timestamp_ms)) {
        /*
         * 兼容还没 register 但已有聚合数据的旧快照；正常路径优先使用 child_registry，
         * 因为 voice_busy/link_lost 不能只靠 heartbeat last_seen 判断。
         */
        return CHILD_REGISTRY_STATUS_ONLINE;
    }
    return CHILD_REGISTRY_STATUS_OFFLINE;
}

static bool registry_status_is_onlineish(child_registry_status_t status)
{
    return status == CHILD_REGISTRY_STATUS_ONLINE ||
           status == CHILD_REGISTRY_STATUS_VOICE_BUSY ||
           status == CHILD_REGISTRY_STATUS_LINK_LOST;
}

static void report_status_change_if_needed(const char *device_id)
{
    child_registry_status_t current = CHILD_REGISTRY_STATUS_OFFLINE;
    child_registry_status_t previous = CHILD_REGISTRY_STATUS_OFFLINE;
    if (!child_registry_refresh_status_change(device_id, &current, &previous)) {
        return;
    }

    const bool now_online = registry_status_is_onlineish(current);
    const bool was_online = registry_status_is_onlineish(previous);
    if (now_online && !was_online) {
        (void)gateway_event_reporter_system(device_id,
                                            "info",
                                            "child online",
                                            child_registry_status_name(current));
    } else if (!now_online && was_online) {
        (void)gateway_event_reporter_alarm(device_id,
                                           "warning",
                                           "C5 offline",
                                           "child did not report before offline timeout",
                                           child_registry_status_name(current));
    }
}

static void refresh_child_status_events(void)
{
    const gateway_runtime_config_t *config = gateway_config_get();
    for (size_t i = 0; i < config->children_allowlist_count; i++) {
        report_status_change_if_needed(config->children_allowlist[i]);
    }
}

static void update_device_from_envelope_locked(const protocol_adapter_envelope_t *envelope)
{
    sensor_aggregator_device_t *device = find_device_locked(envelope->device_id);
    if (device == NULL) {
        return;
    }

    int64_t timestamp_ms = now_ms();
    device->online = true;
    device->last_seen_ms = timestamp_ms;
    device->timestamp_ms = timestamp_ms;
    device->last_uptime_ms = envelope->uptime_ms;
    if (envelope->alias[0] != '\0') {
        strlcpy(device->name, envelope->alias, sizeof(device->name));
    }
    if (envelope->room_id[0] != '\0') {
        strlcpy(device->room_name, envelope->room_id, sizeof(device->room_name));
    }
    if (envelope->has_wifi_rssi) {
        device->has_wifi_rssi = true;
        device->wifi_rssi = envelope->wifi_rssi;
    }

    cJSON *values = cJSON_GetObjectItemCaseSensitive(envelope->root,
                                                     ESP111_PROTOCOL_LOCAL_JSON_VALUES);
    if (protocol_adapter_message_kind(envelope->message_type) == PROTOCOL_ADAPTER_MESSAGE_REGISTER ||
        protocol_adapter_message_kind(envelope->message_type) == PROTOCOL_ADAPTER_MESSAGE_HEARTBEAT ||
        protocol_adapter_message_kind(envelope->message_type) == PROTOCOL_ADAPTER_MESSAGE_STATUS) {
        device->wifi_connected = json_array_bool(values, 0, true);
        device->voice_ready = json_array_bool(values, 1, true);
        device->command_ready = json_array_bool(values, 2, true);
        device->free_heap = json_array_u32(values, 3, device->free_heap);
        device->min_free_heap = json_array_u32(values, 4, device->min_free_heap);
    }

    if (protocol_adapter_message_kind(envelope->message_type) == PROTOCOL_ADAPTER_MESSAGE_SENSOR_BME690 &&
        envelope->payload != NULL) {
        device->has_sensor = true;
        device->temperature = json_number(envelope->payload, "temperature_c", device->temperature);
        device->humidity = json_number(envelope->payload, "humidity_percent", device->humidity);
        device->pressure = json_number(envelope->payload, "pressure_hpa", device->pressure);
        device->gas_resistance = json_number(envelope->payload,
                                             "gas_resistance_ohm",
                                             device->gas_resistance);
        device->air_quality_score = json_int(envelope->payload,
                                             "air_quality_score",
                                             device->air_quality_score);
        strlcpy(device->air_quality_level,
                json_string(envelope->payload, "air_quality_level", "unknown"),
                sizeof(device->air_quality_level));
        strlcpy(device->air_quality_source,
                json_string(envelope->payload, "air_quality_source", "s3_mapped"),
                sizeof(device->air_quality_source));

        sensor_aggregator_history_t *history = &s_history[s_history_cursor];
        memset(history, 0, sizeof(*history));
        history->valid = true;
        strlcpy(history->device_id, device->device_id, sizeof(history->device_id));
        history->timestamp_ms = timestamp_ms;
        history->temperature = device->temperature;
        history->humidity = device->humidity;
        history->pressure = device->pressure;
        history->gas_resistance = device->gas_resistance;
        history->air_quality_score = device->air_quality_score;
        strlcpy(history->air_quality_level,
                device->air_quality_level,
                sizeof(history->air_quality_level));
        s_history_cursor = (s_history_cursor + 1U) % SENSOR_AGGREGATOR_HISTORY_SIZE;
    }

}

static void add_device_json(cJSON *devices,
                            const sensor_aggregator_device_t *state,
                            int64_t timestamp_ms,
                            size_t *online_count,
                            size_t *offline_count,
                            double *temp_sum,
                            double *humidity_sum,
                            double *air_sum,
                            size_t *sensor_count)
{
    if (devices == NULL || state == NULL || state->device_id[0] == '\0') {
        return;
    }

    child_registry_status_t registry_status =
        device_registry_status(state, timestamp_ms);
    bool online = registry_status_is_onlineish(registry_status);
    if (online) {
        (*online_count)++;
    } else {
        (*offline_count)++;
    }

    cJSON *device = cJSON_CreateObject();
    cJSON_AddStringToObject(device, "device_id", state->device_id);
    cJSON_AddNumberToObject(device, "local_id", state->local_id);
    cJSON_AddStringToObject(device, "name", state->name);
    cJSON_AddStringToObject(device, "room_name", state->room_name);
    cJSON_AddBoolToObject(device, "online", online);
    cJSON_AddStringToObject(device, "status", child_registry_status_name(registry_status));
    cJSON_AddBoolToObject(device, "voice_busy", registry_status == CHILD_REGISTRY_STATUS_VOICE_BUSY);
    cJSON_AddBoolToObject(device, "link_lost", registry_status == CHILD_REGISTRY_STATUS_LINK_LOST);
    if (state->has_wifi_rssi) {
        cJSON_AddNumberToObject(device, "wifi_rssi", state->wifi_rssi);
    } else {
        cJSON_AddNullToObject(device, "wifi_rssi");
    }
    cJSON_AddNumberToObject(device, "timestamp", (double)(state->timestamp_ms > 0 ? state->timestamp_ms : timestamp_ms));

    if (state->has_sensor) {
        cJSON *sensors = cJSON_AddObjectToObject(device, "sensors");
        cJSON_AddNumberToObject(sensors, "temperature", state->temperature);
        cJSON_AddNumberToObject(sensors, "humidity", state->humidity);
        cJSON_AddNumberToObject(sensors, "pressure", state->pressure);
        cJSON_AddNumberToObject(sensors, "gas_resistance", state->gas_resistance);
        cJSON_AddNumberToObject(sensors, "air_quality_score", state->air_quality_score);
        cJSON_AddStringToObject(sensors, "air_quality_level", state->air_quality_level);
        cJSON_AddStringToObject(sensors, "air_quality_source", state->air_quality_source);
        if (online) {
            *temp_sum += state->temperature;
            *humidity_sum += state->humidity;
            *air_sum += state->air_quality_score;
            (*sensor_count)++;
        }
    } else {
        cJSON_AddNullToObject(device, "sensors");
    }

    cJSON_AddItemToArray(devices, device);
}

static cJSON *build_snapshot_locked(void)
{
    int64_t timestamp_ms = now_ms();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddNumberToObject(root,
                            ESP111_PROTOCOL_JSON_SCHEMA_VERSION,
                            ESP111_PROTOCOL_DASHBOARD_SNAPSHOT_SCHEMA_VERSION);
    cJSON_AddStringToObject(root,
                            ESP111_PROTOCOL_JSON_PAYLOAD_TYPE,
                            ESP111_PROTOCOL_MSG_DASHBOARD_SNAPSHOT);
    cJSON_AddStringToObject(root, "source", "s3_gateway");
    add_snapshot_csi_json(root);

    cJSON *gateway = cJSON_AddObjectToObject(root, "gateway");
    cJSON_AddStringToObject(gateway, "gateway_id", gateway_config_get()->gateway_id);
    cJSON_AddBoolToObject(gateway, "online", true);
    cJSON_AddBoolToObject(gateway, "softap_ready", gateway_wifi_is_softap_ready());
    cJSON_AddBoolToObject(gateway, "sta_connected", gateway_wifi_is_sta_connected());
    cJSON_AddBoolToObject(gateway, "server_available", offline_policy_server_available());
    cJSON_AddBoolToObject(gateway, "voice_busy", voice_proxy_is_busy());
    cJSON_AddStringToObject(gateway, "last_error", offline_policy_last_error_code());
    cJSON_AddNumberToObject(gateway, "timestamp", (double)timestamp_ms);

    cJSON *devices = cJSON_AddArrayToObject(root, "devices");
    size_t online_count = 0;
    size_t offline_count = 0;
    size_t sensor_count = 0;
    double temp_sum = 0;
    double humidity_sum = 0;
    double air_sum = 0;
    for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN; i++) {
        add_device_json(devices,
                        &s_devices[i],
                        timestamp_ms,
                        &online_count,
                        &offline_count,
                        &temp_sum,
                        &humidity_sum,
                        &air_sum,
                        &sensor_count);
    }

    cJSON *summary = cJSON_AddObjectToObject(root, "home_summary");
    cJSON_AddNumberToObject(summary, "online_device_count", (double)online_count);
    cJSON_AddNumberToObject(summary, "offline_device_count", (double)offline_count);
    if (sensor_count > 0) {
        cJSON_AddNumberToObject(summary, "avg_temperature", temp_sum / (double)sensor_count);
        cJSON_AddNumberToObject(summary, "avg_humidity", humidity_sum / (double)sensor_count);
        cJSON_AddNumberToObject(summary, "avg_air_quality", air_sum / (double)sensor_count);
    } else {
        cJSON_AddNullToObject(summary, "avg_temperature");
        cJSON_AddNullToObject(summary, "avg_humidity");
        cJSON_AddNullToObject(summary, "avg_air_quality");
    }

    cJSON *voice_events = cJSON_AddArrayToObject(root, "recent_voice_events");
    for (size_t offset = 0; offset < SENSOR_AGGREGATOR_RECENT_SIZE; offset++) {
        size_t index = (s_voice_cursor + offset) % SENSOR_AGGREGATOR_RECENT_SIZE;
        if (!s_voice_events[index].valid) {
            continue;
        }
        cJSON *event = cJSON_CreateObject();
        cJSON_AddStringToObject(event, "device_id", s_voice_events[index].device_id);
        cJSON_AddStringToObject(event, "event", s_voice_events[index].event);
        cJSON_AddNumberToObject(event, "timestamp", (double)s_voice_events[index].timestamp_ms);
        cJSON_AddNumberToObject(event, "duration_ms", s_voice_events[index].duration_ms);
        cJSON_AddStringToObject(event, "source", "s3_gateway");
        cJSON_AddItemToArray(voice_events, event);
    }

    cJSON *commands = cJSON_AddArrayToObject(root, "recent_commands");
    for (size_t offset = 0; offset < SENSOR_AGGREGATOR_RECENT_SIZE; offset++) {
        size_t index = (s_command_cursor + offset) % SENSOR_AGGREGATOR_RECENT_SIZE;
        if (!s_command_events[index].valid) {
            continue;
        }
        cJSON *event = cJSON_CreateObject();
        cJSON_AddStringToObject(event, "command_id", s_command_events[index].command_id);
        cJSON_AddStringToObject(event, "device_id", s_command_events[index].device_id);
        cJSON_AddNumberToObject(event, "command_code", s_command_events[index].command_code);
        cJSON_AddStringToObject(event, "status", s_command_events[index].status);
        cJSON_AddNumberToObject(event, "timestamp", (double)s_command_events[index].timestamp_ms);
        cJSON_AddStringToObject(event, "source", "s3_gateway");
        cJSON_AddItemToArray(commands, event);
    }

    cJSON *history = cJSON_AddArrayToObject(root, "history");
    for (size_t offset = 0; offset < SENSOR_AGGREGATOR_HISTORY_SIZE; offset++) {
        size_t index = (s_history_cursor + offset) % SENSOR_AGGREGATOR_HISTORY_SIZE;
        if (!s_history[index].valid) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "device_id", s_history[index].device_id);
        cJSON_AddStringToObject(item, "sensor_type", "bme690");
        cJSON_AddNumberToObject(item, "timestamp", (double)s_history[index].timestamp_ms);
        cJSON_AddNumberToObject(item, "temperature", s_history[index].temperature);
        cJSON_AddNumberToObject(item, "humidity", s_history[index].humidity);
        cJSON_AddNumberToObject(item, "pressure", s_history[index].pressure);
        cJSON_AddNumberToObject(item, "gas_resistance", s_history[index].gas_resistance);
        cJSON_AddNumberToObject(item, "air_quality_score", s_history[index].air_quality_score);
        cJSON_AddStringToObject(item, "air_quality_level", s_history[index].air_quality_level);
        cJSON_AddItemToArray(history, item);
    }

    return root;
}

void sensor_aggregator_upload_snapshot(void)
{
    if (s_lock == NULL) {
        return;
    }

    refresh_child_status_events();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *snapshot = build_snapshot_locked();
    xSemaphoreGive(s_lock);
    if (snapshot == NULL) {
        ESP_LOGW(TAG, "dashboard snapshot build failed");
        return;
    }

    char *json = cJSON_PrintUnformatted(snapshot);
    cJSON_Delete(snapshot);
    if (json == NULL) {
        ESP_LOGW(TAG, "dashboard snapshot serialize failed");
        return;
    }

    char response[SERVER_CLIENT_SMALL_BODY_BYTES];
    int status = 0;
    esp_err_t ret = server_client_post_gateway_state_json(json, response, sizeof(response), &status);
    cJSON_free(json);
    offline_policy_record_server_result(ret, status);
    gateway_event_reporter_record_server_state(ret == ESP_OK && status >= 200 && status < 300);
    if (ret != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG,
                 "dashboard snapshot upload deferred status=%d ret=%s error_code=%s",
                 status,
                 esp_err_to_name(ret),
                 offline_policy_code_for_result(ret, status));
    }
}

void sensor_aggregator_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        seed_device(&s_devices[0], ESP111_PROTOCOL_LOCAL_DEVICE_ID_C51);
        seed_device(&s_devices[1], ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52);
        for (size_t i = 2; i < GATEWAY_CONFIG_MAX_CHILDREN; i++) {
            memset(&s_devices[i], 0, sizeof(s_devices[i]));
        }
        memset(s_history, 0, sizeof(s_history));
        memset(s_voice_events, 0, sizeof(s_voice_events));
        memset(s_command_events, 0, sizeof(s_command_events));
        memset(&s_latest_csi_fact, 0, sizeof(s_latest_csi_fact));
        s_history_cursor = 0;
        s_voice_cursor = 0;
        s_command_cursor = 0;
        s_has_latest_csi_fact = false;
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "sensor/status aggregator initialized");
}

esp_err_t sensor_aggregator_handle_envelope(const protocol_adapter_envelope_t *envelope,
                                            sensor_aggregator_result_t *result)
{
    if (envelope == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    result->accepted = true;

    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        update_device_from_envelope_locked(envelope);
        xSemaphoreGive(s_lock);
    }
    report_status_change_if_needed(envelope->device_id);

    esp_err_t ret = ESP_OK;
    int status = 0;
    protocol_adapter_message_kind_t kind = protocol_adapter_message_kind(envelope->message_type);
    if (kind == PROTOCOL_ADAPTER_MESSAGE_CSI_RESULT) {
        result->accepted = false;
        result->server_ret = ESP_ERR_NOT_SUPPORTED;
        result->error_code = ESP111_PROTOCOL_ERROR_INVALID_CSI_RESULT;
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (kind == PROTOCOL_ADAPTER_MESSAGE_SENSOR_BME690) {
        char *server_json = NULL;
        ret = protocol_adapter_build_server_ingest_json(envelope, &server_json);
        if (ret != ESP_OK) {
            result->server_ret = ret;
            result->error_code = ESP111_PROTOCOL_ERROR_ADAPTER;
            return ret;
        }

        char response[SERVER_CLIENT_SMALL_BODY_BYTES];
        ret = server_client_post_ingest_json(server_json, response, sizeof(response), &status);
        protocol_adapter_free_json(server_json);
        offline_policy_record_server_result(ret, status);
    }

    result->server_ret = ret;
    result->server_status = status;
    result->forwarded = kind == PROTOCOL_ADAPTER_MESSAGE_SENSOR_BME690 ?
                            (ret == ESP_OK && status >= 200 && status < 300) :
                            true;
    result->error_code = result->forwarded ? "" : offline_policy_code_for_result(ret, status);

    if (!result->forwarded && result->server_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG,
                 "forward deferred device_id=%s message_type=%s error_code=%s status=%d ret=%s",
                 envelope->device_id,
                 envelope->message_type,
                 result->error_code,
                 status,
                 esp_err_to_name(ret));
    }

    sensor_aggregator_upload_snapshot();
    return ESP_OK;
}

esp_err_t sensor_aggregator_handle_csi_fact(const csi_fusion_fact_t *fact,
                                            sensor_aggregator_result_t *result)
{
    if (fact == NULL || result == NULL || !fact->valid) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    result->accepted = true;

    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        update_latest_csi_locked(fact);
        xSemaphoreGive(s_lock);
    }

    char *server_json = NULL;
    if (!build_csi_fact_server_json(fact, &server_json)) {
        result->server_ret = ESP_ERR_NO_MEM;
        result->error_code = ESP111_PROTOCOL_ERROR_ADAPTER;
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    char response[SERVER_CLIENT_SMALL_BODY_BYTES];
    esp_err_t ret = server_client_post_ingest_json(server_json, response, sizeof(response), &status);
    cJSON_free(server_json);
    offline_policy_record_server_result(ret, status);

    result->server_ret = ret;
    result->server_status = status;
    result->forwarded = ret == ESP_OK && status >= 200 && status < 300;
    result->error_code = result->forwarded ? "" : offline_policy_code_for_result(ret, status);
    if (!result->forwarded && result->server_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG,
                 "CSI fact forward deferred state=%s score=%.3f error_code=%s status=%d ret=%s",
                 csi_fusion_state_to_string(fact->state),
                 (double)fact->motion_score,
                 result->error_code,
                 status,
                 esp_err_to_name(ret));
    }

    sensor_aggregator_upload_snapshot();
    return ESP_OK;
}

void sensor_aggregator_record_voice_event(const char *device_id,
                                          size_t pcm_bytes,
                                          uint32_t duration_ms)
{
    if (device_id == NULL || device_id[0] == '\0' || s_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    sensor_aggregator_device_t *device = find_device_locked(device_id);
    int64_t timestamp_ms = now_ms();
    if (device != NULL) {
        device->online = true;
        device->last_seen_ms = timestamp_ms;
        device->timestamp_ms = timestamp_ms;
    }
    sensor_aggregator_voice_event_t *event = &s_voice_events[s_voice_cursor];
    memset(event, 0, sizeof(*event));
    event->valid = true;
    strlcpy(event->device_id, device_id, sizeof(event->device_id));
    strlcpy(event->event, "voice_turn_completed", sizeof(event->event));
    event->timestamp_ms = timestamp_ms;
    event->duration_ms = duration_ms > 0 ? duration_ms : (uint32_t)(pcm_bytes / 32U);
    s_voice_cursor = (s_voice_cursor + 1U) % SENSOR_AGGREGATOR_RECENT_SIZE;
    xSemaphoreGive(s_lock);
    (void)gateway_event_reporter_system(device_id, "info", "voice turn completed", "voice_proxy");
    sensor_aggregator_upload_snapshot();
}

void sensor_aggregator_record_command_ack(const char *device_id,
                                          const char *command_id,
                                          unsigned int command_code,
                                          bool completed)
{
    if (device_id == NULL || device_id[0] == '\0' ||
        command_id == NULL || command_id[0] == '\0' || s_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    sensor_aggregator_device_t *device = find_device_locked(device_id);
    int64_t timestamp_ms = now_ms();
    if (device != NULL) {
        device->online = true;
        device->last_seen_ms = timestamp_ms;
        device->timestamp_ms = timestamp_ms;
    }
    sensor_aggregator_command_event_t *event = &s_command_events[s_command_cursor];
    memset(event, 0, sizeof(*event));
    event->valid = true;
    strlcpy(event->command_id, command_id, sizeof(event->command_id));
    strlcpy(event->device_id, device_id, sizeof(event->device_id));
    event->command_code = command_code;
    strlcpy(event->status, completed ? "completed" : "failed", sizeof(event->status));
    event->timestamp_ms = timestamp_ms;
    s_command_cursor = (s_command_cursor + 1U) % SENSOR_AGGREGATOR_RECENT_SIZE;
    xSemaphoreGive(s_lock);
    if (completed) {
        (void)gateway_event_reporter_system(device_id, "info", "command acknowledged", command_id);
    } else {
        (void)gateway_event_reporter_alarm(device_id,
                                           "warning",
                                           "Command failed",
                                           "command acknowledged as failed",
                                           command_id);
    }
    sensor_aggregator_upload_snapshot();
}

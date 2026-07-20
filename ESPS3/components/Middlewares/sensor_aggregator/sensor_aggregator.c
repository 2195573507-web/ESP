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

#include "app_stack_monitor.h"
#include "bme_cache_manager.h"
#include "cJSON.h"
#include "child_registry.h"
#include "csi_fusion.h"
#include "environment_alarm_engine.h"
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
#include "s3_scheduler.h"
#include "server_client.h"
#include "network_worker.h"
#include "voice_proxy.h"

static const char *TAG = "sensor_aggregator";
static const char *BME_RX_TAG = "BME_RX";
static const char *ENV_ALARM_TAG = "ENV_ALARM";

#define SENSOR_AGGREGATOR_HISTORY_SIZE 8U
#define SENSOR_AGGREGATOR_RECENT_SIZE 4U

#ifndef SENSOR_AGGREGATOR_SNAPSHOT_LOCK_TIMEOUT_MS
#define SENSOR_AGGREGATOR_SNAPSHOT_LOCK_TIMEOUT_MS 100U
#endif

typedef struct {
    bool seeded;
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
    double gas_baseline;
    double gas_ratio;
    int air_quality_score;
    int gas_score;
    int humidity_score;
    int sample_count;
    char air_quality_level[16];
    char air_quality_confidence[16];
    char air_quality_source[24];
    char algorithm_version[64];
    cJSON *air_quality;
    cJSON *bme_diag;
} sensor_aggregator_device_t;

typedef struct {
    bool valid;
    char device_id[CHILD_REGISTRY_DEVICE_ID_LEN];
    int64_t timestamp_ms;
    double temperature;
    double humidity;
    double pressure;
    double gas_resistance;
    double gas_baseline;
    double gas_ratio;
    int air_quality_score;
    int gas_score;
    int humidity_score;
    int sample_count;
    char air_quality_level[16];
    char algorithm_version[64];
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
static int64_t s_last_stack_monitor_ms;
static bool s_peer_active[GATEWAY_CONFIG_MAX_CHILDREN];

static bool upload_gate_open(void)
{
    return gateway_wifi_is_sta_connected() && s3_scheduler_is_server_upload_allowed();
}

static bool bme_realtime_upload_ready(void)
{
    return network_worker_get_link_state() == NETWORK_WORKER_LINK_STABLE &&
           network_worker_is_server_ready();
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static alarm_device_id_t environment_alarm_device_id(const char *device_id)
{
    if (device_id == NULL) {
        return ALARM_DEVICE_INVALID;
    }
    if (strcmp(device_id, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51) == 0) {
        return ALARM_DEVICE_C51;
    }
    if (strcmp(device_id, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52) == 0) {
        return ALARM_DEVICE_C52;
    }
    return ALARM_DEVICE_INVALID;
}

static alarm_air_quality_level_t environment_alarm_air_quality_level(const char *level)
{
    if (level == NULL) {
        return ALARM_AIR_QUALITY_UNKNOWN;
    }
    if (strcmp(level, "excellent") == 0) {
        return ALARM_AIR_QUALITY_EXCELLENT;
    }
    if (strcmp(level, "good") == 0) {
        return ALARM_AIR_QUALITY_GOOD;
    }
    if (strcmp(level, "moderate") == 0) {
        return ALARM_AIR_QUALITY_MODERATE;
    }
    if (strcmp(level, "poor") == 0) {
        return ALARM_AIR_QUALITY_POOR;
    }
    if (strcmp(level, "bad") == 0) {
        return ALARM_AIR_QUALITY_BAD;
    }
    return ALARM_AIR_QUALITY_UNKNOWN;
}

static void environment_alarm_check(const char *device_id,
                                    const alarm_environment_sample_t *sample)
{
    if (device_id == NULL || sample == NULL) {
        return;
    }

    if (alarm_engine_update(sample, NULL) != ESP_OK) {
        return;
    }

    alarm_active_alarm_t active_alarm;
    if (alarm_engine_get_active(sample->device_id, &active_alarm, 1U) == 0U) {
        ESP_LOGI(ENV_ALARM_TAG,
                 "device=%s status=NORMAL temp=%.2f humidity=%.2f alarm=0",
                 device_id,
                 (double)sample->temperature,
                 (double)sample->humidity);
    }
}

static size_t peer_index(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return GATEWAY_CONFIG_MAX_CHILDREN;
    }

    const gateway_runtime_config_t *config = gateway_config_get();
    size_t count = config->children_allowlist_count;
    if (count > GATEWAY_CONFIG_MAX_CHILDREN) {
        count = GATEWAY_CONFIG_MAX_CHILDREN;
    }
    for (size_t i = 0; i < count; ++i) {
        if (config->children_allowlist[i] != NULL &&
            strcmp(config->children_allowlist[i], device_id) == 0) {
            return i;
        }
    }
    return GATEWAY_CONFIG_MAX_CHILDREN;
}

static bool peer_active_locked(const char *device_id)
{
    const size_t index = peer_index(device_id);
    return index < GATEWAY_CONFIG_MAX_CHILDREN && s_peer_active[index];
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
    strlcpy(device->air_quality_confidence,
            "medium",
            sizeof(device->air_quality_confidence));
    strlcpy(device->air_quality_source, "s3_mapped", sizeof(device->air_quality_source));
    strlcpy(device->algorithm_version, "unknown", sizeof(device->algorithm_version));
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

static cJSON *air_quality_json(cJSON *payload)
{
    cJSON *details = cJSON_GetObjectItemCaseSensitive(payload, "air_quality_json");
    return cJSON_IsObject(details) ? details : NULL;
}

static double air_quality_number(cJSON *payload, const char *key, double fallback)
{
    cJSON *details = air_quality_json(payload);
    double value = json_number(payload, key, fallback);
    return details != NULL ? json_number(details, key, value) : value;
}

static const char *air_quality_string(cJSON *payload, const char *key, const char *fallback)
{
    cJSON *details = air_quality_json(payload);
    const char *value = json_string(payload, key, fallback);
    return details != NULL ? json_string(details, key, value) : value;
}

static void replace_air_quality_json(sensor_aggregator_device_t *device, cJSON *payload)
{
    cJSON *source = air_quality_json(payload);
    if (device == NULL || source == NULL) {
        return;
    }

    cJSON *copy = cJSON_Duplicate(source, true);
    if (copy == NULL) {
        ESP_LOGW(TAG, "air_quality snapshot copy failed device=%s", device->device_id);
        return;
    }

    cJSON_Delete(device->air_quality);
    device->air_quality = copy;
}

static void replace_bme_diag_json(sensor_aggregator_device_t *device, cJSON *payload)
{
    cJSON *source = cJSON_GetObjectItemCaseSensitive(payload, "bme_diag");
    if (device == NULL || source == NULL) {
        return;
    }

    cJSON *copy = cJSON_Duplicate(source, true);
    if (copy == NULL) {
        ESP_LOGW(TAG, "bme_diag snapshot copy failed device=%s", device->device_id);
        return;
    }

    cJSON_Delete(device->bme_diag);
    device->bme_diag = copy;
}

static const char *air_quality_level_for_score(int score)
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

static void update_latest_csi_locked(const csi_fusion_fact_t *fact)
{
    if (fact == NULL || !fact->valid) {
        return;
    }

    s_latest_csi_fact = *fact;
    s_has_latest_csi_fact = true;
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
    size_t count = config->children_allowlist_count;
    if (count > GATEWAY_CONFIG_MAX_CHILDREN) {
        ESP_LOGW(TAG,
                 "children allowlist count clamped count=%u max=%u",
                 (unsigned int)count,
                 (unsigned int)GATEWAY_CONFIG_MAX_CHILDREN);
        count = GATEWAY_CONFIG_MAX_CHILDREN;
    }
    for (size_t i = 0; i < count; i++) {
        report_status_change_if_needed(config->children_allowlist[i]);
    }
}

static bool update_device_from_envelope_locked(const protocol_adapter_envelope_t *envelope,
                                               alarm_environment_sample_t *alarm_sample)
{
    if (alarm_sample != NULL) {
        memset(alarm_sample, 0, sizeof(*alarm_sample));
    }
    sensor_aggregator_device_t *device = find_device_locked(envelope->device_id);
    if (device == NULL) {
        return false;
    }

    int64_t timestamp_ms = now_ms();
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
        device->gas_baseline = air_quality_number(envelope->payload,
                                                  "gas_baseline_ohm",
                                                  device->gas_baseline);
        device->gas_ratio = air_quality_number(envelope->payload,
                                               "gas_ratio",
                                               device->gas_ratio);
        device->air_quality_score = json_int(envelope->payload,
                                             "air_quality_score",
                                             device->air_quality_score);
        device->gas_score = (int)air_quality_number(envelope->payload,
                                                    "gas_score",
                                                    device->gas_score);
        device->humidity_score = (int)air_quality_number(envelope->payload,
                                                         "humidity_score",
                                                         device->humidity_score);
        device->sample_count = (int)air_quality_number(envelope->payload,
                                                       "sample_count",
                                                       device->sample_count > 0 ? device->sample_count : 1);
        strlcpy(device->air_quality_level,
                json_string(envelope->payload, "air_quality_level", "unknown"),
                sizeof(device->air_quality_level));
        strlcpy(device->air_quality_confidence,
                json_string(envelope->payload, "air_quality_confidence", "medium"),
                sizeof(device->air_quality_confidence));
        strlcpy(device->air_quality_source,
                json_string(envelope->payload, "air_quality_source", "s3_mapped"),
                sizeof(device->air_quality_source));
        strlcpy(device->algorithm_version,
                air_quality_string(envelope->payload,
                                   "algorithm_version",
                                   json_string(envelope->payload,
                                               "air_quality_algo_version",
                                               "unknown")),
                sizeof(device->algorithm_version));
        replace_air_quality_json(device, envelope->payload);
        replace_bme_diag_json(device, envelope->payload);

        sensor_aggregator_history_t *history = &s_history[s_history_cursor];
        memset(history, 0, sizeof(*history));
        history->valid = true;
        strlcpy(history->device_id, device->device_id, sizeof(history->device_id));
        history->timestamp_ms = timestamp_ms;
        history->temperature = device->temperature;
        history->humidity = device->humidity;
        history->pressure = device->pressure;
        history->gas_resistance = device->gas_resistance;
        history->gas_baseline = device->gas_baseline;
        history->gas_ratio = device->gas_ratio;
        history->air_quality_score = device->air_quality_score;
        history->gas_score = device->gas_score;
        history->humidity_score = device->humidity_score;
        history->sample_count = device->sample_count;
        strlcpy(history->air_quality_level,
                device->air_quality_level,
                sizeof(history->air_quality_level));
        strlcpy(history->algorithm_version,
                device->algorithm_version,
                sizeof(history->algorithm_version));
        s_history_cursor = (s_history_cursor + 1U) % SENSOR_AGGREGATOR_HISTORY_SIZE;
        ESP_LOGI(BME_RX_TAG,
                 "BME_RX device=%s temp=%.2fC humidity=%.2f%% pressure=%.2fhPa "
                 "gas=%.0fOhm baseline=%.0fOhm ratio=%.3f aq=%d level=%s confidence=%s "
                 "samples=%d algo=%s",
                 device->device_id,
                 device->temperature,
                 device->humidity,
                 device->pressure,
                 device->gas_resistance,
                 device->gas_baseline,
                 device->gas_ratio,
                 device->air_quality_score,
                 device->air_quality_level,
                 device->air_quality_confidence,
                 device->sample_count > 0 ? device->sample_count : 1,
                 device->algorithm_version);

        const alarm_device_id_t alarm_device = environment_alarm_device_id(device->device_id);
        if (alarm_sample != NULL && alarm_device != ALARM_DEVICE_INVALID) {
            *alarm_sample = (alarm_environment_sample_t){
                .struct_version = ALARM_ENVIRONMENT_SAMPLE_VERSION,
                .device_id = alarm_device,
                .ingest_seq = envelope->seq,
                .timestamp_valid = envelope->timestamp_ms > 0,
                .timestamp_ms = envelope->timestamp_ms > 0 ?
                                    (uint64_t)envelope->timestamp_ms : 0U,
                .temperature = (float)device->temperature,
                .humidity = (float)device->humidity,
                .pressure = (float)device->pressure,
                .gas_resistance = (float)device->gas_resistance,
                .air_quality_score = (float)device->air_quality_score,
                .air_quality_level =
                    environment_alarm_air_quality_level(device->air_quality_level),
                .gas_ratio = (float)device->gas_ratio,
                .stability_score = 1.0f,
                .sensor_state = ALARM_SENSOR_READY,
            };
            return true;
        }
    }

    return false;
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

    child_registry_status_view_t registry_view;
    (void)child_registry_get_status_view(state->device_id, &registry_view);
    const bool online = registry_view.online;
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
    cJSON_AddStringToObject(device, "status", child_registry_status_name(registry_view.status));
    if (registry_view.offline_reason != NULL) {
        cJSON_AddStringToObject(device, "offline_reason", registry_view.offline_reason);
    } else {
        cJSON_AddNullToObject(device, "offline_reason");
    }
    if (registry_view.last_seen_ms > 0) {
        cJSON_AddNumberToObject(device, "last_seen_ms", (double)registry_view.last_seen_ms);
    } else {
        cJSON_AddNullToObject(device, "last_seen_ms");
    }
    cJSON_AddBoolToObject(device, "link_lost", registry_view.link_lost);
    cJSON_AddBoolToObject(device, "voice_busy", registry_view.voice_busy);
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
        cJSON_AddStringToObject(sensors,
                                "air_quality_confidence",
                                state->air_quality_confidence);
        cJSON_AddStringToObject(sensors, "air_quality_source", state->air_quality_source);
        if (state->air_quality != NULL) {
            cJSON *air_quality = cJSON_Duplicate(state->air_quality, true);
            if (air_quality != NULL) {
                cJSON_AddItemToObject(sensors, "air_quality", air_quality);
            }
        }
        if (state->bme_diag != NULL) {
            cJSON *bme_diag = cJSON_Duplicate(state->bme_diag, true);
            if (bme_diag != NULL) {
                cJSON_AddItemToObject(sensors, "bme_diag", bme_diag);
            }
        }
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

esp_err_t sensor_aggregator_upload_snapshot_now(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!upload_gate_open()) {
        return ESP_ERR_INVALID_STATE;
    }
    app_stack_monitor_log_periodic(TAG,
                                   "sensor_aggregator",
                                   &s_last_stack_monitor_ms,
                                   APP_STACK_MONITOR_INTERVAL_MS);
    refresh_child_status_events();

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(SENSOR_AGGREGATOR_SNAPSHOT_LOCK_TIMEOUT_MS)) !=
        pdTRUE) {
        return ESP_ERR_NOT_FINISHED;
    }
    cJSON *snapshot = build_snapshot_locked();
    xSemaphoreGive(s_lock);
    if (snapshot == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char *json = cJSON_PrintUnformatted(snapshot);
    cJSON_Delete(snapshot);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGD(TAG,
             "dashboard snapshot upload schema=%u payload_type=%s bytes=%u",
             (unsigned int)ESP111_PROTOCOL_DASHBOARD_SNAPSHOT_SCHEMA_VERSION,
             ESP111_PROTOCOL_MSG_DASHBOARD_SNAPSHOT,
             (unsigned int)strlen(json));

    char response[SERVER_CLIENT_SMALL_BODY_BYTES];
    int status = 0;
    esp_err_t ret = server_client_post_gateway_snapshot_json(json,
                                                              response,
                                                              sizeof(response),
                                                              &status);
    cJSON_free(json);
    if (ret == ESP_OK && (status < 200 || status >= 300)) {
        ret = ESP_ERR_INVALID_RESPONSE;
    }
    return ret;
}

void sensor_aggregator_upload_snapshot(void)
{
    if (!upload_gate_open()) {
        return;
    }
    esp_err_t ret = network_worker_enqueue_snapshot_upload();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "dashboard snapshot enqueue failed ret=%s", esp_err_to_name(ret));
    }
}

void sensor_aggregator_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN; ++i) {
            cJSON_Delete(s_devices[i].air_quality);
            cJSON_Delete(s_devices[i].bme_diag);
        }
        memset(s_devices, 0, sizeof(s_devices));
        seed_device(&s_devices[0], ESP111_PROTOCOL_LOCAL_DEVICE_ID_C51);
        seed_device(&s_devices[1], ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52);
        memset(s_history, 0, sizeof(s_history));
        memset(s_voice_events, 0, sizeof(s_voice_events));
        memset(s_command_events, 0, sizeof(s_command_events));
        memset(&s_latest_csi_fact, 0, sizeof(s_latest_csi_fact));
        s_history_cursor = 0;
        s_voice_cursor = 0;
        s_command_cursor = 0;
        s_has_latest_csi_fact = false;
        memset(s_peer_active, 0, sizeof(s_peer_active));
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "sensor/status aggregator initialized");
    if (alarm_engine_init(NULL) == ESP_OK) {
        ESP_LOGI(ENV_ALARM_TAG, "component initialized");
    }
}

esp_err_t sensor_aggregator_suspend_peer(const char *device_id)
{
    const size_t index = peer_index(device_id);
    if (index >= GATEWAY_CONFIG_MAX_CHILDREN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_peer_active[index] = false;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t sensor_aggregator_restore_peer(const char *device_id)
{
    const size_t index = peer_index(device_id);
    if (index >= GATEWAY_CONFIG_MAX_CHILDREN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_peer_active[index] = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool sensor_aggregator_peer_active(const char *device_id)
{
    if (s_lock == NULL) {
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool active = peer_active_locked(device_id);
    xSemaphoreGive(s_lock);
    return active;
}

bool sensor_aggregator_has_active_peers(void)
{
    if (s_lock == NULL) {
        return false;
    }

    bool active = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN; ++i) {
        if (s_peer_active[i]) {
            active = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return active;
}

esp_err_t sensor_aggregator_handle_envelope(const protocol_adapter_envelope_t *envelope,
                                            sensor_aggregator_result_t *result)
{
    if (envelope == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    result->accepted = true;

    alarm_environment_sample_t alarm_sample = {0};
    bool has_alarm_sample = false;
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        has_alarm_sample = update_device_from_envelope_locked(envelope, &alarm_sample);
        xSemaphoreGive(s_lock);
    }
    if (has_alarm_sample) {
        environment_alarm_check(envelope->device_id, &alarm_sample);
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

        uint32_t cache_sequence = 0U;
        ret = bme_cache_manager_push_json(server_json, &cache_sequence, NULL);
        if (ret != ESP_OK) {
            protocol_adapter_free_json(server_json);
            result->server_ret = ret;
            result->error_code = ESP111_PROTOCOL_ERROR_ADAPTER;
            return ret;
        }

        if (sensor_aggregator_peer_active(envelope->device_id) &&
            bme_realtime_upload_ready()) {
            ret = network_worker_submit_bme_cached_json_for_peer(server_json,
                                                                 cache_sequence,
                                                                 envelope->device_id,
                                                                 "sensor_envelope");
            if (ret != ESP_OK) {
                protocol_adapter_free_json(server_json);
                offline_policy_record_server_result(ret, status);
            }
        } else {
            protocol_adapter_free_json(server_json);
            ret = ESP_ERR_INVALID_STATE;
        }
    }

    result->server_ret = ret;
    result->server_status = status;
    result->forwarded = kind == PROTOCOL_ADAPTER_MESSAGE_SENSOR_BME690 ?
                            (ret == ESP_OK) :
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

esp_err_t sensor_aggregator_handle_stream_status(const char *device_id,
                                                 int64_t timestamp_ms,
                                                 double heap,
                                                 double uptime,
                                                 double wifi_rssi,
                                                 sensor_aggregator_result_t *result)
{
    if (device_id == NULL || device_id[0] == '\0' || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    result->accepted = true;
    result->forwarded = true;
    result->server_ret = ESP_OK;
    result->server_status = 0;
    result->error_code = "";

    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        sensor_aggregator_device_t *device = find_device_locked(device_id);
        if (device != NULL) {
            device->last_seen_ms = now_ms();
            device->timestamp_ms = timestamp_ms > 0 ? timestamp_ms : device->last_seen_ms;
            device->last_uptime_ms = (int64_t)uptime;
            device->has_wifi_rssi = true;
            device->wifi_rssi = (int)wifi_rssi;
            device->wifi_connected = true;
            device->voice_ready = true;
            device->command_ready = true;
            device->free_heap = heap > 0.0 ? (uint32_t)heap : device->free_heap;
            device->min_free_heap = device->free_heap;
        }
        xSemaphoreGive(s_lock);
    }

    report_status_change_if_needed(device_id);
    sensor_aggregator_upload_snapshot();
    return ESP_OK;
}

static bool build_stream_sensor_server_json(const char *device_id,
                                            int64_t timestamp_ms,
                                            const char *link_id,
                                            double sensor_value_1,
                                            double sensor_value_2,
                                            double quality,
                                            char **out_json)
{
    if (device_id == NULL || device_id[0] == '\0' || out_json == NULL) {
        return false;
    }

    *out_json = NULL;
    int score = (int)quality;
    const char *level = air_quality_level_for_score(score);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return false;
    }
    cJSON_AddNumberToObject(root,
                            ESP111_PROTOCOL_JSON_SCHEMA_VERSION,
                            ESP111_PROTOCOL_SCHEMA_VERSION);
    cJSON_AddStringToObject(root, ESP111_PROTOCOL_JSON_PAYLOAD_TYPE, ESP111_PROTOCOL_MSG_SENSOR_BME690);
    cJSON_AddStringToObject(root, ESP111_PROTOCOL_JSON_DEVICE_ID, device_id);
    cJSON_AddStringToObject(root, ESP111_PROTOCOL_JSON_GATEWAY_ID, gateway_config_get()->gateway_id);
    cJSON_AddStringToObject(root, "source", "s3_gateway");
    cJSON_AddNumberToObject(root, ESP111_PROTOCOL_JSON_TIMESTAMP_MS, (double)timestamp_ms);
    cJSON_AddBoolToObject(root, ESP111_PROTOCOL_JSON_TIME_SYNCED, false);

    cJSON *payload = cJSON_AddObjectToObject(root, ESP111_PROTOCOL_JSON_PAYLOAD);
    if (payload == NULL) {
        cJSON_Delete(root);
        return false;
    }
    cJSON_AddStringToObject(payload, "sensor_id", link_id != NULL && link_id[0] != '\0' ? link_id : "stream_sensor");
    cJSON_AddNumberToObject(payload, "temperature_c", sensor_value_1);
    cJSON_AddNumberToObject(payload, "humidity_percent", sensor_value_2);
    cJSON_AddNumberToObject(payload, "pressure_hpa", 0.0);
    cJSON_AddNumberToObject(payload, "gas_resistance_ohm", 0.0);
    cJSON_AddNumberToObject(payload, "air_quality_score", score);
    cJSON_AddStringToObject(payload, "air_quality_level", level);
    cJSON_AddStringToObject(payload, "air_quality_confidence", "medium");
    cJSON_AddStringToObject(payload, "air_quality_algo_version", "c5_stream_v1");
    cJSON_AddStringToObject(payload, "air_quality_source", "esp");
    cJSON_AddNumberToObject(payload, "sample_count", 1);
    cJSON *air_quality = cJSON_AddObjectToObject(payload, "air_quality_json");
    if (air_quality == NULL) {
        cJSON_Delete(root);
        return false;
    }
    cJSON_AddStringToObject(air_quality, "algorithm_version", "c5_stream_v1");
    cJSON_AddNumberToObject(air_quality, "gas_baseline_ohm", 0.0);
    cJSON_AddNumberToObject(air_quality, "gas_ratio", 0.0);
    cJSON_AddNumberToObject(air_quality, "gas_score", score);
    cJSON_AddNumberToObject(air_quality, "humidity_score", 100);
    cJSON_AddNumberToObject(air_quality, "sample_count", 1);

    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out_json != NULL;
}

esp_err_t sensor_aggregator_handle_stream_sensor(const char *device_id,
                                                 int64_t timestamp_ms,
                                                 const char *link_id,
                                                 double sensor_value_1,
                                                 double sensor_value_2,
                                                 double quality,
                                                 sensor_aggregator_result_t *result)
{
    if (device_id == NULL || device_id[0] == '\0' || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    result->accepted = true;

    int score = (int)quality;
    const char *level = air_quality_level_for_score(score);
    int64_t effective_timestamp = timestamp_ms > 0 ? timestamp_ms : now_ms();

    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        sensor_aggregator_device_t *device = find_device_locked(device_id);
        if (device != NULL) {
            device->last_seen_ms = now_ms();
            device->timestamp_ms = effective_timestamp;
            device->has_sensor = true;
            device->temperature = sensor_value_1;
            device->humidity = sensor_value_2;
            device->pressure = 0.0;
            device->gas_resistance = 0.0;
            device->gas_baseline = 0.0;
            device->gas_ratio = 0.0;
            device->air_quality_score = score;
            device->gas_score = score;
            device->humidity_score = 100;
            device->sample_count = 1;
            strlcpy(device->air_quality_level, level, sizeof(device->air_quality_level));
            strlcpy(device->air_quality_confidence,
                    "medium",
                    sizeof(device->air_quality_confidence));
            strlcpy(device->air_quality_source, "s3_mapped", sizeof(device->air_quality_source));
            strlcpy(device->algorithm_version, "c5_stream_v1", sizeof(device->algorithm_version));
            cJSON_Delete(device->air_quality);
            device->air_quality = NULL;
            cJSON_Delete(device->bme_diag);
            device->bme_diag = NULL;

            sensor_aggregator_history_t *history = &s_history[s_history_cursor];
            memset(history, 0, sizeof(*history));
            history->valid = true;
            strlcpy(history->device_id, device->device_id, sizeof(history->device_id));
            history->timestamp_ms = effective_timestamp;
            history->temperature = device->temperature;
            history->humidity = device->humidity;
            history->pressure = device->pressure;
            history->gas_resistance = device->gas_resistance;
            history->gas_baseline = device->gas_baseline;
            history->gas_ratio = device->gas_ratio;
            history->air_quality_score = device->air_quality_score;
            history->gas_score = device->gas_score;
            history->humidity_score = device->humidity_score;
            history->sample_count = device->sample_count;
            strlcpy(history->air_quality_level, device->air_quality_level, sizeof(history->air_quality_level));
            strlcpy(history->algorithm_version, device->algorithm_version, sizeof(history->algorithm_version));
            s_history_cursor = (s_history_cursor + 1U) % SENSOR_AGGREGATOR_HISTORY_SIZE;
        }
        xSemaphoreGive(s_lock);
    }

    if (!sensor_aggregator_peer_active(device_id)) {
        result->server_ret = ESP_ERR_INVALID_STATE;
        result->server_status = 0;
        result->forwarded = false;
        result->error_code = offline_policy_code_for_result(ESP_ERR_INVALID_STATE, 0);
        sensor_aggregator_upload_snapshot();
        return ESP_OK;
    }

    char *server_json = NULL;
    if (!build_stream_sensor_server_json(device_id,
                                         effective_timestamp,
                                         link_id,
                                         sensor_value_1,
                                         sensor_value_2,
                                         quality,
                                         &server_json)) {
        result->server_ret = ESP_ERR_NO_MEM;
        result->error_code = ESP111_PROTOCOL_ERROR_ADAPTER;
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t ret = network_worker_submit_peer_server_json(NETWORK_WORKER_SERVER_JSON_INGEST,
                                                           server_json,
                                                           device_id,
                                                           "stream_sensor");
    if (ret != ESP_OK) {
        cJSON_free(server_json);
        offline_policy_record_server_result(ret, status);
    }

    result->server_ret = ret;
    result->server_status = status;
    result->forwarded = ret == ESP_OK;
    result->error_code = result->forwarded ? "" : offline_policy_code_for_result(ret, status);
    if (!result->forwarded && result->server_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG,
                 "stream sensor forward deferred device_id=%s error_code=%s status=%d ret=%s",
                 device_id,
                 result->error_code,
                 status,
                 esp_err_to_name(ret));
    }

    sensor_aggregator_upload_snapshot();
    return ESP_OK;
}

esp_err_t sensor_aggregator_handle_csi_fact(const csi_fusion_fact_t *fact,
                                            const csi_fusion_telemetry_t *telemetry,
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
    esp_err_t build_ret = protocol_adapter_build_csi_event_v2_json(fact,
                                                                   telemetry,
                                                                   &server_json);
    if (build_ret != ESP_OK) {
        result->accepted = false;
        result->server_ret = build_ret;
        result->server_status = 0;
        result->forwarded = false;
        result->error_code = ESP111_PROTOCOL_ERROR_INVALID_CSI_RESULT;
        ESP_LOGW(TAG,
                 "CSI canonical event dropped tick_id=%llu ret=%s",
                 (unsigned long long)fact->tick_id,
                 esp_err_to_name(build_ret));
        return ESP_OK;
    }

    int status = 0;
    esp_err_t ret = network_worker_submit_server_json(NETWORK_WORKER_SERVER_JSON_CSI_EVENT,
                                                      server_json,
                                                      "csi_fact");
    if (ret != ESP_OK) {
        protocol_adapter_free_json(server_json);
        offline_policy_record_server_result(ret, status);
    }

    result->server_ret = ret;
    result->server_status = status;
    result->forwarded = ret == ESP_OK;
    result->error_code = result->forwarded ? "" : offline_policy_code_for_result(ret, status);
    if (!result->forwarded && result->server_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG,
                 "CSI fact forward deferred state=%s confidence=%.3f error_code=%s status=%d ret=%s",
                 csi_fusion_state_to_string(fact->fused_state),
                 (double)fact->confidence,
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

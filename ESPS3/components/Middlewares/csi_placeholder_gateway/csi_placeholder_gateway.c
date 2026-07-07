/**
 * @file csi_placeholder_gateway.c
 * @brief S3 CSI canonical ingest and fusion boundary.
 */

#include "csi_placeholder_gateway.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "child_registry.h"
#include "csi_fusion.h"
#include "device_stream_gateway.h"
#include "esp111_protocol_common.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gateway_config.h"
#include "sensor_aggregator.h"

static const char *TAG = "csi_gateway";

#define CSI_LATEST_DIAGNOSTIC_LOG_INTERVAL_MS 10000LL
#define CSI_RESULT_V2_LOG_INTERVAL_MS 1000LL
#define CSI_LATEST_LINK_COUNT 4U

typedef struct {
    bool valid;
    double frame_energy;
    double variance;
    double cv;
    double rssi;
    double quality;
} csi_result_v2_metrics_t;

typedef struct {
    bool valid;
    char link_id[CSI_FUSION_TEXT_LEN];
    char device_id[CSI_FUSION_TEXT_LEN];
    char state[16];
    float motion_score;
    float quality;
    int rssi;
    uint32_t sample_count;
    uint64_t updated_at_ms;
    csi_result_v2_metrics_t metrics;
} csi_latest_snapshot_t;

static SemaphoreHandle_t s_fusion_lock;
static SemaphoreHandle_t s_latest_lock;
static volatile bool s_csi_worker_running;
static csi_latest_snapshot_t s_latest_links[CSI_LATEST_LINK_COUNT];
static int64_t s_last_latest_log_ms;
static int64_t s_last_result_v2_log_ms;
static const char *const s_required_links[CSI_LATEST_LINK_COUNT] = {
    "S3_TO_C51",
    "S3_TO_C52",
    "C51_TO_C52",
    "C52_TO_C51",
};

static esp_err_t csi_placeholder_gateway_handle_feature_internal(const csi_fusion_feature_t *feature,
                                                                const csi_result_v2_metrics_t *metrics);

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *json_string(cJSON *root, const char *key, const char *fallback)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(value) && value->valuestring != NULL ? value->valuestring : fallback;
}

static double json_number(cJSON *root, const char *key, double fallback)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(value) ? value->valuedouble : fallback;
}

static uint32_t json_u32(cJSON *root, const char *key, uint32_t fallback)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(value) || value->valuedouble < 0.0) {
        return fallback;
    }
    return (uint32_t)value->valuedouble;
}

static uint64_t json_u64(cJSON *root, const char *key, uint64_t fallback)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(value) || value->valuedouble < 0.0) {
        return fallback;
    }
    return (uint64_t)value->valuedouble;
}

static const char *diagnostic_link_id(const char *link_id, const char *device_id)
{
    if (link_id != NULL) {
        for (size_t i = 0; i < CSI_LATEST_LINK_COUNT; ++i) {
            if (strcmp(link_id, s_required_links[i]) == 0) {
                return s_required_links[i];
            }
        }
        if (strcmp(link_id, "C51") == 0) {
            return "S3_TO_C51";
        }
        if (strcmp(link_id, "C52") == 0) {
            return "S3_TO_C52";
        }
    }
    if (device_id != NULL) {
        if (strcmp(device_id, "C51") == 0 ||
            strcmp(device_id, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51) == 0) {
            return "S3_TO_C51";
        }
        if (strcmp(device_id, "C52") == 0 ||
            strcmp(device_id, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52) == 0) {
            return "S3_TO_C52";
        }
    }
    return link_id != NULL && link_id[0] != '\0' ? link_id : "unknown";
}

static int latest_link_index(const char *link_id)
{
    if (link_id == NULL) {
        return -1;
    }
    for (size_t i = 0; i < CSI_LATEST_LINK_COUNT; ++i) {
        if (strcmp(link_id, s_required_links[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool read_v2_metrics(cJSON *payload, csi_result_v2_metrics_t *out)
{
    if (payload == NULL || out == NULL) {
        return false;
    }
    cJSON *metrics = cJSON_GetObjectItemCaseSensitive(payload, "metrics");
    if (!cJSON_IsObject(metrics)) {
        return false;
    }
    cJSON *frame_energy = cJSON_GetObjectItemCaseSensitive(metrics, "frame_energy");
    cJSON *variance = cJSON_GetObjectItemCaseSensitive(metrics, "variance");
    cJSON *cv = cJSON_GetObjectItemCaseSensitive(metrics, "cv");
    cJSON *rssi = cJSON_GetObjectItemCaseSensitive(metrics, "rssi");
    cJSON *quality = cJSON_GetObjectItemCaseSensitive(metrics, "quality");
    if (!cJSON_IsNumber(frame_energy) || !cJSON_IsNumber(variance) ||
        !cJSON_IsNumber(cv) || !cJSON_IsNumber(rssi) || !cJSON_IsNumber(quality)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->frame_energy = frame_energy->valuedouble;
    out->variance = variance->valuedouble;
    out->cv = cv->valuedouble;
    out->rssi = rssi->valuedouble;
    out->quality = quality->valuedouble;
    return true;
}

static void log_csi_result_v2(const csi_fusion_feature_t *feature,
                              const csi_result_v2_metrics_t *metrics)
{
    if (feature == NULL || metrics == NULL || !metrics->valid) {
        return;
    }
    int64_t timestamp_ms = now_ms();
    if (s_last_result_v2_log_ms != 0 &&
        timestamp_ms - s_last_result_v2_log_ms < CSI_RESULT_V2_LOG_INTERVAL_MS) {
        return;
    }
    s_last_result_v2_log_ms = timestamp_ms;

    ESP_LOGI(TAG,
             "CSI_RESULT_V2 link_id=%s device=%s energy=%.3f variance=%.3f cv=%.3f rssi=%.1f quality=%.3f state=%s",
             feature->link_id,
             feature->device_id,
             metrics->frame_energy,
             metrics->variance,
             metrics->cv,
             metrics->rssi,
             metrics->quality,
             feature->has_state ? csi_fusion_state_to_string(feature->state) : "-");
}

static void reset_latest_cache(void)
{
    for (size_t i = 0; i < CSI_LATEST_LINK_COUNT; ++i) {
        memset(&s_latest_links[i], 0, sizeof(s_latest_links[i]));
        strlcpy(s_latest_links[i].link_id, s_required_links[i], sizeof(s_latest_links[i].link_id));
        strlcpy(s_latest_links[i].state, "-", sizeof(s_latest_links[i].state));
    }
}

static void update_latest_snapshot(const csi_fusion_feature_t *feature,
                                   const csi_result_v2_metrics_t *metrics)
{
    if (feature == NULL) {
        return;
    }

    const char *link_id = diagnostic_link_id(feature->link_id, feature->device_id);
    int index = latest_link_index(link_id);
    if (index < 0) {
        return;
    }

    if (s_latest_lock != NULL) {
        xSemaphoreTake(s_latest_lock, portMAX_DELAY);
    }
    csi_latest_snapshot_t *slot = &s_latest_links[index];
    slot->valid = true;
    strlcpy(slot->link_id, link_id, sizeof(slot->link_id));
    strlcpy(slot->device_id, feature->device_id, sizeof(slot->device_id));
    strlcpy(slot->state,
            feature->has_state ? csi_fusion_state_to_string(feature->state) : "-",
            sizeof(slot->state));
    slot->motion_score = feature->confidence;
    slot->quality = feature->quality;
    slot->rssi = feature->rssi;
    if (slot->sample_count < UINT32_MAX) {
        ++slot->sample_count;
    }
    slot->updated_at_ms = feature->timestamp_ms > 0ULL ? feature->timestamp_ms : (uint64_t)now_ms();
    if (metrics != NULL && metrics->valid) {
        slot->metrics = *metrics;
    }
    if (s_latest_lock != NULL) {
        xSemaphoreGive(s_latest_lock);
    }
}

static bool has_forbidden_csi_field(cJSON *payload)
{
    return cJSON_GetObjectItemCaseSensitive(payload, "raw_csi") != NULL ||
           cJSON_GetObjectItemCaseSensitive(payload, "subcarrier_data") != NULL ||
           cJSON_GetObjectItemCaseSensitive(payload, "selected_subcarriers") != NULL ||
           cJSON_GetObjectItemCaseSensitive(payload, "iq") != NULL ||
           cJSON_GetObjectItemCaseSensitive(payload, "phase") != NULL ||
           cJSON_GetObjectItemCaseSensitive(payload, "energy") != NULL ||
           cJSON_GetObjectItemCaseSensitive(payload, "frame_energy") != NULL ||
           cJSON_GetObjectItemCaseSensitive(payload, "variance") != NULL ||
           cJSON_GetObjectItemCaseSensitive(payload, "cv") != NULL ||
           cJSON_GetObjectItemCaseSensitive(payload, "motion_score") != NULL;
}

static esp_err_t feature_from_envelope(const protocol_adapter_envelope_t *envelope,
                                       csi_fusion_feature_t *feature)
{
    if (envelope == NULL || envelope->payload == NULL || feature == NULL ||
        has_forbidden_csi_field(envelope->payload)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(feature, 0, sizeof(*feature));
    strlcpy(feature->device_id,
            json_string(envelope->payload, "device_id", envelope->device_id),
            sizeof(feature->device_id));
    strlcpy(feature->link_id,
            json_string(envelope->payload, "link_id", ""),
            sizeof(feature->link_id));
    strlcpy(feature->trace_id,
            json_string(envelope->payload, "trace_id", ""),
            sizeof(feature->trace_id));

    csi_fusion_state_t input_state;
    if (csi_fusion_state_from_string(json_string(envelope->payload, "state", ""), &input_state)) {
        feature->has_state = true;
        feature->state = input_state;
    }

    cJSON *metrics = cJSON_GetObjectItemCaseSensitive(envelope->payload, "metrics");
    double metric_quality = json_number(metrics, "quality", -1.0);
    double metric_rssi = json_number(metrics, "rssi", 0.0);
    feature->confidence = (float)json_number(envelope->payload, "confidence", metric_quality);
    feature->quality = (float)json_number(envelope->payload, "quality", metric_quality);
    feature->rssi = (int)json_number(envelope->payload, "rssi", metric_rssi);
    feature->frame_seq = json_u32(envelope->payload, "frame_seq", 0U);
    feature->tick_id = json_u64(envelope->payload, "tick_id", 0ULL);
    feature->timestamp_ms =
        json_u64(envelope->payload,
                 "timestamp_ms",
                 json_u64(envelope->payload,
                          "timestamp",
                          json_u64(envelope->payload, "updated_at_ms", (uint64_t)now_ms())));

    if (feature->link_id[0] == '\0' ||
        feature->confidence < 0.0f || feature->confidence > 1.0f ||
        feature->quality < 0.0f || feature->quality > 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t publish_fusion_outputs(const csi_fusion_fact_t *fact,
                                        const csi_fusion_telemetry_t *telemetry,
                                        sensor_aggregator_result_t *result)
{
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }

    if (telemetry != NULL && telemetry->valid) {
        char telemetry_json[384];
        if (csi_fusion_format_telemetry_json(telemetry,
                                             telemetry_json,
                                             sizeof(telemetry_json)) == ESP_OK) {
            ESP_LOGI(TAG, "CSI_FUSION_TELEMETRY %s", telemetry_json);
        }
    }

    if (fact == NULL || !fact->valid) {
        return ESP_OK;
    }

    sensor_aggregator_result_t local_result = {0};
    esp_err_t ret = sensor_aggregator_handle_csi_fact(fact, telemetry, &local_result);
    if (result != NULL) {
        *result = local_result;
    }
    return ret;
}

void csi_placeholder_gateway_init(void)
{
    if (s_fusion_lock == NULL) {
        s_fusion_lock = xSemaphoreCreateMutex();
    }
    if (s_latest_lock == NULL) {
        s_latest_lock = xSemaphoreCreateMutex();
    }
    if (s_latest_lock != NULL) {
        xSemaphoreTake(s_latest_lock, portMAX_DELAY);
        reset_latest_cache();
        xSemaphoreGive(s_latest_lock);
    }
    csi_fusion_init();
    s_csi_worker_running = false;
    ESP_LOGI(TAG, "CSI canonical gateway initialized");
}

esp_err_t csi_placeholder_gateway_start(void)
{
    if (s_fusion_lock == NULL) {
        s_fusion_lock = xSemaphoreCreateMutex();
        if (s_fusion_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_latest_lock == NULL) {
        s_latest_lock = xSemaphoreCreateMutex();
        if (s_latest_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_csi_worker_running = true;
    return ESP_OK;
}

void csi_placeholder_gateway_stop(void)
{
    s_csi_worker_running = false;
}

bool csi_placeholder_gateway_is_running(void)
{
    return s_csi_worker_running;
}

void csi_placeholder_gateway_log_latest_diagnostics(void)
{
    int64_t timestamp_ms = now_ms();
    if (s_last_latest_log_ms != 0 &&
        timestamp_ms - s_last_latest_log_ms < CSI_LATEST_DIAGNOSTIC_LOG_INTERVAL_MS) {
        return;
    }
    s_last_latest_log_ms = timestamp_ms;

    csi_latest_snapshot_t snapshots[CSI_LATEST_LINK_COUNT];
    if (s_latest_lock != NULL) {
        xSemaphoreTake(s_latest_lock, portMAX_DELAY);
    }
    memcpy(snapshots, s_latest_links, sizeof(snapshots));
    if (s_latest_lock != NULL) {
        xSemaphoreGive(s_latest_lock);
    }

    for (size_t i = 0; i < CSI_LATEST_LINK_COUNT; ++i) {
        const csi_latest_snapshot_t *snapshot = &snapshots[i];
        int64_t age_ms = snapshot->valid && snapshot->updated_at_ms > 0ULL ?
                             timestamp_ms - (int64_t)snapshot->updated_at_ms :
                             -1;
        ESP_LOGI(TAG,
                 "CSI_LATEST link_id=%s state=%s motion_score=%.3f quality=%.3f rssi=%d sample_count=%lu updated_at_ms=%llu age_ms=%lld",
                 snapshot->link_id[0] != '\0' ? snapshot->link_id : s_required_links[i],
                 snapshot->valid ? snapshot->state : "-",
                 snapshot->valid ? (double)snapshot->motion_score : 0.0,
                 snapshot->valid ? (double)snapshot->quality : 0.0,
                 snapshot->valid ? snapshot->rssi : 0,
                 (unsigned long)(snapshot->valid ? snapshot->sample_count : 0U),
                 (unsigned long long)(snapshot->valid ? snapshot->updated_at_ms : 0ULL),
                 (long long)age_ms);
    }
}

esp_err_t csi_placeholder_gateway_send_triggers(void)
{
    if (!s_csi_worker_running || !gateway_config_get()->csi_trigger_enabled) {
        return ESP_OK;
    }

    const gateway_runtime_config_t *config = gateway_config_get();
    const char payload[] = "ping trigger csi";
    child_registry_entry_t entries[GATEWAY_CONFIG_MAX_CHILDREN];
    size_t count = child_registry_snapshot(entries, GATEWAY_CONFIG_MAX_CHILDREN);

    esp_err_t first_error = ESP_OK;
    for (size_t i = 0; i < count; ++i) {
        if (config->csi_trigger_target_device_id[0] != '\0' &&
            strcmp(entries[i].device_id, config->csi_trigger_target_device_id) != 0) {
            continue;
        }
        if (!child_registry_is_online(entries[i].device_id) || entries[i].peer_ip[0] == '\0') {
            continue;
        }

        esp_err_t ret = device_stream_gateway_enqueue_udp(entries[i].peer_ip,
                                                          config->csi_trigger_udp_port,
                                                          payload,
                                                          sizeof(payload) - 1U,
                                                          "csi_trigger");
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
        ESP_LOGD(TAG,
                 "CSI trigger queued target=%s peer_ip=%s ret=%s",
                 entries[i].device_id,
                 entries[i].peer_ip,
                 esp_err_to_name(ret));
    }
    return first_error;
}

esp_err_t csi_placeholder_gateway_flush_fusion(void)
{
    if (!s_csi_worker_running || !gateway_config_get()->csi_result_ingest_enabled) {
        return ESP_OK;
    }

    csi_fusion_fact_t fact = {0};
    csi_fusion_telemetry_t telemetry = {0};
    if (s_fusion_lock != NULL) {
        xSemaphoreTake(s_fusion_lock, portMAX_DELAY);
    }
    esp_err_t ret = csi_fusion_flush(&fact, &telemetry);
    if (s_fusion_lock != NULL) {
        xSemaphoreGive(s_fusion_lock);
    }
    if (ret != ESP_OK) {
        return ret;
    }

    sensor_aggregator_result_t result = {0};
    return publish_fusion_outputs(&fact, &telemetry, &result);
}

esp_err_t csi_placeholder_gateway_handle_result(const protocol_adapter_envelope_t *envelope)
{
    if (envelope == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!gateway_config_get()->csi_result_ingest_enabled) {
        ESP_LOGD(TAG,
                 "CSI canonical ingest reserved device_id=%s seq=%u; ingest disabled",
                 envelope->device_id,
                 (unsigned int)envelope->seq);
        return ESP_OK;
    }
    if (!s_csi_worker_running) {
        return ESP_ERR_INVALID_STATE;
    }

    csi_fusion_feature_t feature = {0};
    esp_err_t ret = feature_from_envelope(envelope, &feature);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "CSI canonical ingest rejected ret=%s", esp_err_to_name(ret));
        return ret;
    }

    csi_result_v2_metrics_t metrics = {0};
    (void)read_v2_metrics(envelope->payload, &metrics);
    return csi_placeholder_gateway_handle_feature_internal(&feature, &metrics);
}

static esp_err_t csi_placeholder_gateway_handle_feature_internal(const csi_fusion_feature_t *feature,
                                                                const csi_result_v2_metrics_t *metrics)
{
    if (feature == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_csi_worker_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!gateway_config_get()->csi_result_ingest_enabled) {
        ESP_LOGD(TAG,
                 "CSI canonical ingest reserved device_id=%s link=%s; ingest disabled",
                 feature->device_id,
                 feature->link_id);
        return ESP_OK;
    }
    if (!gateway_config_child_allowed(feature->device_id)) {
        ESP_LOGW(TAG,
                 "CSI canonical feature rejected device_id=%s link=%s reason=not_allowed",
                 feature->device_id,
                 feature->link_id);
        return ESP_ERR_NOT_ALLOWED;
    }

    update_latest_snapshot(feature, metrics);
    log_csi_result_v2(feature, metrics);

    csi_fusion_fact_t fact = {0};
    csi_fusion_telemetry_t telemetry = {0};
    if (s_fusion_lock != NULL) {
        xSemaphoreTake(s_fusion_lock, portMAX_DELAY);
    }
    esp_err_t ret = csi_fusion_update(feature, &fact, &telemetry);
    if (s_fusion_lock != NULL) {
        xSemaphoreGive(s_fusion_lock);
    }
    if (ret != ESP_OK) {
        return ret;
    }

    sensor_aggregator_result_t result = {0};
    ret = publish_fusion_outputs(&fact, &telemetry, &result);
    ESP_LOGD(TAG,
             "CSI canonical ingest accepted link=%s confidence=%.3f quality=%.3f tick_id=%llu event_valid=%d fused_state=%s forwarded=%d status=%d",
             feature->link_id,
             (double)feature->confidence,
             (double)feature->quality,
             (unsigned long long)fact.tick_id,
             fact.valid ? 1 : 0,
             csi_fusion_state_to_string(fact.fused_state),
             result.forwarded ? 1 : 0,
             result.server_status);
    return ret;
}

esp_err_t csi_placeholder_gateway_handle_feature(const csi_fusion_feature_t *feature)
{
    return csi_placeholder_gateway_handle_feature_internal(feature, NULL);
}

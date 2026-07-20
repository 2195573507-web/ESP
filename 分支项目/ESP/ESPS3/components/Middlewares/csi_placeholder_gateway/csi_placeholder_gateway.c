/**
 * @file csi_placeholder_gateway.c
 * @brief S3 CSI canonical 接入和融合边界。
 *
 * 本文件接收 C5 上报的轻量 CSI feature，维护 per-link latest 诊断缓存，并把合法
 * feature 交给 csi_fusion。这里不接受 raw CSI 或子载波级数据。
 */

#include "csi_placeholder_gateway.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
#include "resource_manager.h"
#include "s3_event_bus.h"
#include "s3_scheduler.h"
#include "sensor_aggregator.h"

static const char *TAG = "csi_gateway";

#define CSI_LATEST_DIAGNOSTIC_LOG_INTERVAL_MS 10000LL
#define CSI_RESULT_V2_LOG_INTERVAL_MS 1000LL
#define CSI_RX_TRACE_LOG_INTERVAL_MS 1000LL
#define CSI_ERROR_LOG_INTERVAL_MS 1000LL
#define CSI_LATEST_LINK_COUNT CSI_FUSION_LINK_COUNT

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
    float confidence;
    float quality;
    int rssi;
    uint32_t sample_count;
    uint64_t updated_at_ms;
    uint64_t child_timestamp_ms;
    csi_result_v2_metrics_t metrics;
} csi_latest_snapshot_t;

typedef struct {
    char key[CSI_FUSION_TEXT_LEN];
    int64_t last_log_ms;
} csi_log_rate_slot_t;

static SemaphoreHandle_t s_fusion_lock;
static SemaphoreHandle_t s_latest_lock;
static volatile bool s_csi_worker_running;
static bool s_peer_active[CSI_LATEST_LINK_COUNT];
static csi_latest_snapshot_t s_latest_links[CSI_LATEST_LINK_COUNT];
static int64_t s_last_latest_log_ms;
static csi_log_rate_slot_t s_result_v2_log_slots[CSI_LATEST_LINK_COUNT];
static csi_log_rate_slot_t s_csi_edge_rx_log_slots[CSI_LATEST_LINK_COUNT];
static csi_log_rate_slot_t s_csi_rx_trace_log_slots[CSI_LATEST_LINK_COUNT];
static csi_log_rate_slot_t s_csi_rx_error_log_slots[CSI_LATEST_LINK_COUNT];
static int64_t s_last_telemetry_log_ms;
static int64_t s_last_error_log_ms;
static bool s_last_telemetry_valid;
static csi_fusion_state_t s_last_telemetry_state;
static char s_last_telemetry_links[CSI_FUSION_TEXT_LEN * CSI_FUSION_LINK_COUNT];
static const char *const s_required_links[CSI_LATEST_LINK_COUNT] = {
    "S3_TO_C51",
    "S3_TO_C52",
};

static esp_err_t csi_placeholder_gateway_handle_feature_internal(const csi_fusion_feature_t *feature,
                                                                const csi_result_v2_metrics_t *metrics,
                                                                bool edge_log_requested);

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

static uint64_t fusion_tick_for_rx_time(uint64_t rx_time_ms)
{
    return rx_time_ms / (uint64_t)CSI_FUSION_TICK_MS;
}

static const char *fusion_ret_reason(esp_err_t ret)
{
    switch (ret) {
    case ESP_OK:
        return "accepted";
    case ESP_ERR_NOT_ALLOWED:
        return "not_allowed";
    case ESP_ERR_NOT_SUPPORTED:
        return "unsupported_link";
    case ESP_ERR_INVALID_STATE:
        return "ingest_disabled";
    case ESP_ERR_INVALID_ARG:
        return "invalid_or_stale_tick";
    default:
        return "fusion_error";
    }
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

static int peer_index_for_device_id(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return -1;
    }
    if (strcmp(device_id, "C51") == 0 ||
        strcmp(device_id, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51) == 0) {
        return 0;
    }
    if (strcmp(device_id, "C52") == 0 ||
        strcmp(device_id, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52) == 0) {
        return 1;
    }
    return -1;
}

static bool feature_matches_peer(const csi_fusion_feature_t *feature, int peer_index)
{
    if (feature == NULL || peer_index < 0 || peer_index >= (int)CSI_LATEST_LINK_COUNT) {
        return false;
    }
    const char *link_id = diagnostic_link_id(feature->link_id, NULL);
    return latest_link_index(link_id) == peer_index;
}

static bool feature_values_valid(const csi_fusion_feature_t *feature)
{
    return feature != NULL &&
           feature->device_id[0] != '\0' &&
           feature->link_id[0] != '\0' &&
           feature->motion_score >= 0.0f && feature->motion_score <= 1.0f &&
           feature->confidence >= 0.0f && feature->confidence <= 1.0f &&
           feature->quality >= 0.0f && feature->quality <= 1.0f;
}

static bool any_peer_active_locked(void)
{
    for (size_t i = 0; i < CSI_LATEST_LINK_COUNT; ++i) {
        if (s_peer_active[i]) {
            return true;
        }
    }
    return false;
}

static const char *device_id_for_link(const char *link_id)
{
    if (link_id == NULL) {
        return NULL;
    }
    if (strcmp(link_id, "S3_TO_C51") == 0) {
        return ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51;
    }
    if (strcmp(link_id, "S3_TO_C52") == 0) {
        return ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52;
    }
    return NULL;
}

static bool telemetry_sessions_active(const csi_fusion_telemetry_t *telemetry)
{
    if (telemetry == NULL || !telemetry->valid || telemetry->active_link_count == 0U) {
        return false;
    }
    for (uint8_t i = 0; i < telemetry->active_link_count && i < CSI_FUSION_LINK_COUNT; ++i) {
        const char *device_id = device_id_for_link(telemetry->links[i]);
        if (device_id == NULL || !resource_manager_is_active(device_id)) {
            return false;
        }
    }
    return true;
}

static const char *diagnostic_device_name(const csi_fusion_feature_t *feature)
{
    if (feature == NULL) {
        return "-";
    }
    if (strcmp(feature->device_id, "C51") == 0 ||
        strcmp(feature->device_id, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51) == 0 ||
        strcmp(feature->link_id, "S3_TO_C51") == 0) {
        return "C51";
    }
    if (strcmp(feature->device_id, "C52") == 0 ||
        strcmp(feature->device_id, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52) == 0 ||
        strcmp(feature->link_id, "S3_TO_C52") == 0) {
        return "C52";
    }
    return feature->device_id[0] != '\0' ? feature->device_id : "-";
}

static bool log_rate_slot_due(csi_log_rate_slot_t *slots,
                              size_t slot_count,
                              const char *link_id,
                              int64_t interval_ms,
                              int64_t timestamp_ms)
{
    if (slots == NULL || slot_count == 0U || link_id == NULL || link_id[0] == '\0') {
        return false;
    }

    csi_log_rate_slot_t *candidate = NULL;
    for (size_t i = 0; i < slot_count; ++i) {
        if (slots[i].key[0] == '\0' && candidate == NULL) {
            candidate = &slots[i];
            continue;
        }
        if (strcmp(slots[i].key, link_id) == 0) {
            candidate = &slots[i];
            break;
        }
    }
    if (candidate == NULL) {
        candidate = &slots[0];
    }
    if (candidate->key[0] == '\0' || strcmp(candidate->key, link_id) != 0) {
        strlcpy(candidate->key, link_id, sizeof(candidate->key));
        candidate->last_log_ms = 0;
    }

    if (candidate->last_log_ms != 0 &&
        timestamp_ms - candidate->last_log_ms < interval_ms) {
        return false;
    }
    candidate->last_log_ms = timestamp_ms;
    return true;
}

static bool payload_has_edge_fields(cJSON *payload)
{
    cJSON *state = cJSON_GetObjectItemCaseSensitive(payload, "state");
    if (!cJSON_IsString(state)) {
        state = cJSON_GetObjectItemCaseSensitive(payload, "state_hint");
    }
    cJSON *motion_score = cJSON_GetObjectItemCaseSensitive(payload, "motion_score");
    cJSON *confidence = cJSON_GetObjectItemCaseSensitive(payload, "confidence");
    return cJSON_IsString(state) && state->valuestring != NULL && state->valuestring[0] != '\0' &&
           cJSON_IsNumber(motion_score) && cJSON_IsNumber(confidence);
}

static void log_csi_edge_rx(const csi_fusion_feature_t *feature)
{
    if (feature == NULL) {
        return;
    }

    int64_t timestamp_ms = now_ms();
    const char *link_id = diagnostic_link_id(feature->link_id, feature->device_id);
    if (!log_rate_slot_due(s_csi_edge_rx_log_slots,
                           CSI_LATEST_LINK_COUNT,
                           link_id,
                           CSI_RX_TRACE_LOG_INTERVAL_MS,
                           timestamp_ms)) {
        return;
    }

    ESP_LOGD(TAG,
             "CSI_EDGE_RX device=%s local_hint=%s score=%.3f confidence=%.3f",
             diagnostic_device_name(feature),
             feature->has_state ? csi_fusion_state_to_string(feature->state) : "-",
             (double)feature->motion_score,
             (double)feature->confidence);
}

static bool error_log_due(void)
{
    int64_t timestamp_ms = now_ms();
    if (s_last_error_log_ms != 0 &&
        timestamp_ms - s_last_error_log_ms < CSI_ERROR_LOG_INTERVAL_MS) {
        return false;
    }
    s_last_error_log_ms = timestamp_ms;
    return true;
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
    const char *link_id = diagnostic_link_id(feature->link_id, feature->device_id);
    if (!log_rate_slot_due(s_result_v2_log_slots,
                           CSI_LATEST_LINK_COUNT,
                           link_id,
                           CSI_RESULT_V2_LOG_INTERVAL_MS,
                           timestamp_ms)) {
        return;
    }

    ESP_LOGD(TAG,
             "CSI_RESULT_V2 link_id=%s device=%s energy=%.3f variance=%.3f cv=%.3f rssi=%.1f quality=%.3f local_hint=%s",
             link_id,
             feature->device_id,
             metrics->frame_energy,
             metrics->variance,
             metrics->cv,
             metrics->rssi,
             metrics->quality,
             feature->has_state ? csi_fusion_state_to_string(feature->state) : "-");
}

static void reset_latest_link_locked(size_t index)
{
    if (index >= CSI_LATEST_LINK_COUNT) {
        return;
    }
    memset(&s_latest_links[index], 0, sizeof(s_latest_links[index]));
    strlcpy(s_latest_links[index].link_id,
            s_required_links[index],
            sizeof(s_latest_links[index].link_id));
    strlcpy(s_latest_links[index].state, "-", sizeof(s_latest_links[index].state));
}

static void reset_latest_cache(void)
{
    for (size_t i = 0; i < CSI_LATEST_LINK_COUNT; ++i) {
        reset_latest_link_locked(i);
    }
}

static void reset_log_slot_for_link(csi_log_rate_slot_t *slots, const char *link_id)
{
    if (slots == NULL || link_id == NULL) {
        return;
    }
    for (size_t i = 0; i < CSI_LATEST_LINK_COUNT; ++i) {
        if (strcmp(slots[i].key, link_id) == 0) {
            memset(&slots[i], 0, sizeof(slots[i]));
        }
    }
}

static void reset_peer_log_state_locked(size_t index)
{
    if (index >= CSI_LATEST_LINK_COUNT) {
        return;
    }
    reset_log_slot_for_link(s_result_v2_log_slots, s_required_links[index]);
    reset_log_slot_for_link(s_csi_edge_rx_log_slots, s_required_links[index]);
    reset_log_slot_for_link(s_csi_rx_trace_log_slots, s_required_links[index]);
    reset_log_slot_for_link(s_csi_rx_error_log_slots, s_required_links[index]);
    s_last_latest_log_ms = 0;
    s_last_telemetry_log_ms = 0;
    s_last_telemetry_valid = false;
    s_last_telemetry_links[0] = '\0';
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
    slot->motion_score = feature->motion_score;
    slot->confidence = feature->confidence;
    slot->quality = feature->quality;
    slot->rssi = feature->rssi;
    if (slot->sample_count < UINT32_MAX) {
        ++slot->sample_count;
    }
    slot->updated_at_ms = feature->timestamp_ms > 0ULL ? feature->timestamp_ms : (uint64_t)now_ms();
    slot->child_timestamp_ms = feature->child_timestamp_ms;
    if (metrics != NULL && metrics->valid) {
        slot->metrics = *metrics;
    }
    if (s_latest_lock != NULL) {
        xSemaphoreGive(s_latest_lock);
    }
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

static bool has_forbidden_csi_field(cJSON *payload)
{
    if (payload == NULL) {
        return false;
    }

    for (cJSON *item = payload->child; item != NULL; item = item->next) {
        if (csi_signal_key_forbidden(item->string)) {
            return true;
        }
        if ((cJSON_IsObject(item) || cJSON_IsArray(item)) &&
            has_forbidden_csi_field(item)) {
            return true;
        }
    }
    return false;
}

static esp_err_t feature_from_envelope(const protocol_adapter_envelope_t *envelope,
                                       uint64_t rx_time_ms,
                                       csi_fusion_feature_t *feature)
{
    if (envelope == NULL || envelope->payload == NULL || feature == NULL ||
        rx_time_ms == 0ULL ||
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
    const char *local_hint = json_string(envelope->payload,
                                         "state",
                                         json_string(envelope->payload, "state_hint", ""));
    if (csi_fusion_state_from_string(local_hint, &input_state)) {
        feature->has_state = true;
        feature->state = input_state;
    }

    cJSON *metrics = cJSON_GetObjectItemCaseSensitive(envelope->payload, "metrics");
    double metric_quality = json_number(metrics, "quality", -1.0);
    double metric_rssi = json_number(metrics, "rssi", 0.0);
    feature->motion_score = (float)json_number(envelope->payload,
                                               "motion_score",
                                               json_number(envelope->payload,
                                                           "confidence",
                                                           metric_quality));
    feature->confidence = (float)json_number(envelope->payload, "confidence", metric_quality);
    feature->quality = (float)json_number(envelope->payload, "quality", metric_quality);
    feature->rssi = (int)json_number(envelope->payload, "rssi", metric_rssi);
    if (metrics != NULL && cJSON_IsObject(metrics)) {
        csi_result_v2_metrics_t parsed_metrics = {0};
        if (read_v2_metrics(envelope->payload, &parsed_metrics)) {
            feature->has_metrics = true;
            feature->energy = (float)parsed_metrics.frame_energy;
            feature->variance = (float)parsed_metrics.variance;
            feature->cv = (float)parsed_metrics.cv;
        }
    }
    feature->frame_seq = json_u32(envelope->payload, "frame_seq", 0U);
    feature->child_timestamp_ms =
        json_u64(envelope->payload,
                 "timestamp_ms",
                 json_u64(envelope->payload,
                          "timestamp",
                          json_u64(envelope->payload,
                                   "updated_at_ms",
                                   envelope->timestamp_ms > 0 ? (uint64_t)envelope->timestamp_ms : 0ULL)));
    feature->timestamp_ms = rx_time_ms;
    feature->tick_id = fusion_tick_for_rx_time(rx_time_ms);

    if (feature->link_id[0] == '\0' ||
        feature->confidence < 0.0f || feature->confidence > 1.0f ||
        feature->motion_score < 0.0f || feature->motion_score > 1.0f ||
        feature->quality < 0.0f || feature->quality > 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void log_csi_rx_trace(const csi_fusion_feature_t *feature,
                             esp_err_t ret,
                             const char *reason)
{
    if (feature == NULL) {
        return;
    }
    int64_t timestamp_ms = now_ms();
    const char *link_id = diagnostic_link_id(feature->link_id, feature->device_id);
    csi_log_rate_slot_t *slots = ret == ESP_OK ? s_csi_rx_trace_log_slots :
                                                  s_csi_rx_error_log_slots;
    if (!log_rate_slot_due(slots,
                           CSI_LATEST_LINK_COUNT,
                           link_id,
                           CSI_RX_TRACE_LOG_INTERVAL_MS,
                           timestamp_ms)) {
        return;
    }

    if (ret == ESP_OK) {
        ESP_LOGD(TAG,
                 "CSI_RX did=%s id=%s lid=%s link_id=%s child_ts=%llu rx_time_ms=%llu fusion_tick=%llu ret=%s reason=%s",
                 feature->device_id,
                 feature->device_id,
                 feature->link_id,
                 link_id,
                 (unsigned long long)feature->child_timestamp_ms,
                 (unsigned long long)feature->timestamp_ms,
                 (unsigned long long)feature->tick_id,
                 esp_err_to_name(ret),
                 reason != NULL ? reason : fusion_ret_reason(ret));
    } else {
        ESP_LOGW(TAG,
                 "CSI_RX did=%s id=%s lid=%s link_id=%s child_ts=%llu rx_time_ms=%llu fusion_tick=%llu ret=%s reason=%s",
                 feature->device_id,
                 feature->device_id,
                 feature->link_id,
                 link_id,
                 (unsigned long long)feature->child_timestamp_ms,
                 (unsigned long long)feature->timestamp_ms,
                 (unsigned long long)feature->tick_id,
                 esp_err_to_name(ret),
                 reason != NULL ? reason : fusion_ret_reason(ret));
    }
}

static void telemetry_links_signature(const csi_fusion_telemetry_t *telemetry,
                                      char *out,
                                      size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    out[0] = '\0';
    if (telemetry == NULL || !telemetry->valid) {
        return;
    }

    size_t used = 0U;
    for (uint8_t i = 0; i < telemetry->active_link_count && i < CSI_FUSION_LINK_COUNT; ++i) {
        int written = snprintf(out + used,
                               out_size - used,
                               "%s%s",
                               i == 0U ? "" : ",",
                               telemetry->links[i]);
        if (written <= 0 || (size_t)written >= out_size - used) {
            out[out_size - 1U] = '\0';
            return;
        }
        used += (size_t)written;
    }
}

static bool telemetry_log_due(const csi_fusion_telemetry_t *telemetry)
{
    if (!telemetry_sessions_active(telemetry)) {
        return false;
    }

    char links[CSI_FUSION_TEXT_LEN * CSI_FUSION_LINK_COUNT] = {0};
    telemetry_links_signature(telemetry, links, sizeof(links));

    bool state_changed = !s_last_telemetry_valid ||
                         telemetry->fused_state != s_last_telemetry_state;
    const int64_t timestamp_ms = now_ms();
    bool due = state_changed || s_last_telemetry_log_ms == 0 ||
               timestamp_ms - s_last_telemetry_log_ms >= CSI_RESULT_V2_LOG_INTERVAL_MS;
    if (!due) {
        return false;
    }

    s_last_telemetry_log_ms = timestamp_ms;
    s_last_telemetry_valid = true;
    s_last_telemetry_state = telemetry->fused_state;
    strlcpy(s_last_telemetry_links, links, sizeof(s_last_telemetry_links));
    return true;
}

static esp_err_t publish_fusion_outputs(const csi_fusion_fact_t *fact,
                                        const csi_fusion_telemetry_t *telemetry,
                                        sensor_aggregator_result_t *result)
{
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }

    const bool sessions_active = telemetry_sessions_active(telemetry);
    if (sessions_active && telemetry_log_due(telemetry)) {
        char links[CSI_FUSION_TEXT_LEN * CSI_FUSION_LINK_COUNT] = {0};
        telemetry_links_signature(telemetry, links, sizeof(links));
        ESP_LOGI(TAG,
                 "CSI_FUSION_EDGE links=%s state=%s confidence=%.3f motion_score=%.3f",
                 links[0] != '\0' ? links : "-",
                 csi_fusion_state_to_string(telemetry->fused_state),
                 (double)telemetry->confidence,
                 (double)telemetry->motion_score);

        char telemetry_json[384];
        if (csi_fusion_format_telemetry_json(telemetry,
                                             telemetry_json,
                                             sizeof(telemetry_json)) == ESP_OK) {
            ESP_LOGI(TAG, "CSI_FUSION_TELEMETRY %s", telemetry_json);
        }
    }

    if (fact == NULL || !fact->valid || !sessions_active) {
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
    memset(s_peer_active, 0, sizeof(s_peer_active));
    memset(s_result_v2_log_slots, 0, sizeof(s_result_v2_log_slots));
    memset(s_csi_edge_rx_log_slots, 0, sizeof(s_csi_edge_rx_log_slots));
    memset(s_csi_rx_trace_log_slots, 0, sizeof(s_csi_rx_trace_log_slots));
    memset(s_csi_rx_error_log_slots, 0, sizeof(s_csi_rx_error_log_slots));
    s_last_latest_log_ms = 0;
    s_last_telemetry_log_ms = 0;
    s_last_error_log_ms = 0;
    s_last_telemetry_valid = false;
    s_last_telemetry_links[0] = '\0';
    csi_fusion_init();
    s_csi_worker_running = false;
    ESP_LOGI(TAG, "CSI canonical gateway initialized");
    ESP_LOGI(TAG, "C51_TO_C52 disabled reason=not_implemented");
    ESP_LOGI(TAG, "C52_TO_C51 disabled reason=not_implemented");
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

esp_err_t csi_gateway_suspend_peer(const char *device_id)
{
    return csi_gateway_suspend_peer_at_us(device_id, INT64_MAX);
}

esp_err_t csi_gateway_suspend_peer_at_us(const char *device_id, int64_t cutoff_us)
{
    int peer_index = peer_index_for_device_id(device_id);
    if (peer_index < 0 || peer_index >= (int)CSI_LATEST_LINK_COUNT) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_fusion_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_fusion_lock, portMAX_DELAY);
    s_peer_active[peer_index] = false;
    if (s_latest_lock != NULL) {
        xSemaphoreTake(s_latest_lock, portMAX_DELAY);
        reset_latest_link_locked((size_t)peer_index);
        xSemaphoreGive(s_latest_lock);
    }
    reset_peer_log_state_locked((size_t)peer_index);
    esp_err_t ret = csi_fusion_suspend_link(device_id);
    xSemaphoreGive(s_fusion_lock);

    s3_event_bus_clear_csi_before(device_id, cutoff_us);
    s3_scheduler_clear_csi_peer_before(device_id, cutoff_us, "resource_release");
    return ret;
}

esp_err_t csi_gateway_restore_peer(const char *device_id)
{
    int peer_index = peer_index_for_device_id(device_id);
    if (peer_index < 0 || peer_index >= (int)CSI_LATEST_LINK_COUNT) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_fusion_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_fusion_lock, portMAX_DELAY);
    bool already_active = s_peer_active[peer_index];
    esp_err_t ret = csi_fusion_restore_link(device_id);
    if (ret == ESP_OK) {
        if (!already_active) {
            if (s_latest_lock != NULL) {
                xSemaphoreTake(s_latest_lock, portMAX_DELAY);
                reset_latest_link_locked((size_t)peer_index);
                xSemaphoreGive(s_latest_lock);
            }
            reset_peer_log_state_locked((size_t)peer_index);
        }
        s_peer_active[peer_index] = true;
    }
    xSemaphoreGive(s_fusion_lock);
    return ret;
}

void csi_placeholder_gateway_log_latest_diagnostics(void)
{
    if (!s_csi_worker_running || s_fusion_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_fusion_lock, portMAX_DELAY);
    if (!any_peer_active_locked()) {
        xSemaphoreGive(s_fusion_lock);
        return;
    }

    int64_t timestamp_ms = now_ms();
    if (s_last_latest_log_ms != 0 &&
        timestamp_ms - s_last_latest_log_ms < CSI_LATEST_DIAGNOSTIC_LOG_INTERVAL_MS) {
        xSemaphoreGive(s_fusion_lock);
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
        const char *device_id = device_id_for_link(s_required_links[i]);
        if (!s_peer_active[i] || device_id == NULL ||
            !resource_manager_is_active(device_id)) {
            continue;
        }
        const csi_latest_snapshot_t *snapshot = &snapshots[i];
        int64_t age_ms = snapshot->valid && snapshot->updated_at_ms > 0ULL ?
                             timestamp_ms - (int64_t)snapshot->updated_at_ms :
                             -1;
        if (age_ms < 0) {
            age_ms = 0;
        }
        ESP_LOGD(TAG,
                 "CSI_LATEST link_id=%s local_hint=%s motion_score=%.3f confidence=%.3f quality=%.3f rssi=%d energy=%.3f variance=%.3f cv=%.3f sample_count=%lu child_ts=%llu updated_at_ms=%llu age_ms=%lld",
                 snapshot->link_id[0] != '\0' ? snapshot->link_id : s_required_links[i],
                 snapshot->valid ? snapshot->state : "-",
                 snapshot->valid ? (double)snapshot->motion_score : 0.0,
                 snapshot->valid ? (double)snapshot->confidence : 0.0,
                 snapshot->valid ? (double)snapshot->quality : 0.0,
                 snapshot->valid ? snapshot->rssi : 0,
                 snapshot->valid && snapshot->metrics.valid ? snapshot->metrics.frame_energy : 0.0,
                 snapshot->valid && snapshot->metrics.valid ? snapshot->metrics.variance : 0.0,
                 snapshot->valid && snapshot->metrics.valid ? snapshot->metrics.cv : 0.0,
                 (unsigned long)(snapshot->valid ? snapshot->sample_count : 0U),
                 (unsigned long long)(snapshot->valid ? snapshot->child_timestamp_ms : 0ULL),
                 (unsigned long long)(snapshot->valid ? snapshot->updated_at_ms : 0ULL),
                 (long long)age_ms);
    }
    xSemaphoreGive(s_fusion_lock);
}

esp_err_t csi_placeholder_gateway_send_triggers(void)
{
    if (!s_csi_worker_running || !gateway_config_get()->csi_trigger_enabled) {
        return ESP_OK;
    }
    if (s_fusion_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_fusion_lock, portMAX_DELAY);
    if (!any_peer_active_locked()) {
        xSemaphoreGive(s_fusion_lock);
        return ESP_OK;
    }

    const gateway_runtime_config_t *config = gateway_config_get();
    const char payload[] = "ping trigger csi";
    child_registry_entry_t entries[GATEWAY_CONFIG_MAX_CHILDREN];
    size_t count = child_registry_snapshot(entries, GATEWAY_CONFIG_MAX_CHILDREN);

    esp_err_t first_error = ESP_OK;
    for (size_t i = 0; i < count; ++i) {
        int peer_index = peer_index_for_device_id(entries[i].device_id);
        if (peer_index < 0 || peer_index >= (int)CSI_LATEST_LINK_COUNT ||
            !s_peer_active[peer_index]) {
            continue;
        }
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
    xSemaphoreGive(s_fusion_lock);
    return first_error;
}

esp_err_t csi_placeholder_gateway_flush_fusion(void)
{
    if (!s_csi_worker_running || !gateway_config_get()->csi_result_ingest_enabled) {
        return ESP_OK;
    }

    csi_fusion_fact_t fact = {0};
    csi_fusion_telemetry_t telemetry = {0};
    if (s_fusion_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_fusion_lock, portMAX_DELAY);
    if (!any_peer_active_locked()) {
        xSemaphoreGive(s_fusion_lock);
        return ESP_OK;
    }
    esp_err_t ret = csi_fusion_flush(&fact, &telemetry);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_fusion_lock);
        return ret;
    }

    sensor_aggregator_result_t result = {0};
    ret = publish_fusion_outputs(&fact, &telemetry, &result);
    xSemaphoreGive(s_fusion_lock);
    return ret;
}

esp_err_t csi_placeholder_gateway_handle_result(const protocol_adapter_envelope_t *envelope)
{
    return csi_placeholder_gateway_handle_result_from_peer_at_us(envelope,
                                                                 NULL,
                                                                 esp_timer_get_time());
}

esp_err_t csi_placeholder_gateway_handle_result_at(const protocol_adapter_envelope_t *envelope,
                                                   int64_t rx_time_ms)
{
    return csi_placeholder_gateway_handle_result_from_peer(envelope, NULL, rx_time_ms);
}

esp_err_t csi_placeholder_gateway_handle_result_from_peer(
    const protocol_adapter_envelope_t *envelope,
    const char *peer_ip,
    int64_t rx_time_ms)
{
    return csi_placeholder_gateway_handle_result_from_peer_at_us(
        envelope,
        peer_ip,
        rx_time_ms > 0 ? rx_time_ms * 1000 : 0);
}

esp_err_t csi_placeholder_gateway_handle_result_from_peer_at_us(
    const protocol_adapter_envelope_t *envelope,
    const char *peer_ip,
    int64_t rx_time_us)
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
    const int64_t effective_rx_time_us = rx_time_us > 0 ? rx_time_us : esp_timer_get_time();
    uint64_t effective_rx_time_ms = (uint64_t)(effective_rx_time_us / 1000);
    esp_err_t ret = feature_from_envelope(envelope, effective_rx_time_ms, &feature);
    if (ret != ESP_OK) {
        if (error_log_due()) {
            ESP_LOGW(TAG, "CSI canonical ingest rejected ret=%s", esp_err_to_name(ret));
        }
        return ret;
    }
    if (strcmp(envelope->device_id, feature.device_id) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int peer_index = peer_index_for_device_id(feature.device_id);
    if (!gateway_config_child_allowed(feature.device_id)) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (peer_index < 0 || !feature_matches_peer(&feature, peer_index)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    ret = resource_manager_confirm_peer_at_us(envelope->device_id,
                                              peer_ip,
                                              RESOURCE_MANAGER_SIGNAL_CSI,
                                              effective_rx_time_us);
    if (ret != ESP_OK) {
        return ret;
    }

    csi_result_v2_metrics_t metrics = {0};
    (void)read_v2_metrics(envelope->payload, &metrics);
    if (metrics.valid) {
        feature.has_metrics = true;
        feature.energy = (float)metrics.frame_energy;
        feature.variance = (float)metrics.variance;
        feature.cv = (float)metrics.cv;
    }
    return csi_placeholder_gateway_handle_feature_internal(&feature,
                                                           &metrics,
                                                           payload_has_edge_fields(envelope->payload));
}

static esp_err_t csi_placeholder_gateway_handle_feature_internal(const csi_fusion_feature_t *feature,
                                                                const csi_result_v2_metrics_t *metrics,
                                                                bool edge_log_requested)
{
    if (!feature_values_valid(feature)) {
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
        return ESP_ERR_NOT_ALLOWED;
    }

    int peer_index = peer_index_for_device_id(feature->device_id);
    if (peer_index < 0 || !feature_matches_peer(feature, peer_index)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_fusion_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    csi_fusion_fact_t fact = {0};
    csi_fusion_telemetry_t telemetry = {0};
    xSemaphoreTake(s_fusion_lock, portMAX_DELAY);
    if (!s_peer_active[peer_index] || !resource_manager_is_live(feature->device_id)) {
        xSemaphoreGive(s_fusion_lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (edge_log_requested) {
        log_csi_edge_rx(feature);
    }
    update_latest_snapshot(feature, metrics);
    log_csi_result_v2(feature, metrics);

    esp_err_t ret = csi_fusion_update(feature, &fact, &telemetry);
    log_csi_rx_trace(feature, ret, fusion_ret_reason(ret));
    if (ret != ESP_OK) {
        xSemaphoreGive(s_fusion_lock);
        return ret;
    }

    const bool warmup_complete = csi_fusion_link_warmup_complete(feature->device_id);
    sensor_aggregator_result_t result = {0};
    ret = publish_fusion_outputs(&fact, &telemetry, &result);
    ESP_LOGD(TAG,
             "CSI canonical ingest accepted link=%s local_hint=%s confidence=%.3f quality=%.3f tick_id=%llu event_valid=%d fused_state=%s forwarded=%d status=%d",
             feature->link_id,
             feature->has_state ? csi_fusion_state_to_string(feature->state) : "-",
             (double)feature->confidence,
             (double)feature->quality,
             (unsigned long long)fact.tick_id,
             fact.valid ? 1 : 0,
             csi_fusion_state_to_string(fact.fused_state),
             result.forwarded ? 1 : 0,
             result.server_status);
    xSemaphoreGive(s_fusion_lock);

    if (warmup_complete) {
        esp_err_t complete_ret = resource_manager_complete_restore(feature->device_id,
                                                                   "csi_warmup_complete");
        if (complete_ret != ESP_OK && complete_ret != ESP_ERR_INVALID_STATE) {
            return complete_ret;
        }
        if (complete_ret == ESP_ERR_INVALID_STATE &&
            !resource_manager_is_active(feature->device_id)) {
            return complete_ret;
        }
    }
    return ret;
}

esp_err_t csi_placeholder_gateway_handle_feature(const csi_fusion_feature_t *feature)
{
    if (!feature_values_valid(feature)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_csi_worker_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!gateway_config_get()->csi_result_ingest_enabled) {
        return ESP_OK;
    }
    if (!gateway_config_child_allowed(feature->device_id)) {
        return ESP_ERR_NOT_ALLOWED;
    }

    int peer_index = peer_index_for_device_id(feature->device_id);
    if (peer_index < 0 || !feature_matches_peer(feature, peer_index)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    int64_t observed_at_ms = feature->timestamp_ms > 0ULL ?
                                 (int64_t)feature->timestamp_ms :
                                 now_ms();
    esp_err_t ret = resource_manager_confirm_peer(feature->device_id,
                                                  NULL,
                                                  RESOURCE_MANAGER_SIGNAL_CSI,
                                                  observed_at_ms);
    if (ret != ESP_OK) {
        return ret;
    }
    return csi_placeholder_gateway_handle_feature_internal(feature, NULL, false);
}

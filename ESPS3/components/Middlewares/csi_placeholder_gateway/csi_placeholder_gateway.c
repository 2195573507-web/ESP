/**
 * @file csi_placeholder_gateway.c
 * @brief S3 CSI feature ingress and fusion boundary.
 *
 * C5 terminals upload low-dimensional feature frames. This module rejects raw CSI
 * semantics, updates the S3 fusion state machine, and forwards only canonical
 * S3-owned facts to ESP-server.
 */

#include "csi_placeholder_gateway.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "child_registry.h"
#include "csi_fusion.h"
#include "esp111_protocol_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gateway_config.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "sensor_aggregator.h"

static const char *TAG = "csi_feature_gateway";

#ifndef CSI_LATEST_DIAGNOSTIC_LOG_INTERVAL_MS
#define CSI_LATEST_DIAGNOSTIC_LOG_INTERVAL_MS 10000U
#endif

static TaskHandle_t s_trigger_task;
static SemaphoreHandle_t s_latest_lock;
static uint64_t s_last_diagnostic_log_ms;

typedef struct {
    bool valid;
    char link_id[32];
    char device_id[32];
    float frame_energy;
    float variance;
    float quality;
    int rssi;
    uint32_t frame_seq;
    uint64_t timestamp_ms;
    uint64_t received_at_ms;
} csi_link_latest_t;

static csi_link_latest_t s_latest_links[] = {
    {.link_id = "S3_TO_C51"},
    {.link_id = "S3_TO_C52"},
    {.link_id = "C51_TO_C52"},
    {.link_id = "C52_TO_C51"},
};

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

static int json_int(cJSON *root, const char *key, int fallback)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(value) ? value->valueint : fallback;
}

static uint32_t json_u32(cJSON *root, const char *key, uint32_t fallback)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(value) || value->valuedouble < 0.0) {
        return fallback;
    }
    return (uint32_t)value->valuedouble;
}

static bool has_forbidden_raw_field(cJSON *payload)
{
    return cJSON_GetObjectItemCaseSensitive(payload, "raw_csi") != NULL ||
           cJSON_GetObjectItemCaseSensitive(payload, "subcarrier_data") != NULL ||
           cJSON_GetObjectItemCaseSensitive(payload, "selected_subcarriers") != NULL ||
           cJSON_GetObjectItemCaseSensitive(payload, "iq") != NULL ||
           cJSON_GetObjectItemCaseSensitive(payload, "phase") != NULL;
}

static csi_link_latest_t *find_latest_slot_locked(const char *link_id)
{
    if (link_id == NULL || link_id[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(s_latest_links) / sizeof(s_latest_links[0]); ++i) {
        if (strcmp(s_latest_links[i].link_id, link_id) == 0) {
            return &s_latest_links[i];
        }
    }
    return NULL;
}

static size_t latest_link_count(void)
{
    return sizeof(s_latest_links) / sizeof(s_latest_links[0]);
}

static esp_err_t feature_from_envelope(const protocol_adapter_envelope_t *envelope,
                                       csi_fusion_feature_t *feature)
{
    if (envelope == NULL || envelope->payload == NULL || feature == NULL ||
        has_forbidden_raw_field(envelope->payload)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(feature, 0, sizeof(*feature));
    strlcpy(feature->device_id,
            json_string(envelope->payload, "device_id", envelope->device_id),
            sizeof(feature->device_id));
    strlcpy(feature->link_id,
            json_string(envelope->payload, "link_id", "unknown"),
            sizeof(feature->link_id));
    feature->frame_energy = (float)json_number(envelope->payload, "frame_energy", -1.0);
    feature->variance = (float)json_number(envelope->payload, "variance", -1.0);
    feature->quality = (float)json_number(envelope->payload, "quality", -1.0);
    feature->rssi = json_int(envelope->payload, "rssi", 0);
    feature->frame_seq = json_u32(envelope->payload, "frame_seq", 0U);
    feature->timestamp_ms = (uint64_t)json_number(envelope->payload,
                                                  "timestamp",
                                                  json_number(envelope->payload,
                                                              "updated_at_ms",
                                                              (double)now_ms()));

    if (feature->link_id[0] == '\0' || strcmp(feature->link_id, "unknown") == 0 ||
        feature->frame_energy < 0.0f || feature->variance < 0.0f ||
        feature->quality < 0.0f || feature->quality > 1.0f ||
        cJSON_GetObjectItemCaseSensitive(envelope->payload, "frame_seq") == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void update_latest_feature(const csi_fusion_feature_t *feature,
                                  const protocol_adapter_envelope_t *envelope)
{
    if (feature == NULL || envelope == NULL || envelope->payload == NULL) {
        return;
    }

    uint64_t received_at_ms = (uint64_t)now_ms();

    if (s_latest_lock != NULL) {
        xSemaphoreTake(s_latest_lock, portMAX_DELAY);
    }
    csi_link_latest_t *slot = find_latest_slot_locked(feature->link_id);
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
        slot->valid = true;
        strlcpy(slot->link_id, feature->link_id, sizeof(slot->link_id));
        strlcpy(slot->device_id, feature->device_id, sizeof(slot->device_id));
        slot->frame_energy = feature->frame_energy;
        slot->variance = feature->variance;
        slot->quality = feature->quality;
        slot->rssi = feature->rssi;
        slot->frame_seq = feature->frame_seq;
        slot->timestamp_ms = feature->timestamp_ms;
        slot->received_at_ms = received_at_ms;
    }
    if (s_latest_lock != NULL) {
        xSemaphoreGive(s_latest_lock);
    }
}

void csi_placeholder_gateway_log_latest_diagnostics(void)
{
    const uint64_t timestamp_ms = (uint64_t)now_ms();
    if (s_last_diagnostic_log_ms != 0U &&
        timestamp_ms - s_last_diagnostic_log_ms < CSI_LATEST_DIAGNOSTIC_LOG_INTERVAL_MS) {
        return;
    }
    s_last_diagnostic_log_ms = timestamp_ms;

    csi_link_latest_t snapshot[sizeof(s_latest_links) / sizeof(s_latest_links[0])];
    if (s_latest_lock != NULL) {
        xSemaphoreTake(s_latest_lock, portMAX_DELAY);
    }
    memcpy(snapshot, s_latest_links, sizeof(snapshot));
    if (s_latest_lock != NULL) {
        xSemaphoreGive(s_latest_lock);
    }

    for (size_t i = 0; i < latest_link_count(); ++i) {
        const csi_link_latest_t *slot = &snapshot[i];
        const uint64_t age_ms =
            slot->valid && timestamp_ms >= slot->received_at_ms ? timestamp_ms - slot->received_at_ms : 0U;
        ESP_LOGI(TAG,
                 "CSI_FEATURE_LATEST link_id=%s energy=%.3f variance=%.5f quality=%.5f rssi=%d frame_seq=%u timestamp_ms=%llu age_ms=%llu",
                 slot->link_id,
                 slot->valid ? slot->frame_energy : 0.0f,
                 slot->valid ? slot->variance : 0.0f,
                 slot->valid ? slot->quality : 0.0f,
                 slot->valid ? slot->rssi : 0,
                 (unsigned int)(slot->valid ? slot->frame_seq : 0U),
                 (unsigned long long)(slot->valid ? slot->timestamp_ms : 0U),
                 (unsigned long long)age_ms);
    }
}

static void csi_trigger_task(void *arg)
{
    (void)arg;
    const gateway_runtime_config_t *config = gateway_config_get();
    const char payload[] = "ping trigger csi";

    ESP_LOGI(TAG,
             "csi_trigger ping trigger task started interval_ms=%u udp_port=%u target=%s",
             (unsigned int)config->csi_trigger_interval_ms,
             (unsigned int)config->csi_trigger_udp_port,
             config->csi_trigger_target_device_id[0] != '\0' ? config->csi_trigger_target_device_id : "all");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(config->csi_trigger_interval_ms));

        child_registry_entry_t entries[GATEWAY_CONFIG_MAX_CHILDREN];
        size_t count = child_registry_snapshot(entries, GATEWAY_CONFIG_MAX_CHILDREN);
        for (size_t i = 0; i < count; ++i) {
            if (config->csi_trigger_target_device_id[0] != '\0' &&
                strcmp(entries[i].device_id, config->csi_trigger_target_device_id) != 0) {
                continue;
            }
            if (!child_registry_is_online(entries[i].device_id) || entries[i].peer_ip[0] == '\0') {
                continue;
            }

            int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            if (sock < 0) {
                ESP_LOGD(TAG, "CSI trigger socket open failed");
                continue;
            }

            struct sockaddr_in dest;
            memset(&dest, 0, sizeof(dest));
            dest.sin_family = AF_INET;
            dest.sin_port = htons(config->csi_trigger_udp_port);
            if (inet_pton(AF_INET, entries[i].peer_ip, &dest.sin_addr) != 1) {
                close(sock);
                continue;
            }

            int sent = sendto(sock,
                              payload,
                              sizeof(payload) - 1U,
                              0,
                              (const struct sockaddr *)&dest,
                              sizeof(dest));
            close(sock);
            ESP_LOGD(TAG,
                     "csi_trigger ping trigger target device_id=%s peer_ip=%s sent=%d",
                     entries[i].device_id,
                     entries[i].peer_ip,
                     sent);
        }
    }
}

void csi_placeholder_gateway_init(void)
{
    const gateway_runtime_config_t *config = gateway_config_get();
    if (s_latest_lock == NULL) {
        s_latest_lock = xSemaphoreCreateMutex();
    }
    csi_fusion_init();
    if (!config->csi_trigger_enabled) {
        ESP_LOGI(TAG, "CSI feature gateway initialized; trigger disabled");
        return;
    }

    if (s_trigger_task == NULL) {
        BaseType_t created = xTaskCreate(csi_trigger_task,
                                         "csi_trigger",
                                         4096U,
                                         NULL,
                                         2U,
                                         &s_trigger_task);
        if (created != pdPASS) {
            s_trigger_task = NULL;
            ESP_LOGW(TAG, "create CSI trigger task failed");
        }
    }
}

esp_err_t csi_placeholder_gateway_handle_result(const protocol_adapter_envelope_t *envelope)
{
    if (envelope == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!gateway_config_get()->csi_result_ingest_enabled) {
        ESP_LOGD(TAG,
                 "CSI feature reserved device_id=%s link=%s seq=%u; ingest disabled",
                 envelope->device_id,
                 envelope->payload != NULL ? json_string(envelope->payload, "link_id", "unknown") : "unknown",
                 (unsigned int)envelope->seq);
        return ESP_OK;
    }

    csi_fusion_feature_t feature = {0};
    esp_err_t ret = feature_from_envelope(envelope, &feature);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "CSI feature rejected raw_or_invalid=1 ret=%s", esp_err_to_name(ret));
        return ret;
    }

    update_latest_feature(&feature, envelope);
    csi_fusion_fact_t fact = {0};
    ret = csi_fusion_update(&feature, &fact);
    if (ret != ESP_OK) {
        return ret;
    }

    sensor_aggregator_result_t result = {0};
    ret = sensor_aggregator_handle_csi_fact(&fact, &result);
    ESP_LOGD(TAG,
             "CSI feature accepted link=%s energy=%.3f variance=%.5f quality=%.5f seq=%u fused_state=%s fused_score=%.3f forwarded=%d status=%d raw_csi=unsupported",
             feature.link_id,
             (double)feature.frame_energy,
             (double)feature.variance,
             (double)feature.quality,
             (unsigned int)feature.frame_seq,
             csi_fusion_state_to_string(fact.state),
             (double)fact.motion_score,
             result.forwarded ? 1 : 0,
             result.server_status);
    return ret;
}

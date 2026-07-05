/**
 * @file csi_placeholder_gateway.c
 * @brief S3 网关 CSI 轻量结果接收边界。
 *
 * 本文件属于 ESPS3 网关，保留 /local/v1/csi/result 的 occupancy 摘要入口。
 * 默认构建只接收 C5 已计算好的 occupancy 摘要并上报 csi.motion，不解析 raw CSI；
 * trigger 必须显式打开后才向在线 C5 发送 UDP 小包。
 * 它不解析 raw CSI、不训练模型，也不把 CSI 失败当成整机离线。
 */

#include "csi_placeholder_gateway.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "child_registry.h"
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

static const char *TAG = "csi_placeholder";

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
    char state[16];
    float motion_score;
    float mean_amplitude;
    float variance;
    float cv;
    int rssi;
    char quality[16];
    int sample_count;
    uint64_t updated_at_ms;
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

static cJSON *json_object(cJSON *root, const char *key)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsObject(value) ? value : NULL;
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

static void update_latest_result(const protocol_adapter_envelope_t *envelope)
{
    if (envelope == NULL || envelope->payload == NULL) {
        return;
    }

    cJSON *occupancy = json_object(envelope->payload, "occupancy");
    const char *link_id = json_string(envelope->payload, "link_id", "unknown");
    const char *device_id = json_string(envelope->payload, "device_id", envelope->device_id);
    const char *state = occupancy != NULL ? json_string(occupancy, "state", "unknown") : "unknown";
    double motion_score = json_number(envelope->payload, "motion_score", 0.0);
    double mean_amplitude = json_number(envelope->payload, "mean_amplitude", 0.0);
    double variance = json_number(envelope->payload, "variance", 0.0);
    double cv = json_number(envelope->payload, "cv", 0.0);
    int rssi = json_int(envelope->payload, "rssi", 0);
    const char *quality = json_string(envelope->payload, "quality", "unknown");
    int sample_count = json_int(envelope->payload, "sample_count", 0);
    uint64_t updated_at_ms = (uint64_t)json_number(envelope->payload, "updated_at_ms",
                                                   json_number(envelope->payload,
                                                               "updated_at",
                                                               0.0));
    uint64_t received_at_ms = (uint64_t)now_ms();

    if (s_latest_lock != NULL) {
        xSemaphoreTake(s_latest_lock, portMAX_DELAY);
    }
    csi_link_latest_t *slot = find_latest_slot_locked(link_id);
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
        slot->valid = true;
        strlcpy(slot->link_id, link_id, sizeof(slot->link_id));
        strlcpy(slot->device_id, device_id, sizeof(slot->device_id));
        strlcpy(slot->state, state, sizeof(slot->state));
        slot->motion_score = (float)motion_score;
        slot->mean_amplitude = (float)mean_amplitude;
        slot->variance = (float)variance;
        slot->cv = (float)cv;
        slot->rssi = rssi;
        strlcpy(slot->quality, quality, sizeof(slot->quality));
        slot->sample_count = sample_count;
        slot->updated_at_ms = updated_at_ms;
        slot->received_at_ms = received_at_ms;
    }
    if (s_latest_lock != NULL) {
        xSemaphoreGive(s_latest_lock);
    }

    ESP_LOGD(TAG,
             "CSI_RESULT link=%s dev=%s state=%s score=%.2f var=%.2f cv=%.3f rssi=%d quality=%s samples=%d ts=%llu",
             link_id,
             device_id,
             state,
             motion_score,
             variance,
             cv,
             rssi,
             quality,
             sample_count,
             (unsigned long long)updated_at_ms);
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
                 "CSI_LATEST link_id=%s state=%s motion_score=%.3f quality=%s rssi=%d sample_count=%d updated_at_ms=%llu age_ms=%llu",
                 slot->link_id,
                 slot->valid ? slot->state : "unknown",
                 slot->valid ? slot->motion_score : 0.0f,
                 slot->valid ? slot->quality : "unknown",
                 slot->valid ? slot->rssi : 0,
                 slot->valid ? slot->sample_count : 0,
                 (unsigned long long)(slot->valid ? slot->updated_at_ms : 0U),
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
    if (!config->csi_trigger_enabled) {
        ESP_LOGI(TAG, "CSI gateway initialized; trigger disabled");
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
                 "CSI summary reserved device_id=%s link=%s seq=%u; ingest disabled by GATEWAY_CONFIG_ENABLE_CSI_RESULT_INGEST",
                 envelope->device_id,
                 envelope->payload != NULL ? json_string(envelope->payload, "link_id", "unknown") : "unknown",
                 (unsigned int)envelope->seq);
        return ESP_OK;
    }

    update_latest_result(envelope);
    sensor_aggregator_result_t result = {0};
    esp_err_t ret = sensor_aggregator_handle_envelope(envelope, &result);
    ESP_LOGD(TAG,
             "CSI summary accepted device_id=%s link=%s seq=%u forwarded=%d status=%d raw_csi=unsupported",
             envelope->device_id,
             envelope->payload != NULL ? json_string(envelope->payload, "link_id", "unknown") : "unknown",
             (unsigned int)envelope->seq,
             result.forwarded ? 1 : 0,
             result.server_status);
    return ret;
}

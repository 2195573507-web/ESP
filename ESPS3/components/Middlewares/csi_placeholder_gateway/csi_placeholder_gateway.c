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

#include <string.h>

#include "child_registry.h"
#include "esp111_protocol_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_config.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "sensor_aggregator.h"

static const char *TAG = "csi_placeholder";

static TaskHandle_t s_trigger_task;

static void csi_trigger_task(void *arg)
{
    (void)arg;
    const gateway_runtime_config_t *config = gateway_config_get();
    const char payload[] = "ping trigger csi";

    ESP_LOGI(TAG,
             "csi_trigger ping trigger task started interval_ms=%u udp_port=%u target C51=%s",
             (unsigned int)config->csi_trigger_interval_ms,
             (unsigned int)config->csi_trigger_udp_port,
             config->csi_trigger_target_device_id);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(config->csi_trigger_interval_ms));

        child_registry_entry_t entries[GATEWAY_CONFIG_MAX_CHILDREN];
        size_t count = child_registry_snapshot(entries, GATEWAY_CONFIG_MAX_CHILDREN);
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(entries[i].device_id, config->csi_trigger_target_device_id) != 0) {
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
                     "csi_trigger ping trigger target C51 device_id=%s peer_ip=%s sent=%d",
                     entries[i].device_id,
                     entries[i].peer_ip,
                     sent);
        }
    }
}

void csi_placeholder_gateway_init(void)
{
    const gateway_runtime_config_t *config = gateway_config_get();
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
        ESP_LOGI(TAG,
                 "CSI summary reserved device_id=%s seq=%u; ingest disabled by GATEWAY_CONFIG_ENABLE_CSI_RESULT_INGEST",
                 envelope->device_id,
                 (unsigned int)envelope->seq);
        return ESP_OK;
    }

    sensor_aggregator_result_t result = {0};
    esp_err_t ret = sensor_aggregator_handle_envelope(envelope, &result);
    ESP_LOGI(TAG,
             "CSI summary accepted device_id=%s seq=%u forwarded=%d status=%d raw_csi=unsupported",
             envelope->device_id,
             (unsigned int)envelope->seq,
             result.forwarded ? 1 : 0,
             result.server_status);
    return ret;
}

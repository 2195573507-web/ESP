/**
 * @file screen_server_client.c
 * @brief C5 终端屏幕命令轮询占位客户端。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），当前只保留历史 screen server
 * client 接口并固定返回 ESP_ERR_NOT_SUPPORTED。真实命令轮询由 system_server_client
 * 通过 S3 /local/v1/commands 处理，本文件不直连 Server。
 */

#include "screen_server_client.h"

#include "esp_log.h"

static const char *TAG = "screen_server_client";

esp_err_t screen_server_client_init(void)
{
    ESP_LOGI(TAG, "screen server client reserved");
    return ESP_OK;
}

esp_err_t screen_server_client_poll_commands(const char *device_id)
{
    (void)device_id;
    ESP_LOGD(TAG, "screen command polling reserved");
    return ESP_ERR_NOT_SUPPORTED;
}

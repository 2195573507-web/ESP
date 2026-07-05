/**
 * @file terminal_config.c
 * @brief C5 终端运行配置加载。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），负责从默认宏和 NVS 读取
 * device_id、gateway_id、SoftAP 连接信息和上报周期。本文件不负责发起 HTTP、
 * 不负责协议字段映射，也不保存公网 Server 地址；server_comm_config、wifi_manager、
 * BME/voice/command 模块只通过本模块读取终端身份和 S3 网关地址。
 */

#include "terminal_config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "terminal_config";

#define TERMINAL_CONFIG_NVS_NAMESPACE "terminal_cfg"

static terminal_runtime_config_t s_config = {
    .device_id = TERMINAL_CONFIG_DEFAULT_DEVICE_ID,
    .gateway_id = TERMINAL_CONFIG_DEFAULT_GATEWAY_ID,
    .room_id = TERMINAL_CONFIG_DEFAULT_ROOM_ID,
    .alias = TERMINAL_CONFIG_DEFAULT_ALIAS,
    .gateway_ssid = TERMINAL_CONFIG_DEFAULT_GATEWAY_SSID,
    .gateway_password = TERMINAL_CONFIG_DEFAULT_GATEWAY_PASSWORD,
    .gateway_ip = TERMINAL_CONFIG_DEFAULT_GATEWAY_IP,
    .upload_period_ms = TERMINAL_CONFIG_DEFAULT_UPLOAD_PERIOD_MS,
    .debug_direct_server_enabled = false,
};
static bool s_loaded;

static void terminal_config_reset_defaults(void)
{
    memset(&s_config, 0, sizeof(s_config));
    strlcpy(s_config.device_id, TERMINAL_CONFIG_DEFAULT_DEVICE_ID, sizeof(s_config.device_id));
    strlcpy(s_config.gateway_id, TERMINAL_CONFIG_DEFAULT_GATEWAY_ID, sizeof(s_config.gateway_id));
    strlcpy(s_config.room_id, TERMINAL_CONFIG_DEFAULT_ROOM_ID, sizeof(s_config.room_id));
    strlcpy(s_config.alias, TERMINAL_CONFIG_DEFAULT_ALIAS, sizeof(s_config.alias));
    strlcpy(s_config.gateway_ssid,
            TERMINAL_CONFIG_DEFAULT_GATEWAY_SSID,
            sizeof(s_config.gateway_ssid));
    strlcpy(s_config.gateway_password,
            TERMINAL_CONFIG_DEFAULT_GATEWAY_PASSWORD,
            sizeof(s_config.gateway_password));
    strlcpy(s_config.gateway_ip, TERMINAL_CONFIG_DEFAULT_GATEWAY_IP, sizeof(s_config.gateway_ip));
    s_config.upload_period_ms = TERMINAL_CONFIG_DEFAULT_UPLOAD_PERIOD_MS;
    s_config.debug_direct_server_enabled = false;
}

static void terminal_config_load_string(nvs_handle_t nvs,
                                        const char *key,
                                        char *out,
                                        size_t out_size)
{
    if (key == NULL || out == NULL || out_size == 0) {
        return;
    }

    size_t len = out_size;
    esp_err_t ret = nvs_get_str(nvs, key, out, &len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS read failed key=%s ret=%s", key, esp_err_to_name(ret));
        return;
    }
    out[out_size - 1U] = '\0';
}

esp_err_t terminal_config_load(void)
{
    if (s_loaded) {
        return ESP_OK;
    }

    terminal_config_reset_defaults();

    nvs_handle_t nvs = 0;
    esp_err_t ret = nvs_open(TERMINAL_CONFIG_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        s_loaded = true;
        ESP_LOGI(TAG,
                 "terminal config uses defaults device_id=%s gateway_id=%s ssid=%s",
                 s_config.device_id,
                 s_config.gateway_id,
                 s_config.gateway_ssid);
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        s_loaded = true;
        ESP_LOGW(TAG, "terminal config NVS open failed, using defaults: %s", esp_err_to_name(ret));
        return ret;
    }

    terminal_config_load_string(nvs, "device_id", s_config.device_id, sizeof(s_config.device_id));
    terminal_config_load_string(nvs, "gateway_id", s_config.gateway_id, sizeof(s_config.gateway_id));
    terminal_config_load_string(nvs, "room_id", s_config.room_id, sizeof(s_config.room_id));
    terminal_config_load_string(nvs, "alias", s_config.alias, sizeof(s_config.alias));
    terminal_config_load_string(nvs,
                                "gateway_ssid",
                                s_config.gateway_ssid,
                                sizeof(s_config.gateway_ssid));
    terminal_config_load_string(nvs,
                                "gateway_pass",
                                s_config.gateway_password,
                                sizeof(s_config.gateway_password));
    terminal_config_load_string(nvs, "gateway_ip", s_config.gateway_ip, sizeof(s_config.gateway_ip));

    uint32_t upload_period_ms = s_config.upload_period_ms;
    if (nvs_get_u32(nvs, "upload_ms", &upload_period_ms) == ESP_OK && upload_period_ms > 0) {
        s_config.upload_period_ms = upload_period_ms;
    }

    uint8_t debug_direct = 0;
    if (nvs_get_u8(nvs, "debug_direct", &debug_direct) == ESP_OK) {
        s_config.debug_direct_server_enabled = debug_direct != 0;
    }

    nvs_close(nvs);
    s_loaded = true;
    ESP_LOGI(TAG,
             "terminal config loaded device_id=%s gateway_id=%s room_id=%s alias=%s ssid=%s",
             s_config.device_id,
             s_config.gateway_id,
             s_config.room_id,
             s_config.alias,
             s_config.gateway_ssid);
    return ESP_OK;
}

const terminal_runtime_config_t *terminal_config_get(void)
{
    (void)terminal_config_load();
    return &s_config;
}

const char *terminal_config_get_device_id(void)
{
    return terminal_config_get()->device_id;
}

const char *terminal_config_get_gateway_id(void)
{
    return terminal_config_get()->gateway_id;
}

const char *terminal_config_get_room_id(void)
{
    return terminal_config_get()->room_id;
}

const char *terminal_config_get_alias(void)
{
    return terminal_config_get()->alias;
}

const char *terminal_config_get_gateway_ssid(void)
{
    return terminal_config_get()->gateway_ssid;
}

const char *terminal_config_get_gateway_password(void)
{
    return terminal_config_get()->gateway_password;
}

const char *terminal_config_get_gateway_ip(void)
{
    return terminal_config_get()->gateway_ip;
}

uint32_t terminal_config_get_upload_period_ms(void)
{
    return terminal_config_get()->upload_period_ms;
}

uint8_t terminal_config_get_local_id(void)
{
    const char *device_id = terminal_config_get_device_id();
    if (strcmp(device_id, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52) == 0) {
        return ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52;
    }
    if (strcmp(device_id, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51) == 0) {
        return ESP111_PROTOCOL_LOCAL_DEVICE_ID_C51;
    }
    return TERMINAL_CONFIG_DEFAULT_LOCAL_ID;
}

bool terminal_config_debug_direct_server_enabled(void)
{
    return terminal_config_get()->debug_direct_server_enabled;
}

const char *terminal_config_get_capabilities_json(void)
{
    return TERMINAL_CONFIG_CAPABILITIES_JSON;
}

/**
 * @file offline_policy.c
 * @brief S3 网关 Server 可用性和错误码映射。
 *
 * 本文件属于 ESPS3 网关，负责把 server_client 的 esp_err_t/http_status 映射为协议错误码，
 * 并记录最近一次上云可用性。它不重试请求、不缓存离线数据，也不改变 C5 本地响应格式。
 */

#include "offline_policy.h"

#include <string.h>

#include "esp111_protocol_common.h"
#include "esp_log.h"

static const char *TAG = "offline_policy";

static bool s_server_available;
static char s_last_error_code[32];
static uint32_t s_failure_count;

void offline_policy_init(void)
{
    s_server_available = false;
    strlcpy(s_last_error_code,
            ESP111_PROTOCOL_ERROR_SERVER_UNAVAILABLE,
            sizeof(s_last_error_code));
    s_failure_count = 0;
}

const char *offline_policy_code_for_result(esp_err_t ret, int http_status)
{
    if (ret == ESP_OK && http_status >= 200 && http_status < 300) {
        return "";
    }
    if (ret == ESP_ERR_INVALID_STATE) {
        return ESP111_PROTOCOL_ERROR_GATEWAY_OFFLINE;
    }
    if (ret == ESP_ERR_TIMEOUT) {
        return ESP111_PROTOCOL_ERROR_TIMEOUT;
    }
    if (http_status == 409 || http_status == 429) {
        return ESP111_PROTOCOL_ERROR_VOICE_BUSY;
    }
    if (http_status >= 500 || http_status == 0) {
        return ESP111_PROTOCOL_ERROR_SERVER_UNAVAILABLE;
    }
    if (http_status >= 400) {
        return ESP111_PROTOCOL_ERROR_SERVER_REJECTED;
    }
    return ESP111_PROTOCOL_ERROR_SERVER_UNAVAILABLE;
}

void offline_policy_record_server_result(esp_err_t ret, int http_status)
{
    const char *code = offline_policy_code_for_result(ret, http_status);
    bool ok = code[0] == '\0';
    s_server_available = ok;
    if (ok) {
        s_failure_count = 0;
        s_last_error_code[0] = '\0';
        return;
    }

    s_failure_count++;
    strlcpy(s_last_error_code, code, sizeof(s_last_error_code));
    ESP_LOGW(TAG,
             "server path degraded error_code=%s ret=%s http_status=%d failures=%u",
             s_last_error_code,
             esp_err_to_name(ret),
             http_status,
             (unsigned int)s_failure_count);
}

bool offline_policy_server_available(void)
{
    return s_server_available;
}

const char *offline_policy_last_error_code(void)
{
    return s_last_error_code[0] != '\0' ? s_last_error_code : "";
}

uint32_t offline_policy_failure_count(void)
{
    return s_failure_count;
}

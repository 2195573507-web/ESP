/**
 * @file voice_proxy.c
 * @brief S3 网关 voice turn 代理。
 *
 * 本文件属于 ESPS3 网关，负责接收 C5 上传的 PCM、校验 device_id/单会话锁、转发到
 * Server /api/voice/turn，并把 Server PCM 响应流式回传给 C5。它不做 ASR/LLM/TTS
 * 具体实现，不缓存语义结果，也不执行 C5 本地播放。
 */

#include "voice_proxy.h"

#include <errno.h>
#include <string.h>

#include "child_registry.h"
#include "esp111_protocol_common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gateway_config.h"
#include "offline_policy.h"
#include "protocol_adapter.h"
#include "sensor_aggregator.h"
#include "server_client.h"

static const char *TAG = "voice_proxy";

#define VOICE_PROXY_BUSY_SKIP_LOG_INTERVAL_MS 30000

static SemaphoreHandle_t s_voice_lock;
static char s_active_device_id[CHILD_REGISTRY_DEVICE_ID_LEN];
static int64_t s_last_busy_skip_log_ms;

typedef struct {
    httpd_req_t *req;
    const char *device_id;
    size_t expected_bytes;
    size_t bytes_sent;
    esp_err_t send_error;
    int send_errno;
    bool disconnected;
    bool disconnect_logged;
} voice_proxy_stream_ctx_t;

static esp_err_t send_json_error(httpd_req_t *req,
                                 const char *status,
                                 const char *error_code,
                                 const char *message)
{
    char body[192];
    protocol_adapter_build_error_response(error_code, message, body, sizeof(body));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status);
    esp_err_t ret = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "voice error response send failed status=%s ret=%s errno=%d",
                 status != NULL ? status : "<none>",
                 esp_err_to_name(ret),
                 errno);
    }
    return ret;
}

static esp_err_t read_pcm_body(httpd_req_t *req, uint8_t **out_pcm, size_t *out_len)
{
    if (req == NULL || out_pcm == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_pcm = NULL;
    *out_len = 0;
    if (req->content_len <= 0 ||
        (size_t)req->content_len > gateway_config_get()->voice_upload_max_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = heap_caps_malloc(req->content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = heap_caps_malloc(req->content_len, MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int remaining = req->content_len;
    int offset = 0;
    while (remaining > 0) {
        int read = httpd_req_recv(req, (char *)buf + offset, remaining);
        if (read <= 0) {
            int recv_errno = errno;
            ESP_LOGW(TAG,
                     "child disconnected while receiving voice request recv_ret=%d errno=%d received_bytes=%u expected_bytes=%u",
                     read,
                     recv_errno,
                     (unsigned int)offset,
                     (unsigned int)req->content_len);
            heap_caps_free(buf);
            return read == HTTPD_SOCK_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
        }
        offset += read;
        remaining -= read;
    }

    *out_pcm = buf;
    *out_len = (size_t)req->content_len;
    return ESP_OK;
}

static void voice_proxy_log_child_send_disconnect(voice_proxy_stream_ctx_t *ctx, esp_err_t ret)
{
    if (ctx == NULL || ctx->disconnect_logged) {
        return;
    }

    ctx->disconnect_logged = true;
    ctx->send_error = ret;
    ctx->send_errno = errno;
    ESP_LOGW(TAG,
             "child disconnected while sending voice response device_id=%s errno=%d esp_err=%s sent_bytes=%u expected_bytes=%u",
             ctx->device_id != NULL ? ctx->device_id : "<unknown>",
             ctx->send_errno,
             esp_err_to_name(ret),
             (unsigned int)ctx->bytes_sent,
             (unsigned int)ctx->expected_bytes);
}

static esp_err_t stream_to_httpd(const uint8_t *data, size_t len, void *user_ctx)
{
    voice_proxy_stream_ctx_t *ctx = (voice_proxy_stream_ctx_t *)user_ctx;
    if (ctx == NULL || ctx->req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->disconnected) {
        return ctx->send_error != ESP_OK ? ctx->send_error : ESP_FAIL;
    }

    esp_err_t ret = httpd_resp_send_chunk(ctx->req, (const char *)data, len);
    if (ret == ESP_OK) {
        ctx->bytes_sent += len;
    } else {
        ctx->disconnected = true;
        voice_proxy_log_child_send_disconnect(ctx, ret);
    }
    return ret;
}

static void voice_proxy_set_response_meta(int64_t content_length, void *user_ctx)
{
    voice_proxy_stream_ctx_t *ctx = (voice_proxy_stream_ctx_t *)user_ctx;
    if (ctx == NULL || content_length <= 0) {
        return;
    }

    ctx->expected_bytes = (size_t)content_length;
}

static void voice_proxy_release_active_device(const char *device_id)
{
    /*
     * voice_busy 只表示 C5 正在走语音独占，普通 heartbeat/status 可能暂停；
     * 释放时回到 online，不把语音期间的 heartbeat 缺失误判为 offline。
     */
    if (device_id != NULL && device_id[0] != '\0') {
        child_registry_set_voice_busy(device_id, false);
    }

    xSemaphoreTake(s_voice_lock, portMAX_DELAY);
    s_active_device_id[0] = '\0';
    xSemaphoreGive(s_voice_lock);
}

esp_err_t voice_proxy_init(void)
{
    if (s_voice_lock != NULL) {
        return ESP_OK;
    }

    s_voice_lock = xSemaphoreCreateMutex();
    if (s_voice_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_active_device_id[0] = '\0';
    ESP_LOGI(TAG, "voice proxy initialized single_session=true max_bytes=%u",
             (unsigned int)gateway_config_get()->voice_upload_max_bytes);
    return ESP_OK;
}

bool voice_proxy_is_busy(void)
{
    bool busy = false;
    if (s_voice_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_voice_lock, portMAX_DELAY);
    busy = s_active_device_id[0] != '\0';
    xSemaphoreGive(s_voice_lock);
    return busy;
}

void voice_proxy_log_busy_skip(const char *task_name)
{
    bool should_log = false;
    int64_t now_ms = esp_timer_get_time() / 1000;

    if (s_voice_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_voice_lock, portMAX_DELAY);
    if (s_last_busy_skip_log_ms == 0 ||
        now_ms - s_last_busy_skip_log_ms >= VOICE_PROXY_BUSY_SKIP_LOG_INTERVAL_MS) {
        s_last_busy_skip_log_ms = now_ms;
        should_log = true;
    }
    xSemaphoreGive(s_voice_lock);

    if (should_log) {
        ESP_LOGI(TAG,
                 "voice busy, skip non-voice task%s%s",
                 task_name != NULL && task_name[0] != '\0' ? ": " : "",
                 task_name != NULL && task_name[0] != '\0' ? task_name : "");
    }
}

bool voice_proxy_should_skip_non_voice_task(const char *task_name)
{
    if (!voice_proxy_is_busy()) {
        return false;
    }

    voice_proxy_log_busy_skip(task_name);
    return true;
}

esp_err_t voice_proxy_handle_turn(httpd_req_t *req)
{
    if (req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char device_id[CHILD_REGISTRY_DEVICE_ID_LEN] = {0};
    /* voice_proxy 以完整 X-Device-Id 作为边界校验；C5 轻量 body 不承载 voice JSON envelope。 */
    if (httpd_req_get_hdr_value_str(req, "X-Device-Id", device_id, sizeof(device_id)) != ESP_OK ||
        device_id[0] == '\0') {
        return send_json_error(req,
                               "400 Bad Request",
                               ESP111_PROTOCOL_ERROR_INVALID_DEVICE_ID,
                               "X-Device-Id header is required");
    }

    if (!child_registry_is_allowed(device_id)) {
        return send_json_error(req,
                               "403 Forbidden",
                               ESP111_PROTOCOL_ERROR_INVALID_DEVICE_ID,
                               "device_id is not in gateway allowlist");
    }

    if (xSemaphoreTake(s_voice_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return send_json_error(req,
                               "409 Conflict",
                               ESP111_PROTOCOL_ERROR_VOICE_BUSY,
                               "voice mutex is busy");
    }

    if (s_active_device_id[0] != '\0') {
        xSemaphoreGive(s_voice_lock);
        return send_json_error(req,
                               "409 Conflict",
                               ESP111_PROTOCOL_ERROR_VOICE_BUSY,
                               "another device is speaking");
    }
    strlcpy(s_active_device_id, device_id, sizeof(s_active_device_id));
    xSemaphoreGive(s_voice_lock);
    child_registry_set_voice_busy(device_id, true);

    uint8_t *pcm = NULL;
    size_t pcm_len = 0;
    esp_err_t ret = read_pcm_body(req, &pcm, &pcm_len);
    if (ret != ESP_OK) {
        voice_proxy_release_active_device(device_id);
        if (ret == ESP_ERR_INVALID_STATE || ret == ESP_ERR_TIMEOUT) {
            return ret;
        }
        return send_json_error(req,
                               ret == ESP_ERR_INVALID_SIZE ? "413 Payload Too Large" : "400 Bad Request",
                               ret == ESP_ERR_INVALID_SIZE ?
                                   ESP111_PROTOCOL_ERROR_PAYLOAD_TOO_LARGE :
                                   ESP111_PROTOCOL_ERROR_INVALID_VOICE_PAYLOAD,
                               esp_err_to_name(ret));
    }

    int status = 0;
    int64_t response_content_length = -1;
    int64_t turn_start_ms = esp_timer_get_time() / 1000;
    httpd_resp_set_type(req, ESP111_PROTOCOL_AUDIO_CONTENT_TYPE_L16_16K_MONO);
    voice_proxy_stream_ctx_t stream_ctx = {
        .req = req,
        .device_id = device_id,
    };
    ret = server_client_post_voice_turn(device_id,
                                        pcm,
                                        pcm_len,
                                        stream_to_httpd,
                                        &stream_ctx,
                                        &status,
                                        &response_content_length,
                                        voice_proxy_set_response_meta,
                                        &stream_ctx);
    heap_caps_free(pcm);
    if (response_content_length > 0) {
        stream_ctx.expected_bytes = (size_t)response_content_length;
    }

    voice_proxy_release_active_device(device_id);

    if (stream_ctx.disconnected) {
        offline_policy_record_server_result(ESP_OK, status);
        return stream_ctx.send_error != ESP_OK ? stream_ctx.send_error : ESP_FAIL;
    }

    offline_policy_record_server_result(ret, status);
    if (ret == ESP_OK) {
        esp_err_t end_ret = httpd_resp_send_chunk(req, NULL, 0);
        if (end_ret != ESP_OK) {
            stream_ctx.disconnected = true;
            voice_proxy_log_child_send_disconnect(&stream_ctx, end_ret);
            return end_ret;
        }
        int64_t duration_ms = esp_timer_get_time() / 1000 - turn_start_ms;
        sensor_aggregator_record_voice_event(device_id, pcm_len, (uint32_t)duration_ms);
        ESP_LOGI(TAG,
                 "voice response sent to child device_id=%s response_bytes=%u duration_ms=%lld",
                 device_id,
                 (unsigned int)stream_ctx.bytes_sent,
                 (long long)duration_ms);
        ESP_LOGI(TAG, "voice turn proxied device_id=%s bytes=%u", device_id, (unsigned int)pcm_len);
        return ESP_OK;
    }

    ESP_LOGW(TAG,
             "voice turn failed device_id=%s error_code=%s status=%d ret=%s",
             device_id,
             offline_policy_code_for_result(ret, status),
             status,
             esp_err_to_name(ret));
    if (stream_ctx.bytes_sent > 0) {
        esp_err_t end_ret = httpd_resp_send_chunk(req, NULL, 0);
        if (end_ret != ESP_OK) {
            voice_proxy_log_child_send_disconnect(&stream_ctx, end_ret);
        }
        return ESP_FAIL;
    }

    const char *error_code = offline_policy_code_for_result(ret, status);
    const char *http_status = strcmp(error_code, ESP111_PROTOCOL_ERROR_VOICE_BUSY) == 0 ?
                                  "409 Conflict" :
                              strcmp(error_code, ESP111_PROTOCOL_ERROR_PAYLOAD_TOO_LARGE) == 0 ?
                                  "413 Payload Too Large" :
                              "503 Service Unavailable";
    return send_json_error(req, http_status, error_code, esp_err_to_name(ret));
}

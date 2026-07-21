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

#include "app_stack_monitor.h"
#include "child_registry.h"
#include "esp111_protocol_common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gateway_config.h"
#include "offline_policy.h"
#include "protocol_adapter.h"
#include "s3_scheduler.h"
#include "sensor_aggregator.h"
#include "server_client.h"
#include "lwip/sockets.h"

static const char *TAG = "voice_proxy";

enum {
    VOICE_PROXY_LOCAL_SOCKET_TIMEOUT_SEC =
        (VOICE_REQUEST_TIMEOUT_MS + 999) / 1000,
};

static SemaphoreHandle_t s_voice_lock;
static char s_active_device_id[CHILD_REGISTRY_DEVICE_ID_LEN];
static QueueHandle_t s_voice_queue;
static TaskHandle_t s_voice_worker;
static bool s_voice_pending;

#define VOICE_PROXY_QUEUE_DEPTH 1U
#define VOICE_PROXY_WORKER_STACK 8192U
#define VOICE_PROXY_WORKER_PRIORITY 5U

typedef struct {
    httpd_req_t *req;
    char device_id[CHILD_REGISTRY_DEVICE_ID_LEN];
    int64_t queued_at_ms;
} voice_proxy_job_t;

static void voice_proxy_worker_task(void *arg);

typedef struct {
    httpd_req_t *req;
    const char *device_id;
    size_t expected_bytes;
    size_t bytes_sent;
    size_t chunks_sent;
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

static const char *safe_device_id(const char *device_id)
{
    return device_id != NULL && device_id[0] != '\0' ? device_id : "<unknown>";
}

static void apply_voice_socket_timeout(httpd_req_t *req, const char *device_id)
{
    int sock = httpd_req_to_sockfd(req);
    if (sock < 0) {
        ESP_LOGW(TAG, "voice local socket lookup failed device_id=%s", safe_device_id(device_id));
        return;
    }

    struct timeval timeout = {
        .tv_sec = VOICE_PROXY_LOCAL_SOCKET_TIMEOUT_SEC,
        .tv_usec = 0,
    };
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        ESP_LOGW(TAG,
                 "voice local recv timeout set failed device_id=%s errno=%d",
                 safe_device_id(device_id),
                 errno);
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        ESP_LOGW(TAG,
                 "voice local send timeout set failed device_id=%s errno=%d",
                 safe_device_id(device_id),
                 errno);
    }
}

static esp_err_t read_pcm_body(httpd_req_t *req,
                               const char *device_id,
                               uint8_t **out_pcm,
                               size_t *out_len)
{
    if (req == NULL || out_pcm == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_pcm = NULL;
    *out_len = 0;
    if (req->content_len <= 0 ||
        (size_t)req->content_len > gateway_config_get()->voice_upload_max_bytes) {
        ESP_LOGW(TAG,
                 "voice request invalid content length device_id=%s content_length=%u max_bytes=%u",
                 safe_device_id(device_id),
                 (unsigned int)req->content_len,
                 (unsigned int)gateway_config_get()->voice_upload_max_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = heap_caps_malloc(req->content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = heap_caps_malloc(req->content_len, MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t remaining = (size_t)req->content_len;
    size_t offset = 0;
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
        offset += (size_t)read;
        remaining -= (size_t)read;
    }

    if (offset != (size_t)req->content_len) {
        ESP_LOGW(TAG,
                 "voice request body incomplete device_id=%s received_bytes=%u expected_bytes=%u",
                 safe_device_id(device_id),
                 (unsigned int)offset,
                 (unsigned int)req->content_len);
        heap_caps_free(buf);
        return ESP_ERR_INVALID_SIZE;
    }

    *out_pcm = buf;
    *out_len = offset;
    ESP_LOGI(TAG,
             "received pcm bytes device_id=%s received_bytes=%u content_length=%u",
             safe_device_id(device_id),
             (unsigned int)offset,
             (unsigned int)req->content_len);
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
             "child disconnected while sending voice response device_id=%s errno=%d esp_err=%s sent_bytes=%u sent_chunks=%u expected_bytes=%u",
             ctx->device_id != NULL ? ctx->device_id : "<unknown>",
             ctx->send_errno,
             esp_err_to_name(ret),
             (unsigned int)ctx->bytes_sent,
             (unsigned int)ctx->chunks_sent,
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
        ctx->chunks_sent++;
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
    s3_scheduler_set_voice_busy(false);

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
    s_voice_queue = xQueueCreate(VOICE_PROXY_QUEUE_DEPTH, sizeof(voice_proxy_job_t));
    if (s_voice_queue == NULL) {
        vSemaphoreDelete(s_voice_lock);
        s_voice_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreateWithCaps(voice_proxy_worker_task,
                            "voice_proxy",
                            VOICE_PROXY_WORKER_STACK,
                            NULL,
                            VOICE_PROXY_WORKER_PRIORITY,
                            &s_voice_worker,
                            APP_TASK_STACK_CAPS_PSRAM) != pdPASS) {
        vQueueDelete(s_voice_queue);
        s_voice_queue = NULL;
        vSemaphoreDelete(s_voice_lock);
        s_voice_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    app_stack_monitor_log_task_created(TAG,
                                       "voice_proxy",
                                       s_voice_worker,
                                       VOICE_PROXY_WORKER_STACK);
    s_active_device_id[0] = '\0';
    s_voice_pending = false;
    s3_scheduler_set_voice_busy(false);
    ESP_LOGI(TAG, "voice proxy initialized single_session=true queue_depth=%u max_bytes=%u",
             (unsigned int)VOICE_PROXY_QUEUE_DEPTH,
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
    busy = s_voice_pending || s_active_device_id[0] != '\0';
    xSemaphoreGive(s_voice_lock);
    return busy;
}

static esp_err_t voice_proxy_process_reserved_turn(httpd_req_t *req, const char *reserved_device_id)
{
    if (req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char device_id[CHILD_REGISTRY_DEVICE_ID_LEN] = {0};
    strlcpy(device_id, reserved_device_id != NULL ? reserved_device_id : "", sizeof(device_id));
    if (device_id[0] == '\0') {
        return send_json_error(req, "400 Bad Request", ESP111_PROTOCOL_ERROR_INVALID_DEVICE_ID,
                               "X-Device-Id header is required");
    }
    child_registry_set_voice_busy(device_id, true);
    s3_scheduler_set_voice_busy(true);
    apply_voice_socket_timeout(req, device_id);

    uint8_t *pcm = NULL;
    size_t pcm_len = 0;
    esp_err_t ret = read_pcm_body(req, device_id, &pcm, &pcm_len);
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
    ESP_LOGI(TAG,
             "forward start device_id=%s pcm_bytes=%u upstream=%s timeout_ms=%u",
             device_id,
             (unsigned int)pcm_len,
             ESP111_PROTOCOL_SERVER_ROUTE_VOICE_TURN,
             (unsigned int)VOICE_REQUEST_TIMEOUT_MS);
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

    if (stream_ctx.disconnected) {
        offline_policy_record_server_result(ESP_OK, status);
        ESP_LOGI(TAG,
                 "voice downstream aborted device_id=%s response_bytes=%u response_chunks=%u",
                 device_id,
                 (unsigned int)stream_ctx.bytes_sent,
                 (unsigned int)stream_ctx.chunks_sent);
        voice_proxy_release_active_device(device_id);
        return stream_ctx.send_error != ESP_OK ? stream_ctx.send_error : ESP_FAIL;
    }

    offline_policy_record_server_result(ret, status);
    if (ret == ESP_OK) {
        /* ESP-IDF emits the terminating 0\r\n\r\n chunk for data=NULL,len=0. */
        esp_err_t end_ret = httpd_resp_send_chunk(req, NULL, 0);
        if (end_ret != ESP_OK) {
            stream_ctx.disconnected = true;
            voice_proxy_log_child_send_disconnect(&stream_ctx, end_ret);
            voice_proxy_release_active_device(device_id);
            return end_ret;
        }
        int64_t duration_ms = esp_timer_get_time() / 1000 - turn_start_ms;
        sensor_aggregator_record_voice_event(device_id, pcm_len, (uint32_t)duration_ms);
        ESP_LOGI(TAG,
                 "voice response sent to child device_id=%s response_bytes=%u response_chunks=%u duration_ms=%lld",
                 device_id,
                 (unsigned int)stream_ctx.bytes_sent,
                 (unsigned int)stream_ctx.chunks_sent,
                 (long long)duration_ms);
        ESP_LOGI(TAG, "voice turn proxied device_id=%s bytes=%u", device_id, (unsigned int)pcm_len);
        voice_proxy_release_active_device(device_id);
        return ESP_OK;
    }

    ESP_LOGW(TAG,
             "voice turn failed device_id=%s error_code=%s status=%d ret=%s",
             device_id,
             offline_policy_code_for_result(ret, status),
             status,
             esp_err_to_name(ret));
    if (stream_ctx.bytes_sent > 0) {
        ESP_LOGW(TAG,
                 "voice partial response aborted device_id=%s response_bytes=%u response_chunks=%u terminator_sent=0",
                 device_id,
                 (unsigned int)stream_ctx.bytes_sent,
                 (unsigned int)stream_ctx.chunks_sent);
        voice_proxy_release_active_device(device_id);
        return ESP_FAIL;
    }

    const char *error_code = offline_policy_code_for_result(ret, status);
    const char *http_status = strcmp(error_code, ESP111_PROTOCOL_ERROR_VOICE_BUSY) == 0 ?
                                  "409 Conflict" :
                              strcmp(error_code, ESP111_PROTOCOL_ERROR_PAYLOAD_TOO_LARGE) == 0 ?
                                  "413 Payload Too Large" :
                              "503 Service Unavailable";
    esp_err_t error_ret = send_json_error(req, http_status, error_code, esp_err_to_name(ret));
    voice_proxy_release_active_device(device_id);
    return error_ret;
}

static void voice_proxy_worker_task(void *arg)
{
    (void)arg;
    app_stack_monitor_report(TAG,
                             "voice_proxy",
                             VOICE_PROXY_WORKER_STACK,
                             "entry");
    while (true) {
        voice_proxy_job_t job = {0};
        if (xQueueReceive(s_voice_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const int64_t started_ms = esp_timer_get_time() / 1000;
        const int64_t queue_wait_ms = job.queued_at_ms > 0 ? started_ms - job.queued_at_ms : 0;
        xSemaphoreTake(s_voice_lock, portMAX_DELAY);
        s_voice_pending = false;
        strlcpy(s_active_device_id, job.device_id, sizeof(s_active_device_id));
        xSemaphoreGive(s_voice_lock);

        ESP_LOGI(TAG,
                 "local_http_active_count=1 handler=voice handler_latency=0 voice_http_active=1 telemetry_http_active=0 queue_wait_time=%lld",
                 (long long)queue_wait_ms);
        esp_err_t ret = voice_proxy_process_reserved_turn(job.req, job.device_id);
        const int64_t latency_ms = esp_timer_get_time() / 1000 - started_ms;
        ESP_LOGI(TAG,
                 "local_http_active_count=0 handler=voice handler_latency=%lld voice_http_active=0 telemetry_http_active=0 queue_wait_time=%lld result=%s",
                 (long long)latency_ms,
                 (long long)queue_wait_ms,
                 esp_err_to_name(ret));
        (void)httpd_req_async_handler_complete(job.req);
    }
}

esp_err_t voice_proxy_handle_turn(httpd_req_t *req)
{
    if (req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char device_id[CHILD_REGISTRY_DEVICE_ID_LEN] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Device-Id", device_id, sizeof(device_id)) != ESP_OK ||
        device_id[0] == '\0') {
        return send_json_error(req, "400 Bad Request", ESP111_PROTOCOL_ERROR_INVALID_DEVICE_ID,
                               "X-Device-Id header is required");
    }
    if (!child_registry_is_allowed(device_id)) {
        return send_json_error(req, "403 Forbidden", ESP111_PROTOCOL_ERROR_INVALID_DEVICE_ID,
                               "device_id is not in gateway allowlist");
    }
    if (s_voice_queue == NULL || s_voice_lock == NULL) {
        return send_json_error(req, "503 Service Unavailable", ESP111_PROTOCOL_ERROR_VOICE_BUSY,
                               "voice proxy is not ready");
    }

    xSemaphoreTake(s_voice_lock, portMAX_DELAY);
    const bool busy = s_voice_pending || s_active_device_id[0] != '\0';
    if (!busy) {
        s_voice_pending = true;
    }
    xSemaphoreGive(s_voice_lock);
    if (busy) {
        return send_json_error(req, "409 Conflict", ESP111_PROTOCOL_ERROR_VOICE_BUSY,
                               "another device is speaking");
    }

    httpd_req_t *async_req = NULL;
    esp_err_t ret = httpd_req_async_handler_begin(req, &async_req);
    if (ret != ESP_OK) {
        xSemaphoreTake(s_voice_lock, portMAX_DELAY);
        s_voice_pending = false;
        xSemaphoreGive(s_voice_lock);
        return ret;
    }
    voice_proxy_job_t job = {
        .req = async_req,
        .queued_at_ms = esp_timer_get_time() / 1000,
    };
    strlcpy(job.device_id, device_id, sizeof(job.device_id));
    if (xQueueSend(s_voice_queue, &job, 0) != pdTRUE) {
        xSemaphoreTake(s_voice_lock, portMAX_DELAY);
        s_voice_pending = false;
        xSemaphoreGive(s_voice_lock);
        (void)send_json_error(async_req, "409 Conflict", ESP111_PROTOCOL_ERROR_VOICE_BUSY,
                              "voice queue is busy");
        (void)httpd_req_async_handler_complete(async_req);
        return ESP_OK;
    }
    return ESP_OK;
}

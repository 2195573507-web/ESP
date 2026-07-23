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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "app_stack_monitor.h"
#include "audio_wake_gateway.h"
#include "child_registry.h"
#include "command_router.h"
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

static SemaphoreHandle_t s_voice_lock;
static char s_active_device_id[CHILD_REGISTRY_DEVICE_ID_LEN];
static QueueHandle_t s_voice_queue;
static TaskHandle_t s_voice_worker;
static bool s_voice_pending;

#define VOICE_PROXY_QUEUE_DEPTH 1U
#define VOICE_PROXY_WORKER_STACK 8192U
#define VOICE_PROXY_WORKER_PRIORITY 5U
#define VOICE_PROXY_WAKE_EVENT_QUEUE_DEPTH 2U
#define VOICE_PROXY_WAKE_EVENT_WORKER_STACK 4096U
#define VOICE_PROXY_WAKE_EVENT_WORKER_PRIORITY 5U
#define VOICE_PROXY_COMMAND_CAPTURE_TIMEOUT_MS 8000U
#define VOICE_PROXY_COMMAND_ID_HEADER "X-Command-Id"
#define VOICE_PROXY_COMMAND_GENERATION_HEADER "X-Command-Generation"
#define VOICE_PROXY_COMMAND_STREAM_ID_HEADER "X-Command-Stream-Id"
#define VOICE_PROXY_COMMAND_STATE_HEADER "X-Command-State"
#define VOICE_PROXY_RESPONSE_CONTENT_TYPE_MAX 64U
#define VOICE_PROXY_RESPONSE_AUDIO_FORMAT_MAX 48U
#define VOICE_PROXY_RESPONSE_AUDIO_RATE_MAX 16U
#define VOICE_PROXY_RESPONSE_AUDIO_CHANNELS_MAX 8U

typedef struct {
    httpd_req_t *req;
    char device_id[CHILD_REGISTRY_DEVICE_ID_LEN];
    char command_id[40];
    int64_t queued_at_ms;
    bool is_command_capture;
    uint32_t command_generation;
    uint32_t command_stream_id;
} voice_proxy_job_t;

static void voice_proxy_worker_task(void *arg);

typedef struct {
    uint8_t source_id;
    uint32_t wake_stream_id;
    int64_t detected_at_ms;
} voice_proxy_wake_event_t;

typedef enum {
    VOICE_PROXY_COMMAND_STATE_NONE = 0,
    VOICE_PROXY_COMMAND_STATE_CREATE,
    VOICE_PROXY_COMMAND_STATE_WAIT_ACK,
    VOICE_PROXY_COMMAND_STATE_CAPTURE_READY,
    VOICE_PROXY_COMMAND_STATE_CAPTURING,
    VOICE_PROXY_COMMAND_STATE_UPLOAD,
    VOICE_PROXY_COMMAND_STATE_PROCESSING,
    VOICE_PROXY_COMMAND_STATE_DONE,
    VOICE_PROXY_COMMAND_STATE_FAILED,
} voice_proxy_command_state_t;

#define VOICE_PROXY_COMMAND_ID_MAX_LEN 64U

typedef struct VoiceCommandSession {
    char command_id[VOICE_PROXY_COMMAND_ID_MAX_LEN];
    uint32_t generation;
    uint32_t wake_stream_id;
    uint32_t command_stream_id;
    voice_proxy_command_state_t state;
    int64_t timestamp;
} VoiceCommandSession;

typedef struct {
    bool active;
    bool processing;
    bool acknowledged;
    uint8_t source_id;
    int64_t deadline_ms;
    char device_id[CHILD_REGISTRY_DEVICE_ID_LEN];
    VoiceCommandSession session;
} voice_proxy_command_capture_t;

static QueueHandle_t s_wake_event_queue;
static TaskHandle_t s_wake_event_worker;
static uint32_t s_next_command_generation;
static voice_proxy_command_capture_t s_command_capture;

static void voice_proxy_wake_event_task(void *arg);
static const char *safe_device_id(const char *device_id);

static void voice_proxy_log_command_session(const char *state,
                                            const char *reason,
                                            const char *device_id,
                                            const char *command_id,
                                            uint32_t generation,
                                            uint32_t stream_id)
{
    ESP_LOGI(TAG,
             "VOICE_COMMAND_SESSION state=%s reason=%s device_id=%s command_id=%s generation=%lu stream_id=%lu",
             state != NULL ? state : "UNKNOWN",
             reason != NULL ? reason : "unknown",
             safe_device_id(device_id),
             command_id != NULL && command_id[0] != '\0' ? command_id : "-",
             (unsigned long)generation,
             (unsigned long)stream_id);
}

static const char *voice_proxy_command_state_name(voice_proxy_command_state_t state)
{
    switch (state) {
    case VOICE_PROXY_COMMAND_STATE_CREATE:
        return "CREATE";
    case VOICE_PROXY_COMMAND_STATE_WAIT_ACK:
        return "WAIT_ACK";
    case VOICE_PROXY_COMMAND_STATE_CAPTURE_READY:
        return "CAPTURE_READY";
    case VOICE_PROXY_COMMAND_STATE_CAPTURING:
        return "CAPTURING";
    case VOICE_PROXY_COMMAND_STATE_UPLOAD:
        return "UPLOAD";
    case VOICE_PROXY_COMMAND_STATE_PROCESSING:
        return "PROCESSING";
    case VOICE_PROXY_COMMAND_STATE_DONE:
        return "DONE";
    case VOICE_PROXY_COMMAND_STATE_FAILED:
        return "FAILED";
    case VOICE_PROXY_COMMAND_STATE_NONE:
    default:
        return "NONE";
    }
}

/* Caller holds s_voice_lock so the identity and transition are atomic. */
static void voice_proxy_transition_command_session(voice_proxy_command_state_t next,
                                                   const char *reason)
{
    const voice_proxy_command_state_t previous = s_command_capture.session.state;
    if (previous == next) {
        return;
    }
    s_command_capture.session.state = next;
    s_command_capture.session.timestamp = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG,
             "VOICE_COMMAND_SESSION from=%s to=%s reason=%s device_id=%s command_id=%s "
             "generation=%lu wake_stream_id=%lu command_stream_id=%lu timestamp_ms=%lld",
             voice_proxy_command_state_name(previous),
             voice_proxy_command_state_name(next),
             reason != NULL ? reason : "unspecified",
             safe_device_id(s_command_capture.device_id),
             s_command_capture.session.command_id[0] != '\0' ?
                 s_command_capture.session.command_id : "-",
             (unsigned long)s_command_capture.session.generation,
             (unsigned long)s_command_capture.session.wake_stream_id,
             (unsigned long)s_command_capture.session.command_stream_id,
             (long long)s_command_capture.session.timestamp);
}

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
    bool response_meta_received;
    bool response_audio_valid;
    esp_err_t response_meta_error;
    char content_type[VOICE_PROXY_RESPONSE_CONTENT_TYPE_MAX];
    char audio_format[VOICE_PROXY_RESPONSE_AUDIO_FORMAT_MAX];
    char audio_sample_rate[VOICE_PROXY_RESPONSE_AUDIO_RATE_MAX];
    char audio_channels[VOICE_PROXY_RESPONSE_AUDIO_CHANNELS_MAX];
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

static const char *voice_proxy_device_for_source(uint8_t source_id)
{
    switch (source_id) {
    case ESP111_PROTOCOL_LOCAL_DEVICE_ID_C51:
        return ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51;
    case ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52:
        return ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52;
    default:
        return NULL;
    }
}

static const char *voice_proxy_source_name(uint8_t source_id)
{
    switch (source_id) {
    case ESP111_PROTOCOL_LOCAL_DEVICE_ID_C51: return "C51";
    case ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52: return "C52";
    default: return "UNKNOWN";
    }
}

static bool parse_u32_header(httpd_req_t *req, const char *name, uint32_t *out)
{
    if (req == NULL || name == NULL || out == NULL) {
        return false;
    }
    char value[16] = {0};
    if (httpd_req_get_hdr_value_str(req, name, value, sizeof(value)) != ESP_OK ||
        value[0] == '\0') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0U ||
        parsed > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)parsed;
    return true;
}

static void voice_proxy_expire_command_capture(void)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    char device_id[CHILD_REGISTRY_DEVICE_ID_LEN] = {0};
    char command_id[sizeof(s_command_capture.session.command_id)] = {0};
    uint32_t generation = 0U;
    uint32_t wake_stream_id = 0U;
    uint32_t command_stream_id = 0U;
    xSemaphoreTake(s_voice_lock, portMAX_DELAY);
    if (s_command_capture.active && !s_command_capture.processing &&
        now_ms >= s_command_capture.deadline_ms) {
        strlcpy(device_id, s_command_capture.device_id, sizeof(device_id));
        strlcpy(command_id, s_command_capture.session.command_id, sizeof(command_id));
        generation = s_command_capture.session.generation;
        wake_stream_id = s_command_capture.session.wake_stream_id;
        command_stream_id = s_command_capture.session.command_stream_id;
        voice_proxy_transition_command_session(VOICE_PROXY_COMMAND_STATE_FAILED,
                                               "capture_start_timeout");
        memset(&s_command_capture, 0, sizeof(s_command_capture));
    }
    xSemaphoreGive(s_voice_lock);
    if (generation != 0U) {
        ESP_LOGW(TAG,
                 "COMMAND_CAPTURE_TIMEOUT device_id=%s command_generation=%lu wake_stream_id=%lu command_stream_bound=%u",
                 device_id,
                 (unsigned long)generation,
                 (unsigned long)wake_stream_id,
                 command_stream_id != 0U ? 1U : 0U);
        voice_proxy_log_command_session("FAILED",
                                        "capture_start_timeout",
                                        device_id,
                                        command_id,
                                        generation,
                                        command_stream_id);
    }
}

static void voice_proxy_finish_command_capture(const voice_proxy_job_t *job,
                                               const char *outcome)
{
    if (job == NULL || !job->is_command_capture || s_voice_lock == NULL) {
        return;
    }
    bool cleared = false;
    char command_id[sizeof(s_command_capture.session.command_id)] = {0};
    xSemaphoreTake(s_voice_lock, portMAX_DELAY);
    if (s_command_capture.active &&
        s_command_capture.session.generation == job->command_generation &&
        s_command_capture.session.command_stream_id == job->command_stream_id &&
        strcmp(s_command_capture.session.command_id, job->command_id) == 0 &&
        strcmp(s_command_capture.device_id, job->device_id) == 0) {
        strlcpy(command_id, s_command_capture.session.command_id, sizeof(command_id));
        voice_proxy_transition_command_session(
            outcome != NULL && strcmp(outcome, "completed") == 0 ?
                VOICE_PROXY_COMMAND_STATE_DONE : VOICE_PROXY_COMMAND_STATE_FAILED,
            outcome);
        memset(&s_command_capture, 0, sizeof(s_command_capture));
        cleared = true;
    }
    xSemaphoreGive(s_voice_lock);
    if (cleared) {
        ESP_LOGI(TAG,
                 "COMMAND_STREAM_CLOSE device_id=%s stream_id=%lu generation=%lu outcome=%s",
                 job->device_id,
                 (unsigned long)job->command_stream_id,
                 (unsigned long)job->command_generation,
                 outcome != NULL ? outcome : "unknown");
        voice_proxy_log_command_session(
            outcome != NULL && strcmp(outcome, "completed") == 0 ? "DONE" : "FAILED",
            outcome,
            job->device_id,
            command_id,
            job->command_generation,
            job->command_stream_id);
    }
}

static esp_err_t voice_proxy_accept_command_capture(const char *device_id,
                                                    uint32_t generation,
                                                    uint32_t stream_id,
                                                    const char *command_id)
{
    if (device_id == NULL || device_id[0] == '\0' || command_id == NULL ||
        command_id[0] == '\0' || generation == 0U || stream_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    voice_proxy_expire_command_capture();
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    char session_command_id[sizeof(s_command_capture.session.command_id)] = {0};
    xSemaphoreTake(s_voice_lock, portMAX_DELAY);
    if (s_command_capture.active && !s_command_capture.processing &&
        s_command_capture.acknowledged &&
        s_command_capture.session.state == VOICE_PROXY_COMMAND_STATE_CAPTURE_READY &&
        s_command_capture.session.generation == generation &&
        strcmp(s_command_capture.device_id, device_id) == 0 &&
        strcmp(s_command_capture.session.command_id, command_id) == 0 &&
        s_command_capture.session.command_stream_id == 0U &&
        stream_id != s_command_capture.session.wake_stream_id) {
        s_command_capture.processing = true;
        s_command_capture.session.command_stream_id = stream_id;
        voice_proxy_transition_command_session(VOICE_PROXY_COMMAND_STATE_UPLOAD,
                                               "command_pcm_open");
        strlcpy(session_command_id,
                s_command_capture.session.command_id,
                sizeof(session_command_id));
        ret = ESP_OK;
    }
    xSemaphoreGive(s_voice_lock);
    if (ret == ESP_OK) {
        voice_proxy_log_command_session("UPLOAD",
                                        "command_pcm_open",
                                        device_id,
                                        session_command_id,
                                        generation,
                                        stream_id);
    }
    return ret;
}

static esp_err_t voice_proxy_enqueue_wake_event(const audio_wake_event_t *event, void *context)
{
    (void)context;
    if (event == NULL || s_wake_event_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const voice_proxy_wake_event_t item = {
        .source_id = event->source_id,
        .wake_stream_id = event->wake_stream_id,
        .detected_at_ms = event->detected_at_ms,
    };
    return xQueueSend(s_wake_event_queue, &item, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void voice_proxy_request_command_capture(const voice_proxy_wake_event_t *event)
{
    if (event == NULL) {
        return;
    }
    const char *device_id = voice_proxy_device_for_source(event->source_id);
    if (device_id == NULL || !child_registry_is_allowed(device_id)) {
        ESP_LOGW(TAG,
                 "COMMAND_CAPTURE_REQUEST rejected reason=unknown_source source_device=%s wake_stream_id=%lu",
                 voice_proxy_source_name(event->source_id),
                 (unsigned long)event->wake_stream_id);
        return;
    }
    if (event->wake_stream_id == 0U) {
        ESP_LOGW(TAG,
                 "COMMAND_CAPTURE_REQUEST rejected reason=zero_wake_stream_id source_device=%s",
                 voice_proxy_source_name(event->source_id));
        return;
    }

    voice_proxy_log_command_session("WAKE_EVENT",
                                    "wake_detected",
                                    device_id,
                                    NULL,
                                    0U,
                                    event->wake_stream_id);

    char command_id[VOICE_PROXY_COMMAND_ID_MAX_LEN] = {0};
    uint32_t generation = 0U;
    const int64_t deadline_ms = esp_timer_get_time() / 1000 + VOICE_PROXY_COMMAND_CAPTURE_TIMEOUT_MS;
    xSemaphoreTake(s_voice_lock, portMAX_DELAY);
    if (!s_command_capture.active) {
        generation = ++s_next_command_generation;
        if (generation == 0U) {
            generation = ++s_next_command_generation;
        }
        s_command_capture.active = true;
        s_command_capture.source_id = event->source_id;
        s_command_capture.session.wake_stream_id = event->wake_stream_id;
        s_command_capture.session.generation = generation;
        s_command_capture.deadline_ms = deadline_ms;
        strlcpy(s_command_capture.device_id, device_id, sizeof(s_command_capture.device_id));
        const int written = snprintf(command_id,
                                     sizeof(command_id),
                                     "local-voice-%lu",
                                     (unsigned long)generation);
        if (written <= 0 || written >= (int)sizeof(command_id)) {
            generation = 0U;
            memset(&s_command_capture, 0, sizeof(s_command_capture));
        } else {
            strlcpy(s_command_capture.session.command_id,
                    command_id,
                    sizeof(s_command_capture.session.command_id));
            voice_proxy_transition_command_session(VOICE_PROXY_COMMAND_STATE_CREATE,
                                                   "wake_session_created");
            voice_proxy_transition_command_session(VOICE_PROXY_COMMAND_STATE_WAIT_ACK,
                                                   "capture_command_queued");
        }
    }
    xSemaphoreGive(s_voice_lock);
    if (generation == 0U) {
        ESP_LOGW(TAG,
                 "COMMAND_CAPTURE_REQUEST rejected reason=voice_interaction_busy source_device=%s wake_stream_id=%lu",
                 voice_proxy_source_name(event->source_id),
                 (unsigned long)event->wake_stream_id);
        return;
    }

    esp_err_t ret = command_router_enqueue_voice_command_capture(
        device_id,
        command_id,
        generation,
        event->wake_stream_id,
        VOICE_PROXY_COMMAND_CAPTURE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        xSemaphoreTake(s_voice_lock, portMAX_DELAY);
        if (s_command_capture.active && s_command_capture.session.generation == generation &&
            strcmp(s_command_capture.session.command_id, command_id) == 0) {
            voice_proxy_transition_command_session(VOICE_PROXY_COMMAND_STATE_FAILED,
                                                   "capture_command_enqueue_failed");
            memset(&s_command_capture, 0, sizeof(s_command_capture));
        }
        xSemaphoreGive(s_voice_lock);
        ESP_LOGW(TAG,
                 "COMMAND_CAPTURE_REQUEST failed device_id=%s command_generation=%lu ret=%s",
                 device_id,
                 (unsigned long)generation,
                 esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG,
             "COMMAND_CAPTURE_REQUEST device_id=%s command_id=%s source_device=%s wake_stream_id=%lu command_generation=%lu command_timeout_ms=%u",
             device_id,
             command_id,
             voice_proxy_source_name(event->source_id),
             (unsigned long)event->wake_stream_id,
             (unsigned long)generation,
             (unsigned int)VOICE_PROXY_COMMAND_CAPTURE_TIMEOUT_MS);
    voice_proxy_log_command_session("WAIT_ACK",
                                    "wake_command_queued",
                                    device_id,
                                    command_id,
                                    generation,
                                    event->wake_stream_id);
}

static bool voice_proxy_ack_ok(const cJSON *item, bool *out_ok)
{
    if (item == NULL || out_ok == NULL) {
        return false;
    }
    if (cJSON_IsBool(item)) {
        *out_ok = cJSON_IsTrue(item);
        return true;
    }
    if (cJSON_IsNumber(item) && (item->valueint == 0 || item->valueint == 1)) {
        *out_ok = item->valueint != 0;
        return true;
    }
    return false;
}

esp_err_t voice_proxy_handle_command_ack(const char *command_id,
                                         const char *body,
                                         size_t body_len)
{
    if (command_id == NULL || command_id[0] == '\0' || body == NULL || body_len == 0U ||
        s_voice_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_voice_lock, portMAX_DELAY);
    const bool pending_match = s_command_capture.active &&
                               s_command_capture.session.command_id[0] != '\0' &&
                               strcmp(s_command_capture.session.command_id, command_id) == 0;
    xSemaphoreGive(s_voice_lock);
    if (!pending_match) {
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *root = cJSON_ParseWithLength(body, body_len);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, ESP111_PROTOCOL_LOCAL_JSON_ID);
    const cJSON *cid = cJSON_GetObjectItemCaseSensitive(root,
                                                         ESP111_PROTOCOL_LOCAL_JSON_COMMAND_ID);
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, ESP111_PROTOCOL_LOCAL_JSON_OK);
    const cJSON *generation = cJSON_GetObjectItemCaseSensitive(root, "generation");
    const cJSON *stream_id = cJSON_GetObjectItemCaseSensitive(root, "stream_id");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *long_command_id = cJSON_GetObjectItemCaseSensitive(root, "command_id");
    bool accepted = false;
    esp_err_t ret = voice_proxy_ack_ok(ok, &accepted) ? ESP_OK : ESP_ERR_INVALID_ARG;
    if (!cJSON_IsNumber(id) || id->valueint <= 0 ||
        !cJSON_IsString(cid) || cid->valuestring == NULL ||
        strcmp(cid->valuestring, command_id) != 0 ||
        (long_command_id != NULL &&
         (!cJSON_IsString(long_command_id) || long_command_id->valuestring == NULL ||
          strcmp(long_command_id->valuestring, command_id) != 0)) ||
        (generation != NULL && (!cJSON_IsNumber(generation) || generation->valueint <= 0)) ||
        (stream_id != NULL && (!cJSON_IsNumber(stream_id) || stream_id->valueint != 0)) ||
        (state != NULL &&
         (!cJSON_IsString(state) || state->valuestring == NULL ||
          strcmp(state->valuestring, accepted ? "ACK_SENT" : "RECOVERY") != 0)) ||
        ret != ESP_OK) {
        ret = ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_voice_lock, portMAX_DELAY);
    if (ret == ESP_OK) {
        const uint8_t expected_local_id =
            protocol_adapter_device_id_to_local_id(s_command_capture.device_id);
        if (!s_command_capture.active ||
            strcmp(s_command_capture.session.command_id, command_id) != 0 ||
            expected_local_id == 0U || id->valueint != (int)expected_local_id ||
            s_command_capture.session.generation == 0U ||
            (generation != NULL && generation->valueint != (int)s_command_capture.session.generation)) {
            ret = ESP_ERR_NOT_ALLOWED;
        } else if (!s_command_capture.acknowledged) {
            ret = command_router_ack_local_voice(command_id,
                                                 s_command_capture.device_id,
                                                 s_command_capture.session.generation,
                                                 accepted);
            if (ret == ESP_OK && accepted) {
                s_command_capture.acknowledged = true;
                /* The command PCM stream does not exist until C52 has
                 * accepted this command and re-armed capture. */
                s_command_capture.deadline_ms = esp_timer_get_time() / 1000 +
                                              VOICE_PROXY_COMMAND_CAPTURE_TIMEOUT_MS;
                voice_proxy_transition_command_session(VOICE_PROXY_COMMAND_STATE_CAPTURE_READY,
                                                       "capture_command_accepted");
                ESP_LOGI(TAG,
                         "COMMAND_LISTENING device_id=%s command_id=%s command_generation=%lu state=COMMAND_LISTENING",
                         s_command_capture.device_id,
                         command_id,
                         (unsigned long)s_command_capture.session.generation);
                voice_proxy_log_command_session("CAPTURE_READY",
                                                "capture_command_accepted",
                                                s_command_capture.device_id,
                                                command_id,
                                                s_command_capture.session.generation,
                                                s_command_capture.session.wake_stream_id);
            } else if (ret == ESP_OK) {
                voice_proxy_transition_command_session(VOICE_PROXY_COMMAND_STATE_FAILED,
                                                       "capture_command_rejected");
                voice_proxy_log_command_session("FAILED",
                                                "capture_command_rejected",
                                                s_command_capture.device_id,
                                                command_id,
                                                s_command_capture.session.generation,
                                                s_command_capture.session.wake_stream_id);
                memset(&s_command_capture, 0, sizeof(s_command_capture));
            }
        }
    }
    xSemaphoreGive(s_voice_lock);
    cJSON_Delete(root);
    return ret;
}

static void voice_proxy_wake_event_task(void *arg)
{
    (void)arg;
    while (true) {
        voice_proxy_wake_event_t event = {0};
        if (xQueueReceive(s_wake_event_queue, &event, pdMS_TO_TICKS(250)) == pdTRUE) {
            voice_proxy_request_command_capture(&event);
        }
        voice_proxy_expire_command_capture();
    }
}

static const char *voice_response_failure_code(esp_err_t ret, int status)
{
    return offline_policy_code_for_result(ret, status);
}

static void voice_proxy_log_response_failure(const char *device_id,
                                             esp_err_t ret,
                                             int status,
                                             size_t command_pcm_bytes)
{
    const char *reason = voice_response_failure_code(ret, status);
    ESP_LOGW(TAG,
             "VOICE_RESPONSE_FAILED reason=%s device_id=%s status=%d ret=%s command_pcm_bytes=%u server_available=%d last_error=%s",
             reason,
             safe_device_id(device_id),
             status,
             esp_err_to_name(ret),
             (unsigned int)command_pcm_bytes,
             offline_policy_server_available() ? 1 : 0,
             offline_policy_last_error_code()[0] != '\0' ?
                 offline_policy_last_error_code() : "-");
}

static void apply_voice_socket_timeout(httpd_req_t *req,
                                       const char *device_id,
                                       uint32_t timeout_ms)
{
    int sock = httpd_req_to_sockfd(req);
    if (sock < 0) {
        ESP_LOGW(TAG, "voice local socket lookup failed device_id=%s", safe_device_id(device_id));
        return;
    }

    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000U,
        .tv_usec = (timeout_ms % 1000U) * 1000U,
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

    const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const size_t requested = (size_t)req->content_len;
    const size_t psram_free = heap_caps_get_free_size(psram_caps);
    const size_t psram_largest = heap_caps_get_largest_free_block(psram_caps);
    ESP_LOGI(TAG,
             "S3_MEM stage=voice_body_before internal_free=%u internal_min=%u internal_largest=%u dma_free=%u dma_largest=%u psram_free=%u psram_largest=%u requested=%u",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned int)psram_free,
             (unsigned int)psram_largest,
             (unsigned int)requested);
    if (psram_free < requested || psram_largest < requested) {
        ESP_LOGW(TAG,
                 "VOICE_MEMORY_ADMISSION decision=reject reason=psram_capacity requested=%u psram_free=%u psram_largest=%u",
                 (unsigned int)requested,
                 (unsigned int)psram_free,
                 (unsigned int)psram_largest);
        return ESP_ERR_NO_MEM;
    }
    uint8_t *buf = heap_caps_malloc(requested, psram_caps);
    if (buf == NULL) {
        ESP_LOGW(TAG, "VOICE_MEMORY_ADMISSION decision=reject reason=psram_alloc_failed requested=%u",
                 (unsigned int)requested);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "S3_MEM stage=voice_body_after_alloc internal_free=%u internal_min=%u internal_largest=%u dma_free=%u dma_largest=%u psram_free=%u psram_largest=%u requested=%u",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned int)heap_caps_get_free_size(psram_caps),
             (unsigned int)heap_caps_get_largest_free_block(psram_caps),
             (unsigned int)requested);

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
    if (ctx->response_meta_error != ESP_OK) {
        return ctx->response_meta_error;
    }
    if (!ctx->response_meta_received || !ctx->response_audio_valid) {
        ESP_LOGW(TAG,
                 "RESPONSE_AUDIO_REJECTED reason=missing_or_unsupported_metadata device_id=%s content_type=%s audio_format=%s",
                 safe_device_id(ctx->device_id),
                 ctx->content_type[0] != '\0' ? ctx->content_type : "-",
                 ctx->audio_format[0] != '\0' ? ctx->audio_format : "-");
        return ESP_ERR_INVALID_RESPONSE;
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

static bool voice_proxy_response_is_pcm16_mono_16k(const server_client_voice_response_meta_t *meta)
{
    if (meta == NULL || meta->content_type == NULL || meta->audio_format == NULL ||
        meta->audio_sample_rate == NULL || meta->audio_channels == NULL) {
        return false;
    }
    const bool content_type_ok = strcasecmp(meta->content_type,
                                             ESP111_PROTOCOL_AUDIO_CONTENT_TYPE_L16_16K_MONO) == 0 ||
                                 strcasecmp(meta->content_type,
                                             ESP111_PROTOCOL_AUDIO_RESPONSE_CONTENT_TYPE) == 0;
    const bool format_ok = strcasecmp(meta->audio_format,
                                      ESP111_PROTOCOL_AUDIO_FORMAT_PCM_S16LE_MONO_16K) == 0;
    const bool sample_rate_ok = strcmp(meta->audio_sample_rate, "16000") == 0;
    const bool channels_ok = strcmp(meta->audio_channels, "1") == 0;
    return content_type_ok && format_ok && sample_rate_ok && channels_ok;
}

static esp_err_t voice_proxy_set_response_meta(const server_client_voice_response_meta_t *meta,
                                               void *user_ctx)
{
    voice_proxy_stream_ctx_t *ctx = (voice_proxy_stream_ctx_t *)user_ctx;
    if (ctx == NULL || meta == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ctx->response_meta_received = true;
    ctx->response_audio_valid = voice_proxy_response_is_pcm16_mono_16k(meta);
    if (meta->content_length > 0) {
        ctx->expected_bytes = (size_t)meta->content_length;
    }
    strlcpy(ctx->content_type, meta->content_type != NULL ? meta->content_type : "",
            sizeof(ctx->content_type));
    strlcpy(ctx->audio_format, meta->audio_format != NULL ? meta->audio_format : "",
            sizeof(ctx->audio_format));
    strlcpy(ctx->audio_sample_rate,
            meta->audio_sample_rate != NULL ? meta->audio_sample_rate : "",
            sizeof(ctx->audio_sample_rate));
    strlcpy(ctx->audio_channels, meta->audio_channels != NULL ? meta->audio_channels : "",
            sizeof(ctx->audio_channels));
    ctx->response_meta_error = ESP_OK;
    if (ctx->response_audio_valid) {
        ctx->response_meta_error = httpd_resp_set_type(ctx->req, ctx->content_type);
        if (ctx->response_meta_error == ESP_OK) {
            ctx->response_meta_error = httpd_resp_set_hdr(ctx->req,
                                                          "X-Audio-Format",
                                                          ctx->audio_format);
        }
        if (ctx->response_meta_error == ESP_OK && ctx->audio_sample_rate[0] != '\0') {
            ctx->response_meta_error = httpd_resp_set_hdr(ctx->req,
                                                          "X-Audio-Sample-Rate",
                                                          ctx->audio_sample_rate);
        }
        if (ctx->response_meta_error == ESP_OK && ctx->audio_channels[0] != '\0') {
            ctx->response_meta_error = httpd_resp_set_hdr(ctx->req,
                                                          "X-Audio-Channels",
                                                          ctx->audio_channels);
        }
    }

    ESP_LOG_LEVEL_LOCAL(ctx->response_audio_valid && ctx->response_meta_error == ESP_OK ?
                            ESP_LOG_INFO : ESP_LOG_WARN,
                        TAG,
                        "RESPONSE_AUDIO_READY result=%s device_id=%s format=%s sample_rate=%s channels=%s content_type=%s content_length=%lld chunked=%d header_ret=%s",
                        ctx->response_audio_valid && ctx->response_meta_error == ESP_OK ?
                            "ok" : "rejected",
                        safe_device_id(ctx->device_id),
                        ctx->audio_format[0] != '\0' ? ctx->audio_format : "-",
                        ctx->audio_sample_rate[0] != '\0' ? ctx->audio_sample_rate : "16000",
                        ctx->audio_channels[0] != '\0' ? ctx->audio_channels : "1",
                        ctx->content_type[0] != '\0' ? ctx->content_type : "-",
                        (long long)meta->content_length,
                        meta->chunked ? 1 : 0,
                        esp_err_to_name(ctx->response_meta_error));
    if (!ctx->response_audio_valid) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ctx->response_meta_error;
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
    s_wake_event_queue = xQueueCreate(VOICE_PROXY_WAKE_EVENT_QUEUE_DEPTH,
                                      sizeof(voice_proxy_wake_event_t));
    if (s_wake_event_queue == NULL) {
        vQueueDelete(s_voice_queue);
        s_voice_queue = NULL;
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
        vQueueDelete(s_wake_event_queue);
        s_wake_event_queue = NULL;
        vQueueDelete(s_voice_queue);
        s_voice_queue = NULL;
        vSemaphoreDelete(s_voice_lock);
        s_voice_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreateWithCaps(voice_proxy_wake_event_task,
                            "voice_wake_evt",
                            VOICE_PROXY_WAKE_EVENT_WORKER_STACK,
                            NULL,
                            VOICE_PROXY_WAKE_EVENT_WORKER_PRIORITY,
                            &s_wake_event_worker,
                            APP_TASK_STACK_CAPS_PSRAM) != pdPASS) {
        vTaskDelete(s_voice_worker);
        s_voice_worker = NULL;
        vQueueDelete(s_wake_event_queue);
        s_wake_event_queue = NULL;
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
    memset(&s_command_capture, 0, sizeof(s_command_capture));
    s_next_command_generation = 0U;
    s3_scheduler_set_voice_busy(false);
    esp_err_t wake_handler_ret = audio_wake_gateway_set_event_handler(voice_proxy_enqueue_wake_event,
                                                                       NULL);
    if (wake_handler_ret != ESP_OK) {
        ESP_LOGW(TAG, "WAKE_EVENT_HANDLER_ATTACH failed ret=%s", esp_err_to_name(wake_handler_ret));
    }
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

static esp_err_t voice_proxy_process_reserved_turn(const voice_proxy_job_t *job)
{
    if (job == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    httpd_req_t *req = job->req;
    const char *reserved_device_id = job->device_id;
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
    apply_voice_socket_timeout(req,
                               device_id,
                               job->is_command_capture ?
                                   VOICE_PROXY_COMMAND_CAPTURE_TIMEOUT_MS :
                                   VOICE_REQUEST_TIMEOUT_MS);

    uint8_t *pcm = NULL;
    size_t pcm_len = 0;
    esp_err_t ret = read_pcm_body(req, device_id, &pcm, &pcm_len);
    if (ret != ESP_OK) {
        voice_proxy_release_active_device(device_id);
        voice_proxy_finish_command_capture(job, "capture_read_failed");
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

    if (job->is_command_capture) {
        char command_id[sizeof(s_command_capture.session.command_id)] = {0};
        xSemaphoreTake(s_voice_lock, portMAX_DELAY);
        if (s_command_capture.active &&
            s_command_capture.session.generation == job->command_generation &&
            s_command_capture.session.command_stream_id == job->command_stream_id &&
            strcmp(s_command_capture.session.command_id, job->command_id) == 0) {
            strlcpy(command_id, s_command_capture.session.command_id, sizeof(command_id));
        }
        xSemaphoreGive(s_voice_lock);
        voice_proxy_log_command_session("UPLOAD",
                                        "command_pcm_received",
                                        device_id,
                                        command_id,
                                        job->command_generation,
                                        job->command_stream_id);
        ESP_LOGI(TAG,
                 "COMMAND_PCM_RX_STATS device_id=%s stream_id=%lu generation=%lu frame_count=%u pcm_bytes=%u duration_ms=unknown buffer_capacity=%u dropped_frames=0",
                 device_id,
                 (unsigned long)job->command_stream_id,
                 (unsigned long)job->command_generation,
                 (unsigned int)(pcm_len / (160U * sizeof(int16_t))),
                 (unsigned int)pcm_len,
                 (unsigned int)gateway_config_get()->voice_upload_max_bytes);
        /* The command capture deadline covers C52 upload only; the existing
         * server request budget starts after the complete command is received. */
        apply_voice_socket_timeout(req, device_id, VOICE_REQUEST_TIMEOUT_MS);
        ESP_LOGI(TAG,
                 "SERVER_VOICE_FINALIZE_BEGIN device_id=%s stream_id=%lu generation=%lu pcm_bytes=%u reason=command_stream_close",
                 device_id,
                 (unsigned long)job->command_stream_id,
                 (unsigned long)job->command_generation,
                 (unsigned int)pcm_len);
        ESP_LOGI(TAG,
                 "SERVER_VOICE_STATE state=FINALIZING stream_id=%lu generation=%lu",
                 (unsigned long)job->command_stream_id,
                 (unsigned long)job->command_generation);
    } else {
        bool wake_detected = false;
        ret = audio_wake_gateway_detect_pcm((const int16_t *)pcm,
                                            pcm_len / sizeof(int16_t),
                                            device_id,
                                            &wake_detected);
        if (ret != ESP_OK) {
            heap_caps_free(pcm);
            voice_proxy_release_active_device(device_id);
            return send_json_error(req, "503 Service Unavailable", ESP111_PROTOCOL_ERROR_VOICE_BUSY,
                                   "WakeNet is unavailable");
        }
        if (!wake_detected) {
            heap_caps_free(pcm);
            httpd_resp_set_status(req, "204 No Content");
            (void)httpd_resp_send(req, NULL, 0);
            voice_proxy_release_active_device(device_id);
            return ESP_OK;
        }
    }

    int status = 0;
    int64_t response_content_length = -1;
    int64_t turn_start_ms = esp_timer_get_time() / 1000;
    voice_proxy_stream_ctx_t stream_ctx = {
        .req = req,
        .device_id = device_id,
    };
    ESP_LOGI(TAG,
             "VOICE_TURN_START device_id=%s command_capture=%u command_stream_id=%lu command_generation=%lu reason=command_pcm_ready",
             device_id,
             job->is_command_capture ? 1U : 0U,
             (unsigned long)job->command_stream_id,
             (unsigned long)job->command_generation);
    ESP_LOGI(TAG,
             "SERVER_RESPONSE_BEGIN device_id=%s command_stream_id=%lu generation=%lu command_pcm_bytes=%u reason=upstream_request_started",
             device_id,
             (unsigned long)job->command_stream_id,
             (unsigned long)job->command_generation,
             (unsigned int)pcm_len);
    if (job->is_command_capture) {
        char command_id[sizeof(s_command_capture.session.command_id)] = {0};
        xSemaphoreTake(s_voice_lock, portMAX_DELAY);
        if (s_command_capture.active &&
            s_command_capture.session.generation == job->command_generation &&
            s_command_capture.session.command_stream_id == job->command_stream_id &&
            strcmp(s_command_capture.session.command_id, job->command_id) == 0) {
            voice_proxy_transition_command_session(VOICE_PROXY_COMMAND_STATE_PROCESSING,
                                                   "server_voice_turn_started");
            strlcpy(command_id, s_command_capture.session.command_id, sizeof(command_id));
        }
        xSemaphoreGive(s_voice_lock);
        voice_proxy_log_command_session("PROCESSING",
                                        "server_voice_turn_started",
                                        device_id,
                                        command_id,
                                        job->command_generation,
                                        job->command_stream_id);
        ESP_LOGI(TAG,
                 "SERVER_VOICE_STATE state=WAITING_RESPONSE stream_id=%lu generation=%lu",
                 (unsigned long)job->command_stream_id,
                 (unsigned long)job->command_generation);
    }
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
    if (job->is_command_capture && ret == ESP_OK && status >= 200 && status < 300 &&
        stream_ctx.bytes_sent == 0U) {
        ESP_LOGW(TAG,
                 "RESPONSE_AUDIO_EMPTY device_id=%s stream_id=%lu generation=%lu status=%d content_length=%lld",
                 device_id,
                 (unsigned long)job->command_stream_id,
                 (unsigned long)job->command_generation,
                 status,
                 (long long)response_content_length);
        ret = ESP_ERR_INVALID_SIZE;
    }
    if (job->is_command_capture && ret == ESP_OK && status >= 200 && status < 300) {
        ESP_LOGI(TAG,
                 "SERVER_VOICE_FINALIZE_SUCCESS device_id=%s stream_id=%lu generation=%lu status=%d response_bytes=%u",
                 device_id,
                 (unsigned long)job->command_stream_id,
                 (unsigned long)job->command_generation,
                 status,
                 (unsigned int)stream_ctx.bytes_sent);
    } else if (job->is_command_capture) {
        ESP_LOGW(TAG,
                 "SERVER_VOICE_FINALIZE_FAILED device_id=%s stream_id=%lu generation=%lu esp_err=%s http_status=%d reason=%s",
                 device_id,
                 (unsigned long)job->command_stream_id,
                 (unsigned long)job->command_generation,
                 esp_err_to_name(ret),
                 status,
                 ret == ESP_ERR_INVALID_SIZE ? "response_audio_empty" : "upstream_or_response_failure");
    }
    ESP_LOG_LEVEL_LOCAL(ret == ESP_OK ? ESP_LOG_INFO : ESP_LOG_WARN,
                        TAG,
                        "SERVER_RESPONSE_END result=%s reason=%s device_id=%s status=%d response_bytes=%u",
                        ret == ESP_OK ? "ok" : "failed",
                        ret == ESP_OK ? "upstream_response_complete" : esp_err_to_name(ret),
                        device_id,
                        status,
                        (unsigned int)stream_ctx.bytes_sent);
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
        voice_proxy_finish_command_capture(job, "downstream_disconnected");
        ESP_LOGW(TAG,
                 "VOICE_TURN_FINISH result=failed reason=downstream_disconnected device_id=%s",
                 device_id);
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
            voice_proxy_finish_command_capture(job, "response_terminator_failed");
            ESP_LOGW(TAG,
                     "VOICE_TURN_FINISH result=failed reason=response_terminator_failed device_id=%s ret=%s",
                     device_id,
                     esp_err_to_name(end_ret));
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
        voice_proxy_finish_command_capture(job, "completed");
        ESP_LOGI(TAG,
                 "VOICE_TURN_FINISH result=ok reason=completed device_id=%s response_bytes=%u",
                 device_id,
                 (unsigned int)stream_ctx.bytes_sent);
        return ESP_OK;
    }

    const char *error_code = voice_response_failure_code(ret, status);
    voice_proxy_log_response_failure(device_id, ret, status, pcm_len);
    ESP_LOGW(TAG,
             "voice turn failed device_id=%s error_code=%s status=%d ret=%s",
             device_id,
             error_code,
             status,
             esp_err_to_name(ret));
    if (stream_ctx.bytes_sent > 0) {
        ESP_LOGW(TAG,
                 "voice partial response aborted device_id=%s response_bytes=%u response_chunks=%u terminator_sent=0",
                 device_id,
                 (unsigned int)stream_ctx.bytes_sent,
                 (unsigned int)stream_ctx.chunks_sent);
        voice_proxy_release_active_device(device_id);
        voice_proxy_finish_command_capture(job, "partial_response_failed");
        ESP_LOGW(TAG,
                 "VOICE_TURN_FINISH result=failed reason=partial_response_failed device_id=%s",
                 device_id);
        return ESP_FAIL;
    }

    const char *http_status = strcmp(error_code, ESP111_PROTOCOL_ERROR_VOICE_BUSY) == 0 ?
                                  "409 Conflict" :
                              strcmp(error_code, ESP111_PROTOCOL_ERROR_PAYLOAD_TOO_LARGE) == 0 ?
                                  "413 Payload Too Large" :
                              "503 Service Unavailable";
    esp_err_t error_ret = send_json_error(req, http_status, error_code, esp_err_to_name(ret));
    voice_proxy_release_active_device(device_id);
    voice_proxy_finish_command_capture(job, "server_failed");
    ESP_LOGW(TAG,
             "VOICE_TURN_FINISH result=failed reason=%s device_id=%s status=%d ret=%s",
             error_code,
             device_id,
             status,
             esp_err_to_name(ret));
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
        esp_err_t ret = voice_proxy_process_reserved_turn(&job);
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

    uint32_t command_generation = 0U;
    uint32_t command_stream_id = 0U;
    char command_id[sizeof(s_command_capture.session.command_id)] = {0};
    char command_state[24] = {0};
    const bool has_command_generation = parse_u32_header(req,
                                                          VOICE_PROXY_COMMAND_GENERATION_HEADER,
                                                          &command_generation);
    const bool has_command_stream_id = parse_u32_header(req,
                                                        VOICE_PROXY_COMMAND_STREAM_ID_HEADER,
                                                        &command_stream_id);
    const bool has_command_id =
        httpd_req_get_hdr_value_str(req,
                                    VOICE_PROXY_COMMAND_ID_HEADER,
                                    command_id,
                                    sizeof(command_id)) == ESP_OK &&
        command_id[0] != '\0';
    const bool has_command_state =
        httpd_req_get_hdr_value_str(req,
                                    VOICE_PROXY_COMMAND_STATE_HEADER,
                                    command_state,
                                    sizeof(command_state)) == ESP_OK &&
        command_state[0] != '\0';
    const bool has_command_id_header =
        httpd_req_get_hdr_value_len(req, VOICE_PROXY_COMMAND_ID_HEADER) > 0U;
    const bool has_command_generation_header =
        httpd_req_get_hdr_value_len(req, VOICE_PROXY_COMMAND_GENERATION_HEADER) > 0U;
    const bool has_command_stream_id_header =
        httpd_req_get_hdr_value_len(req, VOICE_PROXY_COMMAND_STREAM_ID_HEADER) > 0U;
    const bool has_command_state_header =
        httpd_req_get_hdr_value_len(req, VOICE_PROXY_COMMAND_STATE_HEADER) > 0U;
    const bool has_any_command_metadata = has_command_id_header ||
                                          has_command_generation_header ||
                                          has_command_stream_id_header ||
                                          has_command_state_header;
    const bool is_command_capture = has_command_id && has_command_generation &&
                                    has_command_stream_id;
    if (has_any_command_metadata && !is_command_capture) {
        ESP_LOGW(TAG,
                 "COMMAND_STREAM_OPEN rejected reason=incomplete_or_invalid_identity device_id=%s command_id=%s generation=%lu stream_id=%lu",
                 device_id,
                 has_command_id ? command_id : "-",
                 (unsigned long)command_generation,
                 (unsigned long)command_stream_id);
        return send_json_error(req, "400 Bad Request", ESP111_PROTOCOL_ERROR_INVALID_VOICE_PAYLOAD,
                               "command_id, nonzero generation, and nonzero stream id are required");
    }
    if (is_command_capture) {
        if (!has_command_state || strcmp(command_state, "CAPTURE_END") != 0) {
            ESP_LOGW(TAG,
                     "COMMAND_STREAM_OPEN rejected reason=invalid_state device_id=%s command_id=%s state=%s",
                     device_id,
                     has_command_id ? command_id : "-",
                     has_command_state ? command_state : "-");
            return send_json_error(req, "400 Bad Request", ESP111_PROTOCOL_ERROR_INVALID_VOICE_PAYLOAD,
                                   "command capture state must be CAPTURE_END");
        }
        const esp_err_t capture_ret = voice_proxy_accept_command_capture(device_id,
                                                                          command_generation,
                                                                          command_stream_id,
                                                                          command_id);
        if (capture_ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "COMMAND_STREAM_OPEN rejected reason=stale_or_mismatched_identity device_id=%s command_id=%s generation=%lu stream_id=%lu ret=%s",
                     device_id,
                     command_id,
                     (unsigned long)command_generation,
                     (unsigned long)command_stream_id,
                     esp_err_to_name(capture_ret));
            return send_json_error(req, "409 Conflict", ESP111_PROTOCOL_ERROR_VOICE_BUSY,
                                   "command capture is not active");
        }
        ESP_LOGI(TAG,
                 "COMMAND_STREAM_OPEN device_id=%s command_id=%s stream_id=%lu generation=%lu",
                 device_id,
                 command_id,
                 (unsigned long)command_stream_id,
                 (unsigned long)command_generation);
    }

    xSemaphoreTake(s_voice_lock, portMAX_DELAY);
    const bool busy = s_voice_pending || s_active_device_id[0] != '\0';
    if (!busy) {
        s_voice_pending = true;
    }
    xSemaphoreGive(s_voice_lock);
    if (busy) {
        if (is_command_capture) {
            voice_proxy_job_t failed_job = {
                .is_command_capture = true,
                .command_generation = command_generation,
                .command_stream_id = command_stream_id,
            };
            strlcpy(failed_job.device_id, device_id, sizeof(failed_job.device_id));
            strlcpy(failed_job.command_id, command_id, sizeof(failed_job.command_id));
            voice_proxy_finish_command_capture(&failed_job, "voice_proxy_busy");
        }
        return send_json_error(req, "409 Conflict", ESP111_PROTOCOL_ERROR_VOICE_BUSY,
                               "another device is speaking");
    }

    httpd_req_t *async_req = NULL;
    esp_err_t ret = httpd_req_async_handler_begin(req, &async_req);
    if (ret != ESP_OK) {
        xSemaphoreTake(s_voice_lock, portMAX_DELAY);
        s_voice_pending = false;
        xSemaphoreGive(s_voice_lock);
        if (is_command_capture) {
            voice_proxy_job_t failed_job = {
                .is_command_capture = true,
                .command_generation = command_generation,
                .command_stream_id = command_stream_id,
            };
            strlcpy(failed_job.device_id, device_id, sizeof(failed_job.device_id));
            strlcpy(failed_job.command_id, command_id, sizeof(failed_job.command_id));
            voice_proxy_finish_command_capture(&failed_job, "async_begin_failed");
        }
        return ret;
    }
    voice_proxy_job_t job = {
        .req = async_req,
        .queued_at_ms = esp_timer_get_time() / 1000,
    };
    strlcpy(job.device_id, device_id, sizeof(job.device_id));
    if (is_command_capture) {
        strlcpy(job.command_id, command_id, sizeof(job.command_id));
    }
    job.is_command_capture = is_command_capture;
    job.command_generation = command_generation;
    job.command_stream_id = command_stream_id;
    if (xQueueSend(s_voice_queue, &job, 0) != pdTRUE) {
        xSemaphoreTake(s_voice_lock, portMAX_DELAY);
        s_voice_pending = false;
        xSemaphoreGive(s_voice_lock);
        voice_proxy_finish_command_capture(&job, "queue_busy");
        (void)send_json_error(async_req, "409 Conflict", ESP111_PROTOCOL_ERROR_VOICE_BUSY,
                              "voice queue is busy");
        (void)httpd_req_async_handler_complete(async_req);
        return ESP_OK;
    }
    return ESP_OK;
}

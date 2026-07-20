/**
 * @file voice_chain.c
 * @brief C5 终端半双工语音链路状态机。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），负责 Mic/VAD、本地唤醒、
 * runtime gate、server_voice_client PCM 上传/播放回调和 Mic 恢复的编排。本文件不实现
 * ASR/LLM/TTS，不直接访问公网 Server；语音请求只到 ESPS3 /local/v1/voice/turn，
 * S3 voice_proxy 再转发到完整 Server 协议。
 */

#include "voice_chain.h"

#include <stdbool.h>
#include <string.h>

#include "app_debug_config.h"
#include "app_runtime.h"
#include "app_stack_monitor.h"
#include "c5_resource_manager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gateway_link.h"
#include "local_wake_word.h"
#include "mic_adc_test.h"
#include "server_voice_client.h"
#include "speaker_player.h"

static const char *TAG = "voice_chain";
static UBaseType_t s_voice_chain_stack_high_water_bytes;

#ifndef VOICE_CHAIN_RELEASE_RETRY_DELAY_MS
#define VOICE_CHAIN_RELEASE_RETRY_DELAY_MS 250U
#endif

typedef enum {
    VOICE_CHAIN_EVENT_LOCAL_WAKE = 0,
    VOICE_CHAIN_EVENT_RECORDING_FINISHED,
    VOICE_CHAIN_EVENT_SERVER_DONE,
    VOICE_CHAIN_EVENT_SERVER_ERROR,
    VOICE_CHAIN_EVENT_GATEWAY_LINK_ABORT,
    VOICE_CHAIN_EVENT_RELEASE_RETRY,
} voice_chain_event_type_t;

typedef struct {
    voice_chain_event_type_t type;
    uint32_t generation;
    uint32_t lease_generation;
    int error_code;
    char error_message[96];
} voice_chain_item_t;

typedef struct {
    QueueHandle_t event_queue;
    TaskHandle_t task;
    voice_chain_state_t state;
    bool started;
    uint32_t mic_generation;
    uint32_t release_retry_count;
    c5_voice_lease_t lease;
} voice_chain_context_t;

static voice_chain_context_t s_voice;
static portMUX_TYPE s_terminal_event_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_terminal_event_pending;
static voice_chain_item_t s_terminal_event;

static esp_err_t voice_chain_pause_mic(void);
static esp_err_t voice_chain_quiesce_mic_for_speaker(const char *reason);
static void voice_chain_abort_round(const char *reason);
static esp_err_t voice_chain_queue_event(const voice_chain_item_t *item, const char *label);
static esp_err_t voice_chain_queue_terminal_event(const voice_chain_item_t *item,
                                                   const char *label);
static void voice_chain_gateway_link_abort_cb(const char *reason);

const char *voice_chain_state_name(voice_chain_state_t state)
{
    switch (state) {
    case VOICE_IDLE:
        return "VOICE_IDLE";
    case VOICE_LISTENING:
        return "VOICE_LISTENING";
    case VOICE_WAKE_ACK:
        return "VOICE_WAKE_ACK";
    case VOICE_RECORDING:
        return "VOICE_RECORDING";
    case VOICE_WAITING_RESPONSE:
        return "VOICE_WAITING_RESPONSE";
    case VOICE_PLAYING:
        return "VOICE_PLAYING";
    case VOICE_ERROR:
        return "VOICE_ERROR";
    default:
        return "VOICE_UNKNOWN";
    }
}

voice_chain_state_t voice_chain_get_state(void)
{
    return s_voice.state;
}

static UBaseType_t voice_chain_current_stack_high_water(void)
{
    return app_stack_monitor_high_water();
}

static UBaseType_t voice_chain_note_stack_high_water(void)
{
    UBaseType_t current = voice_chain_current_stack_high_water();
    if (current != 0 &&
        (s_voice_chain_stack_high_water_bytes == 0 ||
         current < s_voice_chain_stack_high_water_bytes)) {
        s_voice_chain_stack_high_water_bytes = current;
    }
    return current;
}

static void voice_chain_log_heap(const char *label, voice_chain_state_t state)
{
#if ENABLE_VERBOSE_AUDIO_LOG
    if (s_voice.task != NULL && xTaskGetCurrentTaskHandle() == s_voice.task) {
        (void)voice_chain_note_stack_high_water();
    }
    ESP_LOGI(TAG,
             "%s state=%s free_heap=%u min_free_heap=%u largest_free_block=%u voice_stack_hwm=%u",
             label != NULL ? label : "voice",
             voice_chain_state_name(state),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned int)s_voice_chain_stack_high_water_bytes);
#else
    (void)label;
    (void)state;
#endif
}

static void voice_chain_log_audio_dma_heap(const char *phase)
{
    ESP_LOGI(TAG,
             "VOICE_AUDIO_DMA_HEAP phase=%s internal_free=%u internal_largest=%u "
             "dma_free=%u dma_largest=%u",
             phase != NULL ? phase : "none",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

static void voice_chain_log_start_stage(const char *stage, const char *edge, esp_err_t ret)
{
    const bool failed = ret != ESP_OK && ret != ESP_ERR_NOT_FINISHED;
    ESP_LOG_LEVEL_LOCAL(failed ? ESP_LOG_ERROR : ESP_LOG_INFO,
                        TAG,
                        "VOICE_START_STAGE stage=%s edge=%s ret=0x%x(%s) "
                        "internal_free=%u internal_min=%u internal_largest=%u "
                        "dma_free=%u dma_largest=%u",
                        stage != NULL ? stage : "<none>",
                        edge != NULL ? edge : "<none>",
                        (unsigned int)ret,
                        esp_err_to_name(ret),
                        (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                        (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                        (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                        (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA),
                        (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

static void voice_chain_log_start_failure(const char *stage, esp_err_t ret)
{
    ESP_LOGE(TAG,
             "VOICE_START_FAIL stage=%s ret=0x%x(%s)",
             stage != NULL ? stage : "<none>",
             (unsigned int)ret,
             esp_err_to_name(ret));
    voice_chain_log_start_stage(stage, "failure", ret);
}

static void voice_chain_set_state(voice_chain_state_t state)
{
    if (s_voice.state != state) {
        ESP_LOGI(TAG,
                 "state %s -> %s",
                 voice_chain_state_name(s_voice.state),
                 voice_chain_state_name(state));
    }
    s_voice.state = state;
    voice_chain_log_heap("voice state", state);
}

static esp_err_t voice_chain_queue_event(const voice_chain_item_t *item, const char *label)
{
    if (item == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_voice.event_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t sent = xQueueSend(s_voice.event_queue, item, 0);
    if (sent != pdTRUE) {
        ESP_LOGW(TAG,
                 "drop voice event because queue is busy: label=%s type=%d",
                 label != NULL ? label : "<none>",
                 (int)item->type);
        return ESP_ERR_TIMEOUT;
    }
    if (s_voice.task != NULL) {
        xTaskNotifyGive(s_voice.task);
    }
    return ESP_OK;
}

static unsigned int voice_chain_terminal_event_priority(voice_chain_event_type_t type)
{
    switch (type) {
    case VOICE_CHAIN_EVENT_GATEWAY_LINK_ABORT:
        return 4U;
    case VOICE_CHAIN_EVENT_SERVER_ERROR:
        return 3U;
    case VOICE_CHAIN_EVENT_SERVER_DONE:
        return 2U;
    case VOICE_CHAIN_EVENT_RELEASE_RETRY:
        return 1U;
    default:
        return 0U;
    }
}

static esp_err_t voice_chain_queue_terminal_event(const voice_chain_item_t *item,
                                                   const char *label)
{
    if (item == NULL || item->lease_generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_terminal_event_lock);
    if (!s_terminal_event_pending ||
        voice_chain_terminal_event_priority(item->type) >=
            voice_chain_terminal_event_priority(s_terminal_event.type)) {
        s_terminal_event = *item;
        s_terminal_event_pending = true;
    }
    portEXIT_CRITICAL(&s_terminal_event_lock);

    if (s_voice.task != NULL) {
        xTaskNotifyGive(s_voice.task);
    }
    ESP_LOGD(TAG,
             "queued reliable terminal voice event label=%s type=%d lease_generation=%lu",
             label != NULL ? label : "<none>",
             (int)item->type,
             (unsigned long)item->lease_generation);
    return ESP_OK;
}

static bool voice_chain_take_terminal_event(voice_chain_item_t *out_item)
{
    if (out_item == NULL) {
        return false;
    }

    bool pending;
    portENTER_CRITICAL(&s_terminal_event_lock);
    pending = s_terminal_event_pending;
    if (pending) {
        *out_item = s_terminal_event;
        memset(&s_terminal_event, 0, sizeof(s_terminal_event));
        s_terminal_event_pending = false;
    }
    portEXIT_CRITICAL(&s_terminal_event_lock);
    return pending;
}

static esp_err_t voice_chain_queue_terminal_error(int code, const char *message)
{
    voice_chain_item_t item = {
        .type = VOICE_CHAIN_EVENT_SERVER_ERROR,
        .lease_generation = s_voice.lease.generation,
        .error_code = code,
    };
    if (message != NULL && message[0] != '\0') {
        strlcpy(item.error_message, message, sizeof(item.error_message));
    }
    return voice_chain_queue_terminal_event(&item, "server_error");
}

static esp_err_t voice_chain_queue_local_wake_event(void)
{
    const voice_chain_item_t item = {
        .type = VOICE_CHAIN_EVENT_LOCAL_WAKE,
    };
    return voice_chain_queue_event(&item, "local_wake");
}

static esp_err_t voice_chain_server_done_sink(uint32_t lease_generation, void *user_ctx)
{
    (void)user_ctx;
    const voice_chain_item_t item = {
        .type = VOICE_CHAIN_EVENT_SERVER_DONE,
        .lease_generation = lease_generation,
    };
    return voice_chain_queue_terminal_event(&item, "server_done");
}

static esp_err_t voice_chain_server_error_sink(uint32_t lease_generation,
                                               int code,
                                               const char *message,
                                               void *user_ctx)
{
    (void)user_ctx;
    voice_chain_item_t item = {
        .type = VOICE_CHAIN_EVENT_SERVER_ERROR,
        .lease_generation = lease_generation,
        .error_code = code,
    };
    if (message != NULL && message[0] != '\0') {
        strlcpy(item.error_message, message, sizeof(item.error_message));
    }
    return voice_chain_queue_terminal_event(&item, "server_error");
}

static void voice_chain_gateway_link_abort_cb(const char *reason)
{
    c5_resource_snapshot_t snapshot = {0};
    c5_resource_manager_get_snapshot(&snapshot);
    if (!snapshot.lease_valid ||
        (snapshot.state != C5_RESOURCE_STATE_QUIESCING &&
         snapshot.state != C5_RESOURCE_STATE_VOICE_EXCLUSIVE &&
         snapshot.state != C5_RESOURCE_STATE_RELEASING)) {
        return;
    }
    voice_chain_item_t item = {
        .type = VOICE_CHAIN_EVENT_GATEWAY_LINK_ABORT,
        .lease_generation = snapshot.generation,
        .error_code = ESP_ERR_INVALID_STATE,
    };
    strlcpy(item.error_message,
            reason != NULL && reason[0] != '\0' ? reason : "gateway_link_lost",
            sizeof(item.error_message));
    (void)voice_chain_queue_terminal_event(&item, "gateway_link_abort");
}

static esp_err_t voice_chain_server_playback_start_sink(void *user_ctx)
{
    (void)user_ctx;
    if (!c5_resource_manager_lease_is_current(s_voice.lease.generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "server voice PCM playback start");
    voice_chain_set_state(VOICE_PLAYING);
    (void)local_wake_word_on_recording_finished();

    esp_err_t ret = voice_chain_quiesce_mic_for_speaker("server_response");
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "quiesce Mic before server PCM playback failed: %s", esp_err_to_name(ret));
        return ret;
    }
    (void)c5_resource_manager_set_audio_phase(s_voice.lease,
                                              C5_AUDIO_PHASE_SPEAKER_TX_OWNED,
                                              "server_response");
    return ESP_OK;
}

static void voice_chain_enter_listening_ready(const char *reason)
{
    s_voice.mic_generation = mic_adc_test_get_session_generation();
    if (reason != NULL && reason[0] != '\0') {
    ESP_LOGD(TAG,
                 "voice listening ready reason=%s mic_generation=%lu: Mic ADC/VAD and non-voice/BME active, server voice idle",
                 reason,
                 (unsigned long)s_voice.mic_generation);
    }
    voice_chain_log_heap("voice listening stage: before set LISTENING", s_voice.state);
    voice_chain_set_state(VOICE_LISTENING);
    voice_chain_log_heap("voice listening stage: after set LISTENING", s_voice.state);
}

static esp_err_t voice_chain_release_voice_resources(const char *reason)
{
    ESP_LOGD(TAG, "voice resources cleanup start reason=%s", reason != NULL ? reason : "<none>");
    voice_chain_log_heap("heap before cleanup", s_voice.state);
    (void)local_wake_word_on_recording_finished();
    esp_err_t ret = c5_resource_manager_release_voice(s_voice.lease, reason);
    if (ret == ESP_OK) {
        s_voice.lease.generation = 0U;
        s_voice.release_retry_count = 0U;
    } else {
        s_voice.release_retry_count++;
    }
    voice_chain_log_heap("heap after cleanup", s_voice.state);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "voice resources released reason=%s", reason != NULL ? reason : "<none>");
    } else {
        ESP_LOGW(TAG,
                 "voice resources release failed reason=%s ret=%s",
                 reason != NULL ? reason : "<none>",
                 esp_err_to_name(ret));
    }
    return ret;
}

static void voice_chain_schedule_release_retry(const char *reason)
{
    if (s_voice.lease.generation == 0U) {
        return;
    }

    voice_chain_item_t item = {
        .type = VOICE_CHAIN_EVENT_RELEASE_RETRY,
        .lease_generation = s_voice.lease.generation,
    };
    if (reason != NULL && reason[0] != '\0') {
        strlcpy(item.error_message, reason, sizeof(item.error_message));
    }
    esp_err_t ret = voice_chain_queue_terminal_event(&item, "release_retry");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "cannot schedule voice resource release retry generation=%lu ret=%s",
                 (unsigned long)s_voice.lease.generation,
                 esp_err_to_name(ret));
    }
}

static esp_err_t voice_chain_cleanup_mic_for_recover(const char *reason)
{
    ESP_LOGD(TAG,
             "voice recover cleanup Mic reason=%s",
             reason != NULL ? reason : "<none>");
    esp_err_t ret = voice_chain_quiesce_mic_for_speaker(reason);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Mic pause during voice recover failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ret;
}

static esp_err_t voice_chain_restart_mic_vad_standby(const char *reason)
{
    ESP_LOGD(TAG,
             "restart Mic ADC/VAD standby reason=%s",
             reason != NULL ? reason : "<none>");
    voice_chain_log_heap("voice standby: before Mic ADC start", s_voice.state);
    esp_err_t ret = mic_adc_test_start();
    voice_chain_log_heap("voice standby: after Mic ADC start", s_voice.state);
    if (ret != ESP_OK) {
        voice_chain_set_state(VOICE_ERROR);
        ESP_LOGE(TAG, "Mic ADC/VAD standby start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = mic_adc_test_wait_running(VOICE_MIC_PAUSE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        voice_chain_set_state(VOICE_ERROR);
        ESP_LOGE(TAG, "Mic ADC/VAD standby did not become ready: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "MIC_LISTENING_RESTARTED reason=%s", reason != NULL ? reason : "none");

    return ESP_OK;
}

static esp_err_t voice_chain_finish_or_recover_to_listening(const char *reason, bool cleanup_mic)
{
    ESP_LOGD(TAG,
             "voice finish/recover begin reason=%s cleanup_mic=%d",
             reason != NULL ? reason : "<none>",
             cleanup_mic ? 1 : 0);
    voice_chain_log_heap("voice finish/recover: before cleanup", s_voice.state);

    esp_err_t first_ret = ESP_OK;
    if (cleanup_mic) {
        esp_err_t mic_cleanup_ret = voice_chain_cleanup_mic_for_recover(reason);
        if (first_ret == ESP_OK && mic_cleanup_ret != ESP_OK) {
            first_ret = mic_cleanup_ret;
        }
    }

    esp_err_t release_ret = voice_chain_release_voice_resources(reason);
    if (first_ret == ESP_OK && release_ret != ESP_OK) {
        first_ret = release_ret;
    }
    if (release_ret != ESP_OK) {
        voice_chain_set_state(VOICE_ERROR);
        voice_chain_schedule_release_retry(reason);
        return first_ret;
    }

    esp_err_t ret = voice_chain_restart_mic_vad_standby(reason);
    if (ret != ESP_OK) {
        return ret;
    }

    voice_chain_enter_listening_ready(reason);
    if (first_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "voice finish/recover reached LISTENING with cleanup warning: %s",
                 esp_err_to_name(first_ret));
    }
    return first_ret;
}

static void voice_chain_abort_round(const char *reason)
{
    ESP_LOGW(TAG, "voice round abort reason=%s", reason != NULL ? reason : "<none>");
    voice_chain_set_state(VOICE_ERROR);
    (void)voice_chain_finish_or_recover_to_listening(reason, true);
}

static esp_err_t voice_chain_prepare_for_server_voice_start(void *user_ctx)
{
    (void)user_ctx;
    if (!s_voice.started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!gateway_link_can_start_voice_turn()) {
        /*
         * WakeNet/VAD 可以保持低功耗监听，但 gateway_link 不是 READY 时禁止进入
         * server voice turn，避免 S3 离线时反复创建 HTTP client 和占用 speaker/mic。
         */
        return ESP_ERR_INVALID_STATE;
    }

    if (!local_wake_word_is_recording_window_open()) {
        if (s_voice.state != VOICE_LISTENING) {
            ESP_LOGW(TAG,
                     "local wake rejected: state=%s",
                     voice_chain_state_name(s_voice.state));
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "enter voice exclusive reason=local_wake");
        voice_chain_set_state(VOICE_WAKE_ACK);
        esp_err_t ret = c5_resource_manager_begin_voice("voice_chain", &s_voice.lease);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "voice resource quiesce failed before local wake ack: %s", esp_err_to_name(ret));
            local_wake_word_cancel_recording_window();
            voice_chain_set_state(VOICE_LISTENING);
            return ret;
        }
        ESP_LOGI(TAG, "voice resources quiesced reason=local_wake generation=%lu",
                 (unsigned long)s_voice.lease.generation);

        ret = server_voice_client_prepare_async();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "server voice prepare failed: %s", esp_err_to_name(ret));
            (void)voice_chain_queue_terminal_error(ret, "server voice prepare failed");
            return ret;
        }

        ret = mic_adc_test_pause();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Mic pause request before local wake ack failed: %s", esp_err_to_name(ret));
            (void)voice_chain_queue_terminal_error(ret, "mic pause request failed");
            return ret;
        }
        ret = voice_chain_queue_local_wake_event();
        if (ret != ESP_OK) {
            (void)voice_chain_queue_terminal_error(ret, "local wake event handoff failed");
            return ret;
        }
        return ESP_ERR_NOT_FINISHED;
    }

    if (!local_wake_word_should_record_after_vad_start()) {
        ESP_LOGI(TAG, "server voice recording waits for local wake ack cooldown");
        return ESP_ERR_NOT_FINISHED;
    }

    if (s_voice.state != VOICE_RECORDING) {
        ESP_LOGW(TAG,
                 "server voice recording rejected: state=%s",
                 voice_chain_state_name(s_voice.state));
        return ESP_ERR_INVALID_STATE;
    }

    voice_chain_log_heap("server voice turn start before", s_voice.state);
    if (!gateway_link_can_start_voice_turn()) {
        (void)voice_chain_queue_terminal_error(ESP_ERR_INVALID_STATE,
                                               "gateway offline before voice start");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = server_voice_client_start_turn();
    voice_chain_log_heap("server voice turn start after", s_voice.state);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "server voice turn start failed: %s", esp_err_to_name(ret));
        (void)voice_chain_queue_terminal_error(ret, "server voice turn start failed");
        return ret;
    }

    voice_chain_set_state(VOICE_RECORDING);
    ESP_LOGI(TAG, "server voice recording window accepted");
    return ESP_OK;
}

static esp_err_t voice_chain_server_voice_append_pcm(const int16_t *pcm,
                                                     size_t samples,
                                                     void *user_ctx)
{
    (void)user_ctx;
    if (!gateway_link_is_ready()) {
        (void)voice_chain_queue_terminal_error(ESP_ERR_INVALID_STATE,
                                               "gateway offline during voice upload");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = server_voice_client_append_pcm(pcm, samples);
    if (ret != ESP_OK) {
        (void)voice_chain_queue_terminal_error(ret, "server voice PCM upload failed");
    }
    return ret;
}

static esp_err_t voice_chain_server_voice_finish(void *user_ctx)
{
    (void)user_ctx;
    if (!gateway_link_is_ready()) {
        (void)voice_chain_queue_terminal_error(ESP_ERR_INVALID_STATE,
                                               "gateway offline before voice finish");
        return ESP_ERR_INVALID_STATE;
    }
    if (!c5_resource_manager_lease_is_current(s_voice.lease.generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    voice_chain_set_state(VOICE_WAITING_RESPONSE);
    (void)c5_resource_manager_note_phase(s_voice.lease, "waiting_response");
    esp_err_t ret = mic_adc_test_pause();
    if (ret == ESP_OK) {
        const voice_chain_item_t item = {
            .type = VOICE_CHAIN_EVENT_RECORDING_FINISHED,
            .generation = s_voice.mic_generation,
        };
        ret = voice_chain_queue_event(&item, "recording_finished");
    }
    if (ret != ESP_OK) {
        (void)voice_chain_queue_terminal_error(ret,
                                                "server voice recording finish handoff failed");
    }
    return ret;
}

static bool voice_chain_server_voice_is_idle(void *user_ctx)
{
    (void)user_ctx;
    return server_voice_client_is_idle();
}

static bool voice_chain_server_voice_is_ready(void *user_ctx)
{
    (void)user_ctx;
    return s_voice.state == VOICE_RECORDING &&
           local_wake_word_should_record_after_vad_start() &&
           c5_resource_manager_lease_is_current(s_voice.lease.generation) &&
           server_voice_client_is_idle();
}

static void voice_chain_cleanup_start_failure(void)
{
    if (s_voice.task != NULL) {
        vTaskDelete(s_voice.task);
        s_voice.task = NULL;
    }
    mic_adc_test_set_voice_stream_ops(NULL);
    local_wake_word_cancel_recording_window();
    (void)server_voice_client_cancel_turn();
    if (s_voice.event_queue != NULL) {
        vQueueDelete(s_voice.event_queue);
        s_voice.event_queue = NULL;
    }
    s_voice.started = false;
}

static esp_err_t voice_chain_pause_mic(void)
{
    esp_err_t ret = mic_adc_test_pause();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = mic_adc_test_wait_paused(VOICE_MIC_PAUSE_TIMEOUT_MS);
    if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "Mic pause wait timeout, abort this round to preserve half-duplex");
        return ret;
    }
    return ret;
}

static esp_err_t voice_chain_quiesce_mic_for_speaker(const char *reason)
{
    const char *handoff_reason = reason != NULL ? reason : "unspecified";
    ESP_LOGI(TAG, "VOICE_AUDIO_HANDOFF direction=MIC_TO_SPEAKER reason=%s", handoff_reason);
    (void)c5_resource_manager_set_audio_phase(s_voice.lease,
                                              C5_AUDIO_PHASE_MIC_PAUSE_REQUESTED,
                                              handoff_reason);
    ESP_LOGI(TAG, "MIC_LISTENING_PAUSE_BEGIN reason=%s", handoff_reason);
    voice_chain_log_audio_dma_heap("mic_pause_before");

    esp_err_t ret = voice_chain_pause_mic();
    if (ret != ESP_OK) {
        return ret;
    }
    ESP_LOGI(TAG, "MIC_ADC_STOPPED reason=%s", handoff_reason);

    ret = mic_adc_test_clear_audio_cache();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = mic_adc_test_release_for_speaker(VOICE_MIC_PAUSE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return ret;
    }
    voice_chain_log_audio_dma_heap("mic_dma_released");
    ESP_LOGI(TAG, "MIC_DMA_RELEASED reason=%s", handoff_reason);
    (void)c5_resource_manager_set_audio_phase(s_voice.lease,
                                              C5_AUDIO_PHASE_MIC_DMA_RELEASED,
                                              handoff_reason);
    (void)c5_resource_manager_note_phase(s_voice.lease, "MIC_LISTENING_QUIESCED");
    ESP_LOGI(TAG, "MIC_LISTENING_QUIESCED reason=%s", handoff_reason);
    return ESP_OK;
}

static void voice_chain_handle_local_wake(void)
{
    if (s_voice.state != VOICE_WAKE_ACK) {
        ESP_LOGW(TAG,
                 "ignore local wake event outside wake ack state=%s",
                 voice_chain_state_name(s_voice.state));
        return;
    }

    esp_err_t ret = voice_chain_quiesce_mic_for_speaker("wake_ack");
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Mic quiesce before local wake ack failed: %s", esp_err_to_name(ret));
        voice_chain_abort_round("local_wake_mic_pause_fail");
        return;
    }

    ESP_LOGI(TAG, "local wake ack playback and server prepare window");
    ret = c5_resource_manager_note_phase(s_voice.lease, "before_i2s_init");
    if (ret != ESP_OK) {
        voice_chain_abort_round("local_wake_before_i2s_phase_fail");
        return;
    }
    ret = local_wake_word_on_local_wake_detected();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "local wake ack playback failed: %s", esp_err_to_name(ret));
        voice_chain_abort_round("local_wake_ack_fail");
        return;
    }
    (void)c5_resource_manager_set_audio_phase(s_voice.lease,
                                              C5_AUDIO_PHASE_SPEAKER_TX_OWNED,
                                              "wake_ack");
    (void)c5_resource_manager_note_phase(s_voice.lease, "after_i2s_init");
    (void)c5_resource_manager_note_phase(s_voice.lease, "playback_complete");

    ret = audio_player_release_session(VOICE_MIC_PAUSE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Speaker deinit after wake ack failed: %s", esp_err_to_name(ret));
        voice_chain_abort_round("wake_ack_speaker_deinit_fail");
        return;
    }
    voice_chain_log_audio_dma_heap("speaker_tx_released");
    ESP_LOGI(TAG, "SPEAKER_TX_DEINIT_OK reason=wake_ack");
    (void)c5_resource_manager_set_audio_phase(s_voice.lease,
                                              C5_AUDIO_PHASE_SPEAKER_TX_RELEASED,
                                              "wake_ack");

    ESP_LOGI(TAG, "VOICE_AUDIO_HANDOFF direction=SPEAKER_TO_MIC reason=wake_ack");
    ret = voice_chain_restart_mic_vad_standby("wake_ack_recording");
    if (ret != ESP_OK) {
        voice_chain_abort_round("server_voice_recording_restart_fail");
        return;
    }
    (void)c5_resource_manager_set_audio_phase(s_voice.lease,
                                              C5_AUDIO_PHASE_MIC_RECORD_READY,
                                              "wake_ack");
    ESP_LOGI(TAG, "MIC_RECORD_INIT_OK");
    ESP_LOGI(TAG, "MIC_RECORD_READY");
    ret = local_wake_word_open_recording_window();
    if (ret != ESP_OK) {
        voice_chain_abort_round("recording_window_open_fail");
        return;
    }
    voice_chain_set_state(VOICE_RECORDING);
}

static void voice_chain_handle_recording_finished(void)
{
    if (s_voice.state != VOICE_WAITING_RESPONSE) {
        ESP_LOGW(TAG,
                 "ignore recording-finished event outside waiting-response state=%s",
                 voice_chain_state_name(s_voice.state));
        return;
    }

    esp_err_t ret = voice_chain_quiesce_mic_for_speaker("waiting_response");
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Mic pause after recording failed: %s", esp_err_to_name(ret));
        voice_chain_abort_round("recording_finish_mic_pause_fail");
        return;
    }
    (void)local_wake_word_on_recording_finished();
    ESP_LOGI(TAG, "RECORDING_WINDOW_CLOSED");

    voice_chain_log_heap("server voice upload finish before", s_voice.state);
    ret = server_voice_client_finish_turn();
    voice_chain_log_heap("server voice upload finish after", s_voice.state);
    if (ret != ESP_OK) {
        (void)voice_chain_queue_terminal_error(ret, "server voice upload finish failed");
    }
}

static void voice_chain_handle_server_done(void)
{
    ESP_LOGI(TAG, "server voice turn done");
    ESP_LOGI(TAG, "SERVER_PCM_PLAYBACK_DONE");
    (void)local_wake_word_on_recording_finished();
    (void)voice_chain_finish_or_recover_to_listening("server_voice_done", true);
}

static void voice_chain_handle_server_error(const voice_chain_item_t *item)
{
    int code = item != NULL ? item->error_code : ESP_FAIL;
    const char *message = (item != NULL && item->error_message[0] != '\0') ?
                          item->error_message : "<none>";
    ESP_LOGW(TAG,
             "server voice error, abort current round: code=%d message=%s",
             code,
             message);
    local_wake_word_cancel_recording_window();
    (void)server_voice_client_cancel_turn();
    voice_chain_abort_round("server_voice_error");
}

static void voice_chain_handle_gateway_link_abort(const voice_chain_item_t *item)
{
    const char *message = (item != NULL && item->error_message[0] != '\0') ?
                          item->error_message : "gateway_link_lost";
    ESP_LOGI(TAG, "gateway link lost, abort voice round: %s", message);
    local_wake_word_cancel_recording_window();
    (void)server_voice_client_request_abort(message);
    voice_chain_abort_round(message);
}

static void voice_chain_handle_release_retry(const voice_chain_item_t *item)
{
    const char *reason = (item != NULL && item->error_message[0] != '\0') ?
                             item->error_message : "resource_release_retry";
    ESP_LOGW(TAG,
             "retry voice resource release generation=%lu attempt=%lu reason=%s",
             (unsigned long)s_voice.lease.generation,
             (unsigned long)s_voice.release_retry_count,
             reason);
    vTaskDelay(pdMS_TO_TICKS(VOICE_CHAIN_RELEASE_RETRY_DELAY_MS));
    (void)voice_chain_finish_or_recover_to_listening(reason, true);
}

static bool voice_chain_event_requires_current_lease(voice_chain_event_type_t type)
{
    return type == VOICE_CHAIN_EVENT_SERVER_DONE ||
           type == VOICE_CHAIN_EVENT_SERVER_ERROR ||
           type == VOICE_CHAIN_EVENT_GATEWAY_LINK_ABORT ||
           type == VOICE_CHAIN_EVENT_RELEASE_RETRY;
}

static void voice_chain_task(void *arg)
{
    (void)arg;
    app_stack_monitor_log(TAG, "voice_chain", "entry");

    while (1) {
        (void)voice_chain_note_stack_high_water();
        voice_chain_item_t item = {0};
        if (!voice_chain_take_terminal_event(&item) &&
            xQueueReceive(s_voice.event_queue, &item, 0) != pdTRUE) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (voice_chain_event_requires_current_lease(item.type)) {
            if (item.lease_generation == 0U ||
                item.lease_generation != s_voice.lease.generation) {
                ESP_LOGW(TAG,
                         "drop stale terminal voice event type=%d lease_generation=%lu current_lease=%lu",
                         (int)item.type,
                         (unsigned long)item.lease_generation,
                         (unsigned long)s_voice.lease.generation);
                continue;
            }
        } else if (item.generation != 0U && item.generation != s_voice.mic_generation) {
            ESP_LOGW(TAG,
                     "drop stale voice event type=%d event_generation=%lu mic_generation=%lu",
                     (int)item.type,
                     (unsigned long)item.generation,
                     (unsigned long)s_voice.mic_generation);
            continue;
        }
        switch (item.type) {
        case VOICE_CHAIN_EVENT_LOCAL_WAKE:
            voice_chain_handle_local_wake();
            app_stack_monitor_log(TAG, "voice_chain", "after_local_wake");
            break;
        case VOICE_CHAIN_EVENT_RECORDING_FINISHED:
            voice_chain_handle_recording_finished();
            app_stack_monitor_log(TAG, "voice_chain", "after_recording_finished");
            break;
        case VOICE_CHAIN_EVENT_SERVER_DONE:
            voice_chain_handle_server_done();
            app_stack_monitor_log(TAG, "voice_chain", "after_server_done");
            break;
        case VOICE_CHAIN_EVENT_SERVER_ERROR:
            voice_chain_handle_server_error(&item);
            app_stack_monitor_log(TAG, "voice_chain", "after_server_error");
            break;
        case VOICE_CHAIN_EVENT_GATEWAY_LINK_ABORT:
            voice_chain_handle_gateway_link_abort(&item);
            app_stack_monitor_log(TAG, "voice_chain", "after_gateway_link_abort");
            break;
        case VOICE_CHAIN_EVENT_RELEASE_RETRY:
            voice_chain_handle_release_retry(&item);
            app_stack_monitor_log(TAG, "voice_chain", "after_release_retry");
            break;
        default:
            ESP_LOGW(TAG, "ignore unknown voice event type=%d", (int)item.type);
            break;
        }
        (void)voice_chain_note_stack_high_water();
    }
}

esp_err_t voice_chain_start(void)
{
    if (s_voice.started) {
        return ESP_OK;
    }

    memset(&s_voice, 0, sizeof(s_voice));
    portENTER_CRITICAL(&s_terminal_event_lock);
    memset(&s_terminal_event, 0, sizeof(s_terminal_event));
    s_terminal_event_pending = false;
    portEXIT_CRITICAL(&s_terminal_event_lock);
    s_voice_chain_stack_high_water_bytes = 0;
    s_voice.state = VOICE_IDLE;
    voice_chain_log_heap("voice start", s_voice.state);

    ESP_LOGI(TAG, "voice backend=server_voice_turn");
    voice_chain_log_start_stage("local_wake_model_init", "before", ESP_ERR_NOT_FINISHED);
    esp_err_t ret = local_wake_word_init();
    voice_chain_log_start_stage("local_wake_model_init", "after", ret);
    if (ret != ESP_OK) {
        voice_chain_log_start_failure("local_wake_word_init", ret);
        return ret;
    }
    voice_chain_log_start_stage("c5_resource_manager_init", "before", ESP_ERR_NOT_FINISHED);
    ret = c5_resource_manager_init();
    voice_chain_log_start_stage("c5_resource_manager_init", "after", ret);
    if (ret != ESP_OK) {
        voice_chain_log_start_failure("c5_resource_manager_init", ret);
        return ret;
    }
    server_voice_client_config_t server_config = {
        .done_cb = voice_chain_server_done_sink,
        .done_ctx = NULL,
        .playback_start_cb = voice_chain_server_playback_start_sink,
        .playback_start_ctx = NULL,
        .error_cb = voice_chain_server_error_sink,
        .error_ctx = NULL,
    };
    voice_chain_log_start_stage("server_voice_client_init", "before", ESP_ERR_NOT_FINISHED);
    ret = server_voice_client_init(&server_config);
    voice_chain_log_start_stage("server_voice_client_init", "after", ret);
    if (ret != ESP_OK) {
        voice_chain_log_start_failure("server_voice_client_init", ret);
        return ret;
    }
    voice_chain_log_start_stage("gateway_voice_abort_callback", "before", ESP_ERR_NOT_FINISHED);
    gateway_link_set_voice_abort_callback(voice_chain_gateway_link_abort_cb);
    voice_chain_log_start_stage("gateway_voice_abort_callback", "after", ESP_OK);

    voice_chain_log_start_stage("voice_event_queue_create", "before", ESP_ERR_NOT_FINISHED);
    s_voice.event_queue = xQueueCreate(VOICE_CHAIN_QUEUE_DEPTH, sizeof(voice_chain_item_t));
    if (s_voice.event_queue == NULL) {
        ret = ESP_ERR_NO_MEM;
        voice_chain_log_start_stage("voice_event_queue_create", "after", ret);
        voice_chain_log_start_failure("xQueueCreate", ret);
        return ret;
    }
    voice_chain_log_start_stage("voice_event_queue_create", "after", ESP_OK);

    const mic_adc_voice_stream_ops_t server_voice_ops = {
        .prepare_cb = voice_chain_prepare_for_server_voice_start,
        .append_pcm_cb = voice_chain_server_voice_append_pcm,
        .finish_cb = voice_chain_server_voice_finish,
        .is_idle_cb = voice_chain_server_voice_is_idle,
        .is_ready_cb = voice_chain_server_voice_is_ready,
        .user_ctx = NULL,
        .stream_name = "server_voice",
    };
    voice_chain_log_start_stage("mic_voice_stream_ops_register", "before", ESP_ERR_NOT_FINISHED);
    mic_adc_test_set_voice_stream_ops(&server_voice_ops);
    voice_chain_log_start_stage("mic_voice_stream_ops_register", "after", ESP_OK);

    voice_chain_log_start_stage("voice_chain_task_create", "before", ESP_ERR_NOT_FINISHED);
    BaseType_t created = xTaskCreate(voice_chain_task,
                                     "voice_chain",
                                     VOICE_CHAIN_TASK_STACK,
                                     NULL,
                                     VOICE_CHAIN_TASK_PRIORITY,
                                     &s_voice.task);
    if (created != pdPASS) {
        ret = ESP_ERR_NO_MEM;
        voice_chain_log_start_stage("voice_chain_task_create", "after", ret);
        voice_chain_log_start_failure("xTaskCreate(voice_chain)", ret);
        voice_chain_cleanup_start_failure();
        return ret;
    }
    voice_chain_log_start_stage("voice_chain_task_create", "after", ESP_OK);

    s_voice.started = true;

    voice_chain_log_start_stage("mic_adc_vad_start", "before", ESP_ERR_NOT_FINISHED);
    ret = mic_adc_test_start();
    voice_chain_log_start_stage("mic_adc_vad_start", "after", ret);
    if (ret != ESP_OK) {
        voice_chain_log_start_failure("mic_adc_test_start", ret);
        voice_chain_cleanup_start_failure();
        voice_chain_set_state(VOICE_ERROR);
        return ret;
    }

    voice_chain_enter_listening_ready("start");
    return ESP_OK;
}

#include "voice_chain.h"

#include <stdbool.h>
#include <string.h>

#include "app_debug_config.h"
#include "app_runtime.h"
#include "app_stack_monitor.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "local_wake_word.h"
#include "mic_adc_test.h"
#include "server_voice_client.h"
#include "speaker_player.h"

static const char *TAG = "voice_chain";
static UBaseType_t s_voice_chain_stack_high_water_bytes;

typedef enum {
    VOICE_CHAIN_EVENT_LOCAL_WAKE = 0,
    VOICE_CHAIN_EVENT_SERVER_DONE,
    VOICE_CHAIN_EVENT_SERVER_ERROR,
} voice_chain_event_type_t;

typedef struct {
    voice_chain_event_type_t type;
    int error_code;
    char error_message[96];
} voice_chain_item_t;

typedef struct {
    QueueHandle_t event_queue;
    TaskHandle_t task;
    voice_chain_state_t state;
    bool started;
} voice_chain_context_t;

static voice_chain_context_t s_voice;

static esp_err_t voice_chain_pause_mic(void);
static void voice_chain_abort_round(const char *reason);
static esp_err_t voice_chain_queue_event(const voice_chain_item_t *item, const char *label);

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
    return ESP_OK;
}

static esp_err_t voice_chain_queue_local_wake_event(void)
{
    const voice_chain_item_t item = {
        .type = VOICE_CHAIN_EVENT_LOCAL_WAKE,
    };
    return voice_chain_queue_event(&item, "local_wake");
}

static esp_err_t voice_chain_server_done_sink(void *user_ctx)
{
    (void)user_ctx;
    const voice_chain_item_t item = {
        .type = VOICE_CHAIN_EVENT_SERVER_DONE,
    };
    return voice_chain_queue_event(&item, "server_done");
}

static esp_err_t voice_chain_server_error_sink(int code, const char *message, void *user_ctx)
{
    (void)user_ctx;
    voice_chain_item_t item = {
        .type = VOICE_CHAIN_EVENT_SERVER_ERROR,
        .error_code = code,
    };
    if (message != NULL && message[0] != '\0') {
        strlcpy(item.error_message, message, sizeof(item.error_message));
    }
    return voice_chain_queue_event(&item, "server_error");
}

static esp_err_t voice_chain_server_playback_start_sink(void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "server voice PCM playback start");
    voice_chain_set_state(VOICE_PLAYING);
    (void)local_wake_word_on_recording_finished();

    esp_err_t ret = voice_chain_pause_mic();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "pause Mic before server PCM playback failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = mic_adc_test_clear_audio_cache();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "clear Mic cache before server PCM playback failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static void voice_chain_enter_listening_ready(const char *reason)
{
    if (reason != NULL && reason[0] != '\0') {
        ESP_LOGD(TAG,
                 "voice listening ready reason=%s: Mic ADC/VAD and non-voice/BME active, server voice idle",
                 reason);
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
    esp_err_t ret = server_voice_client_cancel_turn();
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

static esp_err_t voice_chain_cleanup_mic_for_recover(const char *reason)
{
    ESP_LOGD(TAG,
             "voice recover cleanup Mic reason=%s",
             reason != NULL ? reason : "<none>");
    esp_err_t ret = voice_chain_pause_mic();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Mic pause during voice recover failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (!mic_adc_test_is_paused()) {
        return ESP_OK;
    }

    ret = mic_adc_test_clear_audio_cache();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Mic audio cache clear during voice recover failed: %s", esp_err_to_name(ret));
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

    ret = mic_adc_test_resume();
    if (ret != ESP_OK) {
        voice_chain_set_state(VOICE_ERROR);
        ESP_LOGE(TAG, "Mic ADC/VAD standby resume failed: %s", esp_err_to_name(ret));
        return ret;
    }

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

    esp_err_t ret = voice_chain_restart_mic_vad_standby(reason);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = app_runtime_resume_non_voice(reason);
    if (ret != ESP_OK) {
        voice_chain_set_state(VOICE_ERROR);
        ESP_LOGW(TAG, "non-voice/BME resume failed: %s", esp_err_to_name(ret));
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

    if (!local_wake_word_is_recording_window_open()) {
        if (s_voice.state != VOICE_LISTENING) {
            ESP_LOGW(TAG,
                     "local wake rejected: state=%s",
                     voice_chain_state_name(s_voice.state));
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "enter voice exclusive reason=local_wake");
        voice_chain_set_state(VOICE_WAKE_ACK);
        esp_err_t ret = app_runtime_pause_non_voice("local_wake");
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "non-voice pause failed before local wake ack: %s", esp_err_to_name(ret));
            (void)voice_chain_server_error_sink(ret, "non-voice pause failed", NULL);
            return ret;
        }
        ESP_LOGI(TAG, "non-voice paused reason=local_wake");

        ret = server_voice_client_prepare_async();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "server voice prepare failed: %s", esp_err_to_name(ret));
            (void)voice_chain_server_error_sink(ret, "server voice prepare failed", NULL);
            return ret;
        }

        ret = mic_adc_test_pause();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Mic pause request before local wake ack failed: %s", esp_err_to_name(ret));
            (void)voice_chain_server_error_sink(ret, "mic pause request failed", NULL);
            return ret;
        }
        (void)voice_chain_queue_local_wake_event();
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
    esp_err_t ret = server_voice_client_start_turn();
    voice_chain_log_heap("server voice turn start after", s_voice.state);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "server voice turn start failed: %s", esp_err_to_name(ret));
        (void)voice_chain_server_error_sink(ret, "server voice turn start failed", NULL);
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
    esp_err_t ret = server_voice_client_append_pcm(pcm, samples);
    if (ret != ESP_OK) {
        (void)voice_chain_server_error_sink(ret, "server voice PCM upload failed", NULL);
    }
    return ret;
}

static esp_err_t voice_chain_server_voice_finish(void *user_ctx)
{
    (void)user_ctx;
    voice_chain_set_state(VOICE_WAITING_RESPONSE);
    voice_chain_log_heap("server voice upload finish before", s_voice.state);
    esp_err_t ret = server_voice_client_finish_turn();
    voice_chain_log_heap("server voice upload finish after", s_voice.state);
    if (ret != ESP_OK) {
        (void)voice_chain_server_error_sink(ret, "server voice upload finish failed", NULL);
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

static void voice_chain_handle_local_wake(void)
{
    if (s_voice.state != VOICE_WAKE_ACK) {
        ESP_LOGW(TAG,
                 "ignore local wake event outside wake ack state=%s",
                 voice_chain_state_name(s_voice.state));
        return;
    }

    esp_err_t ret = mic_adc_test_wait_paused(VOICE_MIC_PAUSE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Mic pause wait before local wake ack failed: %s", esp_err_to_name(ret));
        voice_chain_abort_round("local_wake_mic_pause_fail");
        return;
    }

    ret = mic_adc_test_clear_audio_cache();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Mic cache clear before local wake ack failed: %s", esp_err_to_name(ret));
        voice_chain_abort_round("local_wake_cache_clear_fail");
        return;
    }

    ESP_LOGI(TAG, "local wake ack playback and server prepare window");
    ret = local_wake_word_on_local_wake_detected();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "local wake ack playback failed: %s", esp_err_to_name(ret));
        voice_chain_abort_round("local_wake_ack_fail");
        return;
    }

    voice_chain_set_state(VOICE_RECORDING);
    ret = mic_adc_test_resume();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Mic resume for server voice recording failed: %s", esp_err_to_name(ret));
        voice_chain_abort_round("server_voice_recording_resume_fail");
        return;
    }
    ESP_LOGI(TAG, "server voice recording window ready");
}

static void voice_chain_handle_server_done(void)
{
    ESP_LOGI(TAG, "server voice turn done");
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

static void voice_chain_task(void *arg)
{
    (void)arg;
    app_stack_monitor_log(TAG, "voice_chain", "entry");

    while (1) {
        (void)voice_chain_note_stack_high_water();
        voice_chain_item_t item = {0};
        if (xQueueReceive(s_voice.event_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (item.type) {
        case VOICE_CHAIN_EVENT_LOCAL_WAKE:
            voice_chain_handle_local_wake();
            app_stack_monitor_log(TAG, "voice_chain", "after_local_wake");
            break;
        case VOICE_CHAIN_EVENT_SERVER_DONE:
            voice_chain_handle_server_done();
            app_stack_monitor_log(TAG, "voice_chain", "after_server_done");
            break;
        case VOICE_CHAIN_EVENT_SERVER_ERROR:
            voice_chain_handle_server_error(&item);
            app_stack_monitor_log(TAG, "voice_chain", "after_server_error");
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
    s_voice_chain_stack_high_water_bytes = 0;
    s_voice.state = VOICE_IDLE;
    voice_chain_log_heap("voice start", s_voice.state);

    ESP_LOGI(TAG, "voice backend=server_voice_turn");
    esp_err_t ret = local_wake_word_init();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = audio_player_init();
    if (ret != ESP_OK) {
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
    ret = server_voice_client_init(&server_config);
    if (ret != ESP_OK) {
        return ret;
    }

    voice_chain_log_heap("voice start before event queue create", s_voice.state);
    s_voice.event_queue = xQueueCreate(VOICE_CHAIN_QUEUE_DEPTH, sizeof(voice_chain_item_t));
    voice_chain_log_heap("voice start after event queue create", s_voice.state);
    if (s_voice.event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const mic_adc_voice_stream_ops_t server_voice_ops = {
        .prepare_cb = voice_chain_prepare_for_server_voice_start,
        .append_pcm_cb = voice_chain_server_voice_append_pcm,
        .finish_cb = voice_chain_server_voice_finish,
        .is_idle_cb = voice_chain_server_voice_is_idle,
        .is_ready_cb = voice_chain_server_voice_is_ready,
        .user_ctx = NULL,
        .stream_name = "server_voice",
    };
    mic_adc_test_set_voice_stream_ops(&server_voice_ops);

    voice_chain_log_heap("voice start before voice task create", s_voice.state);
    BaseType_t created = xTaskCreate(voice_chain_task,
                                     "voice_chain",
                                     VOICE_CHAIN_TASK_STACK,
                                     NULL,
                                     VOICE_CHAIN_TASK_PRIORITY,
                                     &s_voice.task);
    voice_chain_log_heap("voice start after voice task create", s_voice.state);
    if (created != pdPASS) {
        voice_chain_cleanup_start_failure();
        return ESP_ERR_NO_MEM;
    }

    s_voice.started = true;

    voice_chain_log_heap("voice start before Mic ADC/VAD standby", s_voice.state);
    ret = mic_adc_test_start();
    voice_chain_log_heap("voice start after Mic ADC/VAD standby", s_voice.state);
    if (ret != ESP_OK) {
        voice_chain_cleanup_start_failure();
        voice_chain_set_state(VOICE_ERROR);
        return ret;
    }

    voice_chain_enter_listening_ready("start");
    return ESP_OK;
}

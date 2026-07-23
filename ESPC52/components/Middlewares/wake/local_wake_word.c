#include "local_wake_word.h"

#include <stddef.h>

#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "speaker_player.h"

static const char *TAG = "remote_wake_gate";
static bool s_recording_window_open;
static bool s_prompt_active;
static bool s_prompt_task_ready;
static uint32_t s_prompt_generation;
static local_wake_prompt_done_cb_t s_prompt_done_callback;
static void *s_prompt_done_context;
static TaskHandle_t s_prompt_task;
static StaticTask_t s_prompt_task_storage;

#define LOCAL_WAKE_PROMPT_TASK_STACK_BYTES 4096U
#define LOCAL_WAKE_PROMPT_TASK_STACK_WORDS \
    ((LOCAL_WAKE_PROMPT_TASK_STACK_BYTES + sizeof(StackType_t) - 1U) / sizeof(StackType_t))
static StackType_t s_prompt_task_stack[LOCAL_WAKE_PROMPT_TASK_STACK_WORDS];
static portMUX_TYPE s_prompt_lock = portMUX_INITIALIZER_UNLOCKED;

extern const uint8_t _binary_wake_ack_zh_pcm_start[] asm("_binary_wake_ack_zh_pcm_start");
extern const uint8_t _binary_wake_ack_zh_pcm_end[] asm("_binary_wake_ack_zh_pcm_end");

static void local_wake_prompt_task(void *arg)
{
    (void)arg;
    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint32_t generation = 0U;
        local_wake_prompt_done_cb_t callback = NULL;
        void *callback_context = NULL;
        portENTER_CRITICAL(&s_prompt_lock);
        generation = s_prompt_generation;
        callback = s_prompt_done_callback;
        callback_context = s_prompt_done_context;
        portEXIT_CRITICAL(&s_prompt_lock);

        const size_t pcm_bytes = (size_t)(_binary_wake_ack_zh_pcm_end -
                                          _binary_wake_ack_zh_pcm_start);
        const uint32_t samples = (uint32_t)(pcm_bytes / sizeof(int16_t));
        const int64_t started_ms = esp_timer_get_time() / 1000;
        esp_err_t result = ESP_ERR_INVALID_SIZE;
        if (pcm_bytes != 0U && (pcm_bytes % sizeof(int16_t)) == 0U) {
            ESP_LOGI(TAG,
                     "WAKE_PROMPT_PLAYBACK_BEGIN prompt_id=local_wake_ack sample_rate=%u pcm_bytes=%u",
                     (unsigned int)LOCAL_WAKE_ACK_SAMPLE_RATE_HZ,
                     (unsigned int)pcm_bytes);
            result = audio_player_play_16k_pcm((const int16_t *)_binary_wake_ack_zh_pcm_start,
                                                samples,
                                                (int)LOCAL_WAKE_ACK_SAMPLE_RATE_HZ);
        }
        const uint32_t duration_ms = (uint32_t)((esp_timer_get_time() / 1000) - started_ms);
        if (result == ESP_OK) {
            ESP_LOGI(TAG,
                     "WAKE_PROMPT_PLAYBACK_END prompt_id=local_wake_ack duration_ms=%u",
                     (unsigned int)duration_ms);
        } else {
            ESP_LOGW(TAG,
                     "WAKE_PROMPT_FAILED prompt_id=local_wake_ack reason=%s fallback=continue_command_capture",
                     esp_err_to_name(result));
        }

        portENTER_CRITICAL(&s_prompt_lock);
        const bool current = s_prompt_active && generation == s_prompt_generation;
        if (current) {
            s_prompt_active = false;
        }
        portEXIT_CRITICAL(&s_prompt_lock);
        if (current && callback != NULL) {
            callback(generation, result, duration_ms, callback_context);
        }
    }
}

esp_err_t local_wake_word_init(void)
{
    s_recording_window_open = false;
    if (!s_prompt_task_ready) {
        s_prompt_task = xTaskCreateStatic(local_wake_prompt_task,
                                          "wake_prompt",
                                          LOCAL_WAKE_PROMPT_TASK_STACK_WORDS,
                                          NULL,
                                          4U,
                                          s_prompt_task_stack,
                                          &s_prompt_task_storage);
        if (s_prompt_task == NULL) {
            return ESP_ERR_NO_MEM;
        }
        s_prompt_task_ready = true;
    }
    ESP_LOGI(TAG, "C5 local VAD is the PCM gate; S3 owns wake detection");
    return ESP_OK;
}

bool local_wake_word_should_record_after_vad_start(void) { return true; }

esp_err_t local_wake_word_on_local_wake_detected(void)
{
    return ESP_ERR_INVALID_STATE;
}

void local_wake_word_set_prompt_done_callback(local_wake_prompt_done_cb_t callback,
                                              void *user_ctx)
{
    portENTER_CRITICAL(&s_prompt_lock);
    s_prompt_done_callback = callback;
    s_prompt_done_context = user_ctx;
    portEXIT_CRITICAL(&s_prompt_lock);
}

esp_err_t local_wake_word_start_prompt_async(uint32_t generation)
{
    if (!s_prompt_task_ready || s_prompt_task == NULL || generation == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_prompt_lock);
    if (s_prompt_active) {
        portEXIT_CRITICAL(&s_prompt_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_prompt_active = true;
    s_prompt_generation = generation;
    portEXIT_CRITICAL(&s_prompt_lock);

    ESP_LOGI(TAG, "WAKE_PROMPT_REQUEST prompt_id=local_wake_ack generation=%lu",
             (unsigned long)generation);
    xTaskNotifyGive(s_prompt_task);
    return ESP_OK;
}
esp_err_t local_wake_word_open_recording_window(void) { s_recording_window_open = true; return ESP_OK; }
esp_err_t local_wake_word_on_recording_finished(void) { s_recording_window_open = false; return ESP_OK; }
void local_wake_word_cancel_recording_window(void) { s_recording_window_open = false; }
bool local_wake_word_is_recording_window_open(void) { return s_recording_window_open; }
bool local_wake_word_is_ack_active(void)
{
    bool active;
    portENTER_CRITICAL(&s_prompt_lock);
    active = s_prompt_active;
    portEXIT_CRITICAL(&s_prompt_lock);
    return active;
}

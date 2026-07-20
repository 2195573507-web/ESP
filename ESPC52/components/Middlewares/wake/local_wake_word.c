#include "local_wake_word.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "app_debug_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "model_path.h"
#include "speaker_player.h"
#include "server_comm_errors.h"
#include "wake_prompt_cache.h"

static const char *TAG = "local_wake_word";
static const char *LOCAL_WAKE_MODEL_PARTITION = "model";
static const char *LOCAL_WAKE_MODEL_KEYWORD = "nihaoxiaozhi";

#define LOCAL_WAKE_FALLBACK_BEEP_SAMPLES 1600U
#define LOCAL_WAKE_FALLBACK_BEEP_HALF_PERIOD_SAMPLES 8U
#define LOCAL_WAKE_FALLBACK_BEEP_AMPLITUDE 1200

static portMUX_TYPE s_wake_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static bool s_detector_ready;
static bool s_detection_latched;
static bool s_recording_window_open;
static bool s_ack_active;
static TickType_t s_record_after_tick;
static srmodel_list_t *s_models;
static const esp_wn_iface_t *s_wakenet;
static model_iface_data_t *s_model_data;
static size_t s_wakenet_chunk_samples;
static int16_t s_fallback_beep[LOCAL_WAKE_FALLBACK_BEEP_SAMPLES];
static bool s_fallback_beep_ready;

static void local_wake_word_log_heap(const char *label)
{
#if ENABLE_VERBOSE_AUDIO_LOG
    ESP_LOGI(TAG,
             "%s free_heap=%u min_free_heap=%u largest_free_block=%u",
             label != NULL ? label : "wake heap",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#else
    (void)label;
#endif
}

static void local_wake_word_mark_detector_unready(void)
{
    portENTER_CRITICAL(&s_wake_lock);
    s_detector_ready = false;
    s_detection_latched = false;
    s_wakenet_chunk_samples = 0;
    portEXIT_CRITICAL(&s_wake_lock);
}

static void local_wake_word_reset_runtime_flags(void)
{
    portENTER_CRITICAL(&s_wake_lock);
    s_recording_window_open = false;
    s_ack_active = false;
    s_record_after_tick = 0;
    s_detection_latched = false;
    portEXIT_CRITICAL(&s_wake_lock);
}

static void local_wake_word_log_available_models(const srmodel_list_t *models)
{
    if (models == NULL) {
        ESP_LOGW(TAG, "WakeNet model list is NULL");
        return;
    }
#if ENABLE_VERBOSE_AUDIO_LOG
    ESP_LOGI(TAG, "WakeNet model list count=%d", models->num);
    for (int i = 0; i < models->num; i++) {
        const char *name = models->model_name != NULL ? models->model_name[i] : NULL;
        const char *info = models->model_info != NULL ? models->model_info[i] : NULL;
        ESP_LOGI(TAG,
                 "WakeNet model[%d] name=%s info=%s",
                 i,
                 name != NULL ? name : "<null>",
                 info != NULL ? info : "<null>");
    }
#endif
}

static esp_err_t local_wake_word_play_ack(void)
{
    esp_err_t cache_ret = wake_prompt_cache_play();
    if (cache_ret == ESP_OK) {
        return ESP_OK;
    }
    ESP_LOGW(TAG,
             "wake prompt cache fallback short_beep reason=%s",
             server_comm_err_to_name(cache_ret));

    ESP_LOGI(TAG, "WAKE_ACK_FALLBACK_CLEANUP before_short_beep");
    esp_err_t release_ret = audio_player_release_session(3000U);
    if (release_ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "WAKE_ACK_FALLBACK_CLEANUP failed ret=%s",
                 esp_err_to_name(release_ret));
        return release_ret;
    }

    if (!s_fallback_beep_ready) {
        for (uint32_t i = 0; i < LOCAL_WAKE_FALLBACK_BEEP_SAMPLES; i++) {
            bool high = ((i / LOCAL_WAKE_FALLBACK_BEEP_HALF_PERIOD_SAMPLES) % 2U) == 0;
            s_fallback_beep[i] = high ? LOCAL_WAKE_FALLBACK_BEEP_AMPLITUDE :
                                 -LOCAL_WAKE_FALLBACK_BEEP_AMPLITUDE;
        }
        s_fallback_beep_ready = true;
    }

    ESP_LOGI(TAG,
             "local wake short beep playback start samples=%u",
             (unsigned int)LOCAL_WAKE_FALLBACK_BEEP_SAMPLES);
    esp_err_t ret = audio_player_play_16k_pcm(s_fallback_beep,
                                              LOCAL_WAKE_FALLBACK_BEEP_SAMPLES,
                                              LOCAL_WAKE_ACK_SAMPLE_RATE_HZ);
    ESP_LOGI(TAG, "local wake short beep playback done ret=%s", esp_err_to_name(ret));
    return ret;
}

esp_err_t local_wake_word_init(void)
{
    local_wake_word_reset_runtime_flags();
    local_wake_word_mark_detector_unready();
    local_wake_word_log_heap("WakeNet init before model load");

    s_models = esp_srmodel_init(LOCAL_WAKE_MODEL_PARTITION);
    if (s_models == NULL || s_models->num <= 0) {
        ESP_LOGE(TAG,
                 "WakeNet model init failed partition=%s; ordinary VAD will not start server voice",
                 LOCAL_WAKE_MODEL_PARTITION);
        local_wake_word_log_available_models(s_models);
        portENTER_CRITICAL(&s_wake_lock);
        s_initialized = true;
        portEXIT_CRITICAL(&s_wake_lock);
        return ESP_OK;
    }
    local_wake_word_log_available_models(s_models);

    char *model_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, LOCAL_WAKE_MODEL_KEYWORD);
    if (model_name == NULL) {
        ESP_LOGE(TAG,
                 "WakeNet model for fixed phrase nihaoxiaozhi not found; ordinary VAD will not start server voice");
        portENTER_CRITICAL(&s_wake_lock);
        s_initialized = true;
        portEXIT_CRITICAL(&s_wake_lock);
        return ESP_OK;
    }

    s_wakenet = esp_wn_handle_from_name(model_name);
    if (s_wakenet == NULL || s_wakenet->create == NULL) {
        ESP_LOGE(TAG, "WakeNet handle missing model=%s", model_name);
        portENTER_CRITICAL(&s_wake_lock);
        s_initialized = true;
        portEXIT_CRITICAL(&s_wake_lock);
        return ESP_OK;
    }

    s_model_data = s_wakenet->create(model_name, DET_MODE_95);
    if (s_model_data == NULL) {
        ESP_LOGE(TAG, "WakeNet create failed model=%s", model_name);
        local_wake_word_log_heap("WakeNet create failed");
        return ESP_ERR_NO_MEM;
    }

    int chunk_samples = s_wakenet->get_samp_chunksize != NULL ?
                        s_wakenet->get_samp_chunksize(s_model_data) : 0;
    int sample_rate = s_wakenet->get_samp_rate != NULL ?
                      s_wakenet->get_samp_rate(s_model_data) : 0;
    int channels = s_wakenet->get_channel_num != NULL ?
                   s_wakenet->get_channel_num(s_model_data) : 1;
    if (chunk_samples <= 0 || sample_rate != (int)LOCAL_WAKE_ACK_SAMPLE_RATE_HZ || channels != 1) {
        ESP_LOGE(TAG,
                 "WakeNet format mismatch model=%s chunk=%d sample_rate=%d channels=%d",
                 model_name,
                 chunk_samples,
                 sample_rate,
                 channels);
        if (s_wakenet->destroy != NULL) {
            s_wakenet->destroy(s_model_data);
        }
        s_model_data = NULL;
        portENTER_CRITICAL(&s_wake_lock);
        s_initialized = true;
        portEXIT_CRITICAL(&s_wake_lock);
        return ESP_OK;
    }

    char *wake_words = esp_srmodel_get_wake_words(s_models, model_name);
    ESP_LOGI(TAG, "WakeNet model load success model=%s", model_name);
    portENTER_CRITICAL(&s_wake_lock);
    s_initialized = true;
    s_detector_ready = true;
    s_detection_latched = false;
    s_wakenet_chunk_samples = (size_t)chunk_samples;
    portEXIT_CRITICAL(&s_wake_lock);

    ESP_LOGI(TAG,
             "WakeNet ready phrase=nihaoxiaozhi model=%s wake_words=%s chunk_samples=%u sample_rate=%d",
             model_name,
             wake_words != NULL ? wake_words : "<unknown>",
             (unsigned int)s_wakenet_chunk_samples,
             sample_rate);
    free(wake_words);
    local_wake_word_log_heap("WakeNet init after create");
    return ESP_OK;
}

size_t local_wake_word_get_chunk_samples(void)
{
    size_t chunk_samples = 0;
    portENTER_CRITICAL(&s_wake_lock);
    chunk_samples = s_wakenet_chunk_samples;
    portEXIT_CRITICAL(&s_wake_lock);
    return chunk_samples;
}

bool local_wake_word_is_detection_ready(void)
{
    bool ready = false;
    portENTER_CRITICAL(&s_wake_lock);
    ready = s_initialized && s_detector_ready && !s_detection_latched;
    portEXIT_CRITICAL(&s_wake_lock);
    return ready;
}

esp_err_t local_wake_word_detect_chunk(const int16_t *pcm, size_t samples, bool *detected)
{
    if (detected != NULL) {
        *detected = false;
    }
    if (pcm == NULL || detected == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool ready = false;
    bool latched = false;
    size_t expected_samples = 0;
    portENTER_CRITICAL(&s_wake_lock);
    ready = s_initialized && s_detector_ready;
    latched = s_detection_latched;
    expected_samples = s_wakenet_chunk_samples;
    portEXIT_CRITICAL(&s_wake_lock);

    if (!ready || latched) {
        return ESP_ERR_INVALID_STATE;
    }
    if (samples != expected_samples) {
        ESP_LOGW(TAG,
                 "WakeNet chunk size mismatch got=%u expected=%u",
                 (unsigned int)samples,
                 (unsigned int)expected_samples);
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_wakenet == NULL || s_model_data == NULL || s_wakenet->detect == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    wakenet_state_t state = s_wakenet->detect(s_model_data, (int16_t *)pcm);
    if (state == WAKENET_DETECTED) {
        int channel = s_wakenet->get_triggered_channel != NULL ?
                      s_wakenet->get_triggered_channel(s_model_data) : -1;
        portENTER_CRITICAL(&s_wake_lock);
        s_detection_latched = true;
        portEXIT_CRITICAL(&s_wake_lock);
        *detected = true;
        ESP_LOGI(TAG, "WakeNet detected fixed phrase nihaoxiaozhi channel=%d", channel);
    } else if (state != WAKENET_NO_DETECT && state != WAKENET_CHANNEL_VERIFIED) {
        ESP_LOGW(TAG, "WakeNet returned unexpected state=%d", (int)state);
    }
    return ESP_OK;
}

void local_wake_word_reset_detector(void)
{
    /* This ESP-SR WN9S build can crash inside clean(); reset only our wrapper latch. */
    portENTER_CRITICAL(&s_wake_lock);
    s_detection_latched = false;
    portEXIT_CRITICAL(&s_wake_lock);
}

bool local_wake_word_should_record_after_vad_start(void)
{
    bool should_record = false;
    portENTER_CRITICAL(&s_wake_lock);
    TickType_t now = xTaskGetTickCount();
    should_record = s_initialized &&
                    s_recording_window_open &&
                    !s_ack_active &&
                    (s_record_after_tick == 0 ||
                     (int32_t)(now - s_record_after_tick) >= 0);
    portEXIT_CRITICAL(&s_wake_lock);
    return should_record;
}

esp_err_t local_wake_word_on_local_wake_detected(void)
{
    portENTER_CRITICAL(&s_wake_lock);
    if (s_ack_active) {
        portEXIT_CRITICAL(&s_wake_lock);
        ESP_LOGW(TAG, "WAKE_ACK_PLAYBACK_REJECTED reason=already_active");
        return ESP_ERR_INVALID_STATE;
    }
    s_recording_window_open = false;
    s_ack_active = true;
    s_record_after_tick = 0;
    portEXIT_CRITICAL(&s_wake_lock);
    ESP_LOGI(TAG, "WAKE_ACK_PLAYBACK_START");
    esp_err_t ret = local_wake_word_play_ack();
    portENTER_CRITICAL(&s_wake_lock);
    s_ack_active = false;
    portEXIT_CRITICAL(&s_wake_lock);
    ESP_LOGI(TAG, "WAKE_ACK_PLAYBACK_DONE ret=%s", esp_err_to_name(ret));
    return ret;
}

esp_err_t local_wake_word_open_recording_window(void)
{
    portENTER_CRITICAL(&s_wake_lock);
    if (!s_initialized || s_ack_active) {
        portEXIT_CRITICAL(&s_wake_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_recording_window_open = true;
    s_record_after_tick = xTaskGetTickCount() +
                          pdMS_TO_TICKS(LOCAL_WAKE_RECORD_DELAY_AFTER_ACK_MS);
    portEXIT_CRITICAL(&s_wake_lock);
    ESP_LOGI(TAG, "RECORDING_WINDOW_OPEN");
    return ESP_OK;
}

esp_err_t local_wake_word_on_recording_finished(void)
{
    portENTER_CRITICAL(&s_wake_lock);
    s_recording_window_open = false;
    s_ack_active = false;
    s_record_after_tick = 0;
    s_detection_latched = false;
    portEXIT_CRITICAL(&s_wake_lock);
    local_wake_word_reset_detector();
    ESP_LOGI(TAG, "local wake recording window closed");
    return ESP_OK;
}

void local_wake_word_cancel_recording_window(void)
{
    portENTER_CRITICAL(&s_wake_lock);
    s_recording_window_open = false;
    s_ack_active = false;
    s_record_after_tick = 0;
    s_detection_latched = false;
    portEXIT_CRITICAL(&s_wake_lock);
    local_wake_word_reset_detector();
    ESP_LOGI(TAG, "local wake recording window canceled");
}

bool local_wake_word_is_recording_window_open(void)
{
    bool open = false;
    portENTER_CRITICAL(&s_wake_lock);
    open = s_recording_window_open;
    portEXIT_CRITICAL(&s_wake_lock);
    return open;
}

bool local_wake_word_is_ack_active(void)
{
    bool active = false;
    portENTER_CRITICAL(&s_wake_lock);
    active = s_ack_active;
    portEXIT_CRITICAL(&s_wake_lock);
    return active;
}

#include "app_runtime.h"

#include "app_debug_config.h"
#include "bme_sensor_service.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static const char *TAG = "app_runtime";

static portMUX_TYPE s_app_runtime_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_non_voice_paused;

static void app_runtime_log_heap(const char *label, const char *reason)
{
#if ENABLE_VERBOSE_AUDIO_LOG
    ESP_LOGI(TAG,
             "%s reason=%s free_heap=%u min_free_heap=%u largest_free_block=%u",
             label != NULL ? label : "app runtime",
             reason != NULL ? reason : "<none>",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#else
    (void)label;
    (void)reason;
#endif
}

esp_err_t app_runtime_pause_non_voice(const char *reason)
{
    bool first_pause = false;

    portENTER_CRITICAL(&s_app_runtime_lock);
    if (!s_non_voice_paused) {
        s_non_voice_paused = true;
        first_pause = true;
    }
    portEXIT_CRITICAL(&s_app_runtime_lock);

    if (first_pause) {
        app_runtime_log_heap("non-voice pause begin", reason);
    }

    bme_sensor_service_pause();
    esp_err_t ret = bme_sensor_service_wait_paused(APP_RUNTIME_NON_VOICE_PAUSE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "non-voice pause wait failed reason=%s ret=%s",
                 reason != NULL ? reason : "<none>",
                 esp_err_to_name(ret));
        return ret;
    }

    if (first_pause) {
        app_runtime_log_heap("non-voice paused", reason);
    }
    return ESP_OK;
}

esp_err_t app_runtime_resume_non_voice(const char *reason)
{
    bool should_resume = false;

    portENTER_CRITICAL(&s_app_runtime_lock);
    if (s_non_voice_paused) {
        s_non_voice_paused = false;
        should_resume = true;
    }
    portEXIT_CRITICAL(&s_app_runtime_lock);

    if (!should_resume) {
        return ESP_OK;
    }

    app_runtime_log_heap("non-voice resume begin", reason);
    bme_sensor_service_resume();
    app_runtime_log_heap("non-voice resumed", reason);
    return ESP_OK;
}

bool app_runtime_non_voice_is_paused(void)
{
    bool paused;

    portENTER_CRITICAL(&s_app_runtime_lock);
    paused = s_non_voice_paused;
    portEXIT_CRITICAL(&s_app_runtime_lock);

    return paused;
}

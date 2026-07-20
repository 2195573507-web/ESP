/**
 * @file app_runtime.c
 * @brief C5 终端语音独占期间的非语音资源 gate。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），由 voice_chain 调用，用于在
 * voice turn 前暂停 BME、heartbeat、command poll、status upload 和普通本地 HTTP，
 * 播放结束后恢复。语音独占模式的目的，是避免语音长连接期间普通轮询打满
 * socket/HTTP server，保证录音、等待响应、播放过程稳定。它不暂停 WiFi、
 * WakeNet/VAD、Mic、speaker 或 server_voice_client，也不改变各模块的协议参数。
 */

#include "app_runtime.h"

#include "app_debug_config.h"
#include "bme_sensor_service.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "server_comm_http.h"

static const char *TAG = "app_runtime";

static portMUX_TYPE s_app_runtime_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_non_voice_paused;
static int64_t s_last_voice_busy_log_ms;

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

esp_err_t app_runtime_pause_non_voice_timed(const char *reason,
                                            uint32_t http_timeout_ms,
                                            uint32_t bme_timeout_ms)
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

    server_comm_http_set_non_voice_paused(true);
    esp_err_t ret = server_comm_http_wait_for_non_voice_idle(http_timeout_ms);
    if (ret != ESP_OK) {
        server_comm_http_set_non_voice_paused(false);
        portENTER_CRITICAL(&s_app_runtime_lock);
        s_non_voice_paused = false;
        portEXIT_CRITICAL(&s_app_runtime_lock);
        ESP_LOGW(TAG,
                 "non-voice HTTP quiesce failed reason=%s ret=%s",
                 reason != NULL ? reason : "<none>",
                 esp_err_to_name(ret));
        return ret;
    }
    bme_sensor_service_pause();
    ret = bme_sensor_service_wait_paused(bme_timeout_ms);
    if (ret != ESP_OK) {
        bme_sensor_service_resume();
        server_comm_http_set_non_voice_paused(false);
        portENTER_CRITICAL(&s_app_runtime_lock);
        s_non_voice_paused = false;
        portEXIT_CRITICAL(&s_app_runtime_lock);
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

esp_err_t app_runtime_pause_non_voice(const char *reason)
{
    return app_runtime_pause_non_voice_timed(reason,
                                             APP_RUNTIME_NON_VOICE_PAUSE_TIMEOUT_MS,
                                             APP_RUNTIME_NON_VOICE_PAUSE_TIMEOUT_MS);
}

esp_err_t app_runtime_resume_non_voice_bme(const char *reason)
{
    if (!app_runtime_non_voice_is_paused()) {
        return ESP_OK;
    }

    app_runtime_log_heap("non-voice resume begin", reason);
    bme_sensor_service_resume();
    app_runtime_log_heap("non-voice BME resumed", reason);
    return ESP_OK;
}

esp_err_t app_runtime_finish_non_voice_resume(const char *reason)
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

    server_comm_http_set_non_voice_paused(false);
    app_runtime_log_heap("non-voice resumed", reason);
    return ESP_OK;
}

esp_err_t app_runtime_resume_non_voice(const char *reason)
{
    esp_err_t ret = app_runtime_resume_non_voice_bme(reason);
    if (ret != ESP_OK) {
        return ret;
    }
    return app_runtime_finish_non_voice_resume(reason);
}

bool app_runtime_non_voice_is_paused(void)
{
    bool paused;

    portENTER_CRITICAL(&s_app_runtime_lock);
    paused = s_non_voice_paused;
    portEXIT_CRITICAL(&s_app_runtime_lock);

    return paused;
}

void app_runtime_log_voice_busy_skip(const char *task_name)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    bool should_log = false;

    portENTER_CRITICAL(&s_app_runtime_lock);
    if (s_last_voice_busy_log_ms == 0 ||
        now_ms - s_last_voice_busy_log_ms >= (int64_t)APP_RUNTIME_VOICE_BUSY_LOG_INTERVAL_MS) {
        s_last_voice_busy_log_ms = now_ms;
        should_log = true;
    }
    portEXIT_CRITICAL(&s_app_runtime_lock);

    if (should_log) {
        ESP_LOGI(TAG,
                 "voice busy, skip non-voice task%s%s",
                 task_name != NULL && task_name[0] != '\0' ? ": " : "",
                 task_name != NULL && task_name[0] != '\0' ? task_name : "");
    }
}

bool app_runtime_should_skip_non_voice_task(const char *task_name)
{
    if (!app_runtime_non_voice_is_paused()) {
        return false;
    }

    app_runtime_log_voice_busy_skip(task_name);
    return true;
}

#include "local_wake_word.h"

#include "esp_log.h"

/* Compatibility hooks preserve the existing voice-chain lease while the S3
 * audio pipeline owns the only wake detector. */
static const char *TAG = "remote_wake_gate";
static bool s_recording_window_open;

esp_err_t local_wake_word_init(void)
{
    s_recording_window_open = false;
    ESP_LOGI(TAG, "C5 local VAD is the PCM gate; S3 owns wake detection");
    return ESP_OK;
}

bool local_wake_word_should_record_after_vad_start(void) { return true; }
esp_err_t local_wake_word_on_local_wake_detected(void) { return ESP_OK; }
esp_err_t local_wake_word_open_recording_window(void) { s_recording_window_open = true; return ESP_OK; }
esp_err_t local_wake_word_on_recording_finished(void) { s_recording_window_open = false; return ESP_OK; }
void local_wake_word_cancel_recording_window(void) { s_recording_window_open = false; }
bool local_wake_word_is_recording_window_open(void) { return s_recording_window_open; }
bool local_wake_word_is_ack_active(void) { return false; }

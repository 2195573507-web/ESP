/**
 * @file c5_resource_manager.c
 * @brief Single-owner voice lease and bounded background-resource arbitration.
 */

#include "c5_resource_manager.h"

#include <string.h>

#include "app_runtime.h"
#include "c5_runtime_workers.h"
#include "csi_service.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "server_voice_client.h"
#include "speaker_player.h"

static const char *TAG = "c5_resource";

typedef struct {
    bool initialized;
    c5_resource_state_t state;
    uint32_t generation;
    uint32_t pending_ack_mask;
    bool lease_valid;
    c5_audio_phase_t audio_phase;
    char owner[24];
    int64_t transition_started_ms;
} c5_resource_context_t;

static c5_resource_context_t s_resource = {
    .state = C5_RESOURCE_STATE_STANDBY,
};
static portMUX_TYPE s_resource_lock = portMUX_INITIALIZER_UNLOCKED;

static int64_t c5_resource_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

const char *c5_resource_manager_audio_phase_name(c5_audio_phase_t phase)
{
    switch (phase) {
    case C5_AUDIO_PHASE_NONE: return "NONE";
    case C5_AUDIO_PHASE_MIC_LISTENING_ACTIVE: return "MIC_LISTENING_ACTIVE";
    case C5_AUDIO_PHASE_MIC_PAUSE_REQUESTED: return "MIC_PAUSE_REQUESTED";
    case C5_AUDIO_PHASE_MIC_DMA_RELEASED: return "MIC_DMA_RELEASED";
    case C5_AUDIO_PHASE_SPEAKER_TX_OWNED: return "SPEAKER_TX_OWNED";
    case C5_AUDIO_PHASE_SPEAKER_TX_RELEASED: return "SPEAKER_TX_RELEASED";
    case C5_AUDIO_PHASE_MIC_RECORD_READY: return "MIC_RECORD_READY";
    default: return "UNKNOWN";
    }
}

const char *c5_resource_manager_state_name(c5_resource_state_t state)
{
    switch (state) {
    case C5_RESOURCE_STATE_STANDBY:
        return "STANDBY";
    case C5_RESOURCE_STATE_QUIESCING:
        return "QUIESCING";
    case C5_RESOURCE_STATE_VOICE_EXCLUSIVE:
        return "VOICE_EXCLUSIVE";
    case C5_RESOURCE_STATE_RELEASING:
        return "RELEASING";
    case C5_RESOURCE_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

static void c5_resource_log_transition(c5_resource_state_t old_state,
                                       c5_resource_state_t new_state,
                                       uint32_t generation,
                                       uint32_t pending_ack_mask,
                                       const char *reason,
                                       const char *owner,
                                       int64_t quiesce_duration_ms,
                                       int64_t release_duration_ms)
{
    ESP_LOGI(TAG,
             "C5_RESOURCE_STATE generation=%lu old_state=%s new_state=%s reason=%s owner=%s "
             "pending_ack_mask=0x%08lx quiesce_duration_ms=%lld release_duration_ms=%lld "
             "internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u psram_free=%u "
             "standby_voice_session_bytes=%u",
             (unsigned long)generation,
             c5_resource_manager_state_name(old_state),
             c5_resource_manager_state_name(new_state),
             reason != NULL ? reason : "none",
             owner != NULL && owner[0] != '\0' ? owner : "none",
             (unsigned long)pending_ack_mask,
             (long long)quiesce_duration_ms,
             (long long)release_duration_ms,
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned int)C5_RESOURCE_STANDBY_VOICE_SESSION_BYTES);
}

static void c5_resource_set_state(c5_resource_state_t state,
                                  uint32_t pending_ack_mask,
                                  const char *reason,
                                  int64_t quiesce_duration_ms,
                                  int64_t release_duration_ms)
{
    c5_resource_state_t old_state;
    uint32_t generation;
    char owner[sizeof(s_resource.owner)] = {0};

    portENTER_CRITICAL(&s_resource_lock);
    old_state = s_resource.state;
    s_resource.state = state;
    s_resource.pending_ack_mask = pending_ack_mask;
    generation = s_resource.generation;
    strlcpy(owner, s_resource.owner, sizeof(owner));
    portEXIT_CRITICAL(&s_resource_lock);

    c5_resource_log_transition(old_state,
                               state,
                               generation,
                               pending_ack_mask,
                               reason,
                               owner,
                               quiesce_duration_ms,
                               release_duration_ms);
}

static void c5_resource_clear_pending_ack(uint32_t ack_bit, const char *reason)
{
    c5_resource_state_t state;
    uint32_t generation;
    uint32_t pending_ack_mask;
    char owner[sizeof(s_resource.owner)] = {0};

    portENTER_CRITICAL(&s_resource_lock);
    s_resource.pending_ack_mask &= ~ack_bit;
    state = s_resource.state;
    generation = s_resource.generation;
    pending_ack_mask = s_resource.pending_ack_mask;
    strlcpy(owner, s_resource.owner, sizeof(owner));
    portEXIT_CRITICAL(&s_resource_lock);

    c5_resource_log_transition(state,
                               state,
                               generation,
                               pending_ack_mask,
                               reason,
                               owner,
                               0,
                               0);
}

esp_err_t c5_resource_manager_init(void)
{
    bool should_log = false;

    portENTER_CRITICAL(&s_resource_lock);
    if (!s_resource.initialized) {
        memset(&s_resource, 0, sizeof(s_resource));
        s_resource.initialized = true;
        s_resource.state = C5_RESOURCE_STATE_STANDBY;
        should_log = true;
    }
    portEXIT_CRITICAL(&s_resource_lock);

    if (should_log) {
        c5_resource_log_transition(C5_RESOURCE_STATE_STANDBY,
                                   C5_RESOURCE_STATE_STANDBY,
                                   0U,
                                   0U,
                                   "standby",
                                   "wake_coordinator",
                                   0,
                                   0);
    }
    return ESP_OK;
}

static void c5_resource_rollback_quiesce(const char *reason)
{
    c5_runtime_workers_resume();
    csi_service_resume();
    (void)app_runtime_resume_non_voice(reason);
}

esp_err_t c5_resource_manager_begin_voice(const char *owner, c5_voice_lease_t *out_lease)
{
    if (out_lease == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out_lease->generation = 0U;
    (void)c5_resource_manager_init();

    uint32_t generation;
    portENTER_CRITICAL(&s_resource_lock);
    if (s_resource.state != C5_RESOURCE_STATE_STANDBY || s_resource.lease_valid) {
        portEXIT_CRITICAL(&s_resource_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_resource.generation++;
    if (s_resource.generation == 0U) {
        s_resource.generation = 1U;
    }
    generation = s_resource.generation;
    s_resource.lease_valid = true;
    s_resource.audio_phase = C5_AUDIO_PHASE_MIC_LISTENING_ACTIVE;
    s_resource.pending_ack_mask = C5_RESOURCE_ACK_HTTP |
                                  C5_RESOURCE_ACK_BME |
                                  C5_RESOURCE_ACK_CSI |
                                  C5_RESOURCE_ACK_WORKERS;
    s_resource.state = C5_RESOURCE_STATE_QUIESCING;
    s_resource.transition_started_ms = c5_resource_now_ms();
    strlcpy(s_resource.owner,
            owner != NULL && owner[0] != '\0' ? owner : "voice_chain",
            sizeof(s_resource.owner));
    portEXIT_CRITICAL(&s_resource_lock);

    c5_resource_log_transition(C5_RESOURCE_STATE_STANDBY,
                               C5_RESOURCE_STATE_QUIESCING,
                               generation,
                               C5_RESOURCE_ACK_HTTP | C5_RESOURCE_ACK_BME |
                                   C5_RESOURCE_ACK_CSI | C5_RESOURCE_ACK_WORKERS,
                               "quiesce_begin",
                               owner,
                               0,
                               0);

    esp_err_t ret = app_runtime_pause_non_voice_timed("voice_lease",
                                                       C5_RESOURCE_HTTP_QUIESCE_TIMEOUT_MS,
                                                       C5_RESOURCE_BME_QUIESCE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        goto fail;
    }
    c5_resource_clear_pending_ack(C5_RESOURCE_ACK_HTTP, "http_quiesced");
    c5_resource_clear_pending_ack(C5_RESOURCE_ACK_BME, "bme_paused");

    ret = csi_service_pause_and_wait(C5_RESOURCE_CSI_QUIESCE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        goto fail;
    }
    c5_resource_clear_pending_ack(C5_RESOURCE_ACK_CSI, "csi_paused");

    ret = c5_runtime_workers_quiesce(C5_RESOURCE_WORKER_QUIESCE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        goto fail;
    }
    c5_resource_clear_pending_ack(C5_RESOURCE_ACK_WORKERS, "workers_quiesced");

    const int64_t now_ms = c5_resource_now_ms();
    int64_t quiesce_duration_ms = 0;
    portENTER_CRITICAL(&s_resource_lock);
    quiesce_duration_ms = now_ms - s_resource.transition_started_ms;
    portEXIT_CRITICAL(&s_resource_lock);
    c5_resource_set_state(C5_RESOURCE_STATE_VOICE_EXCLUSIVE,
                          0U,
                          "quiesce_complete",
                          quiesce_duration_ms,
                          0);
    out_lease->generation = generation;
    return ESP_OK;

fail:
    c5_resource_set_state(C5_RESOURCE_STATE_ERROR,
                          0U,
                          "quiesce_failed",
                          0,
                          0);
    c5_resource_rollback_quiesce("voice_lease_rollback");
    portENTER_CRITICAL(&s_resource_lock);
    s_resource.lease_valid = false;
    s_resource.pending_ack_mask = 0U;
    s_resource.audio_phase = C5_AUDIO_PHASE_NONE;
    portEXIT_CRITICAL(&s_resource_lock);
    c5_resource_set_state(C5_RESOURCE_STATE_STANDBY,
                          0U,
                          "quiesce_rollback_complete",
                          0,
                          0);
    return ret;
}

bool c5_resource_manager_lease_is_current(uint32_t generation)
{
    bool current;
    portENTER_CRITICAL(&s_resource_lock);
    current = s_resource.lease_valid && generation != 0U &&
              generation == s_resource.generation &&
              s_resource.state == C5_RESOURCE_STATE_VOICE_EXCLUSIVE;
    portEXIT_CRITICAL(&s_resource_lock);
    return current;
}

bool c5_resource_manager_is_voice_exclusive(void)
{
    bool exclusive;
    portENTER_CRITICAL(&s_resource_lock);
    exclusive = s_resource.state == C5_RESOURCE_STATE_QUIESCING ||
                s_resource.state == C5_RESOURCE_STATE_VOICE_EXCLUSIVE ||
                s_resource.state == C5_RESOURCE_STATE_RELEASING;
    portEXIT_CRITICAL(&s_resource_lock);
    return exclusive;
}

uint32_t c5_resource_manager_current_generation(void)
{
    uint32_t generation = 0U;
    portENTER_CRITICAL(&s_resource_lock);
    if (s_resource.lease_valid && s_resource.state == C5_RESOURCE_STATE_VOICE_EXCLUSIVE) {
        generation = s_resource.generation;
    }
    portEXIT_CRITICAL(&s_resource_lock);
    return generation;
}

esp_err_t c5_resource_manager_note_phase(c5_voice_lease_t lease, const char *phase)
{
    c5_resource_state_t state;
    uint32_t pending_ack_mask;
    char owner[sizeof(s_resource.owner)] = {0};
    bool phase_lease_is_current;
    portENTER_CRITICAL(&s_resource_lock);
    state = s_resource.state;
    pending_ack_mask = s_resource.pending_ack_mask;
    strlcpy(owner, s_resource.owner, sizeof(owner));
    phase_lease_is_current = s_resource.lease_valid && lease.generation != 0U &&
                             lease.generation == s_resource.generation &&
                             (state == C5_RESOURCE_STATE_VOICE_EXCLUSIVE ||
                              state == C5_RESOURCE_STATE_RELEASING);
    portEXIT_CRITICAL(&s_resource_lock);
    if (!phase_lease_is_current) {
        return ESP_ERR_INVALID_STATE;
    }
    c5_resource_log_transition(state,
                               state,
                               lease.generation,
                               pending_ack_mask,
                               phase != NULL ? phase : "phase",
                               owner,
                               0,
                               0);
    return ESP_OK;
}

esp_err_t c5_resource_manager_set_audio_phase(c5_voice_lease_t lease,
                                              c5_audio_phase_t phase,
                                              const char *reason)
{
    c5_resource_state_t state;
    bool valid;
    portENTER_CRITICAL(&s_resource_lock);
    state = s_resource.state;
    valid = s_resource.lease_valid && lease.generation != 0U &&
            lease.generation == s_resource.generation &&
            (state == C5_RESOURCE_STATE_VOICE_EXCLUSIVE ||
             state == C5_RESOURCE_STATE_RELEASING);
    if (valid) {
        s_resource.audio_phase = phase;
    }
    portEXIT_CRITICAL(&s_resource_lock);
    if (!valid) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG,
             "C5_AUDIO_PHASE generation=%lu phase=%s reason=%s state=%s dma_free=%u dma_largest=%u",
             (unsigned long)lease.generation,
             c5_resource_manager_audio_phase_name(phase),
             reason != NULL ? reason : "none",
             c5_resource_manager_state_name(state),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    return ESP_OK;
}

esp_err_t c5_resource_manager_release_voice(c5_voice_lease_t lease, const char *reason)
{
    if (lease.generation == 0U) {
        return ESP_OK;
    }

    int64_t release_started_ms;
    char owner[sizeof(s_resource.owner)] = {0};
    bool release_retry = false;
    portENTER_CRITICAL(&s_resource_lock);
    if (!s_resource.lease_valid || lease.generation != s_resource.generation) {
        portEXIT_CRITICAL(&s_resource_lock);
        return ESP_OK;
    }
    if (s_resource.state != C5_RESOURCE_STATE_VOICE_EXCLUSIVE &&
        s_resource.state != C5_RESOURCE_STATE_RELEASING) {
        portEXIT_CRITICAL(&s_resource_lock);
        return ESP_ERR_INVALID_STATE;
    }
    release_retry = s_resource.state == C5_RESOURCE_STATE_RELEASING;
    if (release_retry) {
        release_started_ms = s_resource.transition_started_ms;
    } else {
        release_started_ms = c5_resource_now_ms();
        s_resource.state = C5_RESOURCE_STATE_RELEASING;
        s_resource.transition_started_ms = release_started_ms;
    }
    strlcpy(owner, s_resource.owner, sizeof(owner));
    portEXIT_CRITICAL(&s_resource_lock);
    c5_resource_log_transition(release_retry ? C5_RESOURCE_STATE_RELEASING :
                                                    C5_RESOURCE_STATE_VOICE_EXCLUSIVE,
                               C5_RESOURCE_STATE_RELEASING,
                               lease.generation,
                               0U,
                               release_retry ? "release_retry" :
                                               (reason != NULL ? reason : "release_begin"),
                               owner,
                               0,
                               0);

    esp_err_t first_ret = server_voice_client_request_abort(reason);
    if (first_ret != ESP_OK) {
        c5_resource_set_state(C5_RESOURCE_STATE_RELEASING,
                              0U,
                              "release_abort_failed",
                              0,
                              c5_resource_now_ms() - release_started_ms);
        return first_ret;
    }
    esp_err_t ret = server_voice_client_wait_for_idle(C5_RESOURCE_RESPONSE_SHUTDOWN_TIMEOUT_MS);
    if (ret != ESP_OK) {
        c5_resource_set_state(C5_RESOURCE_STATE_RELEASING,
                              0U,
                              "release_response_shutdown_failed",
                              0,
                              c5_resource_now_ms() - release_started_ms);
        return ret;
    }

    ret = audio_player_release_session(C5_RESOURCE_SPEAKER_RELEASE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        c5_resource_set_state(C5_RESOURCE_STATE_RELEASING,
                              0U,
                              "release_audio_failed",
                              0,
                              c5_resource_now_ms() - release_started_ms);
        return ret;
    }
    (void)c5_resource_manager_set_audio_phase(lease,
                                              C5_AUDIO_PHASE_SPEAKER_TX_RELEASED,
                                              "release_audio_complete");
    (void)c5_resource_manager_note_phase(lease, "after_i2s_deinit");

    csi_service_resume();
    ret = app_runtime_resume_non_voice_bme(reason);
    if (first_ret == ESP_OK && ret != ESP_OK) {
        first_ret = ret;
    }
    c5_runtime_workers_resume();
    ret = app_runtime_finish_non_voice_resume(reason);
    if (first_ret == ESP_OK && ret != ESP_OK) {
        first_ret = ret;
    }

    const int64_t release_duration_ms = c5_resource_now_ms() - release_started_ms;
    portENTER_CRITICAL(&s_resource_lock);
    s_resource.lease_valid = false;
    s_resource.pending_ack_mask = 0U;
    s_resource.audio_phase = C5_AUDIO_PHASE_NONE;
    portEXIT_CRITICAL(&s_resource_lock);
    c5_resource_set_state(C5_RESOURCE_STATE_STANDBY,
                          0U,
                          first_ret == ESP_OK ? "release_complete" : "release_resume_failed",
                          0,
                          release_duration_ms);
    return first_ret;
}

void c5_resource_manager_get_snapshot(c5_resource_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_resource_lock);
    out_snapshot->state = s_resource.state;
    out_snapshot->generation = s_resource.generation;
    out_snapshot->pending_ack_mask = s_resource.pending_ack_mask;
    out_snapshot->lease_valid = s_resource.lease_valid;
    out_snapshot->audio_phase = s_resource.audio_phase;
    strlcpy(out_snapshot->owner, s_resource.owner, sizeof(out_snapshot->owner));
    portEXIT_CRITICAL(&s_resource_lock);
}

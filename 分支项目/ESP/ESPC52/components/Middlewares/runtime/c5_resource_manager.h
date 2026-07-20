#ifndef C5_RESOURCE_MANAGER_H
#define C5_RESOURCE_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    C5_RESOURCE_STATE_STANDBY = 0,
    C5_RESOURCE_STATE_QUIESCING,
    C5_RESOURCE_STATE_VOICE_EXCLUSIVE,
    C5_RESOURCE_STATE_RELEASING,
    C5_RESOURCE_STATE_ERROR,
} c5_resource_state_t;

typedef enum {
    C5_AUDIO_PHASE_NONE = 0,
    C5_AUDIO_PHASE_MIC_LISTENING_ACTIVE,
    C5_AUDIO_PHASE_MIC_PAUSE_REQUESTED,
    C5_AUDIO_PHASE_MIC_DMA_RELEASED,
    C5_AUDIO_PHASE_SPEAKER_TX_OWNED,
    C5_AUDIO_PHASE_SPEAKER_TX_RELEASED,
    C5_AUDIO_PHASE_MIC_RECORD_READY,
} c5_audio_phase_t;

typedef struct {
    uint32_t generation;
} c5_voice_lease_t;

typedef struct {
    c5_resource_state_t state;
    uint32_t generation;
    uint32_t pending_ack_mask;
    bool lease_valid;
    c5_audio_phase_t audio_phase;
    char owner[24];
} c5_resource_snapshot_t;

enum {
    C5_RESOURCE_ACK_HTTP = 1U << 0,
    C5_RESOURCE_ACK_BME = 1U << 1,
    C5_RESOURCE_ACK_CSI = 1U << 2,
    C5_RESOURCE_ACK_WORKERS = 1U << 3,
};

/* Voice-session allocations are intentionally absent while WakeNet is idle. */
#ifndef C5_RESOURCE_STANDBY_VOICE_SESSION_BYTES
#define C5_RESOURCE_STANDBY_VOICE_SESSION_BYTES 0U
#endif

#ifndef C5_RESOURCE_HTTP_QUIESCE_TIMEOUT_MS
#define C5_RESOURCE_HTTP_QUIESCE_TIMEOUT_MS 1000U
#endif

#ifndef C5_RESOURCE_BME_QUIESCE_TIMEOUT_MS
#define C5_RESOURCE_BME_QUIESCE_TIMEOUT_MS 2000U
#endif

#ifndef C5_RESOURCE_CSI_QUIESCE_TIMEOUT_MS
#define C5_RESOURCE_CSI_QUIESCE_TIMEOUT_MS 2000U
#endif

#ifndef C5_RESOURCE_WORKER_QUIESCE_TIMEOUT_MS
#define C5_RESOURCE_WORKER_QUIESCE_TIMEOUT_MS 2000U
#endif

#ifndef C5_RESOURCE_RESPONSE_SHUTDOWN_TIMEOUT_MS
#define C5_RESOURCE_RESPONSE_SHUTDOWN_TIMEOUT_MS 3000U
#endif

#ifndef C5_RESOURCE_SPEAKER_RELEASE_TIMEOUT_MS
#define C5_RESOURCE_SPEAKER_RELEASE_TIMEOUT_MS 3000U
#endif

/** @brief Initialize the static state owner. This never allocates voice playback resources. */
esp_err_t c5_resource_manager_init(void);

/**
 * @brief Quiesce background resources and acquire the sole voice lease.
 *
 * The function never waits while holding the manager lock. A failed stage rolls
 * back every earlier stage before returning an error.
 */
esp_err_t c5_resource_manager_begin_voice(const char *owner, c5_voice_lease_t *out_lease);

/** @brief Release a voice lease in audio-first order, then restore background work. */
esp_err_t c5_resource_manager_release_voice(c5_voice_lease_t lease, const char *reason);

/** @brief Return true only for the current VOICE_EXCLUSIVE lease generation. */
bool c5_resource_manager_lease_is_current(uint32_t generation);

/** @brief Return true while normal background work must remain paused. */
bool c5_resource_manager_is_voice_exclusive(void);

/** @brief Return the active generation, or zero when no voice lease is active. */
uint32_t c5_resource_manager_current_generation(void);

/** @brief Record a generation-checked phase checkpoint without changing ownership. */
esp_err_t c5_resource_manager_note_phase(c5_voice_lease_t lease, const char *phase);

/**
 * @brief Record the hardware-audio direction within an unchanged voice lease.
 *
 * VOICE_EXCLUSIVE only denotes the background lease. Consumers must inspect
 * this phase before assuming that Mic DMA or Speaker TX owns audio hardware.
 */
esp_err_t c5_resource_manager_set_audio_phase(c5_voice_lease_t lease,
                                              c5_audio_phase_t phase,
                                              const char *reason);

const char *c5_resource_manager_audio_phase_name(c5_audio_phase_t phase);

/** @brief Read a lock-protected snapshot without exposing mutable state. */
void c5_resource_manager_get_snapshot(c5_resource_snapshot_t *out_snapshot);

const char *c5_resource_manager_state_name(c5_resource_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* C5_RESOURCE_MANAGER_H */

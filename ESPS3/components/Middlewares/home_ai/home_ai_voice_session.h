#ifndef HOME_AI_VOICE_SESSION_H
#define HOME_AI_VOICE_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOME_AI_VOICE_SESSION_ID_LEN 48U
#define HOME_AI_VOICE_OWNER_ID_LEN 48U

typedef enum {
    HOME_AI_VOICE_SESSION_IDLE = 0,
    HOME_AI_VOICE_SESSION_LOCKED,
    HOME_AI_VOICE_SESSION_RECORDING,
    HOME_AI_VOICE_SESSION_WAITING_SERVER,
    HOME_AI_VOICE_SESSION_PLAYING,
    HOME_AI_VOICE_SESSION_ENDING,
    HOME_AI_VOICE_SESSION_PREEMPTED,
} home_ai_voice_session_state_t;

typedef struct {
    char voice_session_id[HOME_AI_VOICE_SESSION_ID_LEN];
    char owner_device_id[HOME_AI_VOICE_OWNER_ID_LEN];
    uint32_t generation;
    int64_t lease_expires_at_ms;
    home_ai_voice_session_state_t state;
} home_ai_voice_session_lease_t;

esp_err_t home_ai_voice_session_manager_init(void);
esp_err_t home_ai_voice_session_acquire(const char *device_id,
                                        home_ai_voice_session_lease_t *out_lease);
esp_err_t home_ai_voice_session_renew(const char *device_id,
                                      const char *voice_session_id,
                                      uint32_t generation,
                                      home_ai_voice_session_lease_t *out_lease);
esp_err_t home_ai_voice_session_transition(const char *device_id,
                                           const char *voice_session_id,
                                           uint32_t generation,
                                           home_ai_voice_session_state_t next_state,
                                           home_ai_voice_session_lease_t *out_lease);
esp_err_t home_ai_voice_session_release(const char *device_id,
                                        const char *voice_session_id,
                                        uint32_t generation);
/** @brief Release the active lease only when a trusted disconnect matches its owner. */
bool home_ai_voice_session_release_owner(const char *device_id);
bool home_ai_voice_session_validate(const char *device_id,
                                    const char *voice_session_id,
                                    uint32_t generation);
bool home_ai_voice_session_get(home_ai_voice_session_lease_t *out_lease);
bool home_ai_voice_session_preempt_for_emergency(home_ai_voice_session_lease_t *out_preempted);
const char *home_ai_voice_session_state_name(home_ai_voice_session_state_t state);
bool home_ai_voice_session_state_from_name(const char *name,
                                           home_ai_voice_session_state_t *out_state);

#ifdef __cplusplus
}
#endif

#endif /* HOME_AI_VOICE_SESSION_H */

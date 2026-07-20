#ifndef C5_VOICE_SESSION_CLIENT_H
#define C5_VOICE_SESSION_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define C5_VOICE_SESSION_ID_LEN 48U
#define C5_VOICE_SESSION_OWNER_ID_LEN 48U

typedef enum {
    C5_VOICE_SESSION_STATE_INVALID = 0,
    C5_VOICE_SESSION_STATE_LOCKED,
    C5_VOICE_SESSION_STATE_RECORDING,
    C5_VOICE_SESSION_STATE_WAITING_SERVER,
    C5_VOICE_SESSION_STATE_PLAYING,
    C5_VOICE_SESSION_STATE_ENDING,
} c5_voice_session_state_t;

typedef struct {
    char voice_session_id[C5_VOICE_SESSION_ID_LEN];
    char owner_device_id[C5_VOICE_SESSION_OWNER_ID_LEN];
    uint32_t generation;
    int64_t lease_expires_at_ms;
    c5_voice_session_state_t state;
} c5_voice_session_lease_t;

/* The client owns no task, queue, or heap buffer. It is used only by voice_chain. */
esp_err_t c5_voice_session_client_init(void);
esp_err_t c5_voice_session_client_acquire(c5_voice_session_lease_t *out_lease);
esp_err_t c5_voice_session_client_renew(c5_voice_session_lease_t *in_out_lease);
esp_err_t c5_voice_session_client_transition(c5_voice_session_lease_t *in_out_lease,
                                             c5_voice_session_state_t next_state);
esp_err_t c5_voice_session_client_release(c5_voice_session_lease_t *lease);
bool c5_voice_session_client_is_valid(const c5_voice_session_lease_t *lease);

#ifdef __cplusplus
}
#endif

#endif /* C5_VOICE_SESSION_CLIENT_H */

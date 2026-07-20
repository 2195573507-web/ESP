#ifndef HOME_AI_EMERGENCY_COORDINATOR_H
#define HOME_AI_EMERGENCY_COORDINATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "environment_alarm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOME_AI_EMERGENCY_MAX_ACTIVE 8U
#define HOME_AI_EMERGENCY_EVENT_ID_LEN 64U
#define HOME_AI_EMERGENCY_ACKNOWLEDGEMENT_MAX 8U

typedef enum {
    HOME_AI_EMERGENCY_DETECTED = 0,
    HOME_AI_EMERGENCY_ACTIVE_UNACKNOWLEDGED,
    HOME_AI_EMERGENCY_ACKNOWLEDGED,
    HOME_AI_EMERGENCY_ESCALATED,
    HOME_AI_EMERGENCY_RECOVERING,
    HOME_AI_EMERGENCY_RESOLVED,
} home_ai_emergency_state_t;

typedef struct {
    bool used;
    uint64_t alarm_id;
    uint8_t alarm_level;
    home_ai_emergency_state_t state;
    bool user_acknowledged;
    uint32_t playback_generation;
    uint64_t activated_at_ms;
    uint64_t next_prompt_at_ms;
    char event_id[HOME_AI_EMERGENCY_EVENT_ID_LEN];
    char room_id[32];
} home_ai_emergency_snapshot_t;

typedef struct {
    char event_id[HOME_AI_EMERGENCY_EVENT_ID_LEN];
    uint64_t acknowledged_at_ms;
} home_ai_emergency_acknowledgement_t;

bool home_ai_emergency_coordinator_init(void);
void home_ai_emergency_coordinator_tick(uint64_t now_ms);

/* Called only by environment_alarm_reporter after it owns a deep copy. */
void home_ai_emergency_coordinator_on_alarm_event(const alarm_event_t *event,
                                                  void *context);

/* Only a completed, generation-matched playback round may advance recovery. */
bool home_ai_emergency_coordinator_playback_completed(const char *event_id,
                                                      uint32_t playback_generation,
                                                      bool ok,
                                                      uint64_t now_ms);

/* User acknowledgement lowers cadence but never disables the safety rule. */
bool home_ai_emergency_coordinator_acknowledge_user(const char *event_id,
                                                    uint64_t now_ms);

/* Apply Server-owned acknowledgements from a validated config snapshot. */
bool home_ai_emergency_coordinator_replace_acknowledgements(
    const home_ai_emergency_acknowledgement_t *items,
    size_t count,
    uint64_t now_ms);

size_t home_ai_emergency_coordinator_snapshot(home_ai_emergency_snapshot_t *out,
                                              size_t capacity);
const char *home_ai_emergency_state_name(home_ai_emergency_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* HOME_AI_EMERGENCY_COORDINATOR_H */

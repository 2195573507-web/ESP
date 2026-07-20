#ifndef HOME_AI_EVENT_REPORTER_H
#define HOME_AI_EVENT_REPORTER_H

#include <stdbool.h>
#include <stdint.h>

#include "home_ai_history_store.h"
#include "home_ai_rule_engine.h"
#include "home_ai_virtual_device.h"

#ifdef __cplusplus
extern "C" {
#endif

bool home_ai_event_reporter_init(void);
void home_ai_event_reporter_record_decision(const home_ai_rule_decision_t *decision,
                                            const home_ai_virtual_device_execution_t *execution,
                                            uint64_t occurred_at_ms);
void home_ai_event_reporter_record_virtual_state(const home_ai_virtual_device_state_t *state,
                                                 uint64_t occurred_at_ms);
void home_ai_event_reporter_record_room_state(const home_ai_room_state_t *state,
                                              uint64_t occurred_at_ms);
void home_ai_event_reporter_record_rule_activation(
    const home_ai_rule_activation_result_t *activation,
    uint64_t occurred_at_ms);
void home_ai_event_reporter_record_user_override(const home_ai_user_override_t *override,
                                                 const char *device_id,
                                                 uint64_t occurred_at_ms);
void home_ai_event_reporter_record_emergency(const char *event_id,
                                             const char *room_id,
                                             const char *state,
                                             uint8_t priority,
                                             uint64_t occurred_at_ms);
void home_ai_event_reporter_record_playback_ack(const char *event_id,
                                                const char *room_id,
                                                uint32_t playback_generation,
                                                bool emergency,
                                                bool ok,
                                                uint64_t occurred_at_ms);
/** @brief Report bounded offline-history pressure without creating a reporter recursion. */
void home_ai_event_reporter_tick(uint64_t occurred_at_ms);

#ifdef __cplusplus
}
#endif

#endif /* HOME_AI_EVENT_REPORTER_H */

#ifndef COMMAND_ACK_POLICY_H
#define COMMAND_ACK_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    COMMAND_ACK_POLICY_ACCEPT_LEGACY = 0,
    COMMAND_ACK_POLICY_ACCEPT_PLAYBACK,
    COMMAND_ACK_POLICY_REJECT_UNKNOWN,
    COMMAND_ACK_POLICY_REJECT_DUPLICATE,
    COMMAND_ACK_POLICY_REJECT_STALE,
    COMMAND_ACK_POLICY_REJECT_GENERATION_MISSING,
    COMMAND_ACK_POLICY_REJECT_GENERATION_MISMATCH,
} command_ack_policy_result_t;

command_ack_policy_result_t command_ack_policy_evaluate(bool command_known,
                                                        bool command_active,
                                                        bool command_already_acked,
                                                        uint32_t expected_generation,
                                                        bool received_generation_present,
                                                        uint32_t received_generation);

bool command_ack_policy_accepts(command_ack_policy_result_t result);
const char *command_ack_policy_result_name(command_ack_policy_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* COMMAND_ACK_POLICY_H */

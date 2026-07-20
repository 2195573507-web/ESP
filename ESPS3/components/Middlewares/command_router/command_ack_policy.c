#include "command_ack_policy.h"

command_ack_policy_result_t command_ack_policy_evaluate(bool command_known,
                                                        bool command_active,
                                                        bool command_already_acked,
                                                        uint32_t expected_generation,
                                                        bool received_generation_present,
                                                        uint32_t received_generation)
{
    if (!command_known) return COMMAND_ACK_POLICY_REJECT_UNKNOWN;
    if (command_already_acked) return COMMAND_ACK_POLICY_REJECT_DUPLICATE;
    if (!command_active) return COMMAND_ACK_POLICY_REJECT_STALE;
    if (expected_generation == 0U) return COMMAND_ACK_POLICY_ACCEPT_LEGACY;
    if (!received_generation_present || received_generation == 0U) {
        return COMMAND_ACK_POLICY_REJECT_GENERATION_MISSING;
    }
    if (received_generation != expected_generation) {
        return COMMAND_ACK_POLICY_REJECT_GENERATION_MISMATCH;
    }
    return COMMAND_ACK_POLICY_ACCEPT_PLAYBACK;
}

bool command_ack_policy_accepts(command_ack_policy_result_t result)
{
    return result == COMMAND_ACK_POLICY_ACCEPT_LEGACY ||
           result == COMMAND_ACK_POLICY_ACCEPT_PLAYBACK;
}

const char *command_ack_policy_result_name(command_ack_policy_result_t result)
{
    switch (result) {
    case COMMAND_ACK_POLICY_ACCEPT_LEGACY: return "accepted_legacy";
    case COMMAND_ACK_POLICY_ACCEPT_PLAYBACK: return "accepted_playback";
    case COMMAND_ACK_POLICY_REJECT_UNKNOWN: return "unknown_command";
    case COMMAND_ACK_POLICY_REJECT_DUPLICATE: return "duplicate_ack";
    case COMMAND_ACK_POLICY_REJECT_STALE: return "stale_command";
    case COMMAND_ACK_POLICY_REJECT_GENERATION_MISSING: return "playback_generation_missing";
    case COMMAND_ACK_POLICY_REJECT_GENERATION_MISMATCH: return "playback_generation_mismatch";
    default: return "invalid_ack_policy";
    }
}

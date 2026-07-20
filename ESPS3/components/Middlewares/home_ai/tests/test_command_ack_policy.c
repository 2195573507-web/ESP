#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "command_ack_policy.h"

int main(void)
{
    assert(command_ack_policy_evaluate(false, false, false, 7U, true, 7U) ==
           COMMAND_ACK_POLICY_REJECT_UNKNOWN);
    assert(command_ack_policy_evaluate(true, false, true, 7U, true, 7U) ==
           COMMAND_ACK_POLICY_REJECT_DUPLICATE);
    assert(command_ack_policy_evaluate(true, false, false, 7U, true, 7U) ==
           COMMAND_ACK_POLICY_REJECT_STALE);
    assert(command_ack_policy_evaluate(true, true, false, 7U, false, 0U) ==
           COMMAND_ACK_POLICY_REJECT_GENERATION_MISSING);
    assert(command_ack_policy_evaluate(true, true, false, 7U, true, 6U) ==
           COMMAND_ACK_POLICY_REJECT_GENERATION_MISMATCH);
    assert(command_ack_policy_evaluate(true, true, false, 7U, true, 7U) ==
           COMMAND_ACK_POLICY_ACCEPT_PLAYBACK);
    assert(command_ack_policy_evaluate(true, true, false, 0U, false, 0U) ==
           COMMAND_ACK_POLICY_ACCEPT_LEGACY);
    assert(command_ack_policy_accepts(COMMAND_ACK_POLICY_ACCEPT_PLAYBACK));
    assert(command_ack_policy_accepts(COMMAND_ACK_POLICY_ACCEPT_LEGACY));
    assert(!command_ack_policy_accepts(COMMAND_ACK_POLICY_REJECT_DUPLICATE));
    assert(strcmp(command_ack_policy_result_name(COMMAND_ACK_POLICY_REJECT_GENERATION_MISMATCH),
                  "playback_generation_mismatch") == 0);

    puts("command ack policy host tests: PASS");
    return 0;
}

#include "server_client_voice_eof.h"

#include <stdbool.h>
#include <stdio.h>

static bool expect_state(server_client_voice_eof_state_t actual,
                         server_client_voice_eof_state_t expected)
{
    return actual == expected;
}

bool server_client_voice_eof_run_offline_tests(char *summary, size_t summary_size)
{
    const bool known_wait = expect_state(
        server_client_voice_eof_state(8, 4, false, false),
        SERVER_CLIENT_VOICE_EOF_WAIT);
    const bool known_exact = expect_state(
        server_client_voice_eof_state(8, 8, false, false),
        SERVER_CLIENT_VOICE_EOF_COMPLETE);
    const bool known_early_eof = expect_state(
        server_client_voice_eof_state(8, 4, false, true),
        SERVER_CLIENT_VOICE_EOF_INCOMPLETE);
    const bool known_overread = expect_state(
        server_client_voice_eof_state(8, 10, false, false),
        SERVER_CLIENT_VOICE_EOF_OVERREAD);
    const bool unknown_wait = expect_state(
        server_client_voice_eof_state(-1, 4, false, true),
        SERVER_CLIENT_VOICE_EOF_WAIT);
    const bool unknown_complete = expect_state(
        server_client_voice_eof_state(-1, 4, true, false),
        SERVER_CLIENT_VOICE_EOF_COMPLETE);
    const bool unknown_zero_complete = expect_state(
        server_client_voice_eof_state(-1, 4, true, true),
        SERVER_CLIENT_VOICE_EOF_COMPLETE);
    const bool ok = known_wait && known_exact && known_early_eof && known_overread &&
                    unknown_wait && unknown_complete && unknown_zero_complete;

    if (summary != NULL && summary_size > 0U) {
        snprintf(summary,
                 summary_size,
                 "voice eof offline tests: %s",
                 ok ? "PASS" : "FAIL");
    }
    return ok;
}

#ifdef SERVER_CLIENT_VOICE_EOF_TEST_MAIN
int main(void)
{
    char summary[96] = {0};
    const bool ok = server_client_voice_eof_run_offline_tests(summary, sizeof(summary));
    puts(summary);
    return ok ? 0 : 1;
}
#endif

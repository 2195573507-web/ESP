#ifndef SERVER_CLIENT_VOICE_EOF_H
#define SERVER_CLIENT_VOICE_EOF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SERVER_CLIENT_VOICE_EOF_WAIT = 0,
    SERVER_CLIENT_VOICE_EOF_COMPLETE,
    SERVER_CLIENT_VOICE_EOF_INCOMPLETE,
    SERVER_CLIENT_VOICE_EOF_OVERREAD,
} server_client_voice_eof_state_t;

static inline server_client_voice_eof_state_t server_client_voice_eof_state(
    int64_t content_length,
    size_t total_read,
    bool transport_complete,
    bool zero_read)
{
    if (content_length >= 0) {
        const uint64_t expected = (uint64_t)content_length;
        const uint64_t received = (uint64_t)total_read;
        if (received > expected) {
            return SERVER_CLIENT_VOICE_EOF_OVERREAD;
        }
        if (received == expected) {
            return SERVER_CLIENT_VOICE_EOF_COMPLETE;
        }
        return zero_read ? SERVER_CLIENT_VOICE_EOF_INCOMPLETE : SERVER_CLIENT_VOICE_EOF_WAIT;
    }

    return transport_complete ? SERVER_CLIENT_VOICE_EOF_COMPLETE : SERVER_CLIENT_VOICE_EOF_WAIT;
}

#endif

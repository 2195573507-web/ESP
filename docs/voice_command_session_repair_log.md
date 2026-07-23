# Voice Command Session Repair Log

Date: 2026-07-22

## Scope

Rebuilt the C52 to S3 command-capture session protocol. Verification is build-only; no firmware was flashed and no device, network, or server acceptance test was run.

## Problems And Root Causes

| Symptom | Root cause |
| --- | --- |
| `COMMAND_CAPTURE_TIMEOUT` | Capture state was not governed by a single identity-bound session and failure paths could return to listening without a terminal session state. |
| `COMMAND_STREAM_OPEN rejected stale_or_mismatched_generation` | S3 admitted command PCM using incomplete metadata and did not carry the command id through the asynchronous proxy job. |
| `/local/v1/commands/local-1/ack` 404 / ineffective handshake | The S3 wildcard route already existed, but the C52 client started capture before a successful ACK and therefore did not treat the real endpoint as a protocol gate. |

## Protocol

`VoiceCommandSession` is the shared C52/S3 session record:

```text
command_id, generation, wake_stream_id, command_stream_id, state, timestamp
```

Normal lifecycle:

```text
CREATE -> WAIT_ACK -> CAPTURE_READY -> CAPTURING -> UPLOAD -> PROCESSING -> DONE
```

Failure lifecycle:

```text
any active state -> FAILED -> RECOVERY -> WAKE_LISTENING
```

Every C52 and S3 session transition emits `VOICE_COMMAND_SESSION` with the prior and next state, full stream identity, and timestamp. S3 no longer stores the older `ACK_SENT`, `CAPTURE_START`, `RESPONSE`, or `RECOVERY` values as protocol state.

## Changes

- C52 creates the session using a nonzero `wake_stream_id`, posts `POST /local/v1/commands/{id}/ack`, and queues capture only after a successful response.
- C52 assigns a nonzero command stream only when Mic capture is armed, then sends `X-Command-Id`, `X-Command-Generation`, `X-Command-Stream-Id`, and `X-Command-State: CAPTURE_END` with the voice turn.
- C52 clears command metadata on pre-HTTP abort so a later turn cannot reuse stale identity.
- S3 command creation requires nonzero generation and wake stream; it moves through CREATE, WAIT_ACK, CAPTURE_READY, or FAILED after validating id, target, generation, and prior dispatch state.
- S3 owns an embedded `VoiceCommandSession`, uses the same state vocabulary as C52, and centralizes state transitions so timestamped `VOICE_COMMAND_SESSION` evidence is emitted before a terminal session is cleared.
- S3 `COMMAND_STREAM_OPEN` requires the complete command identity tuple, rejects zero or partial values, preserves command id in asynchronous work, and rechecks identity before processing/finalizing.
- S3 continues to expose `POST /local/v1/commands/*/ack` through its registered wildcard local HTTP route.

## Files

- `ESPC52/components/Middlewares/voice_domain/voice_chain.[ch]`
- `ESPC52/components/Middlewares/command_domain/system_command/system_server_client.c`
- `ESPC52/components/Middlewares/server_voice/server_voice_client.[ch]`
- `ESPS3/components/Middlewares/command_router/command_router.[ch]`
- `ESPS3/components/Middlewares/voice_proxy/voice_proxy.c`

## Build Results

| Target | Build directory | Result |
| --- | --- | --- |
| ESPC52 / esp32c5 | `ESPC52/build-voice-command-session-c52` | Passed: compile, link, and image generation completed. |
| ESPS3 / esp32s3 | `ESPS3/build-voice-command-session-s3` | Passed: compile, link, and image generation completed. |

Both builds used ESP-IDF 5.5.4 and the configured Python 3.14 environment.

## Remaining Risks

- Build success does not verify the runtime timing of wake detection, ACK delivery, capture timeout, or wake-listener restoration.
- The wildcard ACK route is source-registered; an on-device request is still needed to prove the deployed HTTP server accepts the exact URI.
- Real C52/S3 traffic must prove that command stream ids stay unique across reconnects and that late PCM is rejected without blocking a newer session.

# ESPS3 Local Voice Command ACK Routing Repair - 2026-07-22

## Scope

- Target: ESPS3 local voice command ACK only.
- Endpoint: `POST /local/v1/commands/{command_id}/ack`.
- Excluded: remote command ACK delivery, Server command flow, flash, monitor, and hardware tests.

## Problem and Root Cause

The S3 route wildcard already existed, but every ACK was placed on the generic
event scheduler path. That path calls `command_router_ack()`, which marks a
generic command complete and queues a Server ACK. It neither identifies the
pending `voice_proxy` capture nor validates its generation, so the local voice
state remained in its pre-capture timeout state.

The C52 ACK body is the established lightweight command ACK envelope:
`p`, `id`, `t`, `cid`, `ok`, `e`, `u`, and `q`. Its command generation is not
serialized; it is carried in the pending command arguments and retained by C52
only for local request admission. S3 therefore validates the generation stored
in the matched pending voice command rather than inventing a new C52 wire field.

## Repair

- `command_router` now returns the generated local command id to the voice
  caller and has a local-only voice ACK state update that does not queue a
  Server ACK.
- `voice_proxy` binds that id to the pending capture. A local ACK must match
  the path/body `cid`, the expected local device id, an `ok` value, and the
  pending nonzero generation.
- A successful ACK changes the pending capture to `acknowledged` and removes
  only the pre-ACK deadline. The later command HTTP request must still provide
  the same generation and stream id before capture can start.
- The existing wildcard handler dispatches a matching local voice ACK directly
  to `voice_proxy` and returns exactly `{"status":"ok"}`. ACKs that do not
  match a pending local voice command keep the existing
  `enqueue_body_buffer -> s3_scheduler -> command_router_ack` remote flow.

## State Change

`WAKE_CONFIRMED -> COMMAND_CAPTURE_REQUEST -> pending generation`

`pending generation -> matching local ACK -> COMMAND_LISTENING`

`COMMAND_LISTENING -> matching command HTTP generation/stream -> processing`

The former pre-ACK timeout applies only while the capture is unacknowledged;
therefore a valid ACK does not subsequently emit that timeout. Completion or
failure of the command stream clears the pending capture.

## Modified Files

- `ESPS3/components/Middlewares/local_http_server/local_http_server.c`
- `ESPS3/components/Middlewares/voice_proxy/voice_proxy.c`
- `ESPS3/components/Middlewares/voice_proxy/voice_proxy.h`
- `ESPS3/components/Middlewares/command_router/command_router.c`
- `ESPS3/components/Middlewares/command_router/command_router.h`

## Build Verification

- Environment: ESP-IDF 5.5.4 with Python 3.14.5.
- Command: `idf.py -C ESPS3 -B /tmp/esps3-local-voice-ack-build build`.
- Result: passed (exit code 0); the isolated project configured, compiled, and
  linked for `esp32s3`.
- Not performed: flash, monitor, network transaction, C52 interaction, or
  hardware/end-to-end voice acceptance.

## Remaining Risk

A successful local ACK intentionally removes the S3 pre-ACK deadline. If C52
acknowledges then never opens its command stream, that pending capture remains
until its later command-stream lifecycle clears it. This task preserves the
requested no-timeout-after-ACK behavior; target runtime testing is still needed
to validate recovery from a disconnected C52 after ACK.

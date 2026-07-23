# 2026-07-23 - C5/S3 voice session lifecycle repair

**Status:** multi-agent source audit and scoped implementation complete.
Isolated C51, C52, and S3 builds passed; runtime acceptance remains pending.

## Scope

This task is limited to the C5 PCM transport stream lifecycle, S3 WakeNet
session ownership, and S3 command-capture stream binding. It does not alter
hardware drivers, PCM wire security, protocol security checks, or the broader
audio architecture.

## Triggering Evidence

The observed C52 PCM stream advanced to `37675186`, while a command session
still carried `wake_stream_id=37675185`. The S3 proxy later timed out with
`command_stream_id=0`; meanwhile delayed packets emitted unbounded
`S3_PCM_RX_DROP reason=stream_mismatch` warnings and a two-frame sequence gap
closed the WakeNet session.

## Multi-Agent Audit

| Role | Owned boundary | Audit conclusion |
| --- | --- | --- |
| Agent 1 | C5 `c5_audio_transport` | Closing a stream must retain an admission barrier until the matching END/ABORT is sent; queued slots must remain bound to their stream generation. |
| Agent 2 | S3 `audio_wake_gateway` | A delayed old START must not replace a newer active session. Stream mismatch is diagnostic-only and must be rate limited; small gaps should be repaired with silence. |
| Agent 3 | S3 `voice_proxy` and `command_router` | A command capture has to retain one identity tuple through dispatch, ACK, capture, upload, and completion. A zero or stale command stream must not bind or complete the active session. |
| Documentation | This record | Source and build evidence are kept separate from device/runtime acceptance. |

## Intended Lifecycle Contract

```text
C5 PCM: OPEN(current stream) -> PCM(current stream) -> VAD silence -> END(current stream)
S3 WakeNet: START(current stream) -> PCM/current-or-small-gap recovery -> CLOSE(current stream)
S3 command: CREATE(wake stream) -> WAIT_ACK -> CAPTURE_READY ->
            CAPTURING(command stream) -> UPLOAD -> PROCESSING -> terminal
```

Old stream frames may be counted and rate-limited in logs, but they may not
reset, close, replace, acknowledge, or complete a newer session. The targeted
PCM policy is to synthesize silence for gaps of at most five frames and close
only when the missing span exceeds five frames.

## Implemented Changes

- `ESPC51/components/Middlewares/voice_transport/c5_audio_transport.[ch]`
  now keeps `ACTIVE -> CLOSING -> IDLE` ownership. New streams wait for the
  matching END or ABORT send; queued slots retain their stream identity; abort
  and sender-failure paths discard old queued slots before reuse.
- `ESPC51/components/Middlewares/mic/mic_adc_test.c` explicitly starts
  pre-roll before live PCM. Its VAD-silence path reaches the matching transport
  END before a later VAD start can allocate another stream. C52's equivalent
  close barrier and immutable queued-stream logic were already present and
  preserved.
- `ESPS3/components/Middlewares/audio_wake_gateway/audio_wake_gateway.c`
  retains a bounded per-source retirement history. Delayed START, PCM, END,
  and ABORT from a retired stream cannot mutate a newer session. Mismatches
  are aggregated in rate-limited `S3_PCM_RX_DROP_STATS`; gaps of one through
  five frames inject zero samples and continue, while larger gaps close.
- `ESPS3/components/Middlewares/voice_proxy/voice_proxy.c` creates the command
  ID before dispatch, binds the current nonzero wake stream at CREATE, and
  admits one later nonzero command stream only in CAPTURE_READY. Command ID,
  generation, device ID, and the unbound command-stream slot must all match;
  stale streams cannot rebind or complete the session. The command-capture
  deadline begins after the accepted ACK, rather than consuming time while the
  command is still waiting for dispatch and acknowledgment.
- `ESPS3/components/Middlewares/command_router/command_router.[ch]` carries
  the caller-allocated command ID, generation, and immutable wake-stream
  identity atomically into the C5 pending command, preventing an ACK race.

`wake_stream_id` and `command_stream_id` intentionally remain distinct:
the former identifies the WakeNet PCM stream and the latter the subsequent
command-capture PCM stream. They form one session tuple: wake identity is
immutable and exactly one nonzero command stream binds after CAPTURE_READY.

## Build Result

All builds used ESP-IDF 5.5.4 with the configured Python 3.14 environment and
isolated output directories.

| Target | Directory | Result |
| --- | --- | --- |
| ESPC51 / esp32c5 | `/tmp/c5-s3-session-lifecycle-c51` | Passed: `00_Learn.elf` and `00_Learn.bin`. |
| ESPC52 / esp32c5 | `/tmp/c5-s3-session-lifecycle-c52` | Passed: `00_Learn.elf` and `00_Learn.bin`. |
| ESPS3 / esp32s3 | `/tmp/c5-s3-session-lifecycle-s3` | Passed: `sensair_s3_gateway.elf` and `sensair_s3_gateway.bin`. |

## Validation Boundary

The requested validation is build-only. No flash, serial monitor, microphone,
UDP, WakeNet, network, server, or end-to-end hardware test is run or implied.

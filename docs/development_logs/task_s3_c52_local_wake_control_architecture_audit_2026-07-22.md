# S3/C52 local wake control architecture audit - 2026-07-22

## Scope and Boundary

This record is a source-level audit of the ESP32-S3 and ESP32-C52 local
voice-wake control path. It covers:

- C52 microphone acquisition, VAD, wake PCM upload, and command PCM capture.
- S3 UDP WakeNet detection, wake-event dispatch, command routing, voice proxy,
  and local HTTP command ACK handling.
- The control transition following a successful S3 WakeNet detection.

It does not change functional code, PCM transport, speaker playback, server
ASR/LLM/TTS behavior, device configuration, radar, or sensor paths. No build,
flash, serial monitor, microphone exercise, network transaction, or hardware
acceptance was performed for this audit.

## Observed Phenomenon

The intended user interaction is: C52 samples and uploads PCM, S3 detects the
wake word, then S3 tells C52 to enter command-recording mode. The current
implementation reaches that outcome through C52's generic command polling and
HTTP command ACK lifecycle:

1. S3 detects the wake word from C52 UDP PCM.
2. S3 creates a synthetic `voice.start_command_capture` local command with an
   id such as `local-N`.
3. C52 discovers the command on its later `/local/v1/commands/pending` poll.
4. C52 starts its local capture preparation and posts
   `/local/v1/commands/{id}/ack`.
5. Only after that ACK does S3 set the pending capture to acknowledged and log
   `COMMAND_LISTENING`.

Consequently, a local, S3-authoritative wake event is delayed and gated by an
HTTP polling and acknowledgement exchange intended for generic commands.

## Source and Log Evidence

### WakeNet ownership

- `ESPS3/components/Middlewares/audio_wake_gateway/audio_wake_gateway.c`
  creates the `nihaoxiaozhi` WakeNet model and UDP receiver in
  `audio_wake_gateway_init()` (lines 636-690).
- Its packet processing feeds `s_wakenet->detect()` and emits
  `WAKE_DETECTED`, `WAKE_EVENT`, and `WAKE_CONFIRMED` on a positive result
  (lines 230-291).
- C52's `local_wake_word_init()` explicitly states that C5 local VAD is the
  PCM gate and S3 owns wake detection; the local wake-detected API rejects
  invocation with `ESP_ERR_INVALID_STATE`
  (`ESPC52/components/Middlewares/wake/local_wake_word.c`, lines 83-108).
- `ESPC52/components/Middlewares/voice_transport/c5_audio_transport.c`
  serializes and sends PCM frames to the audio transport endpoint; it contains
  no WakeNet inference or confirmation logic (lines 205-260).

### Current wake-to-capture handoff

- S3 `voice_proxy_init()` subscribes to `audio_wake_gateway` events
  (`ESPS3/components/Middlewares/voice_proxy/voice_proxy.c`, lines 754-828).
- The wake worker creates S3-owned capture state, assigns a generation, then
  enqueues `voice.start_command_capture` through `command_router` with a
  generated local command id (lines 272-364).
- `command_router` maps this command to
  `ESP111_PROTOCOL_LOCAL_COMMAND_START_COMMAND_CAPTURE` (lines 180-203),
  exposes it from `/local/v1/commands/pending`, and changes its state to
  dispatched (lines 513-574).
- C52 `system_server_client_poll_commands()` polls that endpoint and executes
  the decoded command (`ESPC52/components/Middlewares/command_domain/system_command/system_server_client.c`,
  lines 937-1005). Its voice command execution calls
  `voice_chain_request_command_capture()` and immediately posts an ACK (lines
  744-782).
- C52 `voice_chain` logs `WAKE_CONFIRMED` and `COMMAND_LISTENING` while it
  receives this routed request, not when it independently detects a wake word
  (`ESPC52/components/Middlewares/voice_domain/voice_chain.c`, lines
  1412-1439). It later arms the microphone command-capture stream after the
  existing prompt/mic handoff (lines 1356-1409).

### ACK gate

- `local_http_server` routes `POST /local/v1/commands/*/ack` to
  `command_ack_handler()` (`ESPS3/components/Middlewares/local_http_server/local_http_server.c`,
  lines 1042-1115 and 1225-1241).
- That handler gives a matching ACK to `voice_proxy_handle_command_ack()`
  before generic command processing (lines 1079-1089).
- `voice_proxy_handle_command_ack()` validates the C52 id and command id, calls
  `command_router_ack_local_voice()`, sets `acknowledged=true`, then logs
  `COMMAND_LISTENING` (lines 382-444).
- `voice_proxy_accept_command_capture()` refuses a C52 command PCM stream
  unless the S3 pending state is acknowledged, generation/device match, and the
  stream differs from the wake stream (lines 235-255).

## Current State Machine

```text
C52 mic_adc_test / VAD
    -> c5_audio_transport UDP PCM
    -> S3 audio_wake_gateway / WakeNet
    -> WAKE_DETECTED
    -> WAKE_CONFIRMED
    -> S3 voice_proxy creates voice.start_command_capture (local-N)
    -> command_router queue
    -> C52 periodic GET /local/v1/commands/pending
    -> C52 voice_chain_request_command_capture
    -> C52 POST /local/v1/commands/local-N/ack
    -> S3 voice_proxy marks acknowledged
    -> COMMAND_LISTENING
    -> C52 command VAD and command PCM HTTP upload
    -> ASR_CAPTURE through S3 voice_proxy and existing server voice turn
```

The named semantic chain therefore currently behaves as follows:

```text
WAKE_DETECTED (S3)
  -> WAKE_CONFIRMED (S3)
  -> voice.start_command_capture (S3 generic local command)
  -> waiting for C52 poll and HTTP ACK
  -> COMMAND_LISTENING (S3 only after ACK)
  -> ASR_CAPTURE (after accepted command PCM)
```

## Root Cause

`command_router` and `/local/v1/commands/{id}/ack` are a generic device-command
delivery protocol. `command_router.c` documents their role as mapping C5 ACKs
to server ACKs for queued commands. The local voice capture request was added
to this generic model, including a special local-only ACK path.

That makes the C52 ACK do more than report delivery or admission: it becomes
the control-plane authority for a state transition that S3 already knows it
must perform after its own WakeNet result. This reverses the desired control
direction and adds dependencies on polling cadence, C52 command worker timing,
HTTP availability, and ACK parsing before S3 will accept command PCM.

C52 does not own WakeNet confirmation. It should act on an S3 command-start
instruction and own only local execution details: prompt/speaker handoff,
microphone/VAD mode, stream allocation, capture timeout, and PCM upload.

## Desired State Machine

```text
C52 MIC acquisition
    -> unchanged PCM upload
    -> S3 WakeNet
    -> WAKE_DETECTED
    -> WAKE_CONFIRMED
    -> S3 command_router / voice_proxy owns capture generation and session
    -> S3 sends VOICE_COMMAND_START to the selected C52
    -> C52 enters COMMAND_LISTENING and arms command VAD/recording
    -> C52 command PCM upload with generation and stream id
    -> S3 validates S3-owned pending capture and enters ASR_CAPTURE
    -> existing upstream voice turn and speaker response path
```

The key change is that `VOICE_COMMAND_START` is an S3-to-C52 control action,
not a generic command whose ACK grants S3 permission to enter
`COMMAND_LISTENING`. A C52 delivery/admission response may remain available as
observability or compatibility data, but it must not be a prerequisite for the
S3 capture session to be authoritative.

## Minimal Modification Plan

1. Keep `audio_wake_gateway` and `c5_audio_transport` unchanged. WakeNet input,
   inference, source id, stream id, and existing PCM framing remain as-is.
2. Keep the C52 microphone capture and existing command PCM metadata unchanged.
   Reuse `voice_chain_request_command_capture()` and
   `mic_adc_test_arm_command_capture()` as the C52 command-mode actuator.
3. Add or reuse a direct S3-to-C52 `VOICE_COMMAND_START` delivery path that
   carries the S3-created `command_generation` and timeout. This path should
   make C52 enter command listening without waiting for a later generic poll.
4. In `voice_proxy`, transition the S3-owned pending capture from
   `WAKE_CONFIRMED` to command-listening state when S3 sends the start action;
   maintain the generation, selected device id, wake stream id, and capture
   deadline there.
5. Permit matching C52 command PCM to enter `voice_proxy_accept_command_capture()`
   based on that S3-owned state. Do not use generic HTTP ACK as the required
   `acknowledged` flag for the local wake flow.
6. Preserve `/local/v1/commands/pending` and
   `/local/v1/commands/{id}/ack` for ordinary server-originated commands and
   existing compatibility clients. Retain the voice ACK route only as a
   compatibility/observability path; do not introduce new wake-flow reliance
   on it and do not expand its semantics.
7. Preserve existing speaker playback and the downstream `/api/voice/turn`
   transport. The repair is limited to wake-to-command control ownership.

## Files Expected to Be Touched by a Follow-up Implementation

- `ESPS3/components/Middlewares/voice_proxy/voice_proxy.c`
- `ESPS3/components/Middlewares/voice_proxy/voice_proxy.h`
- `ESPS3/components/Middlewares/command_router/command_router.c` and `.h`
  only if it supplies the direct control delivery or compatibility adapter.
- `ESPS3/components/Middlewares/local_http_server/local_http_server.c` only if
  the direct control endpoint or compatibility behavior requires registration.
- `ESPC52/components/Middlewares/voice_domain/voice_chain.c` and `.h`
  only for a direct C52 command-start receiver that reuses current capture
  preparation.
- `ESPC52/components/Middlewares/command_domain/system_command/system_server_client.c`
  only to preserve existing generic ACK behavior while preventing it from
  gating S3-local wake control.

No changes are expected in `mic_adc_test`, `c5_audio_transport`, WakeNet model
creation/feeding, or speaker playback for this architecture correction.

## Build Verification and Risks

The unmodified source snapshot was built in isolated directories with ESP-IDF
5.5.4 and Python 3.14.5:

- `idf.py -C ESPC52 -B /tmp/espc52-local-voice-control-audit build` completed;
  it generated `00_Learn.bin` (`0x1d6830`) and reports 63% free in the
  smallest app partition.
- `idf.py -C ESPS3 -B /tmp/esps3-local-voice-control-audit build` completed;
  it generated `sensair_s3_gateway.bin` (`0x17b780`) and reports 79% free in
  the smallest app partition.

These are current-source compile/link and image-size checks only. They do not
flash or establish device, microphone, UDP, HTTP, WakeNet, ASR, speaker, or
end-to-end hardware acceptance.

Primary risks to test at source and build level:

- A direct delivery path must remain non-blocking in the WakeNet callback path.
- Capture generation, source device, and stream id validation must remain
  strict so stale PCM cannot attach to a later wake session.
- A busy or offline C52 needs a finite S3-owned timeout and cleanup path.
- Existing generic command ACK forwarding must retain its server-command
  behavior and must not create duplicate acknowledgements.
- C51/C52 compatibility needs review if both devices share the same control
  contracts, while only C52 is in the requested local voice path.

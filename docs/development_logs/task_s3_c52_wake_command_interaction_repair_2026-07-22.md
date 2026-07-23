# S3/C52 wake-to-command interaction repair - 2026-07-22

## Scope

- ESPS3 WakeNet event dispatch and local command-capture control.
- ESPC52 command capture, UI prompt state, and existing voice turn transport.
- Existing S3 `voice_proxy` and ESP-server voice route failure reporting.
- Excluded: BME, radar, device identity configuration, memory admission thresholds,
  flash, monitor, and hardware acceptance.

## Root Cause

The S3 UDP WakeNet receiver detected the wake word and logged `WAKE_EVENT`, but
had no event consumer.  Its subsequent wake-stream `VOICE_END` synchronously
closed that receiver session.  No independent C52 command capture was armed,
so no HTTP command turn reached `voice_proxy`.

The existing `voice_proxy` was initialized and its local route was registered,
but it treated every HTTP voice body as a second WakeNet candidate.  A command
utterance without the wake word therefore returned `204 No Content` before
ASR/LLM/TTS forwarding.  The LCD voice overlay also had no visible state text.

## Implemented Contract

1. Wake PCM remains a UDP WakeNet stream and closes with `WAKE_STREAM_CLOSE`.
   Its `voice_end detected=1` ends only that stream.
2. Wake detection emits S3 `WAKE_CONFIRMED` and queues a non-blocking local
   `START_COMMAND_CAPTURE` command with `command_generation` and
   `command_timeout_ms`.
3. C52 accepts that command through its existing local command polling, logs
   `WAKE_CONFIRMED` and `COMMAND_CAPTURE_REQUEST`, then allocates a command-only
   stream id/generation.  Command PCM uses the existing HTTP voice-turn client,
   not the wake UDP transport.
4. S3 treats a valid command-generation HTTP turn as command PCM and forwards
   it to the existing Server voice API without another WakeNet decision.
5. C52 states follow `WAKE_LISTENING -> WAKE_DETECTED -> COMMAND_LISTENING ->
   COMMAND_SPEECH_END -> PROCESSING -> TTS_PLAYING -> WAKE_LISTENING` through
   the established voice-chain state and resource lease ownership.
6. The Latin-only bundled LCD font now shows `WAKE CONFIRMED` and `SPEAK NOW`,
   the visible equivalents of the required wake and speak-now states.  It does
   not add a CJK font asset or alter LCD memory admission.
7. The existing wake-prompt cache is deliberately not invoked: it performs a
   synchronous C5-to-S3 HTTP/spool/play transaction with a 9-second total
   timeout and would block the command-capture handoff.  There is no existing
   asynchronous local beep owner.  Therefore this repair provides the immediate
   LCD prompt and logs, but does not claim an audible wake prompt until a
   separately owned asynchronous speaker job is implemented and tested.  C52
   now logs `WAKE_PROMPT_SKIPPED reason=no_nonblocking_local_prompt_resource`
   and uses a `MIC_TO_IDLE -> IDLE_TO_MIC` capture restart, rather than claiming
   a wake-ack speaker playback or speaker ownership that does not occur.

## Server Chain

The Server implementation already exists: `POST /api/voice/turn` runs ASR,
LLM, and TTS and returns PCM.  This task reuses it; it does not add a second
ASR/LLM/TTS protocol.

`server=0` is the S3 network worker's debounced `server_ready` condition, not
evidence that the voice modules are absent. With a healthy local STA/link but
`server=0`, `server_client_post_voice_turn()` returns a synthesized `503`, so
S3 and C52 both record `VOICE_RESPONSE_FAILED reason=server_unavailable` rather
than silently returning a generic failure. A real local STA/link loss retains
`gateway_offline`. The local capture control does not preflight or depend on
Server readiness; later command turns use the existing route automatically
after the network worker probe marks the Server ready again.

## Required Runtime Log Order

1. `WAKENET_DETECTED source_device=C52 ...`
2. `WAKE_EVENT ... voice_state=WAKE_DETECTED`
3. `WAKE_CONFIRMED` on S3, then C52
4. `WAKE_EVENT_DISPATCHED`
5. `COMMAND_CAPTURE_REQUEST ... command_generation=...`
6. `WAKE_STREAM_CLOSE ... reason=voice_end detected=1`
7. `COMMAND_STREAM_OPEN ... stream_id=... generation=...`
8. `COMMAND_PCM_RX ... stream_id=... generation=...`
9. `COMMAND_STREAM_CLOSE ...`
10. Server-side `[voice-turn] ASR_STREAM_START`, `ASR_RESULT`, `LLM_REQUEST`,
    and `TTS_START`, followed by `tts_success`; C52 then logs
    `AUDIO_PLAYBACK_START` when the Server chain is available. Otherwise S3
    and C52 report `VOICE_RESPONSE_FAILED reason=server_unavailable`.

## Verification Boundary

- Source-level review verifies event subscription, local control dispatch,
  separate wake/command stream identity, and explicit Server failure paths.
- `idf.py -C ESPC52 -B /tmp/espc52-wake-command-build build` passed.  The
  generated `00_Learn.bin` is `0x1c5f80`; the smallest app partition has 65%
  free space.
- `idf.py -C ESPS3 -B /tmp/esps3-wake-command-build build` passed.  The
  generated `sensair_s3_gateway.bin` is `0x178a30`; the smallest app partition
  has 79% free space.
- No flash, serial monitor, microphone exercise, LCD inspection, Server
  availability probe, external ASR/LLM/TTS call, or end-to-end hardware claim
  is included in this record.

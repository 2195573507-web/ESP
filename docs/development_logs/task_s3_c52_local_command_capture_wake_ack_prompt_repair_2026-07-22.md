# S3/C52 local command capture wake-ack prompt repair - 2026-07-22

## Scope

- S3 remains the owner of WakeNet detection and generation-tagged command
  creation.
- C52 remains the owner of microphone, prompt playback, MIC/ADC handoff, and
  command-audio capture.
- The S3 local endpoint is
  `POST /local/v1/commands/{command_id}/ack`.
- Excluded: WakeNet PCM transport and inference behavior, server ASR/LLM/TTS
  behavior, flash, serial monitor, and hardware/end-to-end acceptance.

## Fault and Correction

The observed C52 trace stopped at `MIC_ADC_STOPPED reason=wake_ack_prompt`.
That stop is required for half-duplex speaker ownership, but a successful
prompt handoff must return ownership to MIC and explicitly restart the ADC/VAD
before command PCM can be captured.

The repair keeps the prompt asynchronous and uses the prompt completion event
as the single transition point. After `wake_ack_prompt` completes, C52 releases
the speaker session, arms the command-capture stream and its S3 metadata,
opens the recording window, restarts MIC/ADC/VAD, and only then marks the
command capture ready. A stale prompt completion is rejected by lease
generation and state checks; a failed handoff or MIC restart aborts the round
instead of capturing on an unknown audio owner.

## Source Flow

```text
S3 WakeNet detected
  -> voice_proxy creates voice.start_command_capture
  -> C52 polls and accepts command
  -> C52 posts /local/v1/commands/{command_id}/ack
  -> S3 voice_proxy validates local id, command id, success flag, and pending generation
  -> WAKE_CONFIRMED -> COMMAND_LISTENING
  -> wake_ack_prompt playback owns speaker; MIC ADC is stopped and released
  -> prompt completion releases speaker
  -> command stream armed for command-capture VAD -> MIC/ADC/VAD restart
  -> COMMAND_AUDIO_CAPTURE / command PCM to the existing S3 voice-turn path
```

The previous terminal `MIC_ADC_STOPPED reason=wake_ack_prompt` is therefore an
intermediate half-duplex state, not the intended terminal state. The expected
source-level follow-up is `VOICE_AUDIO_HANDOFF direction=SPEAKER_TO_MIC`,
`MIC_LISTENING_RESTARTED reason=wake_prompt_recording`,
`MIC_ADC_START reason=wake_ack_prompt`, `MIC_RECORD_READY`, and
`COMMAND_AUDIO_CAPTURE` before command speech and PCM markers arrive.

## ACK Endpoint Behavior

`local_http_server` keeps the existing wildcard registration for the local
command ACK endpoint. It first gives an ACK to `voice_proxy`; a matching local
voice command is consumed there and returns `{"status":"ok"}`. The match
requires the path/body command id, the expected C52 local device id, an `ok`
value, and an active pending command-capture generation. The matched ACK is
marked locally by `command_router_ack_local_voice()` without forwarding a
second generic Server ACK.

ACKs not owned by a pending local voice capture retain the established generic
scheduler and `command_router_ack()` path. This preserves remote command ACK
behavior while making `COMMAND_LISTENING` a real local voice transition.

## Affected Paths

- `ESPS3/components/Middlewares/local_http_server/local_http_server.c`
- `ESPS3/components/Middlewares/voice_proxy/voice_proxy.[ch]`
- `ESPS3/components/Middlewares/command_router/command_router.[ch]`
- `ESPC52/components/Middlewares/command_domain/system_command/system_server_client.c`
- `ESPC52/components/Middlewares/voice_domain/voice_chain.[ch]`
- `ESPC52/components/Middlewares/mic/mic_adc_test.[ch]`

## Expected State and Log Order

1. S3 records `WAKE_CONFIRMED` and emits `voice.start_command_capture`.
2. C52 accepts the command, ACKs it locally, and records `WAKE_CONFIRMED` then
   `COMMAND_LISTENING`.
3. C52 enters `VOICE_WAKE_ACK`; `wake_ack_prompt` transitions audio ownership
   `MIC_TO_SPEAKER` and logs `MIC_ADC_STOPPED reason=wake_ack_prompt`.
4. Prompt completion releases speaker ownership, arms the command stream,
   starts/restarts MIC/ADC/VAD, and records `MIC_ADC_START`, `MIC_RECORD_READY`,
   and `COMMAND_AUDIO_CAPTURE`.
5. C52 command speech emits the existing command stream and PCM markers toward
   S3.

This ordering is source-level intent. It does not prove timing, physical audio
ownership, ADC restart, prompt audibility, or PCM delivery on a device.

## Verification Boundary

- Source review confirms the local ACK routing, generation/lease guards,
  half-duplex ownership handoff, command-stream arming, and post-prompt MIC
  restart path.
- ESP-IDF 5.5.4 with the configured Python 3.14 environment completed both
  isolated builds successfully:
  `idf.py -C ESPC52 -B /tmp/espc52-command-capture-build build` produced
  `/tmp/espc52-command-capture-build/00_Learn.elf`; and
  `idf.py -C ESPS3 -B /tmp/esps3-command-ack-build build` produced
  `/tmp/esps3-command-ack-build/sensair_s3_gateway.elf`.
- `git diff --check` was clean for the relevant firmware changes.
- No flash, serial monitor, microphone exercise, WakeNet inference test,
  prompt playback inspection, local HTTP transaction, or end-to-end voice
  claim is included in this record.

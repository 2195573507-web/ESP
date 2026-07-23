# Voice Pipeline Repair Log

Date: 2026-07-22

## Scope and Current Faults

This repair covers the ESPC52 to ESPS3 voice pipeline only. It does not flash either target, open a serial monitor, or claim device or end-to-end acceptance.

The audit found four closure failures:

1. `WAKE_PROMPT_PLAYBACK_END` was queued but not consumed by the C52 `voice_chain` task, so wake acknowledgement could end without command capture or listener restoration.
2. Audio direction ownership could be changed from callbacks and did not record a complete Mic-to-Speaker-to-Mic path.
3. UDP pre-roll could be emitted as a burst faster than real audio time, increasing socket-queue loss; the receiver had gap recovery behavior that could continue WakeNet inference after a discontinuity.
4. The S3 command-capture session lacked a single identity across wake, ACK, PCM upload, and response. In particular, an ACK could disable the only timeout and retain the session indefinitely when capture never began.

## Repair Summary

### C52 state ownership

`voice_chain` is the sole transition and resource-orchestration task. Wake prompt, server playback, server completion/error, gateway loss, and mic terminal callbacks enqueue an event only. Playback-start callback uses a bounded reply semaphore, but the task performs the state and audio-phase change.

Every actual change is logged as:

```text
VOICE_STATE_TRANSITION from=<state> to=<state> reason=<reason>
```

The post-prompt task path is now mandatory:

```text
WAKE_PROMPT_PLAYBACK_END
  -> Speaker DMA release
  -> Mic DMA rebuild
  -> COMMAND_CAPTURE_START
  -> COMMAND_CAPTURE_END
  -> COMMAND_UPLOAD
```

Any release/rebuild/metadata failure transitions through `VOICE_RECOVERY` and logs `WAKE_LISTENER_RESTORE` before re-entering wake listening.

### Audio transport

C52 retains UDP and the 64-slot FIFO. Only its sender task assigns a sequence when the FIFO head is serialized. PCM, including pre-roll, is paced by `samples / 16000` so replay cannot burst faster than the audio clock. S3 treats a real sequence gap as fail-closed: it records loss metrics, closes that stream, and does not feed discontinuous PCM to WakeNet.

Both sides emit rate-limited `PCM_STREAM_STATS` with `tx_frames`, `rx_frames`, `lost_frames`, `gap_count`, and `max_gap`. There is no per-frame UART logging.

### Command-session contract

The S3 session is bound to `command_id`, `generation`, `stream_id`, and `state`.

- Command poll creates `COMMAND_SESSION_CREATE` and retains legacy `command_generation` and `command_timeout_ms` fields.
- C52 ACK retains `id`, `cid`, and `ok`, and adds `command_id`, `generation`, `stream_id: 0`, and `state: ACK_SENT` or `RECOVERY`.
- C52 command PCM retains generation and stream headers and adds `X-Command-Id` and `X-Command-State: CAPTURE_END`.
- S3 accepts the legacy ACK form for compatibility but validates all supplied session fields. Its timeout remains live after ACK until capture starts; timeout clears the pending session into `RECOVERY`.

Lifecycle logs are `WAKE_EVENT -> COMMAND_SESSION_CREATE -> ACK_SENT -> CAPTURE_START -> CAPTURE_END -> PROCESSING -> RESPONSE`; terminal failure logs `RECOVERY`.

## State Machine

```text
VOICE_IDLE
  -> VOICE_WAKE_LISTENING
  -> VOICE_WAKE_DETECTED
  -> VOICE_ACK_PLAYBACK
  -> VOICE_COMMAND_CAPTURE
  -> VOICE_COMMAND_UPLOAD
  -> VOICE_RESPONSE_PLAYBACK
  -> VOICE_RECOVERY
  -> VOICE_WAKE_LISTENING

VOICE_ACK_PLAYBACK --(prompt end failure)--> VOICE_RECOVERY
VOICE_COMMAND_CAPTURE --(timeout/error)--> VOICE_RECOVERY
VOICE_COMMAND_UPLOAD --(server error)--> VOICE_RECOVERY
VOICE_RESPONSE_PLAYBACK --(done/error)--> VOICE_RECOVERY
```

## Resource Ownership Table

| Resource | Owner while listening | Owner during acknowledgement/response | Release and restore rule |
| --- | --- | --- | --- |
| Mic ADC DMA | Mic/VAD | Released by `voice_chain` before Speaker TX; rebuilt by `voice_chain` for command capture/listening | `MIC_LISTENING_ACTIVE -> MIC_PAUSE_REQUESTED -> MIC_DMA_RELEASED`; no callback deinit |
| Speaker I2S TX DMA | None | `speaker_player` writer only after the voice task records `SPEAKER_TX_OWNED` | Drain/release precedes `SPEAKER_TX_RELEASED` and Mic handoff |
| WiFi | Network subsystem | Network subsystem | Never paused by the voice lease |
| HTTP | Voice requests plus admitted command ACK | Voice requests plus admitted command ACK | Normal requests gated before BME/workers; reopened last |
| BME | BME service | Paused | Resume after audio release, before normal HTTP gate opens |
| Radar | Radar state client / system worker | Paused as non-voice worker work | Worker queues quiesce after HTTP/BME; resume before normal HTTP gate opens |

The resource manager performs no wait while holding its lock. It uses the fixed order `HTTP -> BME -> workers` for quiesce and `audio -> BME -> workers -> HTTP` for release. Generation checks reject stale audio-phase events. The allowed phase graph now includes final listener restoration from `MIC_RECORD_READY` or `SPEAKER_TX_RELEASED` to `MIC_LISTENING_ACTIVE`.

## Modified Files

- `ESPC52/components/Middlewares/voice_domain/voice_chain.c`
- `ESPC52/components/Middlewares/voice_domain/voice_chain.h`
- `ESPC52/components/Middlewares/wake/local_wake_word.c`
- `ESPC52/components/Middlewares/wake/local_wake_word.h`
- `ESPC52/components/Middlewares/mic/mic_adc_test.c`
- `ESPC52/components/Middlewares/mic/mic_adc_test.h`
- `ESPC52/components/Middlewares/mic/mic_vad.c`
- `ESPC52/components/Middlewares/mic/mic_vad.h`
- `ESPC52/components/Middlewares/speaker/speaker_player.c`
- `ESPC52/components/Middlewares/runtime/c5_resource_manager.c`
- `ESPC52/components/Middlewares/server_voice/server_voice_client.c`
- `ESPC52/components/Middlewares/server_voice/server_voice_client.h`
- `ESPC52/components/Middlewares/command_domain/system_command/system_server_client.c`
- `ESPC52/components/Middlewares/voice_transport/c5_audio_transport.c`
- `ESPS3/components/Middlewares/audio_wake_gateway/audio_wake_gateway.c`
- `ESPS3/components/Middlewares/command_router/command_router.c`
- `ESPS3/components/Middlewares/command_router/command_router.h`
- `ESPS3/components/Middlewares/voice_proxy/voice_proxy.c`
- `ESPS3/components/Middlewares/voice_proxy/voice_proxy.h`

## Build-Only Verification

ESP-IDF 5.5.4 with `IDF_PYTHON_ENV_PATH=/Users/zhiqin/.espressif/tools/python_env/idf5.5_py3.14_env` completed clean isolated builds:

```text
idf.py -C ESPC52 -B /tmp/voice-pipeline-c52-build build
  PASS: /tmp/voice-pipeline-c52-build/00_Learn.bin

idf.py -C ESPS3 -B /tmp/voice-pipeline-s3-build build
  PASS: /tmp/voice-pipeline-s3-build/sensair_s3_gateway.bin
```

`git diff --check` also passes for this repair surface. No flash, monitor, UDP runtime test, DMA stress test, or C5-to-S3 command turn was performed.

## Remaining Risk

Build proof does not establish device behavior. In particular, UDP can still lose or reorder datagrams in the real RF environment; the new design fails closed rather than recovering. Device acceptance must confirm S3 `PCM_STREAM_STATS` for a real stream reports `lost_frames=0 gap_count=0 max_gap=0`, and must exercise prompt completion, command capture timeout, response playback, and resource restoration under BME/radar/HTTP load.

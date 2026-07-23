# C52, S3, and Server Voice Round Repair - 2026-07-22

## Scope and Boundaries

- Scope: C52 -> S3 PCM transport, S3 WakeNet, local C52 wake prompt, command capture,
  S3 Server voice proxy, response playback handoff, and voice request admission.
- Audio contract retained: 16000 Hz, mono, PCM16 little-endian.
- Excluded: ADC gain/channel configuration, PCM scaling, WakeNet model/name, LCD business
  pages, Radar, BME690, Alarm, unrelated networking, flashing, monitor, and hardware tests.
- Verification is limited to `idf.py build` for C52 and S3 plus the Server's existing minimal
  static check when applicable.

## Execution Record

### 2026-07-22 - Start

- Task evidence: three supplied runtime-log segments and the stated end-to-end state chain.
- Initial source review confirms the active roots are `ESPC52`, `ESPS3`, and `ESP-server`.
- Existing workspace changes, including deleted generated build artifacts and unrelated Server
  edits, are preserved and excluded from this task's ownership.
- Parallel ownership:
  - PCM Transport + WakeNet: C52 transport sequencing and S3 session/feed logic.
  - Wake Prompt + Command Capture + Resource: C52 half-duplex handoff, prompt completion,
    command lifecycle, staged timeouts, and audio-phase ownership.
  - Server Voice: S3 proxy/finalize/response and `voice_busy` request classification.
  - Integration/Documentation: conflict review, build validation, and this record.

## Decisions

- The PCM sequence is assigned only by the C52 transmission owner in final wire order; S3 keeps
  strict gap detection.
- Wake and command streams remain distinct transaction types. A detected wake session only
  authorizes the command transaction; it does not reuse its PCM stream state.
- Prompt playback is half-duplex: Mic DMA must be released before speaker ownership and restored
  only from a completion/failure event, never a guessed delay.

## PCM Transport + WakeNet Agent Record

### Root-Cause Mapping

- The supplied `S3_PCM_RX_GAP expected=8 received=42` evidence, whose total missing frames
  matched `VOICE_PRE_ROLL_TX frame_count=40`, maps to the former sequencing boundary: PCM
  producers could reserve a sequence while another queued descriptor could still precede it on
  the wire. That made the queue's actual send order disagree with the assigned sequence. The
  repair does not relax S3 gap detection.
- The supplied `WAKENET_FEED_STATS ... inference_count=150 last_result=0` followed by
  `WAKE_NOT_DETECTED` proves that frames reached the model, but not that its input history was
  continuous or session-clean. The repaired path makes discontinuity, stale-session residue, and
  insufficient final frames observable before attributing a miss to the model or microphone.
- This mapping is source-level only. It does not establish that a physical C52/S3 pair has stopped
  producing gaps or that WakeNet detects the wake word on captured audio.

### C52 Ordered Wire Design

- `c5_audio_transport_start()` creates a new nonzero stream id from the per-transport seed and
  generation, resets `s_sequence` and `s_sample_counter` to zero, emits
  `PCM_SEQUENCE_RESET`, and queues `VOICE_START`.
- The Mic side queues `PRE_ROLL_BEGIN`, every pre-roll PCM slot, then the `PRE_ROLL_END` /
  `LIVE_BEGIN` marker before it admits live PCM. The bounded 64-slot FIFO is the single ordering
  authority, so live descriptors cannot overtake the queued pre-roll descriptors.
- Only `c5_audio_sender_task()` materializes the protocol frame at FIFO head. It assigns the
  current sequence immediately before `sendto()` and increments it only after the full datagram
  is sent. No capture-side producer owns or reserves a sequence.
- `c5_audio_transport_finish()` queues `VOICE_END` behind all earlier PCM. `s_closing` prevents a
  new stream while that close is pending; an abort intentionally discards queued PCM before it
  queues `VOICE_ABORT`.

The intended successful wire order is therefore:

`VOICE_START -> PRE_ROLL_BEGIN marker -> pre-roll PCM (0..n) -> PRE_ROLL_END/LIVE_BEGIN marker -> live PCM (n+1..) -> VOICE_END`.

### PCM and WakeNet Contract

- C52 Mic configuration is 16000 Hz. `mic_adc_pcm_convert_sample()` produces signed `int16_t`
  PCM, and C52 transport stamps every audio frame as 16000 Hz, one channel, 16 bits. The task
  contract is mono `pcm16le`; neither ADC gain nor PCM scaling was changed for this workstream.
- S3 rejects any packet whose sample rate, bit width, or channel count differs from
  `16000 / 16 / 1`, and rejects malformed PCM payload/sample-count pairs before buffering.
- On `VOICE_START`, S3 records the start sequence as the next expected value and calls
  `reset_session()`. It cleans WakeNet internal history, clears the PCM residue and all per-stream
  counters/flags, then binds the active source and stream. A duplicate `VOICE_START` for the
  already-active stream is ignored so it cannot create a false subsequent gap; a different start
  closes the old session before resetting the new one.
- Network PCM is appended in arrival order to the session buffer. The Mic's normal live block is
  160 samples; `feed_available_chunks()` waits until the model-reported `s_chunk_samples` is
  available, feeds one contiguous `int16_t` frame, then retains and compacts the remaining
  samples. The configured model must report 16 kHz mono; the required 512-sample WakeNet-frame
  case is thus handled as four accumulated 160-sample blocks plus the next samples, without an
  artificial frame boundary. A final remainder smaller than the model frame is deliberately
  logged and dropped at session close.
- A detected sequence gap remains a hard diagnostic: S3 logs the missing range and phase,
  clears the partial PCM residue, and cleans WakeNet history before accepting later frames. It is
  not hidden or counted as a successful continuous feed.

### Added Diagnostics

- C52: `PCM_SEQUENCE_RESET`, `PCM_STREAM_OPEN`, `PCM_PRE_ROLL_BEGIN`, `PCM_PRE_ROLL_END`,
  `PCM_LIVE_BEGIN`, queue-pressure/drop summaries, and `PCM_STREAM_CLOSE_SENT` make allocator
  lifecycle and wire-boundary order observable.
- S3: `WAKENET_SESSION_RESET`, `S3_PCM_FIRST_FRAME`, strict `S3_PCM_RX_GAP` with
  `stream_id`, `expected`, `received`, `missing`, and `phase`; `WAKENET_FIRST_FEED` with
  `pcm_format=pcm16le` and `sample_rate=16000`; throttled `WAKENET_FEED_STATS`; and terminal
  `WAKE_DETECTED`, `WAKE_NOT_DETECTED`, and `WAKENET_SESSION_TAIL_DROPPED` distinguish a model
  miss, a sequence discontinuity, and an incomplete final model frame.

## Completion Evidence

### Prompt, Command, and Resource Ownership

- `wake/resources/wake_ack_zh.pcm` is a firmware-embedded 58,752-byte PCM16LE resource
  (`16000 Hz`, mono, about 1.836 seconds). `local_wake_word_start_prompt_async()` queues the
  playback task and reports completion only after `audio_player_play_16k_pcm()` has drained the
  speaker stream; it does not use `vTaskDelay()` as a completion signal.
- The unique half-duplex order is Mic pause/release -> `SPEAKER_TX_OWNED` -> prompt playback ->
  speaker session release -> `SPEAKER_TX_RELEASED` -> Mic restart -> `MIC_RECORD_READY`.
  Prompt failure reports `WAKE_PROMPT_FAILED ... fallback=continue_command_capture` and uses the
  same completion path. A late prompt callback is rejected as `WAKE_PROMPT_EVENT_STALE`.
- Command capture arms before the Mic restart. After a fixed 20 ms command pre-roll it opens the
  HTTP command stream and starts PCM TX regardless of VAD start state. The command VAD profile
  (`350 RMS`, `900` peak, one confirmation frame, 1.5 s hangover, 15 s max active duration) only
  marks speech and closes it; the wait/open logic no longer relies on the standby 1800-RMS gate.
- `voice_chain` now owns phase timers: wait-speech is 8 s, active speech is 15 s, local finalize
  is 5 s, and server response wait is 90 s. When max active speech expires, it requests Mic-owner
  finalize and retains recorded PCM. Only no-speech wait expiry or an unrecoverable later phase
  takes the abort path. Every timeout validates command generation and voice lease first.
- `c5_resource_manager` now records both accepted and refused transitions as
  `AUDIO_PHASE_TRANSITION`; generation mismatch also emits `AUDIO_PHASE_EVENT_STALE`. This keeps
  the legal graph explicit: `MIC_LISTENING_ACTIVE|MIC_RECORD_READY -> MIC_PAUSE_REQUESTED ->
  MIC_DMA_RELEASED -> SPEAKER_TX_OWNED -> SPEAKER_TX_RELEASED -> MIC_RECORD_READY`.

### Server Round and Admission

- The Mic's normal VAD completion closes the command PCM stream, pauses and releases ADC DMA,
  then `server_voice_client_finish_turn()` enters the S3 proxy request. S3 logs
  `SERVER_VOICE_FINALIZE_BEGIN`, forwards the fixed 16 kHz mono PCM to the local Server, validates
  response metadata, and returns audio to the persistent C52 response worker.
- C52 validates the returned `X-Audio-Sample-Rate: 16000` and `X-Audio-Channels: 1`, streams the
  PCM through `speaker_player`, waits for its drain event, releases speaker DMA, and restores the
  listening runtime. Empty successful audio is an explicit failure, rather than a silent success.
- The Server emits the matching audio headers. S3 labels its single completed command-body report
  `COMMAND_PCM_RX_STATS`, avoiding per-160-sample request logging.
- While the voice lease is busy, C52's `server_comm_http` accepts only a matching current
  command-generation / command-id ACK to the exact local ACK endpoint as `VOICE_CONTROL`;
  stale, mismatched, and ordinary requests remain blocked and emit `HTTP_VOICE_BUSY_ADMISSION`.

### Source Files Touched for This Repair

- C52 transport: `voice_transport/c5_audio_transport.{c,h}` and Mic transport hooks make the
  sender FIFO the sequence allocator and preserve the ordered pre-roll/live/close boundary.
- S3 wake ingress: `audio_wake_gateway/audio_wake_gateway.{c,h}` validates PCM format, resets
  model/residue state per stream, reassembles model-size frames, and keeps strict phase-aware gaps.
- C52 round owner: `voice_chain`, `mic_adc_test`, `mic_vad`, `local_wake_word`,
  `c5_resource_manager`, and `app_debug_config` implement prompt completion, immediate command
  capture, distinct VAD profiles, staged deadlines, and generation-safe ownership.
- S3/Server control: `voice_proxy`, C52 `server_comm_http` and `system_server_client`, plus
  Server `src/voice/http.js` and `src/voice/chain.js`, carry capture metadata, response format,
  finalization diagnostics, and narrow busy-time admission.

### New Diagnostics

- Transport/WakeNet: `PCM_SEQUENCE_RESET`, `PCM_PRE_ROLL_BEGIN`, `PCM_PRE_ROLL_END`,
  `PCM_LIVE_BEGIN`, `S3_PCM_FIRST_FRAME`, `S3_PCM_RX_GAP`, `WAKENET_SESSION_RESET`,
  `WAKENET_FIRST_FEED`, `WAKE_DETECTED`, and `WAKE_NOT_DETECTED`.
- Prompt/command/resource: `WAKE_PROMPT_REQUEST`, `WAKE_PROMPT_PLAYBACK_BEGIN`,
  `WAKE_PROMPT_PLAYBACK_END`, `COMMAND_STREAM_OPEN`, `COMMAND_PCM_TX_START`, throttled
  `COMMAND_PCM_TX_STATS`, `COMMAND_SPEECH_DETECTED`, `COMMAND_SPEECH_END`,
  `COMMAND_CAPTURE_TIMEOUT`, `VAD_CONFIG`, `VAD_MAX_ACTIVE_REACHED`,
  `AUDIO_PHASE_TRANSITION`, and `AUDIO_PHASE_EVENT_STALE`.
- Finalization/playback: `SERVER_VOICE_FINALIZE_BEGIN`, `SERVER_RESPONSE_BEGIN`,
  `RESPONSE_AUDIO_READY`, `PLAYBACK_BEGIN`, `PLAYBACK_END`, and `VOICE_ROUND_COMPLETE`.

### Validation

- `git diff --check` on all task-owned C52, S3, Server, and task-log files: passed.
- `ESPC52`: `idf.py -B /tmp/espc52-voice-round-final build`: passed under ESP-IDF 5.5.4 and the
  configured Python 3.14 environment.
- `ESPS3`: `idf.py -B /tmp/esps3-voice-round-final build`: passed under the same environment.
- `ESP-server`: `node --check src/voice/http.js` and `node --check src/voice/chain.js`: passed.

No flash, monitor, microphone capture, physical WakeNet detection, local-Server transaction, or
end-to-end voice-round test was performed. The repaired state chain is source- and build-verified
only; device/runtime acceptance remains pending hardware evidence.

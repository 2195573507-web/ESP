# Findings: C52/S3/Server Voice Round Repair

## Initial Source Findings

- `ESPC52/components/Middlewares/voice_transport/c5_audio_transport.c` is the PCM wire-order
  owner. Before this task it assigned a sequence when enqueueing, allowing queue scheduling to
  become the source of wire-order disagreement.
- `ESPS3/components/Middlewares/audio_wake_gateway/audio_wake_gateway.c` maintains strict
  expected-sequence tracking and feeds the model in its declared frame size. Its session reset
  and diagnostics need stream/phase evidence adequate to distinguish pre-roll from live data.
- `ESPC52/components/Middlewares/voice_domain/voice_chain.c` visibly contains the old
  `wake_ack_no_prompt` handoff and a single command timeout path. This is the C52 integration
  focus for local-prompt completion and staged command capture.
- `ESPS3/components/Middlewares/voice_proxy/voice_proxy.c` currently owns the S3 command
  authorization and proxy transaction. It logs command PCM without throttling and uses one
  pending capture expiry, which must no longer abort valid recorded audio.

## PCM Transport + WakeNet Findings

### C52 Transport Root Cause and Repair

- The gap pattern in the supplied log is consistent with the old split between enqueue-time
  sequence reservation and later sender scheduling: 40 missing sequences align with the 40-frame
  pre-roll, so pre-roll/live ordering was the first cause to remove. This is a root-cause mapping,
  not a claim that the supplied device log has been reproduced after the change.
- `c5_audio_transport.c` now owns a stream-scoped allocator in its single sender task. Producers
  enqueue slots without a sequence. At FIFO head, `c5_audio_transport_send_slot()` encodes the
  current sequence, sends the datagram, and only then increments it on a successful full send.
- The Mic queues the pre-roll marker and all pre-roll samples before the live marker. Because the
  same bounded FIFO carries markers, PCM, and the eventual close control frame, the implemented
  sequence is `VOICE_START`, pre-roll, live, then `VOICE_END`; live PCM cannot bypass pre-roll and
  close cannot bypass already-queued PCM.
- New stream admission resets sequence, sample counter, counters, and phase. A close remains
  pending until its queued control frame is sent; this prevents reusing sequence state while the
  old stream still has a wire operation outstanding.

### Format and S3 Session Behavior

- The C52 capture/transport contract is 16000 Hz, mono, signed 16-bit little-endian PCM. The Mic
  header fixes the sample rate at 16000 and defines a 160-sample (10 ms) live chunk;
  `mic_adc_pcm_convert_sample()` returns `int16_t`; transport stamps `16000 / 16 / 1` on every
  frame. No ADC gain, channel setup, PCM scaling, or WakeNet model selection was changed here.
- `audio_wake_gateway.c` rejects packets outside that contract, tracks the expected stream
  sequence, and logs rather than suppresses gaps. A gap records the missing sequence units,
  clears incomplete PCM residue, keeps the existing WakeNet model instance, and resumes feeding
  once a complete model chunk is reassembled. The defective binary `clean()` path is never used.
- Each accepted `VOICE_START` calls `reset_session()`: it captures `buffered_samples_before`,
  clears the application-owned 4096-sample PCM workspace accounting and detection flags while
  retaining the boot-created model, and establishes the new source/stream. Duplicate opens for
  the active stream are ignored; replacement streams close the old session first.
- Incoming PCM is copied in order into a contiguous `int16_t` buffer. Feed occurs only once the
  WakeNet-reported chunk size is available. The task requirement's 512-sample frame is therefore
  covered by buffered reassembly of 160-sample network frames; source intentionally derives the
  actual size from the selected model and rejects non-16 kHz/non-mono models. On close, any
  remainder smaller than that model frame is reported as `WAKENET_SESSION_TAIL_DROPPED`, not fed
  as a partial inference frame.

### New Diagnostic Evidence

- C52 logging: `PCM_SEQUENCE_RESET`, `PCM_PRE_ROLL_BEGIN`, `PCM_PRE_ROLL_END`,
  `PCM_LIVE_BEGIN`, `PCM_STREAM_OPEN`, send/drop/queue summaries, and `PCM_STREAM_CLOSE_SENT`.
- S3 logging: `WAKENET_SESSION_RESET`, `S3_PCM_FIRST_FRAME`, phase-qualified `S3_PCM_RX_GAP`,
  `WAKENET_FIRST_FEED` (`pcm16le`, 16000), `WAKENET_FEED_STATS`, `WAKE_DETECTED`,
  `WAKE_NOT_DETECTED`, and `WAKENET_SESSION_TAIL_DROPPED`.
- These markers provide the next hardware-validation checklist. No flashing, monitor session,
  audio capture, WakeNet detection run, or end-to-end voice round was performed while preparing
  this documentation.

## Non-Task State Preserved

- The worktree already contains broad generated-build deletions plus unrelated source edits.
  They are not part of this repair and will not be restored, staged, or cited as task changes.

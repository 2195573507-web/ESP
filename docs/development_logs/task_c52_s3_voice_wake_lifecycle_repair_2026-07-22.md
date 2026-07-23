# 2026-07-22 - C52 to S3 voice wake lifecycle repair

**Status:** scoped source changes and isolated C52/S3 builds are complete.
Flash, serial, microphone, and end-to-end wake acceptance remain pending.

## Scope and ownership

This record covers only the C52-to-S3 voice wake chain.  It excludes LCD,
radar, BME, alarm, network business logic, HTTP business logic, ADC gain,
clipping counters, and PCM amplitude calibration.

| Role | Scope | Status |
| --- | --- | --- |
| Agent 1 | C52 VAD state machine | Complete |
| Agent 2 | C52 UDP voice stream and S3 sequence lifecycle | Complete |
| Agent 3 | S3 WakeNet-to-voice-turn audit and traceability | Complete |
| Agent 4 | C52 statistics lifecycle | Complete |
| Documentation | This record and development index | Active |

## Recorded work

1. Repaired `ESPC52/components/Middlewares/mic/mic_adc_test.c`
   statistics path.
   `mic_adc_window_reset()` resets all per-window sample metrics except
   `zero_sample_count`, while `mic_adc_window_add()` increments it and
   `MIC_SIGNAL_STATS` prints it.  This leaks the previous window's count into
   each later window and explains unbounded, untrustworthy values.  The fix
   now resets that counter with the rest of the window state; it does not alter
   ADC conversion, gain, clipping accounting, or PCM calibration.
2. Repaired `ESPC52/components/Middlewares/mic/mic_vad.[ch]`: fixed-frame
   normal termination was replaced with a named configured timeout, while
   normal completion requires the configured continuous silence.  A renewed
   speech frame in HANGOVER returns to SPEECH without ending the active stream.
   Once VAD emits `VOICE_START`, it emits exactly one matching `VOICE_END` for
   silence or timeout, and the C52 `VAD_SPEECH_END` trace now prints that
   reason.
3. Repaired C52 stream ownership in
   `ESPC52/components/Middlewares/voice_transport/c5_audio_transport.c`.
   Initialization now takes a randomized stream-ID seed; every open advances a
   nonzero generation and derives a new nonzero stream ID.  A close barrier
   prevents a following open until the queued END or ABORT is sent, while a
   sender failure drains stale queued frames before reuse.  This preserves the
   existing wire protocol and prevents an old queued close or PCM frame from
   contaminating the next stream.
4. Repaired S3 sequence and stream ownership in
   `ESPS3/components/Middlewares/audio_wake_gateway/audio_wake_gateway.c`.
   The observed `sequence_gap=34/36` was a false positive: a duplicate or
   reordered `VOICE_START` for the active source/stream called
   `reset_session()`, reset the expected sequence to `1`, and then treated the
   normal next PCM sequence `35/37` as missing `34/36` frames.  Same-stream
   START is now idempotent; a distinct incoming stream closes the stale one
   before opening; and gap accounting logs expected, received, and missing
   values using a wrap-safe signed delta.  Only a genuinely forward sequence
   jump increments `sequence_gap`.
5. Completed WakeNet-path audit and observability in
   `ESPS3/components/Middlewares/audio_wake_gateway/audio_wake_gateway.c`.
   The UDP path is: C52 PCM frame decode, WakeNet feed, detect callback/event
   enqueue, `voice_proxy`, `command_router`, C52 command capture,
   generation-tagged server voice turn, Server PCM response, and C52 playback.
   The gateway now records `WAKE_SESSION_START`, `WAKE_DETECTED`,
   `WAKE_NOT_DETECTED`, and `WAKE_TRIGGER_FORWARD`; all negative outcomes name
   a reason.  The established proxy and playback boundaries emit
   `VOICE_TURN_START`, `VOICE_TURN_FINISH`, `SERVER_RESPONSE_BEGIN`,
   `SERVER_RESPONSE_END`, `PLAYBACK_BEGIN`, and `PLAYBACK_END` with success or
   failure reasons.  HTTP PCM fallback uses equivalent wake-session markers.

## VAD state flow

`IDLE -- qualified speech --> SPEECH -- below-end threshold --> HANGOVER`

`HANGOVER -- renewed speech --> SPEECH`

`HANGOVER -- configured continuous silence --> VOICE_END(silence) --> IDLE`

`SPEECH or HANGOVER -- configured maximum-record timeout --> VOICE_END(timeout) --> IDLE`

The timeout is an explicit failsafe, not a normal fixed-frame completion.
The HANGOVER recovery transition retains the active voice stream, preventing a
new speech segment from being discarded or prematurely closed.

## Build result

Both required isolated builds passed with exit code 0:

- `idf.py -C ESPC52 -B /tmp/c52-s3-voice-lifecycle-c52 build` produced
  `00_Learn.bin`.
- `idf.py -C ESPS3 -B /tmp/c52-s3-voice-lifecycle-s3 build` produced
  `sensair_s3_gateway.bin`.

This is compile/link evidence only.  No flash, serial monitor, server,
microphone, WakeNet, UDP, or other hardware validation was run or claimed.

## Remaining risks

- Build evidence cannot prove UDP delivery, WakeNet model detection, command
  forwarding, server response, or playback on hardware.
- Runtime testing is still required to confirm that duplicated/reordered UDP
  START datagrams are ignored as intended and that real sequence loss produces
  the new expected/received/missing diagnostics.
- Existing unrelated dirty workspace changes are preserved and are not treated
  as evidence for this task.

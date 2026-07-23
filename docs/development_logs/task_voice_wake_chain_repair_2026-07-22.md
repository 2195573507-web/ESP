# 2026-07-22 - C5 VAD to S3 WakeNet voice-chain repair

**Status:** source repair and isolated C51/C52/S3 builds passed; flash, serial, microphone, and end-to-end hardware acceptance pending

## Scope

This task repairs the local wake path in `ESPC51`, `ESPC52`, and `ESPS3`.
The cloud/server voice conversation remains a separate path and is not required
for local WakeNet inference.

## Confirmed breakpoints

- C5 VAD start events were discarded when the legacy HTTP recording window was
  closed, so the S3 wake stream never opened.
- `c5_audio_transport` was compiled but had no start/append/finish call chain;
  only phase markers were reachable.
- S3 had no receiver for the C5 UDP/33435 PCM stream. WakeNet was only fed by
  the complete-body HTTP compatibility entry point.

## Repaired data flow

1. C5 ADC1_CH5 TYPE2 samples at 16 kHz are converted to mono signed 16-bit
   PCM using the existing converter and passed through the existing VAD.
2. On `VAD_SPEECH_START`, C51/C52 open one bounded UDP stream, send the bounded
   pre-roll, then append live PCM while VAD is active.
3. On `VAD_SPEECH_END`, C5 marks tail, sends the configured post-roll, and
   emits a stream end frame. Reset/pause paths abort any live stream first.
4. S3 listens on UDP/33435, validates the framed protocol, rejects mismatched
   or duplicate frames, reassembles fixed WakeNet chunks, and logs detection.
5. The existing HTTP detector remains available for compatibility and copies
   input chunks into S3-owned workspace before calling the ESP-SR interface.

## Diagnostics

The path now emits `MIC_SIGNAL_STATS`, `VAD_STATE`, `VAD_SPEECH_START`,
`VAD_SPEECH_END`, `VOICE_STREAM_OPEN`, `VOICE_PRE_ROLL_TX`,
`VOICE_PCM_TX_STATS`, `VOICE_STREAM_CLOSE`, `S3_PCM_RX_STATS`,
`WAKENET_INIT_*`, `WAKENET_FEED_STATS`, and `WAKE_EVENT` markers.

The PCM contract is 16,000 Hz, 16-bit, mono, little-endian samples in bounded
framed UDP payloads. ADC amplitude, DC offset, gain, clipping, packet loss,
and actual WakeNet detection still require hardware logs. The current UDP
header carries `source_id` and `stream_id`, but not the C5 VAD generation; S3
diagnostics therefore report `generation=not_on_wire` rather than inferring it.

## Build evidence

Commands use ESP-IDF 5.5.4 with the configured Python 3.14 environment and
isolated build directories:

```text
idf.py -C ESPC51 -B /tmp/voice-audit-espc51 build
idf.py -C ESPC52 -B /tmp/voice-audit-espc52 build
idf.py -C ESPS3 -B /tmp/voice-audit-esps3 build
```

All three isolated builds completed successfully. The builds must still be
reported separately from device acceptance. No flash, monitor, server smoke
test, or microphone test is part of this record.

## Hardware acceptance markers

Capture C51/C52 `MIC_SIGNAL_STATS` with quiet and spoken input, then verify
`VAD_SPEECH_START -> VOICE_STREAM_OPEN -> VOICE_PRE_ROLL_TX ->
VOICE_PCM_TX_STATS -> VOICE_STREAM_CLOSE`. On S3 verify
`S3_VOICE_RX_READY -> S3_PCM_STREAM_OPEN -> WAKENET_FEED_STATS` and finally
`WAKENET_DETECTED`/`WAKE_EVENT` for the configured wake word. Confirm
`sequence_gap=0`, no `PCM_STREAM_DROP`, and no `S3_PCM_RX_DROP` during a clean
test. Repeat with local server/cloud unavailable to prove this path remains
local and fail-closed.

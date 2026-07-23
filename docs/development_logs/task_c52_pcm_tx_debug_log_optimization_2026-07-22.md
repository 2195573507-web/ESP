# C52 PCM TX Debug Log Optimization

**Date:** 2026-07-22
**Status:** Isolated C52 build passed. This record does not claim flash,
monitor, or hardware audio acceptance.
**Scope:** `ESPC52/components/Middlewares/voice_transport/c5_audio_transport.c`
PCM TX diagnostic output only.

## Problem Evidence

The supplied watchdog report identifies `c5_audio_tx` on CPU0. The reported
blocking path is `uart_write -> uart_tx_char`. The existing transport emits a
high-frequency `PCM_TX_FRAME` UART record for each queue push, queue pop, and
successful socket send. During PCM traffic, those synchronous UART writes can
consume the sender task's scheduling budget even when the UDP path itself is
accepting datagrams normally.

## Logging Change

The transport retains diagnostics, but aggregates normal-path events into one
`PCM_TX_STATS` record rather than emitting a `PCM_TX_FRAME` line for every
frame. The record contains:

```text
stream_id
push_count
pop_count
send_count
send_error_count
drop_count
max_queue_depth
last_sequence
```

The statistics are emitted after every 100 successful wire sends, with a final
snapshot when `VOICE_END` or `VOICE_ABORT` is sent. `queue_push` and
`queue_pop` increment counters without normal-path UART output. Successful
`socket_send` is likewise represented by the summary cadence. Socket failures
retain their immediate error log and also increment `send_error_count`;
admission or queue failure paths continue contributing to `drop_count`.

## Preserved Behavior

- UDP packet format, destination, and transport ownership are unchanged.
- PCM sample format and payload construction are unchanged.
- Sequence allocation and the existing send-success advancement behavior are
  unchanged; `last_sequence` is observational only.
- WakeNet and VAD state machines, thresholds, and gating boundaries are
  unchanged.
- This change does not claim to repair any network delivery issue. It removes
  diagnostic UART load from the real-time C5 PCM sender so that audio behavior
  can be assessed without per-frame logging interference.

## Validation Boundary

The requested validation is build-only for the affected ESPC52 firmware. It
does not flash a device, collect serial output, establish task-WDT closure,
prove microphone/VAD/WakeNet behavior, or prove end-to-end UDP delivery.

`idf.py -C ESPC52 -B /tmp/espc52-pcm-tx-log-build build` completed with
`Project build complete` under ESP-IDF 5.5.4. The affected
`voice_transport/c5_audio_transport.c` object compiled and the app image was
generated at `/tmp/espc52-pcm-tx-log-build/00_Learn.bin`.

Device acceptance remains a separate step: while real PCM is active, confirm
periodic `PCM_TX_STATS` records, no unexpected increase in `drop_count` or
`send_error_count`, advancing `last_sequence`, and absence of `c5_audio_tx`
watchdog reports. Remote receive and audio-quality conclusions require their
own S3/network measurements.

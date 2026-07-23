# WakeNet to S3 with C5 VAD-gated PCM

**Date:** 2026-07-21  
**Status:** C51/C52/S3 builds passed; protocol completion and hardware acceptance remain pending  
**Scope:** `ESPC51`, `ESPC52`, `ESPS3`, and the shared protocol component. This
log records the migration only; it does not authorize or report flashing,
serial execution, microphone capture, VAD/WakeNet calibration, Wi-Fi, LCD, or
multi-device acceptance.

## Objective and root cause

WakeNet is being removed from both ESP32-C5 terminals and made an ESP32-S3-only
responsibility. The C5 remains responsible for microphone acquisition, local
VAD, pre-roll, tail audio, bounded queuing, and transport. It must not stream
idle/environment PCM.

The motivating C5 failure is contiguous internal-memory pressure, not aggregate
PSRAM exhaustion: the prior C5 WakeNet initialization reduced the internal
largest free block to roughly 5 KiB, while `mic_adc_task` needs a 12 KiB
continuous internal task stack. The resulting task creation can return
`ESP_ERR_NO_MEM`. Moving opaque ESP-SR/WakeNet state off C5 avoids treating
PSRAM total-free memory as a substitute for the required internal contiguous
block.

## Investigation record

The project layout was inspected before implementation: `ESPC51` and `ESPC52`
are peer C5 terminal firmware trees, `ESPS3` is the gateway, and
`shared_components/esp111_protocol_common` is the common protocol contract.
The C5 microphone path is `mic_adc_test.c` -> PCM conversion -> 200 ms window
statistics -> `mic_vad_process()` -> voice stream callbacks. C5 startup enters
`app_orchestrator_start()` from `app_startup_task`; S3 startup enters
`gateway_orchestrator_start()` from `gateway_startup_task`.

The prior C5 local WakeNet source has been replaced by compatibility hooks in
`Middlewares/wake/local_wake_word.c`: it logs that C5 WakeNet is disabled,
returns no WakeNet chunk size, does not detect, and lets a VAD start open the
recording path. The C5 manifests remove `espressif/esp-sr`; the C5 Middlewares
component removes its `espressif__esp-sr` requirement. The S3 Middlewares
component adds `espressif__esp-sr` and the `audio_wake_gateway` source; the
S3 root `main/idf_component.yml` now declares `espressif/esp-sr ==2.4.6` so the
component manager resolves the package before the CMake requirement is used.

The implementation is intentionally not a copy of the old C5 detector. The
S3 gateway reconstructs PCM independently of UDP datagram arrival and feeds
WakeNet in its reported fixed chunk size.

## Architecture

### Before the migration

```text
C5 microphone/ADC DMA -> PCM conversion -> local VAD + C5 WakeNet
                                           -> C5 voice-chain/server voice path
S3 SoftAP/local HTTP -> protocol adapter -> voice proxy -> Server
```

This topology placed the WakeNet model/runtime on both resource-constrained C5
boards and left S3 as a voice HTTP proxy rather than the wake decision owner.

### Implemented migration topology

```text
C51/C52
  ADC DMA -> PCM conversion -> local VAD -> 500 ms pre-roll ring
                                     | idle: retain ring only, no audio UDP
                                     v start
                         VOICE_START -> pre-roll -> live PCM -> tail -> VOICE_END
                                               bounded PSRAM slot queues / sender task
                                                               |
                                                               | UDP/33435
                                                               v
S3 audio_wake_gateway
  receiver task -> bounded packet pool -> worker -> per-source PCM reassembly
                                   -> S3-only WakeNet -> future S3 voice session/pipeline
```

`audio_wake_gateway` starts in the S3 orchestrator's last, voice-oriented
startup phase, before `voice_proxy_init()`. It initializes ESP-SR models and
two source contexts, then starts a UDP receiver task and a separate worker.
Network receive callbacks do not run WakeNet inference.

### Ownership and migration boundary

| Owner | Current responsibility |
| --- | --- |
| C51/C52 `mic_adc_test`, `mic_vad` | ADC/DMA sampling, PCM conversion, 200 ms VAD features, start/end decisions, pre-roll/tail timing |
| C51/C52 `c5_audio_transport` | fixed pool, nonblocking enqueue, UDP serialization and transmission |
| Shared `esp111_protocol_common` | byte-oriented v1 audio frame definition, little-endian accessors, CRC16 |
| S3 `audio_wake_gateway` | UDP receipt, source/stream association, packet validation, sequencing diagnostics, PCM accumulation, S3 WakeNet lifecycle |
| S3 `voice_proxy` | existing C5 HTTP-to-Server voice proxy; not made a second WakeNet manager |
| Server | existing downstream ASR/LLM/TTS coordination; not changed by this migration |

No C5 WakeNet model creation, feed, detection, model loading, or ESP-SR
component dependency is retained in the C5 implementation path. No S3/C5
dual-detector path is intended. Existing radar, BME, Wi-Fi, command, server
schema, and LCD modules are not part of the audio transport redesign.

## C5 VAD and timing design

The C5 keeps continuously sampling while idle. It writes converted 16 kHz,
16-bit, mono PCM to the pre-roll ring and evaluates features once per 3,200
samples (200 ms). Until a valid VAD start, it creates no audio UDP frames.

| Parameter | Value | Basis |
| --- | ---: | --- |
| PCM format | 16 kHz, signed 16-bit LE, mono | current mic PCM contract and S3 WakeNet format check |
| VAD report window | 200 ms / 3,200 samples | `MIC_ADC_REPORT_SAMPLES` |
| start confirmation | 2 windows = 400 ms | reduces a single-window noise trigger; fully covered by pre-roll |
| pre-roll | 5 x 100 ms = 500 ms / 8,000 samples / 16,000 B | configured `APP_VOICE_PRE_SPEECH_PACKETS=5`; captures the 400 ms confirmation delay with margin and stays within the required 200-500 ms range |
| live C5 PCM enqueue chunk | 160 samples = 10 ms / 320 B | `MIC_ADC_VOICE_LIVE_CHUNK_SAMPLES`; less than the 640 B protocol maximum |
| VAD silence end debounce | 1,500 ms | `APP_VOICE_VAD_SILENCE_END_MS`; tolerates pauses inside an utterance |
| post-roll/tail | 700 ms / 11,200 samples / 22,400 B sent after VAD end | `APP_VOICE_POST_ROLL_MS`; reduces tail truncation after the VAD end decision |
| min/max VAD speech | 400 ms / 8,000 ms | rejects brief false starts and bounds a stuck stream |
| restart cooldown | 1,200 ms | prevents immediate re-trigger after a completed voice action |
| failed-start retry delay | 2,000 ms | prevents continuous reconnect/start churn |

The 500 ms selection is therefore not an unexplained constant: it is five
existing 100 ms speech packets and exceeds the 400 ms two-window VAD start
confirmation by 100 ms. The ring is PSRAM allocated; the 12 KiB mic task stack
remains an internal control allocation because its execution/driver path has
different capability requirements.

### C5 state machine

```text
IDLE_LISTENING --(local VAD start)--> VAD_TRIGGERED
VAD_TRIGGERED --(queue VOICE_START)--> SEND_PREROLL
SEND_PREROLL --(pre-roll enqueued in order)--> STREAMING
STREAMING --(VAD speech)--> STREAMING
STREAMING --(VAD end)--> SEND_TAIL
SEND_TAIL --(700 ms sent)--> ENDING --(queue VOICE_END)--> IDLE_LISTENING

Any enqueue/start/transport preparation failure -> ABORTING -> RECOVERING
RECOVERING --(bounded retry/backoff)--> IDLE_LISTENING
```

The actual C5 transport uses two 64-entry queues backed by a 64-slot PSRAM
pool. The mic path uses zero-tick queue operations: full queues return an
error and log overflow instead of blocking ADC/VAD. A dedicated PSRAM-capable,
4,096-byte sender task performs `sendto()`. This is the intended acquisition
and network decoupling boundary. The FIFO order means the successful
`VOICE_START` enqueue precedes pre-roll PCM, which precedes live PCM, tail PCM,
and `VOICE_END`.

## UDP audio protocol v1

The protocol extends the existing shared component rather than adding a
separate application stack. It is UDP port `33435`, version `1`; the existing
UDP/33434 device-stream JSON and existing `/local/v1` HTTP paths are unchanged.
The wire layout is serialized field-by-field, so C struct padding is not sent.
All multi-byte fields are little-endian.

| Offset | Bytes | Field | Meaning |
| ---: | ---: | --- | --- |
| 0 | 1 | version | must be `1` |
| 1 | 1 | type | `VOICE_START=1`, `PCM=2`, `VOICE_END=3`, `VOICE_ABORT=4` |
| 2 | 1 | source_id | `1=C51`, `2=C52` |
| 3 | 1 | flags | pre-roll `0x01`, tail `0x02`, end `0x04` |
| 4 | 4 | stream_id | C5 stream epoch/id |
| 8 | 4 | sequence | monotonically incremented per stream, including controls |
| 12 | 4 | sample_counter | emitted PCM sample position in the stream |
| 16 | 2 | sample_rate | currently `16000` |
| 18 | 1 | bits_per_sample | currently `16` |
| 19 | 1 | channels | currently `1` |
| 20 | 2 | frame_samples | samples carried by a PCM frame |
| 22 | 2 | payload_length | PCM byte length; maximum 640 |
| 24 | 2 | crc16 | CRC16 over bytes 0-23 XOR CRC16(payload) |
| 26 | 2 | reserved | encoded as zero |
| 28 | N | payload | signed 16-bit PCM LE when `type=PCM` |

`VOICE_START` has no payload and is enqueued before PCM. Pre-roll PCM frames
are flagged before `c5_audio_transport_mark_live()` switches to live frames.
After VAD end, tail frames are flagged before `VOICE_END` carries `END`. A C5
stream ID is currently derived from monotonic milliseconds XOR the source ID in
the high byte; source ID plus stream ID form the S3 stream key. This gives
different source namespaces but requires collision/reboot testing.

The decoder rejects a wrong version, short/long frame, payload exceeding 640
bytes, length mismatch, or CRC mismatch. S3 also validates source ID, nonzero
stream ID, 16 kHz/16-bit/mono format, and `payload_length == frame_samples * 2`.
There is deliberately no retransmission: for real-time wake detection the
current policy reports/advances across loss rather than adding unbounded delay.

## S3 receive and WakeNet lifecycle

Two fixed per-source contexts are allocated, one each for C51 and C52. Each
contains a PSRAM 2,048-sample reassembly buffer, source ID, stream ID, expected
sequence, last-arrival time, detection latch, and active state. UDP packets
enter a fixed 16-slot PSRAM packet pool; the receiver drops/logs a packet when
the ingress queue is full. The worker decodes and processes it.

For `VOICE_START`, S3 rejects a second source while another source is active,
then recreates the single WakeNet instance using the `nihaoxiaozhi` model,
checks 16 kHz/mono, and initializes the selected context. This is an explicit
single-active-source policy for the N32R16 S3: C51 and C52 have separate stream
contexts and are distinguishable, but only the first active source owns the
single mutable WakeNet instance. A concurrent second source gets a recognisable
`VOICE_BUSY` log and does not contaminate the active stream. It should be
extended later only with measured CPU/RAM evidence for a second instance or a
defined fair scheduler.

For PCM, S3 detects duplicates and packet loss from `sequence`, appends valid
PCM, and repeatedly calls `detect()` only when the reassembly buffer holds at
least WakeNet's advertised chunk size. It neither assumes packet size equals a
WakeNet chunk nor invokes inference in the receiver task. `VOICE_END` feeds all
complete chunks before context cleanup; a 5,000 ms inactivity check cleans a
stream missing a terminal control frame. A `VOICE_ABORT` received from a peer
also cleans the matching context.

The current source implementation does **not** implement a reorder window or a
jitter delay queue. It detects loss and treats a backward sequence as a
duplicate; out-of-order UDP is therefore discarded rather than resequenced.
Nor does it pad or otherwise process a final partial WakeNet chunk after end.
These are documented risks, not claims of full jitter-buffer behavior.

### S3 state machine

```text
NO_STREAM --(valid VOICE_START, selected source)--> WAITING_PCM
WAITING_PCM --(pre-roll PCM)--> RECEIVING_PREROLL --> STREAMING
STREAMING --(more PCM)--> STREAMING
STREAMING --(VOICE_END)--> DRAINING --(complete chunks fed)--> STREAM_ENDED -> NO_STREAM
WAITING_PCM/STREAMING --(5 s inactivity)--> STREAM_TIMEOUT -> NO_STREAM
WAITING_PCM/STREAMING --(VOICE_ABORT)--> STREAM_ABORTED -> NO_STREAM
```

`RECEIVING_PREROLL`, `STREAMING`, and `DRAINING` describe the required semantic
states. The present implementation represents them with `active`, `ending`,
and the reassembly/sample state in the per-source context rather than exposing
an enum. This is sufficient for the current single worker but should become an
explicit enum before complex reordering or multi-active scheduling is added.

## Source changes recorded in this task

| File(s) | Change and reason |
| --- | --- |
| `shared_components/esp111_protocol_common/include/esp111_protocol_common.h` | Adds bounded, explicit-endian UDP audio frame v1, type/flag definitions, CRC16, encode/decode validation. |
| `ESPC51/components/Middlewares/voice_transport/c5_audio_transport.[ch]` and C52 peer | Adds fixed-pool, queued sender so capture does not block on UDP; abort clears pending PCM before enqueuing the terminal `VOICE_ABORT` control frame. |
| `ESPC51/ESPC52/components/Middlewares/mic/mic_adc_test.[ch]` | Routes VAD-gated pre-roll/live/tail PCM through the transport while preserving local VAD timing. |
| `ESPC51/ESPC52/components/Middlewares/wake/local_wake_word.c` | Removes C5 ESP-SR/WakeNet behavior and leaves compatibility gates for the existing voice-chain. |
| `ESPC51/ESPC52/components/Middlewares/CMakeLists.txt` | Adds transport source/include and removes C5 ESP-SR dependency. |
| `ESPC51/ESPC52/main/idf_component.yml` | Removes C5 `espressif/esp-sr` manifest dependency. |
| `ESPS3/components/Middlewares/audio_wake_gateway/[audio_wake_gateway.c](../../ESPS3/components/Middlewares/audio_wake_gateway/audio_wake_gateway.c)` | Adds UDP receive, per-source contexts, S3 model/instance lifecycle, fixed-frame feeding, timeout cleanup. |
| `ESPS3/components/Middlewares/CMakeLists.txt` | Builds the S3 audio gateway and adds the S3 ESP-SR requirement. |
| `ESPS3/main/idf_component.yml` | Declares `espressif/esp-sr ==2.4.6` at the S3 root manifest so dependency resolution includes the S3-only WakeNet package. |
| `ESPS3/components/Middlewares/gateway_orchestrator/gateway_orchestrator.c` | Initializes/starts S3 audio wake before the existing voice proxy. |

`ESPC51` and `ESPC52` are intended to remain behaviorally aligned except for
their configured source/device identity. The documented code snapshot must be
rechecked against the final integration diff before release.

## Memory and task impact

The migration removes the opaque C5 ESP-SR model/runtime allocation and C5
WakeNet detection path, which is the intended relief for C5 internal largest
free-block pressure. The exact reclaimed internal/PSRAM bytes cannot be
derived safely from source because ESP-SR allocator placement is opaque; it
must be measured with capability-specific heap logs and map/size output.

New known C5 allocations are a 16,000 B PSRAM pre-roll ring, a 64-slot PSRAM
transport pool of 668 B slots (42,752 B before allocator overhead), FreeRTOS
queue metadata, and a 4 KiB PSRAM-capable sender task. The C5 mic task remains
12 KiB internal. This is deliberately a trade of bounded PSRAM buffering for
the removed C5 WakeNet/internal pressure; it is not evidence that all newly
allocated task control structures are PSRAM-only.

New known S3 bounded buffers are two 2,048-sample PCM reassembly regions
(8,192 B total), sixteen 668 B packet slots (10,688 B total), queue metadata,
a 6 KiB wake worker and 4 KiB receiver task requested with PSRAM-capable
allocation, plus model/runtime memory controlled by ESP-SR. The exact S3
WakeNet model/runtime split across internal RAM, PSRAM, DMA, and flash remains
to be measured. Both boards must report total free, minimum free, and largest
free for internal, DMA, and PSRAM; total free alone is not an admission result.

## Risk register

| Level | Trigger and effect | Present protection | Remaining action/status |
| --- | --- | --- | --- |
| High | 400 ms VAD confirmation can clip a wake-word onset | 500 ms pre-roll precedes live PCM | Validate at real mic levels/noise; tune only from captured evidence. |
| High | C5 queue fills or UDP blocks | nonblocking pool queues; sender task owns `sendto()` | Packets can still be dropped; measure sustained loss and decide abort/backpressure policy. |
| High | C5 loses link before terminal frame | C5 abort clears pending PCM then sends `VOICE_ABORT`; S3 also has a 5 s stream timeout | UDP delivery is not guaranteed, so validate immediate abort and timeout recovery on hardware. |
| High | Two C5 boards speak together | S3 source contexts and `VOICE_BUSY` rejection | Validate source priority/user feedback; second source is intentionally not queued or mixed. |
| High | S3 WakeNet model allocation fails or fragments memory | init returns failure and startup logs module result | Measure capability-specific largest blocks and task high-water marks on target hardware. |
| Medium | UDP reorders/losses/duplicates | CRC, sequence diagnostics, duplicate discard, loss logging | No reorder window/jitter buffer yet; test loss/reorder and consider bounded reorder policy. |
| Medium | Final non-full WakeNet chunk carries useful tail | complete chunks are fed before cleanup | Current code drops a residual partial chunk; decide whether model-safe padding/flush is supported. |
| Medium | stream ID repeats after C5 reset/time relation | source ID is included in stream key | Stress restart/collision cases; add boot nonce/random epoch if observed. |
| Medium | malformed/flooded UDP consumes the fixed pool | max length, CRC, source/format checks, bounded pool | Validate rate limiting and source/authentication requirements for the deployment network. |
| Medium | VAD silence/end logic splits normal speech | 1,500 ms hangover plus 700 ms tail | Confirm tolerable latency and conversational pauses on hardware. |
| Medium | C5/S3 source changes diverge | shared common header and paired C5 trees | Run paired builds, protocol encode/decode tests, and C51/C52 diff review. |
| Low | high-frequency logs disturb real-time timing | most event logs are boundary/error logs | Keep per-packet verbose logs disabled during timing tests. |

## ESPLCD observation and UI recommendation

No `ESPLCD` directory was present at the requested workspace root at this
documentation snapshot. The existing read-only comparison record
`docs/主项目_ESPLCD_LCD迁移对比审计_2026-07-20.md` was therefore used as the
available ESPLCD observation. It describes an ST7789/LVGL baseline with a
small status-oriented C52 surface, and a richer C51 reference with voice/Wi-Fi
state, animations, touch wake requests, and a dashboard. The latter is not a
fit to copy into this migration because it directly couples UI interactions to
voice behavior and brings unrelated touch/CSI concerns.

The current main-project LCD service already has a periodic copied-snapshot
model and command visibility TTL. This migration makes no LCD/UI code changes.
For a later, isolated UI task, publish a read-only voice snapshot and render:
`idle/listening`, `C51/C52 source`, `pre-roll/streaming`, `WakeNet detected`,
`busy/rejected second source`, `network unavailable`, and `stream timeout`.
The UI must consume copied state through its low-priority timer; it must not
read mic buffers, send UDP/HTTP, invoke WakeNet, or acquire voice resources.

## Verification ledger

### Completed static/source checks in this documentation pass

- Inspected the live C5/S3/common source and current diff for migration
  ownership, protocol layout, dependencies, timing, and startup wiring.
- Inspected the C5 VAD timing constants and S3 packet/reassembly limits.
- Ran `git diff --check` on the current workspace snapshot; it returned
  success with no reported whitespace errors.
- Confirmed the C5 manifests remove `espressif/esp-sr` and S3 Middlewares adds
  it in the current source snapshot.
- Built C51 successfully with `idf.py -B build-wakenet-migrate-c51 build`.
  The integration result reported no task-introduced final warnings.
- Built C52 successfully with `idf.py -B build-wakenet-migrate-c52 build`.
  The integration result reported no task-introduced final warnings.
- After obsolete C5 detector declarations and code were removed, C51 and C52
  were rebuilt successfully again. C52 printed Ninja's
  `premature end of file; recovering` message and still completed successfully;
  integration traced this to concurrent local access to build files, so it is a
  build-environment/infrastructure note rather than a compiler warning.
- A final C52 `idf.py reconfigure && ninja` full rebuild also succeeded. It
  refreshed `ESPC52/dependencies.lock`, removed the stale C5 ESP-SR dependency,
  and completed with no compiler warnings.
- After adding terminal `VOICE_ABORT` transmission, C51 and C52 rebuilt
  successfully again after the C5 transport send-failure recovery edit. Their
  final application binaries were `0x221290` and `0x225a30`, respectively.
- The first S3 configure attempt exposed a dependency-resolution issue: the
  new ESP-SR package was required by the Middlewares component but absent from
  the root manifest used for component-manager resolution. Adding
  `espressif/esp-sr ==2.4.6` to `ESPS3/main/idf_component.yml` corrected it.
- Built S3 successfully with
  `idf.py -B build-wakenet-migrate-s3 reconfigure && idf.py -B build-wakenet-migrate-s3 build`.
  ESP-SR 2.4.6 resolved and the model-partition image was generated. The
  successful build is source/build evidence only.
- A final S3 Ninja build passed with an application binary of `0x172ec0` and
  79% factory-app-partition space free. Ninja printed
  `premature end of file; recovering` before completion; as with the C52
  occurrence, integration attributes it to local concurrent build-file access,
  not a compiler warning.

### Not yet claimed by this record

All three firmware build targets passed after the S3 manifest correction. The
S3 first-configure failure and correction are retained above as build history.
This does not convert a successful build, source scan, prior build directory,
or generated model image into firmware acceptance.

No flash, serial monitor, microphone, VAD threshold, WakeNet accuracy, Wi-Fi
reconnect, UDP loss/reorder, C5 reset, S3 reset, LCD state, or two-device
end-to-end behavior has been performed or accepted by this documentation pass.

## Follow-up checklist

1. Build C51, C52, and S3 from final integrated source using isolated build
   directories; capture size/map and warnings.
2. Add a host-side protocol test for encode/decode, malformed lengths, CRC,
   duplicate/loss ordering, source/stream isolation, and sequence wrap.
3. On hardware, capture capability-specific heap/largest-block measurements
   before and after C5 mic/transport startup and S3 model startup.
4. Exercise VAD onset, pauses, tail, timeout, Wi-Fi reconnect, C5/S3 resets,
   queue overflow, and simultaneous C51/C52 speaking; record latency and loss.
5. Verify C5 `VOICE_ABORT` delivery/recovery, decide bounded reordering and
   final partial-chunk behavior, and define product feedback for a rejected
   second source before calling multi-device voice behavior complete.

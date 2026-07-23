# ESPC52 Mic Internal-Heap Fragmentation and VAD-Gated PCM Repair

**Date:** 2026-07-21  
**Status:** ESPC52 full build passed; hardware/runtime acceptance pending; no flash/monitor claim  
**Scope:** Main-project `ESPC52` C5 microphone/voice startup path, with only
the necessary mirrored C51 or S3 integration evidence recorded by the primary
agents. This record does not authorize flashing, serial monitoring, or
hardware/end-to-end acceptance.

## Objective

Repair the ESPC52 C5 microphone ADC/VAD task startup failure caused by
fragmented internal heap, stop the resulting high-rate retry storm, and retain
the established C5/S3 voice boundary:

```text
C5: ADC/DMA PCM capture -> light VAD -> bounded pre-roll/post-roll -> gated PCM
S3: WakeNet, wake-word decision, and later heavy voice processing
```

The C5 must not reintroduce a WakeNet runtime/model path. It must not send
continuous PCM while VAD is silent.

## Supplied Runtime Evidence

The user supplied the following C52 serial evidence. It is runtime evidence
for the pre-repair firmware, not evidence for the new implementation.

| Observation | Interpretation |
| --- | --- |
| `mic_adc_task_create requested_stack_bytes=12288 source=internal` | `mic_adc_test` currently requests a 12,288-byte contiguous internal task stack. |
| Internal free before creation is approximately 18-24 KiB; `internal_largest=11264` | Aggregate free memory is not a sufficient admission condition; the needed contiguous block does not exist. |
| `xTaskCreateWithCaps(mic_adc_test)` returns `ESP_ERR_NO_MEM` | The immediate failure is task-stack admission, not a microphone/VAD result. |
| Wi-Fi subsequently reaches `LINK_READY` | Wi-Fi failure is not the observed cause of the mic task creation failure. |
| Startup repeats about every 500-700 ms | The old retry path repeatedly creates an event group, allocates a 16,000-byte PSRAM pre-roll buffer, creates the task, fails, and repeats, increasing logging and allocation churn. |

The source-level investigation must still establish the exact allocation and
rollback sequence, all task contexts, and whether the task ever runs while the
flash cache is disabled or otherwise cannot safely access PSRAM.

## Constraints and Acceptance Boundary

- Do not resolve the issue by disabling Wi-Fi, BLE radar, LCD, scheduler, or
  gateway link.
- ADC/DMA/interruption-related buffers remain in the capability-appropriate
  internal/DMA memory; ordinary PSRAM is not a DMA substitute.
- The FreeRTOS TCB remains internal. A PSRAM task stack is permitted only after
  a cache-off/OTA/flash-write execution audit confirms it is safe for this
  task under the current ESP-IDF version.
- If a PSRAM stack is unsafe, use a static internal stack sized from an actual
  high-water measurement and a 20-30 percent margin. Do not guess a smaller
  number.
- Failed starts must have complete, idempotent rollback: event group, pre-roll,
  task stack, TCB, ADC/I2S intermediates, queues, semaphores, timers, and
  callbacks are released or unregistered exactly once; owning pointers are
  nulled before a retry.
- Exactly one voice-start transaction may run at a time. Multiple gateway
  transitions to READY must not concurrently create voice resources.
- Retry is bounded exponential backoff (target sequence 1, 2, 4, 8, 15, 30 s
  with a 30 s maximum). Repeated deterministic memory/configuration failure
  enters `VOICE_ERROR` after a justified threshold and only permits low-rate
  recovery checks or a defined resource-recovery event.
- Identical normal protection logs and repeated errors must be rate-limited.
- Final verification for this task is a complete ESP-IDF build only. Hardware,
  serial, microphone/VAD, Wi-Fi, BLE radar, LCD, task-watermark, PCM transport,
  and C5-to-S3 end-to-end acceptance remain for the user to perform.

## Design and Source Evidence

### Task stack and memory telemetry

The C52 `sdkconfig` enables both `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`
and `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`. That only permits external
task stacks; it does not prove this ADC continuous task can safely execute from
PSRAM through every cache-disabled, OTA, SPI-flash-write, driver, event,
logging, VAD, and transport context. The source review did not establish that
absence-of-risk proof. Therefore this repair explicitly rejects a PSRAM stack
for `mic_adc_test`.

The confirmed design is an internal static Task Control Block (TCB) plus an
internal static 12,288-byte stack. It removes the fragmented-heap allocation
dependency without claiming an unmeasured stack reduction. Runtime high-water
telemetry must first establish a safe later reduction with a 20-30 percent
margin. The final source log must identify the resulting source explicitly,
for example:

```text
TASK_CREATE task=mic_adc_test stack=12288 source=internal_static
```

The implementation must log requested/final stack bytes and source, plus
internal free/largest, DMA free/largest, and PSRAM free before and after
creation. Once the task runs, it must report the FreeRTOS stack high-water
mark. No successful design may continue relying on a dynamic 12,288-byte
contiguous internal-heap allocation at mic startup.

### Implemented FreeRTOS allocation decision

The Memory/FreeRTOS Agent implemented the C52 allocation conversion in
`ESPC52/components/Middlewares/mic/mic_adc_test.c` and its header:

- `StackType_t s_mic_adc_task_stack[3072]` provides exactly `12,288 B` of
  application-owned static internal stack memory.
- `StaticTask_t s_mic_adc_task_storage` provides the application-owned TCB in
  internal `.bss`.
- `xTaskCreateStatic()` replaces the heap-backed `xTaskCreateWithCaps()`
  request. The creation log reports
  `TASK_CREATE task=mic_adc_test stack=12288 source=internal_static`.
- The deletion helper changes from `c5_task_delete_with_caps_safe` to
  `c5_task_delete_safe`. Under the project ESP-IDF 5.5.4, a
  `vTaskDeleteWithCaps` path would free a task stack/TCB it did not allocate,
  which is invalid for application-owned static storage.
- Existing task-entry and post-local-state `app_stack_monitor` calls remain
  the baseline high-water observability. No device high-water number exists
  yet, so this implementation deliberately retains 12,288 B instead of
  asserting that 8,192 B or 10,240 B is safe.

The task still uses capability-appropriate dynamic allocations only for its
separate runtime resources (such as the PSRAM pre-roll and ADC driver/DMA
resources), which must be released by the failure rollback. Its FreeRTOS stack
and TCB themselves no longer require a contiguous dynamic internal heap block.

### VAD-gated transport

```text
SILENT
  -> VAD start confirmed/debounced: send VOICE_START + finite pre-roll
  -> VAD active: send real-time PCM and expose queue/drop pressure
  -> VAD end confirmed/hangover complete: send finite post-roll + VOICE_END
  -> SILENT: retain only bounded local pre-roll; send no continuous PCM
```

The VAD report window is 200 ms. The source preserves the configured finite
pre-roll/post-roll and live chunk limits; those values, VAD thresholds, and
queue behavior are configuration/source facts, not calibrated microphone
acceptance evidence.

### Implemented voice-start, rollback, and transport behavior

The Voice Runtime Agent completed the C52 changes in
`ESPC52/components/Middlewares/voice_domain/voice_chain.c`,
`ESPC52/components/Middlewares/mic/mic_adc_test.c`, and
`ESPC52/components/Middlewares/voice_transport/c5_audio_transport.c`.

`voice_chain` now holds a critical-section-protected `mic_start_in_progress`
single-flight flag. A second gateway/network event cannot create a parallel
Mic start: it returns with `VOICE_MIC_START_SKIP ... already_starting`. Failed
starts are recorded as pending with the exact capped sequence `1, 2, 4, 8, 15,
30 s`. The first three failures, the `VOICE_ERROR` transition, and every eighth
later low-rate check are logged; the old sub-second log storm is therefore not
the retry cadence. On the sixth consecutive failure, the chain latches
`VOICE_ERROR` and continues at the 30-second recovery-check maximum. A
successful start clears the pending flag, attempt count, deadline, and error
latch before returning to listening.

The start transaction waits for the Mic task's READY/running signal. A timeout
or missed READY transition calls `mic_adc_test_stop_and_deinit_for_reconnect()`
before the next retry and logs `VOICE_MIC_START_ROLLBACK`. The Mic-side failed
start cleanup follows the inverse ownership order: it aborts/deletes the
precreated task, resets `s_mic_adc_started`, VAD and stream context, deinitializes
the ADC continuous handle, deletes and nulls the control event group, clears
static audio buffers, and frees/nulls the PSRAM pre-roll allocation. The start
and shutdown guards prevent an overlapping cleanup/create interval. The
static stack/TCB are application lifetime storage and are intentionally not
freed during a failed start.

The Mic VAD state remains the PCM gate. While the stream is `IDLE`, converted
PCM only fills a bounded PSRAM pre-roll ring. A debounced VAD start prepares
the voice backend, emits the finite pre-roll first, and only then switches the
transport to live PCM (`mark_live`). Voice-end moves to a finite tail/post-roll
state (`mark_tail`), flushes the remaining live chunk, then finishes the
stream; it does not send PCM after returning to `IDLE`. The existing 200 ms VAD
window state machine provides start confirmation and hangover/end debounce;
the implementation preserves the configured finite pre-roll/post-roll values
instead of opening an always-on stream.

The UDP audio transport now has observable stream lifecycle and backpressure:
`PCM_STREAM_START`/`STOP`, `PCM_STREAM_DROP` for no free slot, queue overflow,
or send failure, and `PCM_STREAM_QUEUE_PRESSURE` with queued/free-slot counts.
Its summary logs source, stream, PCM frames/bytes, drop count, queue depth, and
free slots. This establishes source-level observability for stream start,
continuation, stop, drop, and pressure. Runtime frequency and delivery still
require hardware validation.

## Resource and Integration Audit Targets

The integration audit covered startup/orchestrator, deferred workers,
`bme_worker` (8,192-byte internal static stack), `system_worker` (8,192-byte
PSRAM static stack), BLE radar, Wi-Fi/gateway, LCD/UI, and `mic_adc_test`. The
documented precondition is that the C52 mic-start attempt sees
`internal_largest=11264` while requesting 12,288 bytes, and the DMA largest
block can decline to roughly 2-4 KiB.

No initial document conclusion attributes these conditions to a particular
unrelated subsystem. The final report must distinguish source/configuration
evidence from device measurements.

### Integration review findings

The source-level integration review confirms the C52 startup order in
`app_orchestrator.c`: Wi-Fi/gateway, system, BME, BLE radar, scheduler, then
voice. Its phase-two BME/system workers are scheduled after the continuation
startup stack is released. The review did not disable or reorder away Wi-Fi,
BLE radar, LCD, scheduler, or gateway link to make Mic startup succeed.

`c5_runtime_workers.c` retains the reported BME worker as an 8,192-byte
internal-static stack and system worker as an 8,192-byte PSRAM-static stack.
The Mic task's permanent 12,288-byte internal static stack/TCB is separate;
its raw/parsed ADC buffers remain internal static while its 16 KiB pre-roll
remains PSRAM-backed. Thus the repair avoids the transient contiguous internal
heap request without placing DMA/ADC buffers in ordinary PSRAM.

`gateway_link` only invokes the voice abort callback on a transition *away*
from READY; it has no READY-to-start callback. In addition, `voice_chain_start`
has its existing started guard and the new mic-start lock/backoff. Consequently
the examined network READY paths do not provide a second concurrent Mic create
route. The integration source scan found no C5 executable WakeNet implementation
or `esp_sr` dependency in the Mic, wake, or voice paths; S3
`audio_wake_gateway` retains WakeNet ownership. Compatibility comments may
still use the term "WakeNet" and are not a C5 runtime path.

The review found and repaired a concrete C52 `c5_audio_transport` partial-init
rollback/idempotency issue. Transport is now treated as ready only when its
PSRAM slot pool, both queues, and sender task all exist. Any slot/queue/task
allocation failure tears down the partially-created queues and slot pool before
returning, so the next `server_voice_client_init` attempt begins from a clean
state rather than retaining a half-initialized transport. The final integration
audit and complete build remain pending below.

The Integration Agent's scoped `git diff --check` passed. Its remaining
source-level caveat is distinct from the Mic stack decision: the transport
sender task currently has a PSRAM stack while it calls lwIP `socket`/`sendto`.
The reviewed source does not provide an explicit cache-disabled/OTA exclusion
for that worker. This must be resolved by the lead or retained as a post-build
cache-off/OTA risk; it is not evidence that Mic ADC/DMA buffers are in PSRAM.

## Modification Ledger

| Area | Result at record creation |
| --- | --- |
| Firmware source/configuration | C52 Mic static-stack/TCB, startup rollback, bounded retry, VAD stream-gating, and PCM transport observability implemented by the corresponding agents. Documentation Agent makes no firmware edits. |
| C52 stack/FreeRTOS audit | PSRAM stack rejected because the cache-disabled safety proof is absent. Implemented internal static TCB + 12,288-byte (`3072` word) stack using `xTaskCreateStatic`; ESPC52 build passed. |
| Voice startup, rollback, retry, VAD gate | Implemented: one in-flight start, `1/2/4/8/15/30 s` retry, sixth-failure `VOICE_ERROR` latch, READY timeout rollback, VAD-only idle/pre-roll, finite live/tail stream, and transport drop/pressure logs. ESPC52 build passed. |
| Wi-Fi/BLE/LCD/scheduler/gateway integration review | Source review complete: existing order preserved and no READY-start duplication path found. C52 transport partial-init cleanup is implemented; ESPC52 build passed. |
| Full build | ESPC52 `idf.py -B /Users/zhiqin/ESP 部分开发 1/build-c52-mic-heap-repair build` passed; log `build-c52-mic-heap-repair-build.log`. |
| Documentation | This record and the development-log index entry. |



## Parallel Agent Execution Note

**Updated:** 2026-07-21 16:22:06  (live run)

| Agent | Status | Notes |
| --- | --- | --- |
| Memory/FreeRTOS | Complete (source) | C52/C51 `mic_adc_test` uses internal static TCB + 12288 B stack via `xTaskCreateStatic`; PSRAM stack rejected without cache-off proof. |
| Voice Runtime | Complete (source) | Bounded 1/2/4/8/15/30 s retry, single-flight start, READY-timeout rollback, VAD-gated PCM, integrity-ok log demoted. |
| Integration | Complete (source) | Startup order preserved; transport partial-init rollback; C51 parity mirrored for mic/voice_chain/transport. |
| Build | Complete | `idf.py -B build-c52-mic-heap-repair build` => Project build complete; `00_Learn.bin` generated; no compile errors observed. |
| Documentation | Complete | Record + index updated after build evidence. |

## Verification Ledger

| Evidence layer | Command or source | Result |
| --- | --- | --- |
| Source review | Live C52/C51 mic/voice/transport sources | Static internal 12288 B stack/TCB; bounded retry; VAD gate; transport partial-init rollback |
| Build | `cd ESPC52 && idf.py -B ../build-c52-mic-heap-repair build` after IDF 5.5.4 export | Project build complete; bootloader + app images generated |
| Build artifact | `build-c52-mic-heap-repair/00_Learn.bin` | Present (size ~2.26 MiB); app partition free ~57% |
| Warning scan | build log `warning:` for mic/voice/Middlewares | No matching warning lines found in this build log |
| Hardware/runtime | flash/monitor/serial/VAD/PCM e2e | Not executed in this task; user-owned |

### Build command

```bash
export IDF_PYTHON_ENV_PATH=/Users/zhiqin/.espressif/tools/python_env/idf5.5_py3.14_env
. /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh
cd "/Users/zhiqin/ESP 部分开发 1/ESPC52"
idf.py -B "/Users/zhiqin/ESP 部分开发 1/build-c52-mic-heap-repair" build
```

### Live run deltas (this turn)

- Demoted successful `RUNTIME_PROTECTION heap_integrity ... result=ok` to `ESP_LOGD` in `app_stack_monitor.h` and `voice_chain_check_heap_integrity()`.
- Mirrored C52 mic/voice_chain/transport fixes into C51 for parity (C52 remains primary acceptance target).
- Confirmed full ESPC52 build success; no flash performed.

### Remaining hardware checklist for the user

1. Confirm serial shows `TASK_CREATE task=mic_adc_test stack=12288 source=internal_static`.
2. Confirm no sub-second `ESP_ERR_NO_MEM` mic create storm; retries if any follow 1/2/4/8/15/30 s.
3. Confirm silence does not continuously stream PCM; VAD start sends pre-roll then live PCM; end sends post-roll then stops.
4. Confirm Wi-Fi, BLE radar, LCD, scheduler, gateway remain operational.
5. Capture mic stack high-water after stable listening for any later stack reduction with 20-30% margin.

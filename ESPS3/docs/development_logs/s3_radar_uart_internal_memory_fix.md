# S3 LD2450 UART internal-memory recovery fix

Date: 2026-07-21

## Status and scope

This log records the ESP32-S3 source change for LD2450 UART recovery when the
internal heap is low or fragmented. It covers source analysis, bounded
recovery, memory instrumentation, and memory-placement changes. It does not
record a flash, monitor, or hardware acceptance run.

Build status: PASS, 2026-07-21. Command: `idf.py build`. Output binary:
`build/sensair_s3_gateway.bin`.

## Observed failure

The reported runtime sequence, about 115 seconds after boot, was:

```text
RADAR_MEMORY_ADMISSION stage=uart_recovery_driver internal_free=9375 internal_largest=5620 admitted=0
RADAR_SOURCE_STATE event=uart_recovery_init_failed ret=257 backoff=1
RADAR_MEMORY_ADMISSION stage=uart_recovery_driver internal_free=6059 internal_largest=5108 admitted=0
RADAR_SOURCE_STATE event=uart_recovery_init_failed ret=257 backoff=1
```

`ret=257` is `ESP_ERR_NO_MEM`. The same period also contained `wifi:m f null`,
`httpd_sock_err: error in recv : 104`, and overview requests taking roughly
1.6 seconds. A reset connection is an error-path case that requires full HTTP
cleanup; it is not treated as the primary root cause without allocator
evidence.

## Root-cause classification

### Confirmed by source

- The immediate UART recovery failure is the admission check: a low internal
  free total or low largest contiguous block prevents `uart_driver_install()`.
  The new check records its estimated driver requirement and a separate safety
  reserve. It does not lower the reserve to make a failing install appear to
  succeed.
- Before this change, the recovery state machine was serialized by the RX
  task in practice, but had no explicit in-progress claim. UART lifecycle
  state and the driver event queue also had no lifecycle lock.
- The old retry cadence was 250 ms, then exponential backoff capped at 8 s.
  That was bounded, but too aggressive for the stated recovery contract.
- The old RX-exit and task-create-failure cleanup paths discarded the return
  value from `ld2450_uart_deinit()`. Normal recovery did check its result.
- The radar retry path does not allocate an RX task or an application event
  queue for every retry. `radar_rx` is created once; `uart_driver_install()`
  is the only UART-driver allocation and the driver owns its event queue.
  Therefore a repeated radar-task/queue allocation leak is not supported by
  the source trace.

### Not yet proven without the new runtime samples

- The specific allocator or request sequence that consumes or fragments
  internal RAM between boot and approximately 115 seconds is not established
  by static inspection. It may be a leak, retained allocation, fragmentation,
  or a combination. The new `S3_MEM` and `HTTP_MEM_DELTA` records are the
  required evidence boundary.
- The strings `wifi:m f null` and `recv : 104` do not identify an ESP-IDF
  Wi-Fi defect in this source review. The implementation only treats them as
  reasons to verify cleanup and concurrent request pressure; it does not edit
  ESP-IDF Wi-Fi internals.

## Memory timeline and instrumentation

All S3 memory records use this bounded-frequency format:

```text
S3_MEM stage=... internal_free=... internal_min=... internal_largest=... dma_free=... dma_largest=...
```

The helper samples `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` and
`MALLOC_CAP_DMA`, including each capability's free and largest block; the
internal value also includes its minimum-free watermark. The planned timeline
is:

| Stage | Purpose |
|---|---|
| `boot_complete` | Establish post-startup baseline. |
| `radar_service_ready` | Establish radar-service baseline. |
| `radar_uart_install_before` / `radar_uart_install_after` | Attribute driver and event-queue installation cost. |
| `radar_uart_delete_before` / `radar_uart_delete_after` | Verify recovery releases the old driver. |
| `uart_recovery_begin` / `uart_recovery_failed` | Correlate an attempted recovery with memory state. |
| `http_request_begin` / `http_request_end` | Bound every server-client request cleanup path. |
| `wifi_reconnect_begin` / `wifi_reconnect_end` | Correlate Wi-Fi reconnect pressure. |
| `periodic` | Sample from the radar RX task at 10-second intervals, not every second. |

The JSON-request and overview-probe paths in `server_client`, including
`/api/dashboard/v1/overview`, record:

```text
HTTP_MEM_DELTA endpoint=... before=... after=... delta=...
```

Interpretation rule: several settled overview requests must not show a
monotonic internal-free decrease. A single negative delta during a request is
not sufficient to label a leak.

## UART lifecycle

### Previous lifecycle

```text
UART error or silence
-> RX task entered BACKOFF
-> flush input
-> uart_driver_delete
-> wait 250 ms, doubling to at most 8 s
-> check fixed free/largest thresholds
-> uart_param_config + uart_set_pin + uart_driver_install
-> wait for three valid frames
```

The ordinary recovery delete result was handled, but task-exit and
task-create-failure deletes were not checked. No explicit lifecycle mutex
protected `s_driver_installed` and the driver-owned event queue.

### New lifecycle

```text
UART error or silence
-> mark source degraded/offline and stop RX processing
-> claim one recovery under the service lock
-> flush and delete the old driver, recording before/after memory
-> retain failed-delete state for the next recovery window
-> wait 1 s, 2 s, 4 s ... up to 30 s
-> check required driver bytes, largest block, and safety reserve
-> only then configure pins and install the driver
-> clear recovery claim on every success and failure path
-> wait for three valid frames before VALID
```

The UART wrapper now protects install, delete, flush, read, write, and event
queue access with a static lifecycle mutex. The service-level
`s_recovery_in_progress` claim prevents simultaneous recovery flows. A
rejected admission leaves the source degraded/offline and waits for the next
bounded window; it does not create a resource or attempt installation every
loop.

Admission now records:

```text
RADAR_MEMORY_ADMISSION stage=... required=... reserve=... internal_free=... internal_largest=... admitted=...
```

`required` is the RX ring, the 20-event UART queue storage estimate, and a
conservative driver-control allowance. `reserve` remains a 12 KiB internal
safety margin. This is intentionally stricter than merely lowering a
threshold when the heap is already unsafe.

## Internal RAM and PSRAM placement

### Kept in internal RAM

- UART-driver control objects, UART ISR-facing state, and driver DMA-related
  requirements.
- The radar RX task stack and other explicitly internal stacks whose call
  paths require it.
- Wi-Fi/protocol-stack objects whose allocator contract requires internal RAM.
- Static synchronization control structures used by the UART lifecycle and
  recovery paths.

### PSRAM placement context

- `radar_local` task workspace and the radar diagnostics snapshot remain
  PSRAM-backed long-lived workspaces.
- Local HTTP request bodies, queued ingress objects, pending-command JSON,
  radar ingest JSON, radar home snapshot JSON, and radar debug JSON visible in
  the working tree use `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` and have explicit
  free paths.
- The `upload_worker`, `command_worker`, and `snapshot_worker` PSRAM stack
  placement, HTTP-server PSRAM task-capability setting, and Wi-Fi scan-record
  PSRAM allocation are pre-existing working-tree changes outside this UART
  lifecycle repair. They were compiled by the recorded build but were not
  modified, hardware-proven, or hardware-accepted by this documentation task.

The UART driver, ISR objects, DMA objects, and proven internal-only task
stacks were not moved to PSRAM as a blanket memory workaround.

## HTTP, Wi-Fi, and reset connections

- The instrumented JSON-request and overview-probe paths close and clean up their
  `esp_http_client` handle before releasing the request slot, then emit
  `HTTP_MEM_DELTA`.
- The measured `recv : 104` is handled as a connection reset. It must execute
  the same close/cleanup and slot-release sequence as other request failures.
- `HTTP_RESOURCE_RELEASE` continues to report scheduling-slot state; the new
  memory delta is the companion evidence for actual transport/request cleanup.
- Wi-Fi reconnect begin/end memory samples make repeated connection creation
  and internal-pressure correlation visible without modifying ESP-IDF Wi-Fi
  source. They do not establish a hardware result for unrelated working-tree
  PSRAM placement.

Source inspection alone does not certify that historical overview traffic had
no leak. The post-change multi-request delta sequence is required.

## Changed files

UART-recovery and instrumentation changes recorded by this log are:

- `components/app_config/app_stack_monitor.h` and
  `components/Middlewares/gateway_orchestrator/gateway_orchestrator.c`: shared
  `S3_MEM` helper and `boot_complete` baseline.
- `components/radar_ld2450/ld2450_uart.c` and
  `components/radar_ld2450/include/ld2450_uart.h`: lifecycle lock, memory
  probes, and explicit driver requirement reporting.
- `components/radar_ld2450/radar_service.c` and
  `components/radar_ld2450/include/radar_config.h`: admission accounting,
  1-second-to-30-second backoff, single-recovery claim, and checked cleanup.
- `components/Middlewares/radar_domain/tests/test_radar_spatial.c`: updated
  recovery-backoff expectations.
- `components/Middlewares/server_client/server_client.c`: HTTP begin/end and
  per-endpoint memory delta logs.
- `components/Middlewares/network_worker/network_worker.c`: Wi-Fi reconnect
  memory-boundary logs.
- `components/Middlewares/local_http_server/local_http_server.c` and
  `components/Middlewares/local_http_server/radar_local_handler.c`: source
  context for PSRAM request/response buffers and full error-path frees; their
  PSRAM placement is build-checked only, not hardware-accepted here.
- This log and `docs/radar-migration-execution-log.md`.

## Stack audit note

`xTaskCreateWithCaps` stack-size arguments are bytes in this project.
`uxTaskGetStackHighWaterMark` returns FreeRTOS words; the shared stack monitor
multiplies by `sizeof(StackType_t)` before reporting `high_water_bytes`.
`radar_log` intentionally labels its direct raw value `free_words`. Thus the
reported `radar_local stack_bytes=8192 high_water_bytes=4604` is byte-based
and is not a word/byte naming defect. No stack is reduced solely from that
single watermark.

## Risks and rollback

- The driver-control requirement is conservative rather than a measured
  private ESP-IDF allocator size. If it rejects a usable heap, retain the
  reserve and refine the estimate only from allocator evidence; do not lower
  the safety margin blindly.
- A failed `uart_driver_delete()` deliberately leaves the driver state intact
  and retries deletion at the next bounded window. This favors correctness
  over forced reinstallation.
- PSRAM object migration must remain limited to non-ISR, non-DMA, and
  call-path-safe objects. Revert an individual placement to its prior
  allocation capability if ESP-IDF or hardware validation shows an internal
  requirement.
- Roll back as one logical change: restore the prior UART lifecycle and
  placement only after preserving the `S3_MEM`/`HTTP_MEM_DELTA` evidence that
  explains the regression. Do not mask memory loss with a reboot, disabled
  watchdog, or unlimited retry loop.

## Verification record

- Source review: COMPLETE for the UART lifecycle and instrumentation paths.
- Build: PASS, 2026-07-21, `idf.py build`, output
  `build/sensair_s3_gateway.bin`.
- Flash/monitor: NOT RUN.
- Hardware UART recovery, Wi-Fi reconnect, HTTP reset, and long-duration
  allocator behavior: NOT VERIFIED. Required acceptance is a controlled UART
  fault plus repeated overview traffic, observing stable periodic `S3_MEM`
  and non-monotonic/settled `HTTP_MEM_DELTA` values.

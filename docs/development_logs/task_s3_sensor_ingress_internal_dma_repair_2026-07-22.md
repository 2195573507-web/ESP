# S3 Sensor Ingress Internal/DMA Repair

- Date: 2026-07-22
- Scope: ESPS3 sensor ingress memory ownership and diagnostics, permanent internal allocation, event-bus attribution, local LD2450 lifecycle, and C51/C52 HTTP reset containment.
- Evidence boundary: current source plus isolated ESP-IDF 5.5.4 builds only. No flash, monitor, serial trace, or hardware acceptance occurred.

## Root Cause and Code-Level Change

The supplied device logs show `httpd_accept_conn` failure immediately after a
1.1 KiB sensor request lowers the available Internal/DMA largest block. Session
accounting does not show a socket leak, so this task targets allocation
lifecycle and fragmentation rather than increasing `max_open_sockets` or
tightening HTTP admission.

The pre-existing active source already put the HTTP receive body and ingress
allocation in PSRAM. It nevertheless parsed sensor/status JSON once to validate
the local device id, parsed again to rediscover that id before enqueue, then
parsed multiple times in the protocol worker. The second handler parse and
successful-worker duplicate parse are removed. This reduces temporary cJSON
node allocation churn; it does not claim that ESP-IDF HTTPD or cJSON private
allocations are DMA-safe or that the historical accept error is fixed on a
device.

## Sensor Ownership Chain

| Stage | Object / owner | Memory and operation | Lifetime / release |
| --- | --- | --- | --- |
| HTTPD receive | `body` in `read_json_body()` | PSRAM, first full body copy; `httpd_req_recv` borrows HTTPD transport data into caller buffer | handler frees after enqueue result |
| validation | `protocol_adapter_envelope_t` | temporary cJSON tree; borrow `body`; validation returns `local_id` | `protocol_adapter_release_envelope()` before handler enqueue |
| ingress create | `s3_runtime_ingress_t` | PSRAM, one full body deep copy plus compact identity/timestamps | ownership transfers to scheduler |
| event wrapper | `s3_scheduler_event_t` | PSRAM event header; shallow pointer to ingress, no body copy | event bus owns then releases |
| event bus | CRITICAL/REALTIME/STATE/BACKGROUND slot | sensor normal priority uses per-device STATE coalesce; old ingress is released on replacement | worker dequeue or replacement/reset |
| consumer | protocol worker envelope | one temporary cJSON parse for successful SENSOR/STATUS; normalized values flow to aggregator/cache/alarm | envelope released, then ingress released |
| response | compact local JSON | stack/static format only on audited sensor success/error path | sent before handler cleanup |

The request contains two simultaneous full payload copies only during handler
admission: PSRAM receive body and PSRAM ingress body. There is no full 1 KiB+
sensor body allocation from `MALLOC_CAP_INTERNAL` in this chain. No raw JSON is
stored by the BME latest-state cache or environment-alarm sample path.

## Diagnostic Trace

`LOCAL_HTTP_MEMORY_DIAGNOSTICS=0` and `S3_SCHEDULER_MEMORY_DIAGNOSTICS=0` are
default-off compile-time switches. When enabled, the traces cover:

- `before_accept`, `after_accept`, `after_recv`, `after_parse`,
  `before_event_enqueue`, `after_event_enqueue`, `before_response`,
  `after_response`, and `request_cleanup`.
- `event_consumer_begin`, `event_consumer_end`, and `payload_destroyed`.

Each record includes Internal/DMA/PSRAM free and largest block. Sensor and
scheduler records also identify payload pointer, size, detected region, active
handler count or event-bus depth. There is no device-side delta in this report;
the required before/after samples must be collected on the next run.

## Permanent Internal Allocation Audit

| Object | Decision | Reason |
| --- | --- | --- |
| `network_worker` stack, 16 KiB | moved to PSRAM | queued Wi-Fi/timer work only; no cache-off entry evidence |
| `radar_log` stack, 6 KiB | moved to PSRAM | drains copied snapshots and formats logs; not UART/DMA/cache-off |
| scheduler/protocol/stream stacks | PSRAM retained | long-lived application workers, already configured for external stacks |
| HTTPD task stack | PSRAM retained | `httpd_config_t.task_caps` is PSRAM |
| ingress, event wrapper, queue payloads, BME/alarm bodies | PSRAM retained | non-DMA payload ownership |
| gateway startup, UART RX stack/parser/ring, UART driver/DMA objects | internal retained | cache-off and DMA/capability boundary |
| FreeRTOS queue/semaphore/event control objects | internal/static retained | small kernel control structures |

The two stack moves remove approximately 22 KiB of permanent internal stack
demand. This is source-level accounting, not a runtime heap-map measurement.

## Event Bus and Radar Lifecycle

- Bus policy remains `CRITICAL > REALTIME > STATE > BACKGROUND`. Critical and
  realtime fullness returns `ESP_ERR_TIMEOUT`; only BACKGROUND may drop.
- Drop counters now identify event type. STATE replacement counters identify
  BME C51/C52 and status C51/C52 coalescing. Periodic stats expose both sets;
  consumer latency and oldest age are still measured by the existing scheduler
  timing diagnostics, not fabricated from static source.
- Local LD2450 is explicitly enabled by active S3 configuration (UART1,
  GPIO18/17). Disabled configuration returns `DISABLED` without driver/task or
  recovery. With the enabled configuration, no data becomes `DEVICE_TIMEOUT`
  while preserving the installed driver; only real read/event faults take the
  recovery path. Repeated identical recovery failure logs are suppressed until
  a successful reinstall resets the suppression state.
- The 12 KiB UART reserve was not lowered. Admission requires both Internal and
  DMA free/largest capacity; the source cannot prove the reserve peak is ideal
  without a real UART allocation trace.

## C5 HTTP Reset Recovery

Ordinary C5 JSON requests use a fresh `esp_http_client` and always close plus
cleanup. Command and radar-home snapshot execute on the shared system worker,
with command scheduled before snapshot. Stream header failure now closes the
transport and clears header/response state before the caller's normal cleanup,
so the next stream begins with a new handle. This does not restart Wi-Fi or
introduce fast retry loops.

## Changed Sources

- `ESPS3/components/Middlewares/local_http_server/local_http_server.c`
- `ESPS3/components/Middlewares/runtime/s3_scheduler.c`
- `ESPS3/components/Middlewares/runtime/s3_event_bus.c`
- `ESPS3/components/Middlewares/runtime/s3_event_bus.h`
- `ESPS3/components/Middlewares/network_worker/network_worker.c`
- `ESPS3/components/Middlewares/radar_domain/radar_log_manager.c`
- `ESPS3/components/radar_ld2450/radar_service.c`
- `ESPC51/components/Middlewares/server_comm/server_comm_http.c`
- `ESPC52/components/Middlewares/server_comm/server_comm_http.c`

Related permanent-allocation report: [ESPS3/s3_memory_event_audit.md](../../ESPS3/s3_memory_event_audit.md).

## Build Evidence and Warning

All commands used `IDF_PYTHON_ENV_PATH=/Users/zhiqin/.espressif/tools/python_env/idf5.5_py3.14_env`
and `/Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh`.

| Project | Command | Result |
| --- | --- | --- |
| ESPS3 | `idf.py -C ESPS3 -B build-goal-s3 build` | PASS; `sensair_s3_gateway.bin=0x176120`, smallest app partition 79% free |
| ESPC51 | `idf.py -C ESPC51 -B build-goal-c51 build` | PASS; `00_Learn.bin=0x2233d0`, smallest app partition 57% free |
| ESPC52 | `idf.py -C ESPC52 -B build-goal-c52 build` | PASS; `00_Learn.bin=0x1c3090`, smallest app partition 65% free |

`git diff --check` passed. One build warning remains outside this task:
`audio_wake_gateway.c:83` passes a `const` PCM pointer to a WakeNet API that
declares a mutable parameter. It is recorded, not changed.

## Next Hardware Observation

1. Enable both memory diagnostic switches and collect all trace phases around
   a 1.1 KiB sensor request, especially lowest Internal and DMA largest blocks.
2. Correlate each `httpd_accept_conn` error with preceding `SENSOR_MEM_TRACE`,
   `S3_MEM_TRACE`, `HTTPD_SESSION`, and event-bus statistics.
3. Confirm cJSON temporary-node pressure does not lower `dma_largest` below
   2 KiB (target 4 KiB) or `internal_largest` below 4 KiB (target 8 KiB).
4. Verify drop attribution, STATE coalesce counts, oldest queue age, and
   consumer latency while mixing sensor, command, snapshot, and diagnostic
   traffic.
5. Verify enabled local UART install, no-device `DEVICE_TIMEOUT`, a genuine
   driver-fault recovery, and that identical backoff failures are not repeated.
6. Force one C5 peer-reset during command and snapshot traffic; confirm the
   command remains first and the next request opens a clean transport.


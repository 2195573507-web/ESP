# ESPS3 DMA 耗尽 / network_worker WDT / env_alarm 栈溢出修复

- Date: 2026-07-21
- Scope: ESPS3 only (`/Users/zhiqin/ESP 部分开发 1/ESPS3`)
- Status: isolated ESPS3 build **passed**; hardware acceptance pending
- Hardware: **not** flashed, **not** monitored; runtime acceptance pending

## Log evidence (user-provided)

| Stage | internal_free | internal_min | internal_largest | dma_free | dma_largest |
| --- | ---: | ---: | ---: | ---: | ---: |
| Startup | 116075 | - | - | 108299 | 32756 |
| ~37s | 6179 | 27 | 5108 | 99 | 32 |
| Ingest fail | 99 | - | 32 | 75 | 32 |

Follow-on symptoms:

1. `wifi:mem fail` / `wifi:m f null`
2. `task_wdt: network_worker`
3. `HTTP overview duration_ms=5660` (WDT timeout is 5s)
4. `lwip_arch: thread_sem_init: out of memory` / `esp-tls connection failed`
5. `A stack overflow in task env_alarm_repor`

## Root cause analysis

### 1. Synchronous server probe on `network_worker`

`server_available_for_gate()` called `server_client_probe_available()` which performs blocking
`GET /api/dashboard/v1/overview` on the **network_worker** task. That task is registered with the
task WDT (`CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`). A 5.6s overview blocks the worker and trips WDT.

Additionally, probe start time was used as the interval anchor. A long probe finished after >3s,
so the next `evaluate_state` (~250ms later) immediately started another overview request. This
matches the “complete then ~270ms start again” observation.

### 2. No unified internal/DMA admission for external HTTP

`server_client` logged `HTTP_MEM_DELTA` / `S3_MEM` but still opened new HTTP/TLS sockets while
DMA free collapsed from ~108KB to tens of bytes. Concurrent best-effort traffic (probe, weather,
snapshot, BME replay) and telemetry could keep allocating Wi-Fi/lwIP/TLS buffers until Wi-Fi failed.

### 3. `env_alarm_report` stack too small for large stack locals

Task name `env_alarm_report` truncates to `env_alarm_repor`. Stack was 3072 bytes while
`submit_head_if_due()` copied a full `environment_alarm_pending_t` / `alarm_event_t` and
`build_payload()` held ~280B of format arrays plus cJSON call depth.

### 4. Local radar kept full tracking when disconnected

`radar_local_adapter` continued parser/tracker/snapshot/log cadence even when UART was disabled
or recovery was not valid and no frames were available, adding CPU/log pressure under memory stress.

## Code changes

| Area | File | Change |
| --- | --- | --- |
| Memory/Network | `server_client.h/.c` | Unified `server_client_http_mem_admission()` with class thresholds (core/telemetry/best_effort), unified backoff, explicit `HTTP_MEM_ADMISSION` / `HTTP_REQUEST_DEFER` logs; gate `perform_json_once` and probe; stop fast retries on `ESP_ERR_HTTP_EAGAIN`/`NO_MEM` |
| Network | `network_worker.c` | Async overview probe via `upload_worker` (`request_server_probe` / `service_server_probe`); interval anchored on **completion**; coalesce while inflight; weather/snapshot defer under mem pressure |
| Alarm | `environment_alarm_reporter.c` | Move format buffers + submit event into PSRAM storage; remove large stack pending copy; stack 3072→5120 with 30s `TASK_STACK_REPORT` |
| Radar | `radar_local_adapter.c` | Disconnected/disabled/non-valid recovery without frames enters light idle (5s snapshot, state-change logs only) |
| Replay | `network_replay_worker.c` | Best-effort mem gate before BME replay HTTP |

## Memory policy (initial thresholds)

Best-effort (probe/weather/snapshot/replay):

- internal_free ≥ 28672, internal_largest ≥ 12288
- dma_free ≥ 24576, dma_largest ≥ 12288

Telemetry (ingest/alarms/logs):

- internal_free ≥ 20480, internal_largest ≥ 10240
- dma_free ≥ 16384, dma_largest ≥ 8192

Core (command/voice):

- internal_free ≥ 12288, internal_largest ≥ 6144
- dma_free ≥ 8192, dma_largest ≥ 4096

Backoff starts at 2s and doubles to 15s while pressure continues. Non-core classes defer during backoff.

Wi-Fi/lwIP/TLS buffers are **not** moved to PSRAM. Local SoftAP sensor receive is not gated by this policy.

## Watchdog policy

- WDT registration retained on `network_worker` / upload workers
- Root fix is removing multi-second sync HTTP from `network_worker`, not deleting WDT or raising timeout

## Build

- Command: isolated ESPS3 build with ESP-IDF 5.5.4 export (see final report)
- Flash/monitor: prohibited for this task

## Risks / still needs hardware verification

1. Whether admission thresholds are tight enough on the real board after ~30–60s soak
2. Async probe latency to first `LINK_STABLE` under poor Wi-Fi
3. `env_alarm_reporter` high-water after stack relocation under real alarm bursts
4. Radar idle correctness when UART is wired but silent
5. Whether other DMA consumers (voice, stream, local_http) still fragment DMA under load


## Build result

```text
idf.py -B build-s3-dma-network-alarm -C ESPS3 build
Project build complete.
sensair_s3_gateway.bin binary size 0x174c20 bytes (79% free in app partition)
```

No flash/monitor was performed for this task.

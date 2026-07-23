# S3 DMA memory, network worker, and environment alarm stability repair

Date: 2026-07-21

## Status and evidence boundary

This development record tracks the ESP32-S3 gateway repair for sustained
internal/DMA heap depletion, Wi-Fi allocation failures, `network_worker` task
watchdog timeouts, and the `env_alarm_repor` stack-overflow panic.

Implementation and ESP-IDF build verification are complete. This file is a
source and build record only: no flash, serial monitor, Wi-Fi, TLS, HTTP-server,
radar UART, or long-duration hardware acceptance result is claimed here.

## Reported runtime evidence

The supplied failure sequence has a distinct capability-heap collapse rather
than merely a low generic heap total:

| Phase | Internal free | Internal minimum | Internal largest | DMA free | DMA largest |
|---|---:|---:|---:|---:|---:|
| Startup | 116075 B | not supplied | not supplied | 108299 B | 32756 B |
| About 37 seconds | 6179 B | 27 B | 5108 B | 99 B | 32 B |
| Before later ingest | 99 B | not supplied | 32 B | 75 B | 32 B |

The resulting observed errors were `wifi:mem fail`, `wifi:m f null`,
`lwip_arch: thread_sem_init: out of memory`, TLS connection failure, an
overview HTTP duration of 5660 ms, a `network_worker` task-watchdog timeout,
and finally a stack-overflow detection in `env_alarm_repor`.

The values show that DMA-capable internal memory and contiguous allocation
capacity are exhausted. Generic free-heap reporting cannot establish whether a
new Wi-Fi/lwIP/TLS allocation is safe, so the repair records and gates the
following capability-specific fields: `internal_free`, `internal_min`,
`internal_largest`, `dma_free`, and `dma_largest`.

## Root-cause analysis

### Confirmed source-level issues

- `network_worker` historically reached the dashboard overview availability
  probe through its link gate. That request can perform synchronous HTTP/TLS
  work, so a slow result can stall the watchdog-registered coordinator instead
  of leaving it responsive to state and queue work.
- Independent dashboard probes could be scheduled too close together. A
  completed request followed by a second attempt about 270 ms later defeats
  intended periodic probing and increases concurrent connection pressure.
- The prior path did not use one common admission decision before every
  best-effort HTTP class. Under low internal/DMA capacity, noncritical weather,
  snapshot, replay, and overview work could still advance toward a new
  connection.
- The `env_alarm_repor` panic identifies a task-stack failure. Its full source
  audit and any conversion of large automatic objects to controlled heap/PSRAM
  storage are recorded below only after the final diff review.
- Local-radar disabled/disconnected behavior is being audited separately. An
  idle source must not keep producing empty frames, tracking work, history
  updates, or high-rate summary logs.
- Deferred network payloads were retained by PSRAM queue control blocks but
  were not necessarily PSRAM allocations themselves. Normal-heap JSON bodies
  held while a link was unavailable were a credible internal-memory
  fragmentation source. The repair copies bounded queued JSON bodies into
  PSRAM before queuing, then releases the owned body on transfer and releases
  the PSRAM copy on queue failure or worker completion/cancellation.
- The old reporter submission call chain held a copied alarm event and several
  string-formatting buffers on a 3072-byte task stack in addition to its JSON
  work. That source structure is consistent with the named reporter task's
  overflow; it is removed rather than concealed with a watchdog or stack-only
  change.

### Not established by static review alone

- The exact historical allocator/request sequence responsible for the roughly
  108 KiB to 99 B DMA loss cannot be proven without post-change device samples.
  It may combine retained allocations, a missing cleanup path, fragmentation,
  and excess connection concurrency. The implementation must expose memory
  samples around request and lifecycle boundaries before assigning a single
  allocator as the cause.
- `wifi:m f null` and the lwIP/TLS failures are consequences of capability
  memory starvation in the supplied trace. This task does not modify ESP-IDF
  Wi-Fi, lwIP, or TLS internals without source evidence.

### Static-audit resolution and residual risk

- Queue control blocks use PSRAM, but queued JSON ownership formerly retained
  `cJSON`/normal-heap bodies while a link was down. With up to 16 upload and 16
  command entries, those retained bodies were a strong static candidate for
  fragmentation. The final boundary now copies and bounds owned deferred JSON
  in PSRAM; the queue/worker/cancellation paths free the PSRAM body with the
  matching allocator.
- Normal server-client JSON/probe paths visibly close and clean up their HTTP
  handles, so static inspection does not support declaring an HTTP-client
  handle leak. The prior independent priority slots could nevertheless allow
  multiple simultaneous TLS/lwIP allocations. The final single shared external
  connection permit, capability admission, and endpoint coalescing address
  that concurrency pressure.
- The wake-prompt cache gateway creates independent HTTP clients and uses
  unconstrained non-DMA buffers. It is outside the visible server-client
  admission path. It remains a residual source risk outside this scoped repair:
  a later change should apply the shared guard and PSRAM allocation for eligible
  buffers, then verify it on target hardware.
- The reporter's cJSON tree remains transient, but its serialized JSON body is
  now allocated in PSRAM before it enters the network queue. This removes the
  previously identified per-alarm body-retention pressure. The cJSON tree still
  needs long-run heap samples because allocation/cleanup correctness alone does
  not prove that its transient allocator behavior is harmless.
- A radar frame-valid flag historically stayed true across disconnect/stop, so
  an idle condition that also required "no historical frame" could continue
  tracker/history work and reprocess a stale frame after unplugging. Final
  logic now makes full processing contingent on UART enabled plus recovery
  `VALID`, with frame-sequence de-duplication; all other states are idle.
- When UART is disabled, prior startup still created the radar adapter, log,
  and RX task stacks. The RX service could also start after an unsupported or
  failed UART init. These fixed internal allocations do not alone explain the
  108 KiB-to-99 B fall. Final startup avoids the RX/log worker allocations
  while disabled.
- `app_s3_mem_log()` already records the required five fields. The startup
  record is now aligned to its DMA capability expression and includes
  `internal_min`, so it can be compared against runtime samples.

## Repair design

### Memory protection and ownership

The S3 network policy must make one bounded admission decision using all of:

- `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` free, minimum-free watermark, and
  largest contiguous block;
- `MALLOC_CAP_DMA` free and largest contiguous block.

Critical local sensor/radar reception remains eligible according to its own
internal/DMA contracts. Best-effort external HTTP is deferred before it creates
new socket, lwIP, or TLS state when the capability thresholds are unsafe.
Deferred requests use bounded shared backoff, and `ESP_ERR_HTTP_EAGAIN` is a
normal defer result rather than a tight retry trigger.

Only long-lived, non-DMA, non-ISR, and non-Wi-Fi/lwIP/TLS objects may move to
PSRAM. Intended candidates include worker stacks, large queued bodies, and
large request/response workspaces where their call paths permit external RAM.
Wi-Fi, lwIP, TLS, driver/ISR/DMA allocations, and confirmed internal-only
stacks must remain internal.

### Network worker responsiveness

The dashboard overview probe is scheduled and coalesced by `network_worker`,
then performed by a dedicated upload/request worker. The coordinator therefore
keeps its watchdog registration and is not made artificially healthy by
removing it or merely lengthening its timeout. Pending/in-flight probes and a
completion-time cadence suppress duplicate endpoint work; logs distinguish
`HTTP_REQUEST_SCHEDULE`, `HTTP_REQUEST_COALESCE`,
`HTTP_REQUEST_DEFER`, and `HTTP_REQUEST_DONE`.

Long-running loops, lock waits, and retry paths require finite waits and a
FreeRTOS yield/reset path consistent with actual responsiveness. Resource slots
must release on every HTTP success, failure, reset, cancellation, and deferred
path.

### Current admission thresholds

The current source diff defines the following initial thresholds. They are
deliberately capability-specific and must be calibrated only from the later
device samples, not reduced merely to make a depleted heap admit more work.

| HTTP class | Internal free | Internal largest | DMA free | DMA largest |
|---|---:|---:|---:|---:|
| Core | 12 KiB | 6 KiB | 8 KiB | 4 KiB |
| Telemetry | 20 KiB | 10 KiB | 16 KiB | 8 KiB |
| Best effort | 28 KiB | 12 KiB | 24 KiB | 12 KiB |

Low-memory admission starts with a 2-second shared backoff and doubles to a
15-second cap. Best-effort work is deferred while this pressure window is
active. The client logs `HTTP_MEM_ADMISSION` with internal/DMA free and largest
values, and performs `esp_http_client_close()`, `esp_http_client_cleanup()`,
endpoint memory delta logging, and HTTP-channel release on its request-exit
path.

### Environment alarm reporter

The reporter must report task high-water data at a bounded cadence, with
FreeRTOS-word values converted to bytes in the shared stack monitor. Stack
sizing must retain at least 25-35 percent headroom after large automatic JSON,
formatting, request/response, and snapshot objects are removed from the task
stack. Heap/PSRAM conversions require explicit allocation checks and cleanup on
each failure branch; increasing stack size alone is not a root-cause repair.

The current reporter implementation uses a 5120-byte PSRAM-backed task stack.
The increase is paired with removal of the large automatic payload state: the
pending FIFO, dead-letter records, drained events, 1024-byte JSON buffer,
formatted IDs/dedup/title buffers, and copied submitted event live in one
checked PSRAM `environment_alarm_reporter_storage_t`. `build_payload()` rejects
an absent storage pointer; JSON-body allocation failure and submission failure
both return the event to the controlled completion/retry path, freeing an
unsent body first. The alarm task reports its configured size at entry and
emits a bounded `TASK_STACK_REPORT` every 30 seconds.

The task name in the panic, `env_alarm_repor`, is the FreeRTOS-truncated form
of `environment_alarm_reporter`, created by
`environment_alarm_reporter_init()`. The 5120-byte configuration has not yet
been hardware-watermarked, so the required 25-35 percent margin remains a
hardware acceptance criterion rather than a completed runtime claim.

### Radar and logging

When S3 local radar is disabled or disconnected, its worker must enter a
lightweight idle wait. `RADAR_TRACK_UPDATE`, `RADAR_SOURCE_STATE`, and
`RADAR_RX_FRAME` output must be rate-limited or state-transition-only, and the
audit must verify that no repeated DMA/internal allocation occurs while idle.

## Implemented source changes

The following files contain the final integrated changes. This is a dirty
workspace, so the record does not assign unrelated pre-existing edits to this
repair:

- `components/Middlewares/network_worker/network_worker.c`
- `components/Middlewares/server_client/server_client.[ch]`
- `components/Middlewares/environment_alarm_reporter/environment_alarm_reporter.c`
- `components/Middlewares/radar_domain/radar_local_adapter.c`
- `components/Middlewares/gateway_orchestrator/gateway_orchestrator.c`
- `docs/development_logs/s3_dma_network_alarm_stability_fix.md`
- `docs/radar-migration-execution-log.md`

The implementation provides asynchronous/coalesced overview probing,
best-effort memory deferral, Wi-Fi lifecycle memory boundary logs, and PSRAM
placement for upload/command/snapshot worker stacks.

### Current implementation detail

## 2026-07-22 HTTP backoff and endpoint dedup repair

### Audited call chain

`network_worker_task` evaluates network state every 250 ms. Once STA/IP
stability is reached, the chain is:

`evaluate_state()` -> `server_available_for_gate()` ->
`request_server_probe()` -> upload-task notification ->
`service_server_probe()` -> `server_client_probe_available()` -> best-effort
memory admission -> low HTTP channel/shared connection permit ->
`esp_http_client_init()`/perform -> epoch unregister -> close/cleanup -> slot
release.

The previous implementation checked best-effort admission in both worker stages
and again inside `server_client_probe_available()`. A rejected admission was
returned without an endpoint deadline; the upload worker also cleared pending
state and advanced the normal probe timestamp. This made the 250 ms polling
loop repeatedly call admission and emit defer logs even though the client had
already calculated a 2 s to 15 s pressure backoff.

### Implemented repair

- `network_worker` protects overview pending, active, dispatch-ready, and
  deferred state with one `portMUX`.
- A monotonic `s_overview_next_allowed_at_ms` is set from the server-client
  admission deadline. While active, no admission call or queue notification is
  made; the current overview period is skipped.
- `HTTP_BACKOFF_SET`, one `HTTP_BACKOFF_SKIP`, and one
  `HTTP_BACKOFF_EXPIRED` are emitted per state/period transition.
- Pending or active overview requests emit one `HTTP_REQUEST_DEDUP` transition
  log and cannot be scheduled again. Probe execution performs one admission
  decision only.
- `server_client` no longer extends an active pressure deadline on each poll,
  and the shared backoff is scoped to `BEST_EFFORT`; telemetry and core classes
  still use their own capability thresholds and are not blocked by a
  best-effort deadline.
- Common HTTP slot release emits `HTTP_REQUEST_COMPLETE endpoint=...` with
  result and duration. Command ACK now passes the allocated response capacity
  instead of `sizeof(char *)`.

### Resource and fragmentation audit

The source audit found complete close/cleanup and slot release paths for normal
JSON, overview probe, cancellation/epoch invalidation, timeout, and voice
requests. Response bodies are caller-owned; queued JSON and command ACK storage
are PSRAM-backed and released on completion, cancellation, drop, or queue
failure. No static HTTP handle leak was established.

The current thresholds remain unchanged because the supplied
`internal_free=11051`, `internal_largest=3828`, `dma_free=3435`, and
`dma_largest=2676` values are below best-effort admission requirements. The
mbedTLS configuration uses internal allocation with 16 KiB input and output
content buffers; TLS/lwIP/Wi-Fi peak contiguous requirements remain opaque and
must be sampled on hardware. Non-DMA wake-prompt buffers and the voice-proxy
large-PCM internal fallback remain residual risks outside this focused repair.
Periodic cJSON serialization/deserialization remains a bounded fragmentation
candidate; this change does not alter protocol payloads or server APIs.

### Verification boundary

Only source/static checks and the ESP-IDF build are in scope. No flash, serial
monitor, hardware, TLS endpoint, or end-to-end acceptance is claimed.

- `server_client.[ch]` exposes HTTP admission classes and applies the gate
  before connection setup. It records `HTTP_MEM_DELTA` after normal HTTP cleanup
  and retains per-channel acquire/release accounting.
- All external transport classes now share one connection permit
  (`SERVER_CLIENT_HTTP_MAX_INFLIGHT=1`). The overview probe uses the low class,
  so a slow probe cannot combine with ingest/replay/weather/snapshot to create
  parallel TLS/lwIP pressure. Busy low-priority work returns
  `ESP_ERR_HTTP_EAGAIN` as a normal defer outcome.
- `network_worker.c` replaces the synchronous coordinator-side overview probe
  with pending/in-flight coalescing and an upload-worker service path. The
  completion timestamp becomes the next cadence anchor, so completion cannot
  immediately launch a duplicate probe. Weather refresh and snapshot work also
  defer under best-effort pressure. Upload, command, and snapshot task stacks
  use the project PSRAM stack capability; the watchdog remains registered on
  `network_worker`. Deferred upload, ingest, alarm, weather, and command-ack
  JSON bodies are copied through a maximum-size checked PSRAM boundary; their
  worker-side release uses the matching capability allocator.
- `environment_alarm_reporter.c` moves its large persistent reporter workspace
  and task stack to PSRAM, removes large formatting and pending-event locals
  from the submission call chain, serializes its queued body in PSRAM, and adds
  30-second stack reporting.
- `radar_local_adapter.c` uses an admitted PSRAM task workspace. With local
  UART disabled/offline and no frame, it performs only a 5-second state poll,
  emits a one-time `local_idle` transition, and sleeps 200 ms between checks;
  full tracking/history work and high-rate logs are skipped.
- The adapter now enters that idle branch whenever UART is disabled or its
  recovery state is not `VALID`, regardless of an old cached frame. A consumed
  frame sequence is remembered so valid-state tracking cannot repeatedly
  process the same frame. Startup does not create UART/RX or radar-log workers
  when UART is disabled, and does not create the radar-log worker if UART
  startup fails.
- `gateway_orchestrator.c` now uses the same internal and DMA definitions as
  runtime reporting and includes `internal_min` in its startup memory record,
  making boot and periodic timelines comparable.

## Verification plan and unverified acceptance

The ESP-IDF build and a whitespace/diff integrity check passed. A successful
build proves compilation and linking only.

Hardware acceptance still required after a successful build:

- Run the gateway beyond the prior 38-second failure window while sampling all
  five memory fields; verify DMA free/largest block no longer collapse.
- Exercise repeated dashboard overview, ingest/replay, weather refresh, and
  snapshot traffic; verify endpoint coalescing/defer logs and absence of rapid
  `ESP_ERR_HTTP_EAGAIN` retries.
- Confirm Wi-Fi/lwIP/TLS can allocate under traffic and no `wifi:mem fail` or
  `thread_sem_init` failure appears.
- Confirm `network_worker` remains watchdog-responsive during deliberately
  slow/reset HTTP responses.
- Confirm the environment-alarm reporter's stack high-water margin under real
  alarm serialization and transport failures.
- Confirm an unplugged/disabled LD2450 remains in low-cost idle without
  empty-frame churn or allocation growth, then validate normal recovery with a
  connected radar.

## Final build result

PASS, 2026-07-21.

```sh
cd '/Users/zhiqin/ESP 部分开发 1/ESPS3'
export IDF_PYTHON_ENV_PATH=/Users/zhiqin/.espressif/tools/python_env/idf5.5_py3.14_env
source /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh
idf.py -B build-s3-dma-network-alarm build
```

The first build exposed a radar field-name typo, which was corrected before the
second build completed successfully. Output:
`build-s3-dma-network-alarm/sensair_s3_gateway.bin`, size `0x1750b0`; the
smallest application partition is `0x700000`, leaving `0x58af50` (79%) free.
`git diff --check -- ESPS3` also passed.

This build proves compilation and linking of the current source only. It does
not prove that the 38-second heap collapse, Wi-Fi allocation failure,
watchdog timeout, or reporter overflow no longer occur on target hardware.

## 2026-07-22 follow-up build

```sh
cd '/Users/zhiqin/ESP 部分开发 1/ESPS3'
export IDF_PYTHON_ENV_PATH=/Users/zhiqin/.espressif/tools/python_env/idf5.5_py3.14_env
source /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh
idf.py -B build-s3-http-backoff build
```

PASS. The isolated build produced
`build-s3-http-backoff/sensair_s3_gateway.bin` at `0x174b40`; the smallest
application partition remains `0x700000` with `0x58b4c0` free. The build log
contains one pre-existing unrelated `audio_wake_gateway.c` discarded-qualifier
warning; no error occurred and the modified HTTP/network sources compiled and
linked successfully. No flash, monitor, hardware, TLS endpoint, or end-to-end
verification was performed.

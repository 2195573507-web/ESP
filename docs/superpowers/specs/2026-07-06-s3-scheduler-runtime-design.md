# ESPS3 Scheduler Runtime Convergence Design

## Goal

Refactor ESPS3 from scattered workers, periodic tasks, and local queues into one
business scheduler:

- one runtime event queue for HTTP, CSI, command, stream, and network events
- one scheduler tick for periodic work
- HIGH, NORMAL, and LOW priority dispatch
- backpressure from queue depth, network state, and voice busy state
- network worker reduced to network state output
- voice proxy kept synchronous and exclusive, only exposing `voice_busy`

The design preserves the current ESP-111 boundaries: C5 terminals remain
lightweight local clients, ESPS3 remains the only Server-facing gateway, and
ESP-server remains the truth/storage layer.

## Current Anchors

Live source inspection on 2026-07-06 shows these active ESPS3 surfaces:

- `gateway_orchestrator` owns startup, a runtime ingest queue, an `s3_worker`
  task, and a `gateway_periodic` task.
- `local_http_server` already enqueues most local POST inputs, but device stream
  HTTP and command pending still call business modules directly.
- `device_stream_gateway` owns a UDP receiver task, sender task, and sender
  queue; it directly calls CSI fusion and sensor aggregation after parsing.
- `csi_placeholder_gateway` owns separate CSI trigger and fusion tick tasks.
- `network_worker` owns a private queue, business gates, HTTP lifecycle,
  CSI/UDP start-stop, upload gating, and STA reconnect scheduling.
- `voice_proxy` is synchronous by design and currently marks child
  `voice_busy`.

These anchors are the sources to migrate or narrow. The target is not a new
parallel architecture; it is moving the existing business dispatch points into a
single runtime scheduler.

## Architectural Choice

Use a business-single scheduler with thin I/O entries.

HTTP server handlers and the UDP receive loop may remain as I/O tasks because
ESP-IDF HTTP and socket receive paths are inherently callback/blocking oriented.
They must not parse business envelopes beyond cheap size and framing checks, and
they must not call business modules such as `sensor_aggregator`,
`csi_placeholder_gateway`, `command_router_ack`, or `smart_home_gateway`.

All business work moves through:

```c
esp_err_t s3_scheduler_enqueue_event(const s3_scheduler_event_t *event);
void s3_scheduler_tick(void);
```

The scheduler task is the only consumer of the runtime event queue and the only
owner of periodic business cadence.

## Runtime Module

Create:

- `ESPS3/components/Middlewares/runtime/s3_scheduler.h`
- `ESPS3/components/Middlewares/runtime/s3_scheduler.c`

Public API:

- `esp_err_t s3_scheduler_init(void)`
- `esp_err_t s3_scheduler_start(void)`
- `esp_err_t s3_scheduler_enqueue_event(const s3_scheduler_event_t *event)`
- `void s3_scheduler_tick(void)`
- `s3_scheduler_load_t s3_scheduler_get_load(void)`
- `void s3_scheduler_set_voice_busy(bool busy)`
- `bool s3_scheduler_is_voice_busy(void)`
- `void s3_scheduler_set_network_state(s3_scheduler_network_state_t state)`
- `s3_scheduler_network_state_t s3_scheduler_get_network_state(void)`

Core types:

- `s3_scheduler_priority_t`: `HIGH`, `NORMAL`, `LOW`
- `s3_scheduler_event_type_t`: `HTTP_INGRESS`, `STREAM_FRAME`,
  `STREAM_SEND`, `CSI_FEATURE`, `COMMAND_ACK`, `COMMAND_PULL`,
  `NETWORK_STATE`, `VOICE_STATE`
- `s3_scheduler_network_state_t`: `NET_BLOCKED`, `NET_READY`,
  `STA_CONNECTED`
- `s3_scheduler_load_t`: queue depth, high/normal/low depth, network state,
  voice busy, and current cadence multipliers

The event payload remains fixed-size and stack-copyable. It can reuse the
existing `s3_runtime_ingress_t` body limits, but those type definitions should
move from `gateway_orchestrator.h` into `s3_scheduler.h` or an adjacent runtime
header so HTTP, UDP, command, and scheduler code share the same entry contract.

## Priority Dispatch

The scheduler keeps one canonical event queue and records priority in each
event. Dispatch drains in priority order without creating separate business
queues:

1. Drain bounded HIGH work.
2. Drain bounded NORMAL work.
3. Drain LOW work only when load permits.
4. Run `s3_scheduler_tick()` at the base tick cadence.

HIGH events:

- network state changes
- register and heartbeat/status health events needed to keep child state fresh
- command ack events
- voice busy state updates

NORMAL events:

- sensor/status payload processing
- CSI feature ingest
- command pending response ingest
- parsed device stream frame ingest

LOW events:

- dashboard snapshot upload
- CSI fusion flush
- CSI trigger send
- smart-home pending poll
- stream sender flush
- periodic diagnostics and heartbeat log

LOW dispatch is skipped or reduced when voice is busy, the queue is deep, or
network state is blocked.

## Periodic Work

Remove independent business timers/tasks:

- `gateway_periodic_task`
- `csi_fusion_tick_task`
- `csi_trigger_task`
- `stream_sender_task` as an independent loop
- `network_worker_task` periodic business gate evaluation

Move their work into scheduler-owned periodic jobs:

- `snapshot_upload`: calls `sensor_aggregator_upload_snapshot()`
- `csi_flush`: calls `csi_fusion_flush()` through the CSI gateway facade
- `csi_trigger`: sends trigger packets to eligible C5 peers
- `smart_home_poll`: calls `smart_home_gateway_poll_once()`
- `stream_send`: sends queued UDP payload events
- `diagnostics`: calls CSI latest diagnostics and gateway heartbeat logging

The scheduler owns last-run timestamps and next intervals for each job. Modules
provide callable operations, not their own loops.

## Backpressure

`s3_scheduler_tick()` reads:

- total queue depth and per-priority pending counts
- network state: `NET_BLOCKED`, `NET_READY`, `STA_CONNECTED`
- `voice_busy`

Cadence policy:

- healthy load, `STA_CONNECTED`, voice idle:
  - CSI flush at normal CSI tick cadence
  - server upload at configured sensor forward cadence
  - smart-home poll at normal cadence
- queue depth above soft watermark:
  - keep HIGH and NORMAL dispatch
  - slow LOW jobs by multiplying their intervals
  - prefer coalescing snapshot upload and diagnostics
- queue depth above hard watermark:
  - drop or coalesce LOW duplicate events
  - skip smart-home poll
  - slow CSI trigger before slowing CSI ingest
- `NET_BLOCKED`:
  - keep local child registry, command ack state, CSI feature ingest, and sensor
    state updates
  - skip Server upload and smart-home poll
  - keep UDP/HTTP local ingress available when SoftAP is up
- `voice_busy`:
  - keep register, heartbeat, status, sensor, CSI, and command ack ingestion
  - slow or skip LOW jobs, especially smart-home poll and diagnostics
  - do not let `voice_proxy` control scheduler decisions directly

This fixes the existing pattern where voice-busy checks are scattered in
business modules.

## Network Worker

`network_worker` becomes a state machine and network event adapter.

Responsibilities kept:

- consume raw Wi-Fi/IP events
- maintain link state
- output scheduler network states:
  - `NET_BLOCKED`
  - `NET_READY`
  - `STA_CONNECTED`
- mark child registry link-lost on AP station disconnect
- request STA connect/reconnect as part of network state management

Responsibilities removed:

- starting/stopping local HTTP
- starting/stopping CSI gateway
- starting/stopping device stream gateway
- upload gate decisions
- CSI gate decisions
- periodic business gate evaluation

`gateway_wifi` continues to expose raw SoftAP/STA facts. Scheduler and
server-facing modules should use scheduler/network state instead of
`network_worker_is_upload_allowed()` and `network_worker_is_csi_allowed()`.

## Event Ingress

HTTP handlers:

- `register`, `heartbeat`, `health update`, `status`, `sensor`, `csi result`,
  and `command ack` enqueue scheduler events.
- `device_stream` HTTP reads the body and enqueues a stream event; parsing and
  business handling happen in scheduler dispatch.
- command pending GET reads local command queue only. Server command polling is
  scheduler-driven, so GET does not fetch from Server inline.
- voice turn remains synchronous in `voice_proxy`.

UDP receiver:

- receives datagrams
- captures peer IP/port and payload
- enqueues `STREAM_FRAME`
- does not call `device_stream_gateway_handle_json()` business logic directly

CSI ingest:

- HTTP CSI and stream CSI both enqueue events
- scheduler dispatch parses/validates feature payloads and calls the CSI gateway
  facade
- CSI fusion flush is periodic under scheduler tick

Command router:

- pending command fetch becomes scheduler-owned low/normal work
- command pending GET builds from the local queue only
- command ack upload is scheduler-dispatched; ack state can be recorded locally
  even when upload is delayed

## Voice Proxy

`voice_proxy` remains synchronous and exclusive:

- handles `/local/v1/voice/turn`
- owns the single-session mutex
- streams Server PCM response back to the child request
- sets and clears `voice_busy` through scheduler runtime flag and child registry

It must not call scheduler cadence controls. Scheduler reads `voice_busy` as one
load input.

## Module Migration

`gateway_orchestrator`:

- start modules
- call `s3_scheduler_init()` and `s3_scheduler_start()`
- remove runtime queue/worker and periodic task
- keep startup heartbeat loop only if it is converted into scheduler
  diagnostics, otherwise remove it as an independent periodic source

`local_http_server`:

- include `s3_scheduler.h`
- replace `gateway_orchestrator_enqueue_ingress()` with
  `s3_scheduler_enqueue_event()`
- make device stream HTTP an enqueue-only path
- make command pending GET local-read only

`device_stream_gateway`:

- keep UDP receive loop as thin I/O
- remove sender task and sender queue
- provide scheduler-callable helpers to parse stream frames and send UDP payloads
- enqueue incoming UDP/HTTP stream events

`csi_placeholder_gateway`:

- remove trigger and fusion tick tasks
- expose scheduler-callable `csi_placeholder_gateway_flush_fusion()`
- expose scheduler-callable `csi_placeholder_gateway_send_triggers()`
- keep feature validation and fusion business functions behind scheduler
  dispatch

`smart_home_gateway`:

- remove internal voice-busy skip
- rely on scheduler to decide whether `smart_home_gateway_poll_once()` runs

`sensor_aggregator` and `server_client`:

- replace upload gate reads with scheduler/network state reads
- keep business serialization and Server HTTP behavior unchanged

## Error Handling

- Queue full on HIGH returns an error to the caller and logs a high-priority
  overload event.
- Queue full on NORMAL may reject the input with the existing local error shape.
- Queue full on LOW coalesces or drops duplicate low-priority work.
- Invalid payloads are rejected during scheduler dispatch and logged with source
  and event type.
- Server upload failures continue to flow through `offline_policy`.
- Network blocked never fabricates Server success; it only defers or skips
  upload-capable jobs.

## Verification Gates

The implementation is complete only when current source proves all of these:

- `runtime/s3_scheduler.c` and `.h` exist and are included in
  `ESPS3/components/Middlewares/CMakeLists.txt`.
- `s3_scheduler_tick()` exists and owns CSI flush, Server upload cadence, and
  smart-home poll cadence.
- HIGH, NORMAL, and LOW priority dispatch are implemented.
- `gateway_periodic_task`, `csi_fusion_tick_task`, `csi_trigger_task`, and
  `stream_sender_task` no longer exist as independent periodic sources.
- HTTP normal ingress, UDP stream receiver, CSI ingest, and command ack paths
  enqueue scheduler events before business handling.
- `network_worker` no longer starts/stops HTTP, CSI, stream, or upload gates.
- `voice_proxy` remains synchronous and only exports `voice_busy` runtime state.
- Server-facing routes remain on ESPS3; C5 code is not expanded to call
  `/api/...` directly.
- Strongest available build check passes for ESPS3, or the exact environment
  blocker is documented with a narrower compile/syntax substitute.

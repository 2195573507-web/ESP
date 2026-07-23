# ESPS3 Memory and Event-Bus Audit

Date: 2026-07-22

## Changes in This Audit

- `network_worker` has a 16 KiB long-lived stack. It only consumes queued
  Wi-Fi state/timer work and is not entered from a cache-off callback, so its
  stack now uses `APP_TASK_STACK_CAPS_PSRAM`.
- `radar_log` has a 6 KiB long-lived stack. It drains copied radar snapshots
  and formats logs outside the UART/cache-off path, so its stack now uses
  `APP_TASK_STACK_CAPS_PSRAM`.
- Event-bus stats now include drop counts by event type and coalesce counts by
  STATE key. The normal enqueue path remains quiet (`ESP_LOGD` for individual
  background drops); the existing periodic stats line exposes attribution.

These two migrations release about 22 KiB of permanent internal stack demand.
They do not change queue ownership, worker priority, or retry behavior.

## Internal-RAM Objects Kept Deliberately

- `gateway_startup_task` stack: retained internal for startup/cache-off safety.
- `radar_rx` stack and UART parser/ring buffers: retained internal because the
  UART/cache-off boundary and DMA admission require internal-capable storage.
- HTTPD task stack remains PSRAM-backed (`config.task_caps`), as do scheduler,
  protocol, stream, upload, command, snapshot, voice, replay, and alarm
  reporter stacks.
- Event-bus queue pointer arrays and mutex/semaphore control blocks are small
  static/internal objects; event envelopes, ingress bodies, stream payloads,
  worker queues, alarm storage, and cache bodies are PSRAM-backed.

## Sensor Ingress Ownership

`local_http_server` reads the body into a PSRAM buffer, parses/validates it,
then allocates one PSRAM `s3_runtime_ingress_t` and copies the body into its
fixed 4 KiB field. Ownership transfers to `s3_scheduler`; `event_alloc` adds a
PSRAM event wrapper but does not copy the ingress body again. The scheduler
passes the ingress pointer to `protocol_worker`; the worker parses a temporary
cJSON envelope, updates the aggregator/cache/alarm with normalized fields, and
releases the envelope before the ingress is released. Thus the full body has
two PSRAM copies during admission (HTTP body + ingress body), with parser
nodes temporary; no full body copy is allocated in the internal heap by this
path. The remaining optimization opportunity is a single parse-to-DTO path,
but it is not required for the current internal/DMA admission fix.

## Event-Bus Drop and Coalesce Semantics

- `CRITICAL` and `REALTIME` are FIFO queues. A full queue returns
  `ESP_ERR_TIMEOUT`; scheduler callers retry, so these events are not silently
  dropped.
- `STATE` uses four keys (`BME_LATEST_C51/C52`, `DEVICE_STATUS_C51/C52`). A
  newer event replaces and releases the older event, incrementing the per-key
  coalesce counter. Sensor ingress is `STATE` at normal priority, so repeated
  sensor samples intentionally retain only the latest sample per device.
- `BACKGROUND` is the only layer allowed to drop in the bus. Its capacity is
  six; command pulls and background stats are low priority and may be dropped
  under pressure. Per-event counters now distinguish these drops.
- Periodic scheduler stats expose queue depth, aggregate drops, per-event
  drops, aggregate coalesces, and per-key coalesces. This makes a future device
  log sufficient to tell a true queue overflow from expected state replacement.

## Verification

```text
git diff --check                 PASS
idf.py -B build-s3-memory-event build  PASS
ESP-IDF 5.5.4 / Python 3.14.5
sensair_s3_gateway.bin 0x176120; smallest app partition free 0x589ee0 (79%)
```

No flashing or runtime/device acceptance was performed in this audit.

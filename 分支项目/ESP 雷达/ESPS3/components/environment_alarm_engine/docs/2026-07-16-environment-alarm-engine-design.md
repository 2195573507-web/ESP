# Environment Alarm Engine Design

## Scope

This component is the ESPS3 BME690 alarm domain layer. It accepts one
normalized `alarm_environment_sample_t` at a time and creates bounded domain
events. It remains independent of HTTP, `network_worker`,
`gateway_event_reporter`, radar, and global BME caches.

The current ESPS3 integration is intentionally outside this component:

1. `environment_alarm_adapter` maps the parsed BME V3 envelope, maintains
   C51/C52 boot and remote-sequence state, and calls `alarm_engine_update()`.
2. `environment_alarm_reporter` deep-copies `peek` results to a separate FIFO
   and only then acknowledges the engine prefix.
3. The additive `network_worker` completion path returns an actual HTTP result
   to the reporter, which decides sent, retry, or dead-letter.

## Public Contract

The public header provides lifecycle, update, reset, `peek`, `ack`, active
state, queue-depth, and diagnostics operations. The caller supplies a S3-local
strictly increasing `ingest_seq`; it must not use a remote C5 sequence as a
permanent monotonic sequence. C51 and C52 use isolated device runtime slots.

## Storage And Concurrency

The engine has fixed storage for four device slots, 13 rules per slot, bounded
history rings, a 64-event FIFO, and a 16-event staging area. There is no
dynamic allocation, timer, task, or network operation in the engine. Critical
sections protect update, reset, peek, ack, and diagnostics.

The event FIFO is reject-on-full. It never evicts a previous active or
recovered event. `ack_events()` validates the requested sequence against the
tail and removes only the acknowledged contiguous prefix.

## State Rules

Rules use `NORMAL`, `PENDING`, and `ACTIVE` state. Activation and recovery use
per-rule debounce and hysteresis. Invalid fields and failed gates cancel
pending accumulation instead of inventing recovery. WARMUP and UNKNOWN retain
independent temperature/humidity rules but gate READY-only air-quality,
pollution, trend, stability, and composite rules. DEGRADED may produce the
sensor-degraded rule and recovers only after READY observations.

Each activation cycle creates a stable `alarm_id` from the S3 engine boot
nonce, device, rule, cycle, local ingest sequence, and available C5 boot id.
The reporter derives a separate payload `dedup_key` from the device, rule,
state, alarm id, and event sequence.

## Verification Boundary

Host tests inject a monotonic clock and exercise the rule/state machine,
history, invalid fields, device isolation, adapter sequence handling, queue
acknowledgement, and delivery policy. They do not prove a physical BME690,
actual C5/S3 transport, server authentication, HTTP persistence, or alert
calibration.

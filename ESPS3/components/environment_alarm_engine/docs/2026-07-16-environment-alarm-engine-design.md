# Environment Alarm Engine Design

## Scope

Create a new, standalone `environment_alarm_engine` component under
`ESPS3/components`. The component receives one normalized BME690 sample per
explicit call and returns bounded alarm events. It does not subscribe to,
modify, or otherwise connect to the existing sensor, network, protocol, or
server paths.

Only files newly created inside this component may be changed. No existing
ESPS3, ESPC51, ESPC52, server, frontend, database, or protocol files are in
scope.

## Public Contract

The public header exposes normalized sample types, event types, diagnostics,
and these lifecycle operations:

- `alarm_engine_init`
- `alarm_engine_update`
- `alarm_engine_peek_events`
- `alarm_engine_ack_events`
- `alarm_engine_get_active`
- `alarm_engine_get_diagnostics`
- `alarm_engine_reset`
- `alarm_engine_deinit`

The caller supplies a strictly increasing `ingest_seq` per allowed device. The
engine obtains monotonic time itself; a supplied clock callback exists only
for deterministic tests. The initial allowed-device list is C51 and C52.

## Internal Architecture

The component is split into focused source modules:

- `environment_alarm_engine.c`: validation, lifecycle, device lookup, and
  deterministic update order.
- `environment_alarm_config.c`: the sole default configuration object and
  startup validation.
- `environment_alarm_history.c`: fixed-capacity, time-pruned ring buffers.
- `environment_alarm_events.c`: staged events, bounded queue, acknowledgement,
  descriptions, and deterministic overflow eviction.
- `environment_alarm_rules.c`: per-rule debouncing, hysteresis, READY gating,
  air-quality mutual exclusion, pollution escalation, and composite alarms.

All memory is bounded: at most four devices, 13 rule runtimes per device,
64 temperature entries, 64 humidity entries, 128 ready air-quality entries,
128 queued events, and 16 staged events per update. No rule starts a task,
timer, network request, file operation, or dynamic container growth.

## State And Events

Every `device_id + alarm_type` uses `NORMAL`, `PENDING`, and `ACTIVE`.
Transition to `ACTIVE` requires both configured duration and sample count.
Recovery uses an independent debounce and produces a single `RECOVERED`
event before returning to `NORMAL`. Invalid fields, gaps, and failed READY
gating cancel pending work or recovery accumulation without manufacturing a
recovery.

Each activation creates one stable, boot-local `alarm_id`; a pollution-spike
upgrade retains that identifier and emits one `ACTIVE` event with
`level_escalated`. Air-quality critical activation emits warning recovery
before critical activation. Composite critical-environment alarms use only
already-active child alarms and retain all observed active branches until all
recorded branches have been absent for 300 seconds.

## Verification

Tests use an injected monotonic clock and cover the specification's state
machine, invalid input, gap, READY, history, mutual-exclusion, escalation,
composite, queue, reset, and determinism cases. Work proceeds module by
module: implement one focused unit, compile it, correct every warning/error,
then continue. The final check builds the target component without modifying
the existing application chain and confirms the Git diff contains only newly
created component files.

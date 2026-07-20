# Environment Alarm Engine

`environment_alarm_engine` is the ESPS3-only domain component for BME690
environment alarms. It is copied from the permitted ESP1 component and adapted
for the current S3 integration. It does not perform HTTP, own a network task,
read global BME state, or depend on radar code.

The production path is:

`sensor_aggregator -> environment_alarm_adapter -> environment_alarm_engine -> environment_alarm_reporter -> network_worker -> /api/logs/v1/alarms`

The adapter supplies validated, real BME V3 fields and maintains separate C51
and C52 boot/sequence state. The engine owns separate per-device rule and
history state. The reporter owns network delivery; the engine never knows
about JSON, HTTP, or retry policy.

## Bounded Storage

- Two configured devices (C51 and C52), with capacity for four engine slots.
- 13 rule runtimes per device.
- Temperature history: 64 entries per device.
- Humidity history: 64 entries per device.
- Air-quality history: 128 entries per device.
- Engine event FIFO: 64 entries; it rejects new work when capacity is
  unavailable and does not evict older events.
- Per-update staging: 16 entries.

`alarm_engine_peek_events()` copies an ordered prefix. The caller must call
`alarm_engine_ack_events()` only after an independent owner has deep-copied the
events. The integrated reporter does this after accepting each contiguous
prefix into its own 24-entry FIFO. A full reporter FIFO leaves engine events
unacknowledged. Its low-priority task periodically drains the engine again
after FIFO capacity becomes available.

All rule timing uses a monotonic clock. Input wall time is carried only as
event metadata when the C5 explicitly marks it synchronized.

## Verification

Run host coverage from the repository root:

```sh
sh ESPS3/components/environment_alarm_engine/test/run_host_tests.sh
```

The suite checks rule transitions, field/state gates, C51/C52 isolation,
adapter boot/sequence behavior, peek/ack/full-queue behavior, and delivery
classification/backoff. Hardware, GATT, and live HTTP acceptance remain
separate device-level validation.

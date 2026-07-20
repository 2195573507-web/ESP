# Environment Alarm API Payload

## Existing Endpoint

The ESPS3 reporter uses the existing authenticated endpoint:

```text
POST /api/logs/v1/alarms
```

The current server route records an alarm and responds with HTTP 201 on a
successful create. Gateway authentication is the actual transport requirement.
The route applies fallback values for missing fields, but ESPS3 deliberately
sends the following top-level compatibility set:

| Field | ESPS3 value | Purpose |
| --- | --- | --- |
| `device_id` | C51/C52 terminal id | device binding and event ownership |
| `level` | `warning`, `error`, or `critical` | server severity |
| `title` | derived rule/state title | readable alarm summary |
| `message` | bounded engine description | readable event text |
| `acknowledged` | `false` | existing alarm field |
| `room_id`, `room_name` | envelope room or existing device default | existing alarm display fields |
| `source` | `s3_environment_alarm` | event origin |
| `payload` | object | extensible environmental metadata |

## Actual Payload Shape

The example below is a shape example only. It is not a captured production
request and contains no credential or token.

```json
{
  "device_id": "sensair_shuttle_01",
  "level": "warning",
  "title": "Environment high_temperature active",
  "message": "environment alarm",
  "acknowledged": false,
  "room_id": "living_room",
  "room_name": "living_room",
  "source": "s3_environment_alarm",
  "payload": {
    "alarm_id": "0123456789abcdef",
    "dedup_key": "env:sensair_shuttle_01:high_temperature:active:...",
    "rule_id": "high_temperature",
    "alarm_type": "high_temperature",
    "severity": "warning",
    "state": "active",
    "active": true,
    "recovered": false,
    "unit": "C",
    "source": "c5_bme690",
    "sensor_state": "READY",
    "event_seq": "42",
    "remote_seq": 7,
    "local_ingest_seq": "15",
    "monotonic_timestamp_ms": 123456,
    "observed_value": 36.0,
    "trigger_threshold": 35.0,
    "recovery_threshold": 33.0,
    "stability_score": 0.95,
    "temperature_c": 36.0,
    "humidity_percent": 50.0,
    "air_quality_score": 80.0,
    "gas_ratio": 1.0,
    "boot_id": 1234,
    "event_timestamp_ms": null,
    "event_time_source": "s3_monotonic"
  }
}
```

Optional/invalid diagnostic values are emitted as JSON `null`, not invented
normal values. A synchronized C5 sample can supply `event_timestamp_ms`; when
`time_synced` is absent or false the reporter uses `null` plus
`event_time_source=s3_monotonic`.

## Identity And State Semantics

- `alarm_id` is stable for one engine activation cycle and changes for a later
  cycle.
- `dedup_key` contains device, rule, state, alarm id, and event sequence.
- `rule_id`, `state`, `active`, `recovered`, severity, observed value,
  trigger/recovery threshold, unit, sensor state, boot/sequence metadata, and
  time source remain inside `payload` to preserve the existing route shape.
- Active and recovered events remain separate FIFO records. A recovered event
  cannot overwrite its preceding active event in reporter memory.

The current server stores `payload` but does not use `alarm_id` or `dedup_key`
as an idempotency constraint. Consequently, a request whose response is lost
after server acceptance may be retried and persisted twice. This is a
server-side residual risk, not a claim of exactly-once delivery.

## Outcome Handling

| Result | Reporter action |
| --- | --- |
| HTTP 2xx, including 201 | remove FIFO head; increment `events_sent` |
| HTTP 408, 429, 500, 502, 503, 504 | retain head; increment retry count; back off |
| transport/DNS/connect/read error or link unavailable | retain head; back off |
| confirmed 4xx other than 408/429 | record dead-letter metadata; remove head |
| local invalid argument/size/not-allowed | dead-letter; remove head |
| local queue pressure | retain head; retry after bounded delay |

Backoff is bounded at 60 seconds. The implementation is not tested against a
live server in this task; classification is covered by a host unit test and
real HTTP acceptance remains a device/test-server step.

## Compatibility Rules

- The reporter posts to the unchanged existing alarm route.
- It does not add a server header, change authentication, modify schema, or
  require a database migration.
- Extension fields stay under the existing object-valued `payload` field.
- Existing ordinary `gateway_event_reporter_alarm()` behavior and global
  throttling remain untouched.

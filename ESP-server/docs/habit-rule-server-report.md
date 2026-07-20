# Habit Rule Server Report

## Scope

This module is isolated to ESP-server. It stores and manages Habit Rules only. It does not change any ESP32 firmware protocol, does not connect to S3, does not deliver rules to a device, and does not evaluate or notify on a rule.

## Database Design

`habitRules` is a new standalone SQLite table. It does not reuse any existing command, reminder, sensor, or device-status table.

| Column | Purpose |
| --- | --- |
| `id` | Stable text primary key for CRUD and future rule references. |
| `name` | Human-readable rule label. |
| `type` | Rule category, currently one of the six declared Habit Rule types. |
| `enabled` | Queryable rule switch, constrained to `0` or `1`. |
| `config_json` | Validated rule configuration; its `enabled` value is kept equal to the table column. |
| `created_at` | SQLite creation timestamp. |
| `updated_at` | SQLite update timestamp for future revision tracking. |

The startup migration uses `CREATE TABLE IF NOT EXISTS` and an additive index on `(type, enabled)`, so existing databases and business tables are unchanged. The defaults are inserted only when their stable IDs do not already exist.

The six generated defaults are `PERSON_ENTER_ROOM`, `PERSON_LEAVE_ROOM`, `PERSON_LONG_STAY`, `ROOM_EMPTY_TIMEOUT`, `NIGHT_ACTIVITY`, and `LONG_OCCUPANCY`. In particular, `PERSON_LEAVE_ROOM` starts with `{ "enabled": true, "room": "bedroom", "duration_minutes": 0 }`, and `LONG_OCCUPANCY` starts with `{ "enabled": true, "room": "living_room", "threshold_minutes": 120 }`.

## API Design

All responses use the existing `{ ok, server_time_ms, data, error }` API envelope.

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/api/habit-rules` | List the stored rules. |
| `POST` | `/api/habit-rules` | Create one validated rule. |
| `GET` | `/api/habit-rules/:id` | Read one rule. |
| `PUT` | `/api/habit-rules/:id` | Replace one validated rule configuration. |
| `DELETE` | `/api/habit-rules/:id` | Remove one rule. |
| `GET` | `/api/habit-rules/version` | Read the derived snapshot version, checksum, and latest update timestamp. |

`habitRulesService` owns CRUD, IDs, schema validation, default generation, and the `listEnabledHabitRules()` interface intended for a later evaluator. The route is intentionally a thin HTTP adapter. There is no `/device/update-rule` endpoint and no code that initiates a device connection.

## Frontend Design

The existing Dashboard now has a `Habit Rules` navigation entry. Its page lists all current rules and supports the requested controls:

- `PERSON_ENTER_ROOM`: enabled switch and room.
- `PERSON_LEAVE_ROOM`: enabled switch, room, and duration in minutes.
- `PERSON_LONG_STAY` and `LONG_OCCUPANCY`: enabled switch, room, and threshold in minutes.
- `ROOM_EMPTY_TIMEOUT`: enabled switch, room, and threshold in minutes.
- `NIGHT_ACTIVITY`: enabled switch, room, start time, and end time.

The single save action issues only `PUT /api/habit-rules/:id` requests. It does not cause evaluation, transmission, OTA, or device status changes.

## Future Synchronization Design

The future path is deliberately a design boundary, not a current implementation:

```text
S3
  -> query published rule version
ESP-server
  -> return rule snapshot
S3
  -> verify checksum
S3
  -> load snapshot
```

`GET /api/habit-rules/version` reserves the read-only metadata shape. It derives a deterministic version and SHA-256 checksum from ordered, validated rules; no separate device state or version table is introduced. When a publication contract is approved, a snapshot layer can use that metadata and the ordered rules. That layer remains separate from `habitRulesService` persistence. Separately, an execution path may be wired as:

```text
Radar State -> Home State Engine -> Habit Rule Evaluator -> Notification / App Push
```

Neither flow is present in this change. `rule_executor`, automatic S3 delivery, OTA, alert evaluation, and notification delivery remain unimplemented by design.

## Test Coverage

`test/habitRulesService.test.js` covers default rule initialization/idempotency, CRUD, the enabled/config JSON mirror, derived version/checksum change, and invalid JSON/config rejection. The existing server smoke suite remains the broad integration regression test.

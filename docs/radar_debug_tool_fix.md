# Radar Debug Tool Fix

Date: 2026-07-19

## State Model

The macOS tool no longer relies on one `currentRadarState` or one `currentTracks` collection. `RadarStateStore` initializes and updates:

```text
states[0] -> S3_LOCAL
states[1] -> C51
states[2] -> C52
states[255] -> UNKNOWN diagnostics only
```

Every `RadarState` owns its target list, filtered target list, track map, history, person counts, coordinate configuration, freshness, parser health, and identity fields. A target with the same track ID in two sources is therefore two independent targets.

## Parser Contract

`RadarLogParser` resolves explicit `source`, integer `source_id`, device ID, and room fields. Conflicting identity fields are routed to UNKNOWN and do not update a room. Offline, heartbeat, recovery, frame, target, person-count, and room-state lines update only the resolved source. Existing millimeter compatibility records remain supported through the source-aware compatibility labels.

The parser accepts the firmware labels:

```text
RADAR_RX_FRAME
RADAR_TRACK_UPDATE
RADAR_PERSON_UPDATE
RADAR_ROOM_STATE
RADAR_HOME_AGGREGATION
```

## UI Binding

Dashboard panels and canvas views receive the selected integer-keyed source state. The three source panels use states 0, 1, and 2; no panel reads a shared current target object. The existing `RadarViewModel` compatibility alias remains available for callers while state ownership is source-keyed.

## Regression Fixture

The parser check injects `T001` at `x=100`, `x=200`, and `x=300` for S3_LOCAL, C51, and C52. It verifies independent target coordinates, source-local person counts, and that a C52 offline event does not change C51 online state. It also checks source-context log fields, integer keys, unknown-source quarantine, history isolation, and replay behavior.

## Verification

| Check | Result |
| --- | --- |
| `sh ESPS3-Radar-Debug/script/run_parser_checks.sh` | PASS |
| `swift build` in `ESPS3-Radar-Debug` | PASS |
| S3 radar host tests | PASS |
| C51 radar host tests | PASS |
| C52 radar host tests | PASS |

The Swift proof is compile and deterministic parser-fixture proof. It is not a claim that a signed application bundle was launched against live UART or BLE data.

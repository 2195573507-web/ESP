# radar_home source state fix

## Purpose

Fix `RADAR_HOME_AGGREGATION` so an incoming C52 event is aggregated from the
current C52 source state instead of a previously cached S3_LOCAL room.

## Data model

`RadarSourceContext[RADAR_SOURCE_COUNT]` is the HOME aggregation input.  Each
source context now owns these state fields in addition to its snapshot and
person counts:

- `presence_state`
- `state_version`
- `last_update_timestamp`

`radar_source_context_publish()` updates the source context before log
publication.  Accepted local and remote registry updates then commit the
same source's presence, online state, sequence, timestamp, and person-count
summary.  A freshness expiry also commits `UNKNOWN` and offline state to that
source context.

No cached active room, active source, or last occupied room is maintained by
HOME.  The legacy `last_transition` output field remains empty rather than
representing persisted room selection.

## Aggregation

`radar_registry_get_room_aggregation()` rebuilds `occupied_rooms` on every
call by traversing `RadarSourceContext[RADAR_SOURCE_COUNT]`.

A source is included only when it has a committed version and timestamp, is
online, and its current presence state is `MOTION`, `PRESENT`, or `HOLD`.
`VACANT_INFERRED` and `UNKNOWN` are excluded; `UNKNOWN` cannot establish an
occupied room after a stale or offline transition.

Each result contains the current `source_id`, `source`, `device_id`, `room`,
presence state, and sequence from that source context.  The aggregation never
uses the registry entry list as a competing cached HOME state.

## Logging

`RADAR_HOME_AGGREGATION` formats the dynamic result as a bracketed list:

```text
RADAR_HOME_AGGREGATION source_id=2 source=C52 device_id=sensair_shuttle_02 room=bedroom sequence=10 occupied_room_count=1 occupied_rooms=[C52:bedroom] last_transition=none
```

`S3_LOCAL:s3_local` appears only when the current S3_LOCAL source context is
online and occupied.  An empty aggregation is emitted as `occupied_rooms=[]`.

## Tests

`ESPS3/components/Middlewares/radar_domain/tests/test_radar_ingest.c` covers:

1. S3_LOCAL vacant, C51 vacant, C52 present: `occupied_room_count == 1` and
   `occupied_rooms[0] == C52:bedroom`.
2. S3_LOCAL vacant, C51 motion, C52 present:
   `occupied_room_count == 2` with C51 and C52 rooms.
3. The C52 context receives `presence_state`, a nonzero `state_version`, and
   `last_update_timestamp == 9000` when its event is accepted.

Verification command:

```sh
sh ESPS3/components/Middlewares/radar_domain/tests/run_host_tests.sh
```

This change does not modify `radar_tracker`, `radar_person`, or
`radar_coordinate_transform`.

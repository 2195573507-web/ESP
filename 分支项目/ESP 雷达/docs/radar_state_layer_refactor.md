# Radar State Layer Refactor

## State ownership

`RadarSourceState` is published by one `RadarSourceContext` for each physical
radar. It contains the fixed `source_id`, device and room identity, presence,
motion, tracks, persons, `source_person_count`, sequence, and timestamp.
S3_LOCAL is `0`, C51 is `1`, and C52 is `2`.

`RadarRoomState` is a derived room record. It contains only `room`, `occupied`,
`motion`, `source_id`, and `last_update_ms`; it owns neither tracks nor persons.

`RadarHomeState` is rebuilt on every read from all current source states. It
contains `occupied_room_count`, `occupied_rooms[]`, and `home_person_count`.
It has no active-source, last-source, cached-room, or transition field.

## Aggregation contract

An online source whose presence is `MOTION`, `PRESENT`, or `HOLD` contributes
one `RadarRoomState`. `home_person_count` is the saturated sum of the included
sources' `source_person_count` values. This is a room-level total, not a
cross-room person identity system.

For concurrent S3_LOCAL and C52 presence, the expected state is:

```text
S3 panel: source_id=0, room=s3_local, S3 tracks only
C52 panel: source_id=2, room=bedroom, C52 tracks only
HOME: occupied_room_count=2
HOME: occupied_rooms=[S3_LOCAL:s3_local|C52:bedroom]
HOME: home_person_count=2
```

## Log contract

- `RADAR_RX_FRAME`: per-source received-frame telemetry.
- `RADAR_SOURCE_STATE`: per-source state and `source_person_count`.
- `RADAR_ROOM_STATE`: derived room occupancy only.
- `RADAR_HOME_STATE`: source-free home aggregation only.

`RADAR_TRACK_UPDATE_COMPAT` and `RADAR_TRACK_UPDATE_METERS` are independently
limited to one record per source per second in the log manager. The primary
track stream remains available for diagnostics.

## Mac debug tool

The macOS tool has distinct `RadarSourceState`, `RadarRoomState`, and
`RadarHomeState` models. Its S3, C51, and C52 panels consume only their source
states. Room and HOME events are stored separately, so HOME records cannot
alter source tracks, counts, or identity.

## Verification

- S3 radar-domain host suite includes S3_LOCAL + C52 aggregation assertions.
- Parser checks replay the same case and confirm that HOME cannot overwrite
  either source state.
- Firmware builds are required separately for ESPS3, ESPC51, and ESPC52.

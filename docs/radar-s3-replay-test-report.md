# S3 Tracker and Spatial Replay Report

## Result

The radar-domain host suite passed after v2 adapter integration. It exercises
coordinate transform, zone mapping, tracker association, lifecycle, spatial
state, stale/offline semantics, and local UART recovery without hardware.

## Covered Deterministic Cases

- Source-specific coordinate transform and zone resolution.
- Two-target crossing/association with stable track IDs and deterministic
  fixed-size assignment.
- TENTATIVE, CONFIRMED, HOLD, and release behavior; business occupancy excludes
  a single unconfirmed observation.
- Dynamic association gate, fixed-lifetime hold/release, filtered motion, and
  motion entry/exit hysteresis.
- Zone change/leave, static jitter, moving target, empty-frame hold,
  `VACANT_INFERRED` only after hold expiry, and stale source returning
  `UNKNOWN`.
- Gate-out jump and coordinate outlier rejection; an outlier does not move an
  accepted track.
- Local UART recovery backoff and valid-frame recovery.

## Boundary Confirmation

Remote observations enter only through `radar_gateway_ingest` and are adapted
to the existing coordinate, zone, tracker, and spatial APIs. C5 slot values are
diagnostic input only; `track_id` is created by the S3 tracker. No cross-source
person merge or shared tracker exists.

## Remaining Hardware Replay

The suite cannot prove physical coordinate orientation, room boundaries,
multi-person ID-switch rate, latency, or LD2450 BLE behavior. Those remain
`HARDWARE_VALIDATION_PENDING` and `HARDWARE_CALIBRATION_PENDING`.


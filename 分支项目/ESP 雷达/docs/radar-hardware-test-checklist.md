# LD2450 C5/S3 Hardware Acceptance Checklist

All items below are pending. Do not mark a row passed without device logs and
observed radar behavior.

## BLE Binding

- [ ] C51 scans only for `C1:BC:3C:3C:3D:60`, with the discovered address type.
- [ ] C52 scans only for `8C:B1:F3:E1:15:41`, with the discovered address type.
- [ ] Record the service UUID, notify UUID, characteristic properties, and
  CCCD subscription result for each radar.
- [ ] Set the explicit UUID and address-type configuration only after the above
  observation; do not infer it from the MAC string.
- [ ] Verify `radar_gatt_validated`, `radar_notify_subscribed`, valid-frame
  counters, and C51/C52 body ids `1`/`2`.

## S3 Isolation and Faults

- [ ] Run C51 and C52 simultaneously and confirm source snapshots never
  overwrite each other or `S3_LOCAL`.
- [ ] Disconnect C51 radar: C51 device stays online, C51 radar becomes offline
  and occupancy becomes `UNKNOWN`; C52/S3 local remain normal.
- [ ] Repeat for C52 and S3 local UART.
- [ ] Exercise bad BLE fragments, a bad tail, reconnect backoff, and recovery.

## Calibration

- [ ] For each source, record front, left-front, right-front, near, far, and
  zone-boundary points.
- [ ] Validate axis orientation, rotation, offsets, room bounds, zone map, and
  maximum distance independently for all three sources.
- [ ] Do not freeze coordinate settings before these measurements.

## Multi-target and Performance

- [ ] Record two-person crossing: slot, S3 track id, raw position, filtered
  position, and ID switches.
- [ ] Validate stationary presence, walking motion, short occlusion/HOLD,
  vacancy timeout, stale source, and source recovery.
- [ ] Record C5/S3 task stack high-water, heap free/largest block, parser and
  upload timing, ingest rate, and accepted/rejected counters.

Current status: `BLOCKED_BY_RADAR_GATT_UUID`,
`HARDWARE_CALIBRATION_PENDING`, `HARDWARE_VALIDATION_PENDING`.


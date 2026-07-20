# C5 radar runtime rate check

## Scope

This diagnostic is implemented only in `ESPC51` and `ESPC52`.

- No BLE connection or GATT behavior changed.
- No LD2450 parser or radar filter behavior changed.
- No upload payload, retry behavior, or S3 source changed.
- `local_id=1` is C51 and `local_id=2` is C52. The existing board MAC settings remain the only BLE binding difference.

## Runtime Logs

The active C5 path is `BLE notify -> raw input queue -> LD2450 parser -> edge filter -> upload queue`.
`RADAR_LOCAL_PROCESS` and `RADAR_LOCAL_SUMMARY` are emitted only after at least one parser-valid frame has completed filtering in the current window. Each log type emits at most once per one-second active window.

```
RADAR_LOCAL_PROCESS local_id=2 frame_count=100 targets=1 target0_id=1 x_mm=1200 y_mm=2300 speed_cm_s=30 distance_mm=2580 confidence=90 interval_ms=98 hz=10.2
RADAR_LOCAL_SUMMARY local_id=2 frames_last_sec=10 avg_interval_ms=99 targets=1 queue_depth=0 drop_count=0
RADAR_BLE_RX_SUMMARY local_id=2 notify_count=10 valid_frame_count=10 invalid_frame_count=0
```

`frame_count` is the boot-lifetime number of frames that reached the filter output. `target0_id` is the LD2450 target slot plus one, matching the existing result codec. When `targets=0`, target-specific values are logged as zero.

`interval_ms` and `hz` describe the most recent pair of filter outputs. `avg_interval_ms` is the mean of non-zero filter-output intervals in the completed window. A first isolated frame has no preceding interval, so both interval fields are zero.

## Frequency Interpretation

All counts below are deltas for the preceding active one-second window. Therefore their numeric value is also the observed per-second frequency.

| Signal | C51 | C52 | Source |
| --- | --- | --- | --- |
| Local process log | At most 1 Hz, `local_id=1` | At most 1 Hz, `local_id=2` | `RADAR_LOCAL_PROCESS` |
| Local filter summary | At most 1 Hz while filter output exists | At most 1 Hz while filter output exists | `RADAR_LOCAL_SUMMARY` |
| BLE notify frequency | `notify_count / 1 s` | `notify_count / 1 s` | `RADAR_BLE_RX_SUMMARY` |
| Parser valid-frame frequency | `valid_frame_count / 1 s` | `valid_frame_count / 1 s` | `RADAR_BLE_RX_SUMMARY` |
| Filter-output frequency | `frames_last_sec / 1 s` | `frames_last_sec / 1 s` | `RADAR_LOCAL_SUMMARY` |
| Raw BLE queue state | `queue_depth`, `drop_count` | `queue_depth`, `drop_count` | `RADAR_LOCAL_SUMMARY` |

`notify_count` counts non-empty BLE notifications passed into the C5 radar domain. `valid_frame_count` is the parser's accepted 30-byte LD2450 frame count. `invalid_frame_count` is the parser's rejected complete-frame bad-tail count; header-resynchronization bytes are not treated as complete invalid frames.

`queue_depth` is the instantaneous depth of the raw BLE input queue immediately before parser processing. `drop_count` is the number of raw BLE input queue drops during the reported one-second window. It does not include upload retry outcomes, so it measures only C5 receive-side pressure.

## Verification Status

Host verification can prove the log plumbing compiles and the C51/C52 source is identical. It cannot establish an actual LD2450 notify rate, parser rate, or queue behavior without a bound radar, flashed firmware, and serial capture. Those hardware values remain pending runtime observation.

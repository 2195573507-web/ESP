# C5 to S3 Radar Local Contract v2

## Endpoint

```http
POST /local/v1/radar/result
Content-Type: application/json
X-Device-Id: sensair_shuttle_01 | sensair_shuttle_02
```

The request body is at most 1024 bytes. The top-level, `v`, and target objects
use exact-key validation. Unknown fields are rejected.

## Frozen Payload

```json
{
  "p": 2,
  "id": 1,
  "t": "radar",
  "u": 123456,
  "q": 10,
  "v": {
    "link_state": 5,
    "sample_valid": 1,
    "frame_seq": 234,
    "frame_uptime_ms": 123450,
    "target_count": 1,
    "targets": [
      {
        "slot": 0,
        "x_mm": 1200,
        "y_mm": 800,
        "speed_cm_s": 15,
        "resolution_mm": 320,
        "distance_mm": 1442
      }
    ]
  }
}
```

| Field | Rule |
|---|---|
| `p` | integer, exactly `2` |
| `id` | `1` for C51, `2` for C52 |
| `t` | exactly `radar` |
| `u`, `q` | C5 monotonic uptime and nonzero request sequence |
| `link_state` | C5 BLE state enum `0..7` |
| `sample_valid` | `0` or `1` |
| `frame_seq`, `frame_uptime_ms` | parser frame sequence and C5 frame uptime |
| `target_count` | `0..3`; exactly equals `targets` length |
| `slot` | `0..2`, diagnostics only; never a person ID |
| coordinates and speed | signed LD2450 values, no C5 coordinate transform |
| `resolution_mm` | unsigned LD2450 resolution value |
| `distance_mm` | C5 integer square-root result from X/Y |

`sample_valid=1,target_count=0` is a valid empty observation. A disconnected
or not-yet-valid radar sends `sample_valid=0,target_count=0`; it means radar
offline/unknown, not vacant.

## Identity and Sequence

`id=1` maps only to `sensair_shuttle_01` and `RADAR_SOURCE_C51`; `id=2` maps
only to `sensair_shuttle_02` and `RADAR_SOURCE_C52`. The request header and
body identity must agree. Equal `q` plus equal body is idempotent; equal `q`
with different content conflicts; backward sequence is rejected unless an
uptime rollback establishes a new session.

The fixed S3 source values are `S3_LOCAL=0`, `C51=1`, and `C52=2`. Each source
has independent spatial state and tracker context.

## Explicit Exclusions

There is no `confidence`, `raw_frame`, raw BLE byte array, base64 payload,
zone, track ID, person count, presence, motion, or occupancy in this request.
S3 rejects an injected target `confidence` field. S3 tracker confidence is
internal lifecycle evidence only and is never accepted from C5 or exposed by
this transport contract.


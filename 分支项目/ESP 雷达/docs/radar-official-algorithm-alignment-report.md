# LD2450 Official Algorithm Alignment Report

Date: 2026-07-18

## Scope

This change is limited to `ESPS3/components/Middlewares/radar_domain/` and its
host tests. It does not modify the C5 BLE path, the LD2450 30-byte protocol,
radar ingest APIs, coordinate-transform APIs, or the existing dashboard HTTP
JSON schema.

## Official Reference Points

| Reference | Confirmed point | Alignment in S3 domain |
| --- | --- | --- |
| `ESP 资料/2450/HLK-LD2450上报解析参考代码/HLK-LD2450--demo/Hardware/Serial.c` | The demo publishes only after the `AA FF 03 00` header, 24-byte payload, and `55 CC` tail have all completed. | The parser is unchanged; the domain continues to consume only completed parser frames. |
| `.../HLK-LD2450--demo/User/main.c` | A target slot is displayed only when its distance-resolution field is not zero. | `radar_domain` repeats this check before coordinate conversion, alongside all-zero and sentinel protection already performed by the parser. |
| `LD2450 串口通信协议 V1.03.pdf` | A real-time frame contains at most three target slots and provides no persistent person ID. | The tracker owns stable IDs, limits current tracks to three, and never treats a protocol slot number as a person ID. |
| Official App/tool availability | The available official sources do not disclose the App's private filter or association implementation. | This implementation uses explicit, testable S3 rules rather than claiming the App uses EMA, Kalman, or a private tracker. |

The LD2450 protocol has no per-target human-confidence field. The confidence
gate below is therefore an S3 track-quality score, not a reinterpretation of
the wire protocol.

## Previous Difference

Before this change, S3 already had parser zero/sentinel handling, coordinate
range checks, global nearest-neighbor association, and EMA smoothing. However:

- A new candidate received a `track_id` on its first frame.
- The association gate could expand above 800 mm.
- No position-derived maximum velocity rejected instant jumps.
- `HOLD` and tentative tracks were kept in the local read-only track list, so
  stale identities could reach dashboard `targets` even after disappearance.
- No independent history snapshot retained stale tracks after current output
  was filtered.

This allowed a one-person stream with intermittent false slots to consume new
IDs repeatedly, producing `T001` through high stale IDs over time.

## Implemented Algorithm

### Candidate validity

Before tracking, the domain rejects a target that is invalid, has zero distance
resolution, uses an LD2450 sentinel coordinate, exceeds the existing maximum
distance, or falls outside the existing room bounds. No coordinate-transform
function signature or coordinate convention changed.

### Association and filtering

- Association uses the predicted filtered location and a strict distance gate
  of less than 800 mm.
- Filtered coordinates use a fixed lightweight EMA with `alpha = 0.35`.
- Track velocity for prediction is calculated from consecutive filtered
  coordinates. A measurement requiring more than 3000 mm/s from the previous
  raw measurement is rejected before it can update a track.
- A first observation is `TENTATIVE` and has no ID. It becomes a visible track
  only after two associated frames and software confidence 70.
- When a confirmed track is not observed, unmatched candidates cannot create a
  replacement ID. Additional candidates are serialized and may begin only when
  an existing confirmed track was observed in the same frame. This prevents a
  dropped or jumping slot for one person from creating a second identity.

### Lifecycle and output

| State | Timing | Output |
| --- | --- | --- |
| Tentative | first matching frame | internal only; no `track_id` |
| Active | confirmed, visible, confidence >= 70 | included in existing dashboard `targets` and registry current target data |
| Stale/HOLD | missing for 500 ms | removed from current output; copied once to independent domain history |
| Deleted | missing for 1500 ms | removed from tracker; history remains available for diagnostics/replay |

Current active tracks remain capped at the LD2450 maximum of three. The
existing HTTP schema is unchanged: its pre-existing `targets` list now contains
only active current tracks, while stale history remains internal to
`radar_spatial_snapshot_t`.

## Modified Files

| File | Change |
| --- | --- |
| `ESPS3/components/Middlewares/radar_domain/radar_coordinate_transform.c` | Added defensive official-style zero-resolution and sentinel rejection without changing the API. |
| `.../radar_spatial_state.c` | Added domain thresholds, candidate validation, current/history snapshots, and stale-history capture. |
| `.../radar_target_tracker.c` | Added two-frame ID confirmation, strict 800 mm association, EMA, position velocity prediction/limit, stale/delete lifecycle, and replacement-ID suppression. |
| `.../radar_local_adapter.c` | Publishes only confirmed current tracks to existing registry and readonly consumers. |
| `.../radar_diagnostics.c` | Added `radar_tracker: active= created= matched= deleted= stale=` summary output. |
| `.../include/radar_spatial_types.h`, `radar_target_tracker.h`, `radar_spatial_state.h` | Added threshold, velocity, current/history, and diagnostics state required by the implementation. |
| `.../tests/test_radar_spatial.c` | Updated lifecycle tests and added the five-minute single-person replay. |

## Test Results

| Check | Result |
| --- | --- |
| `sh ESPS3/components/Middlewares/radar_domain/tests/run_host_tests.sh` | PASS: protocol/registry/ingest, spatial/recovery, gateway, and adapter suites all passed. |
| Five-minute single-person replay | PASS: 3000 frames at 100 ms each retain only `track_id == 1` after confirmation; periodic one-frame ghost slots and zero-resolution slots do not allocate another ID. |
| 500 ms stale rule | PASS: after a stream pause, current active count becomes zero and the final `T001` snapshot moves to history. |
| 1500 ms delete rule | PASS: the live track is deleted while the historical snapshot remains. |
| `idf.py -C ESPS3 build` | PASS: `sensair_s3_gateway.bin` generated at `0x119590`; 84% of the smallest app partition remains free. |

## Hardware Boundary

No flash, serial monitor, raw UART capture, official App comparison, or
hardware acceptance was performed. The five-minute result is deterministic
host replay proof for the S3 algorithm, not a claim of real-device acceptance.

# C5/S3 LD2450 Joint Implementation Execution Log

Date: 2026-07-17

## Delivered Software Boundary

- C51/C52: directed BLE binding framework, fixed 30-byte LD2450 stream parser,
  signed X/Y/speed decoding, 64-bit-safe distance, fixed latest-only upload,
  and v2 JSON codec.
- S3: bounded `POST /local/v1/radar/result`, schema/header identity/sequence
  validation, remote adapter, independent C51/C52 spatial contexts, and the
  existing S3-local adapter path.
- S3 source constants are frozen to `S3_LOCAL=0`, `C51=1`, `C52=2` and covered
  by regression test.
- Tracker/spatial enhancements retain the existing public pipeline and provide
  tentative/confirmed/hold lifecycle, deterministic 3x3 assignment, dynamic
  gate, confirmed-only business state, and stale/offline `UNKNOWN` semantics.
- C5 stale UART/config source was removed so C5 radar ownership is BLE-only.

## Primary Implementation Areas

| Project | Radar implementation areas |
|---|---|
| C51 | `components/radar_ld2450`, `components/Middlewares/radar_ble`, `components/Middlewares/sensor_domain/radar`, minimal middleware/startup integration |
| C52 | same shared radar paths as C51, with only local id/device id/MAC binding differences |
| S3 | `components/Middlewares/local_http_server`, `components/Middlewares/radar_domain`, local radar adapter integration, minimal middleware/orchestrator integration |

## Verification

| Check | Result |
|---|---|
| C51 parser and codec hosts | PASS |
| C52 parser and codec hosts | PASS |
| C5 BLE stream host | PASS |
| C51/C52 parity | PASS; only expected binding identity differs |
| S3 domain/ingest/spatial/recovery hosts | PASS |
| C51 build | PASS, `0x14e810`, 74% smallest-app partition free |
| C52 build | PASS, `0x14e820`, 74% smallest-app partition free |
| S3 build | PASS, `0x118400`, 84% smallest-app partition free |
| latest build warnings | none |
| flash/monitor/ESP-server | not executed |

## Protected and Downstream Scope

No file was written in `ESP-server`, Dashboard, BME690, voice, or command by
this implementation. The working tree already contained unrelated changes in
those paths at audit start; they remain unmodified and were not included in
this radar delivery. No server-facing radar API, Dashboard support, or database
change was added.

## Remaining Gates and Rollback

- `BLOCKED_BY_RADAR_GATT_UUID`: service UUID, notify UUID, and address type are
  intentionally unspecified; real BLE subscription is not claimed.
- `HARDWARE_CALIBRATION_PENDING`: C51/C52 coordinate transforms and zones use
  uncalibrated defaults until measurements are supplied.
- `HARDWARE_VALIDATION_PENDING`: no flash, monitor, real radar, or multi-person
  acceptance was run.
- `BLOCKED_BY_EXISTING_DOWNSTREAM_CONTRACT`: no Dashboard/ESP-server extension
  was authorized, so C51/C52 remain in S3 local snapshots/registry only.

Rollback can be isolated by phase: C5 parser, BLE, or upload; S3 route/ingest;
remote adapter; tracker/spatial enhancement. Retain the v2 contract and adapter
when rolling back only tracker behavior; do not reintroduce raw uploads or C5
occupancy logic.


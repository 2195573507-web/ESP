# LD2450 Radar Chain Audit Report

Date: 2026-07-18

Scope: `ESPC51`, `ESPC52`, and `ESPS3` inside this repository. `ESP-server`, Dashboard frontend, BME690, voice, and command paths were not modified.

Evidence labels: A = source/build/host-test confirmed; B = static design inference; C = requires real hardware.

## Completed Modules

| Layer | Module | Status | Evidence |
|---|---|---|---|
| C51/C52 BLE | `Middlewares/radar_ble` | NimBLE Central init, scan, exact-MAC filter, connect, service/characteristic discovery, CCCD notify subscription, receive callback, bounded reconnect | A |
| C51/C52 binding | `radar_ble_binding_config.h` | Fixed `device_id`, `local_id`, `room_id`, `radar_id`, MAC, service UUID field, notify UUID field | A |
| C51/C52 parser | `radar_ld2450` | Fixed 30-byte LD2450 framing, header/tail checks, fragmented-stream recovery, target x/y/speed/distance decode | A |
| C51/C52 edge domain | `components/radar_domain/radar_filter.c` | Latest-only invalid/out-of-range filtering, distance sorting, speed smoothing, continuity confidence | A |
| C51/C52 codec | `radar_state_codec.c` | Radar v3 JSON carrying `target_count`, `target_id`, `x_mm`, `y_mm`, `velocity_cm_s`, `confidence`, resolution, and distance | A |
| S3 ingress | `Middlewares/radar_ingest` | Strict v3 schema/identity validation and latest-frame replacement | A |
| S3 worker | `radar_worker_task` | Independent 20 ms worker, APP CPU pin on dual-core targets, calls existing gateway/spatial pipeline | A |
| S3 spatial domain | Existing coordinate transform, zone map, tracker, spatial state | Preserved; v3 adapter sends normalized targets to the existing domain boundary | A |
| S3 debug | `GET /local/v1/radar/debug` | Returns tracked targets with source, ID, coordinate, speed, confidence, and history counters | A |

## Binding Configuration

| Terminal | Device | Room | Radar | Fixed MAC |
|---|---|---|---|---|
| C51 | `sensair_shuttle_01` | `living_room` | `ld2450_01` | `C1:BC:3C:3C:3D:60` |
| C52 | `sensair_shuttle_02` | `bedroom` | `ld2450_02` | `8C:B1:F3:E1:15:41` |

Only these exact MAC values can pass the scan filter. The address type is temporarily `ANY` only to discover whether the known fixed MAC is public or random; it does not allow connection to any other MAC.

`radar_service_uuid` and `radar_notify_uuid` remain empty because no C5/LD2450 hardware was available for a GATT capture. In this state, the firmware still starts NimBLE, scans, connects only to the fixed MAC, and logs the service/characteristic inventory. Once UUID values are recorded, it takes the configured discovery path and writes the notify CCCD before accepting frames.

## Data Flow

```text
LD2450 BLE Notify
  -> C51/C52 NimBLE Central (fixed MAC only)
  -> 512 B bounded stream
  -> LD2450 30 B parser
  -> radar_filter (latest target sample only)
  -> v3 radar JSON /local/v1/radar/result
  -> S3 radar_ingest HTTP validation
  -> latest-frame overwrite slot
  -> radar_worker_task (APP CPU)
  -> existing radar_gateway_ingest adapter
  -> coordinate transform -> target tracker -> zone map -> spatial state
  -> radar registry and GET /local/v1/radar/debug
```

The C5 payload contains no zone, occupancy, motion decision, Dashboard model, or raw BLE frame. C5 only performs acquisition and edge pre-processing. S3 remains the owner of spatial interpretation.

## Memory and Scheduling

| Side | Bound | Purpose |
|---|---:|---|
| C5 | 512 B | Notify byte stream; old bytes are discarded on overflow rather than queued. |
| C5 | 244 B | Per-notify temporary copy before callback. |
| C5 | 30 B parser frame plus one latest sample | No raw-frame upload and no frame backlog. |
| C5 | 4096 B + 4096 B tasks | BLE/parser process task and radar report task. |
| S3 | 2 latest pending samples | One overwriteable pending sample per C5 source. |
| S3 | 4096 B `radar_worker` | Independent worker at 20 ms cadence; pinned to APP CPU when dual-core. |
| S3 | 64 history entries in PSRAM | Accepted-frame ring buffer allocated with `MALLOC_CAP_SPIRAM`; no C5 raw payload storage. |
| S3 | 1024 B max request body | Bounded local HTTP radar request. |

## Required Logs

The C5 NimBLE transport includes all requested markers:

`RADAR_BLE_INIT`, `RADAR_SCAN_START`, `RADAR_DEVICE_FOUND`, `RADAR_MAC_MATCH`, `RADAR_CONNECTED`, `RADAR_NOTIFY_READY`, `RADAR_FRAME_RX`, `RADAR_DISCONNECTED`, and `RADAR_RECONNECT`.

Device discovery and frame-receive logs are rate limited to one message per second. State transitions remain immediate.

## Verification

| Check | Result | Scope |
|---|---|---|
| C51 LD2450 parser/filter/v3 codec host test | PASS | Fragmentation, frame decode, invalid filtering, smoothing, sort, confidence, v3 encoding |
| C52 LD2450 parser/filter/v3 codec host test | PASS | Same coverage as C51 |
| S3 radar parser host test | PASS | Existing local LD2450 parser/recovery |
| S3 radar domain host tests | PASS | Existing registry, v2 ingestion, coordinate transform, tracker, zones, spatial state, plus v3 adapter/latest worker |
| C51/C52 shared radar source parity | PASS | BLE transport, orchestrator, codec, filter and C5 local protocol header are byte-identical |
| C51 ESP-IDF build | PASS | `idf.py -C ESPC51 -B build-radar-c51 build` |
| C52 ESP-IDF build | PASS | `idf.py -C ESPC52 -B build-radar-c52 build` |
| S3 ESP-IDF build | PASS | `idf.py -C ESPS3 build` |
| C5 BLE connection, service, notify | Not run | C: no connected C5 terminal or LD2450 was available |
| Real BLE frame parsing | Not run | C: requires Notify capture from the bound LD2450 |
| C5 to S3 Wi-Fi request | Host-tested only | C: requires C5 and S3 running together |
| S3 `/radar/debug` live response | Not run | C: S3 was built but not flashed or monitored |

## Uncompleted Hardware Items

1. Capture each bound LD2450's address type, service UUID, notify UUID, and optional write UUID from the fixed-MAC discovery logs.
2. Replace the temporary address-type `ANY` setting with the measured public/random type and fill the service/notify UUID fields for both C5s.
3. Flash and monitor C51/C52 to verify the nine required BLE markers, including MAC match, service discovery, CCCD write, and Notify receive.
4. Run C51/C52 against the flashed S3 and call `GET /local/v1/radar/debug` to verify coordinates, target count, speed, tracker IDs, zones, occupancy, and motion in the installed rooms.
5. Dashboard frontend and Server persistence were intentionally not changed by this task's explicit scope constraints. The local S3 debug endpoint is the supplied visualization/debug boundary.

## Follow-up Recommendations

1. Keep raw coordinate orientation until measured room installation data proves a required transform; configure per-source transform/zone profiles only after physical calibration.
2. Record a short Notify hex corpus and compare it with the LD2450 tool output before treating edge continuity confidence as a physical RF confidence metric.
3. Run a BLE/Wi-Fi coexistence soak with heap, task high-watermark, stream-overflow, reconnect, and PSRAM-history metrics enabled.
4. Freeze an explicit S3-to-Server/Dashboard contract only if the prohibited scopes are later reopened; it must publish S3 spatial state, never C5 raw frames.

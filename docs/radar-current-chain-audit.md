# Radar Current Chain Audit

Audit date: 2026-07-18

Scope: source-only audit of the current checkout. This document distinguishes code that is compiled into the current S3 middleware from stale files that merely remain in the tree. It does not claim BLE pairing, UART wiring, hardware positions, or server delivery has been verified on a device.

Status words: **implemented** means present on an active build path; **partial** means the code exists but has a material boundary or verification gap; **not implemented** means no active source path was found.

## 1. Hardware topology

### 1.1 Source-bound topology

```text
LD2450 (MAC C1:BC:3C:3C:3D:60)
  -> BLE notify -> ESPC51 (local_id=1, sensair_shuttle_01, living_room)
  -> parsed target result -> HTTP POST /local/v1/radar/result -> ESPS3

LD2450 (MAC 8C:B1:F3:E1:15:41)
  -> BLE notify -> ESPC52 (local_id=2, sensair_shuttle_02, living_room in C5 binding;
     bedroom in S3 registry)
  -> parsed target result -> HTTP POST /local/v1/radar/result -> ESPS3

LD2450 (no MAC identity in source)
  -> UART1, ESPS3 TX=GPIO18 / RX=GPIO17, 256000 8N1
  -> local parser -> local spatial state -> S3 registry
```

* **Implemented:** C51 has `RADAR_BLE_BINDING_ENABLED=1`, the stated MAC, `local_id=1`, and `sensair_shuttle_01` in [ESPC51 binding config](../ESPC51/components/Middlewares/radar_ble/include/radar_ble_binding_config.h). C52 has the stated MAC and `local_id=2` in [ESPC52 binding config](../ESPC52/components/Middlewares/radar_ble/include/radar_ble_binding_config.h).
* **Implemented:** S3's active configuration enables the local UART path and sets UART1 / GPIO18 / GPIO17 in [sdkconfig.defaults](../ESPS3/sdkconfig.defaults) and [radar_config.h](../ESPS3/components/radar_ld2450/include/radar_config.h). `S3_LOCAL` is source 0; C51 and C52 are sources 1 and 2 in [radar_registry.h](../ESPS3/components/Middlewares/radar_domain/include/radar_registry.h) and [radar_registry.c](../ESPS3/components/Middlewares/radar_domain/radar_registry.c).
* **Partial:** there is no source proof that the physical S3 UART radar is a separate third LD2450, nor proof that either C5 MAC was observed and paired at runtime. The C52 room identity also conflicts: the C52 BLE binding says `living_room`, while the S3 registry maps C52 to `bedroom`.

### 1.2 Identity, connection, and recovery

* **BLE MAC whitelist / directed connection: implemented.** C5 scans advertisements, compares every address against its one configured MAC (with NimBLE byte reversal), cancels scanning only on a match, then connects to that advertised peer. Evidence: [radar_ble_transport.c](../ESPC51/components/Middlewares/radar_ble/radar_ble_transport.c).
* **Automatic scanning: implemented.** C5 starts a five-second scan and logs all discovered devices at a throttled cadence before filtering by MAC. The scan is not name- or service-filtered.
* **Reconnect: implemented.** Failed connection, failed discovery/subscription, and disconnect increment counters and retry scan with exponential 1 s to 30 s backoff. See `schedule_retry()` in [radar_ble_transport.c](../ESPC51/components/Middlewares/radar_ble/radar_ble_transport.c).
* **S3 UART recovery: implemented.** Three errors, RX silence for 3 s, or overflow enter recovery; reinitialization is exponential from 250 ms to 8 s, and three valid frames are required before `VALID`. [radar_config.h](../ESPS3/components/radar_ld2450/include/radar_config.h), [radar_uart_recovery.c](../ESPS3/components/radar_ld2450/radar_uart_recovery.c), and [radar_service.c](../ESPS3/components/radar_ld2450/radar_service.c).
* **Device identity mapping: implemented, but split across layers.** C5 binds MAC -> local ID/device ID; S3 fixes `S3_LOCAL`, C51, and C52 device IDs and source slots. Remote payload parsing rejects a `device_id` that does not match the local ID. [radar_registry.c](../ESPS3/components/Middlewares/radar_domain/radar_registry.c) and [radar_ingest.c](../ESPS3/components/Middlewares/radar_ingest/radar_ingest.c).

## 2. BLE transport

### 2.1 Current C5 receive path

```text
SCAN (all advertisements)
  -> exact configured MAC match
  -> CONNECT
  -> discover all services
  -> discover all characteristics of each service
  -> choose notify candidate and its CCCD
  -> write 0x01,0x00 to CCCD
  -> RECEIVE NOTIFY bytes
  -> BLE stream buffer -> LD2450 parser
```

This flow is implemented by [radar_ble_transport.c](../ESPC51/components/Middlewares/radar_ble/radar_ble_transport.c), [radar_ble_runtime.c](../ESPC51/components/Middlewares/radar_ble/radar_ble_runtime.c), and [radar_ble_stream.c](../ESPC51/components/Middlewares/radar_ble/radar_ble_stream.c), with byte-identical C52 counterparts except identity configuration.

### 2.2 UUID and handle facts

* **Service UUID: partial / runtime-discovered.** The implementation enumerates all services and retains the service that contains the selected notify characteristic. It does not configure or validate a fixed service UUID before connection.
* **Notify UUID: partial.** The candidate preference is `FFF1` (highest) then `AE02`, and only Notify-capable characteristics qualify. The selected UUID and value handle are logged at runtime. No checked-in runtime log proves which UUID/handle a physical LD2450 exposed.
* **Write UUID: partial.** The candidate preference is `FFF2` with write-without-response, then `AE01` with Write. It is discovered and logged but is optional for notification subscription; no start command is sent because the configured command length is zero.
* **Characteristic handles: runtime-discovered, not source constants.** Notify value handle, write handle, and CCCD handle are populated after discovery; no fixed handle is present in source.

The source emits a literal `service=FFF0 notify=FFF1` informational log after subscription, but selection itself is dynamic. Treat that string as a diagnostic assumption, not evidence that a connected device exposed `FFF0/FFF1`.

## 3. C5 processing pipeline

### 3.1 Input and parsing

**Implemented pipeline:**

```text
BLE notification bytes
 -> 1024-byte stream buffer
 -> fixed 30-byte LD2450 parser
 -> target sample (up to three targets)
 -> edge filter
 -> compact p=3 JSON
 -> HTTP POST to S3 /local/v1/radar/result
```

The C5 parser accepts header `AA FF 03 00`, requires a 30-byte frame and tail `55 CC`, resynchronizes byte-by-byte after header/tail errors, and rejects all-zero or sentinel target slots. It extracts three eight-byte target blocks: directional `x_mm`, signed `y_mm`, directional `speed_cm_s`, `resolution_mm`, and Euclidean `distance_mm`. Evidence: [ld2450_parser.c](../ESPC51/components/radar_ld2450/ld2450_parser.c), [ld2450_types.h](../ESPC51/components/radar_ld2450/include/ld2450_types.h), and [radar_service.c](../ESPC51/components/radar_ld2450/radar_service.c).

### 3.2 C5 filtering and tracking boundary

* **Implemented:** C5 removes invalid/duplicate slots and targets farther than the edge maximum, smooths speed with a 2:1 previous/current average, derives continuity confidence 50..100, and sorts remaining targets by distance. [radar_filter.c](../ESPC51/components/radar_domain/radar_filter.c).
* **Not implemented:** persistent person tracking or stable cross-frame target IDs on C5. `target_id` in the transport payload is the LD2450 slot plus one, not a tracker identity.
* **Not implemented:** room zones, multi-radar fusion, final occupancy semantics, or advanced multi-target association on C5.

### 3.3 Current C5 output

Both C5s encode the same schema and post to the same local S3 route. The active encoder is [radar_state_codec.c](../ESPC51/components/radar_ld2450/radar_state_codec.c); the worker is [radar_worker.c](../ESPC51/components/Middlewares/radar_domain/radar_worker.c).

```json
{
  "p": 3,
  "id": 1,
  "t": "radar",
  "u": 123456,
  "q": 10,
  "v": {
    "device_id": "sensair_shuttle_01",
    "link_state": 5,
    "sample_valid": 1,
    "frame_seq": 42,
    "frame_uptime_ms": 123450,
    "target_count": 1,
    "targets": [{
      "target_id": 1,
      "x_mm": 100,
      "y_mm": 2000,
      "velocity_cm_s": -15,
      "confidence": 70,
      "resolution_mm": 200,
      "distance_mm": 2002
    }]
  }
}
```

The exact C51/C52 parity checker passes; the only intended difference is binding identity (local ID, device ID, MAC). [tools/check_c5_radar_parity.sh](../tools/check_c5_radar_parity.sh).

## 4. S3 processing pipeline

### 4.1 Inputs and active paths

* **S3 local UART: implemented.** `radar_service` reads UART bytes, feeds the same LD2450 parser, retains the latest valid frame, and exposes diagnostics/recovery. [ESPS3 radar service](../ESPS3/components/radar_ld2450/radar_service.c).
* **C51/C52 p=3 HTTP input: implemented.** `POST /local/v1/radar/result` calls `radar_ingest_process_json`; the active source is [radar_ingest/radar_ingest.c](../ESPS3/components/Middlewares/radar_ingest/radar_ingest.c), registered in [local_http_server.c](../ESPS3/components/Middlewares/local_http_server/local_http_server.c) and compiled in [Middlewares/CMakeLists.txt](../ESPS3/components/Middlewares/CMakeLists.txt).
* **Important source hygiene fact:** [radar_domain/radar_ingest.c](../ESPS3/components/Middlewares/radar_domain/radar_ingest.c) is another same-named legacy parser with a different schema. It is not listed in `gateway_srcs`, therefore it is **not implemented on the active S3 build path**.

### 4.2 Processing

```text
S3_LOCAL UART frame
 -> radar_local_adapter
 -> coordinate_transform
 -> spatial_state / tracker
 -> registry source 0

C51/C52 p=3 result
 -> schema + device identity validation
 -> latest-only per-source pending slot
 -> gateway_ingest
 -> per-source spatial_state / tracker
 -> registry source 1 or 2
```

The local adapter's active sequence is in [radar_local_adapter.c](../ESPS3/components/Middlewares/radar_domain/radar_local_adapter.c). Remote input is queued latest-only, processed at 20 ms, sequenced, then sent to one independent spatial state per C5 source in [radar_ingest.c](../ESPS3/components/Middlewares/radar_ingest/radar_ingest.c) and [radar_gateway_ingest.c](../ESPS3/components/Middlewares/radar_domain/radar_gateway_ingest.c).

### 4.3 Algorithm status

| Capability | S3_LOCAL | C51/C52 after S3 ingest | Evidence |
| --- | --- | --- | --- |
| Coordinate transform | Implemented | Implemented | `radar_coordinate_transform.c` |
| Coordinate EMA | Implemented (60% new sample) | Implemented | `radar_target_tracker.c` |
| Jump / velocity suppression | Implemented | Implemented | `radar_target_tracker.c` |
| Multi-target tracking | Implemented, max three tracks | Implemented per source, max three tracks | `radar_target_tracker.c` |
| Stable track IDs | Implemented within a source | Implemented within a source | `radar_target_tracker.c` |
| Confirmation / hold / expiry | Implemented (2 frames, 800 ms hold, 3 s deletion) | Implemented | `radar_spatial_state.c` |
| Frame-loss handling | Implemented via UART recovery and stale/offline transitions | Implemented via 5 s remote-input timeout | `radar_service.c`, `radar_gateway_ingest.c` |
| Occupancy and motion | Implemented | Implemented | `radar_spatial_state.c` |
| Multi-device fusion | Not implemented | Not implemented | one spatial tracker per source; no cross-source association |

### 4.4 Zones and spatial meaning

The coordinate defaults are mm, no axis flip, no rotation, zero offset, ±6 m room bounds, and a single full-room active zone. [radar_config.h](../ESPS3/components/radar_ld2450/include/radar_config.h). However, `radar_spatial_state_on_frame()` calls `radar_target_tracker_update(..., NULL, ...)`; the initialized zone map is not passed to the tracker. Therefore zone membership output is **partial**: configuration and map code exist, but per-track zone assignment is not active on this frame path.

## 5. Data contract

### 5.1 Frozen source IDs

**Implemented and consistent in active S3 code:** `S3_LOCAL=0`, `C51=1`, `C52=2`. Evidence: [radar_registry.h](../ESPS3/components/Middlewares/radar_domain/include/radar_registry.h), [radar_registry.c](../ESPS3/components/Middlewares/radar_domain/radar_registry.c), and source-ID assertions in [test_radar_gateway_ingest.c](../ESPS3/components/Middlewares/radar_domain/tests/test_radar_gateway_ingest.c).

### 5.2 C5-to-S3 contract

The current real protocol is the p=3 envelope shown in section 3.3, not a flat `{source, device_id, timestamp, ...}` object. C5 emits `target_id`, `x_mm`, `y_mm`, `velocity_cm_s`, `confidence`, `resolution_mm`, and `distance_mm`; S3 validates the exact same keys in [radar_ingest.c](../ESPS3/components/Middlewares/radar_ingest/radar_ingest.c).

S3 then constructs an internal output containing device ID, local ID, online state, occupancy, motion, transformed stable `track_id`, target coordinates, confidence, visibility, and zone summary. [radar_gateway_ingest.h](../ESPS3/components/Middlewares/radar_domain/include/radar_gateway_ingest.h).

**Partial:** the C5 `target_id` is a raw slot, while S3 `track_id` is its own persistent tracker ID. Consumers must not interpret the C5 field as an identity preserved through target crossing.

## 6. Upload pipeline

```text
C51/C52 -> HTTP /local/v1/radar/result -> S3 local ingest + in-memory registry/history
S3_LOCAL -> UART -> in-memory registry
S3 -> ESP-server: no active radar upload call found
```

* **Implemented:** C5 retries its local S3 POST up to four times; S3 keeps 64 in-memory remote history entries. [radar_worker.c](../ESPC51/components/Middlewares/radar_domain/radar_worker.c) and [radar_ingest.c](../ESPS3/components/Middlewares/radar_ingest/radar_ingest.c).
* **Not implemented:** S3 forwarding raw LD2450 bytes, processed targets, occupancy, or radar history to ESP-server. No radar reference occurs in the active S3 `server_client`, `sensor_aggregator`, or `network_worker` sources.
* **Not implemented:** ESP-server radar-specific ingest, persistence, or dashboard endpoints. Current `/api/device/v1/ingest` accepts `sensor.bme690`; dashboard routes serve general gateway/dashboard snapshots, but S3 does not place radar fields into those snapshots. Evidence: [deviceRoutes.js](../ESP-server/src/routes/deviceRoutes.js), [dashboardRoutes.js](../ESP-server/src/routes/dashboardRoutes.js), and [server_client.c](../ESPS3/components/Middlewares/server_client/server_client.c).
* **Existing local API:** `POST /local/v1/radar/result` and `GET /local/v1/radar/debug` are registered. `/local/v1/radar/state` is a shared constant but no S3 route registers it. `/api/device/*` and `/api/dashboard/*` exist in ESP-server generally, but there is no current radar bridge to them.

## 7. Visualization pipeline

### 7.1 S3 local debug API and logs

**Implemented:** `GET /local/v1/radar/debug` returns local and remote targets plus source health (source, device ID, online/sensor/occupancy state, parser counts, recovery state, and last update). [radar_local_handler.c](../ESPS3/components/Middlewares/local_http_server/radar_local_handler.c).

**Implemented log producers:**

* remote: `RADAR_REMOTE_RADAR_RX`, `RADAR_HTTP_INGEST`, accept/drop messages;
* S3 local: `local raw slot=`, `local accepted index=`, `local track=`, `local transition ... recovery=`;
* source registry: `source transition source=...`, `source=... device_id=...`.

Evidence: [radar_ingest.c](../ESPS3/components/Middlewares/radar_ingest/radar_ingest.c) and [radar_diagnostics.c](../ESPS3/components/Middlewares/radar_domain/radar_diagnostics.c).

### 7.2 ESPS3-Radar-Debug

**Partial:** the Swift tool draws a canvas from parsed logs, not from `/local/v1/radar/debug`. It recognizes `local track=... raw_x=... raw_y=...` (and old/compact variants), then renders those points in [RadarCanvas.swift](../ESPS3-Radar-Debug/Sources/Views/RadarCanvas.swift). Parsing is in [RadarLogParser.swift](../ESPS3-Radar-Debug/Sources/Services/RadarLogParser.swift); state storage is [RadarStateStore.swift](../ESPS3-Radar-Debug/Sources/Stores/RadarStateStore.swift).

Why a current tool session can show no plot:

1. **P2:** current detailed S3 spatial logs are deliberately disabled behind `if (false)` in `radar_diagnostics.c`; the parser's preferred `local track=` records are therefore normally absent.
2. **P2:** remote C51/C52 acceptance logs contain source and target count but not per-target `x_mm/y_mm` in the recognized log shape. The tool cannot invent coordinates from a count.
3. **P2:** the tool does not fetch the debug JSON endpoint, even though that endpoint exposes coordinates for all three sources.
4. **Partial isolation:** source detection supports `S3_LOCAL`, C51 and C52, but a coordinate record without a source falls back to the most recently observed source, which can misattribute interleaved logs.

Required tool locations for a future visualization fix: [RadarLogParser.swift](../ESPS3-Radar-Debug/Sources/Services/RadarLogParser.swift), [RadarStateStore.swift](../ESPS3-Radar-Debug/Sources/Stores/RadarStateStore.swift), and a new/updated client next to [SerialPort.swift](../ESPS3-Radar-Debug/Sources/Services/SerialPort.swift) to poll `/local/v1/radar/debug`.

## 8. Problems

### P0: physical BLE GATT profile remains unverified

The C5 code has hard-coded candidate preferences (`FFF1`/`FFF2`, then `AE02`/`AE01`) but no fixed verified service UUID, address type, discovered handles, or captured production GATT result. A mismatch leaves C51/C52 scanning/retrying without target data. This is a source and hardware-evidence gap, not proof that the profile is wrong. Evidence: [radar_ble_transport.c](../ESPC51/components/Middlewares/radar_ble/radar_ble_transport.c).

### P0: radar stops at S3

Processed C5 targets and S3 local occupancy remain only in S3 RAM/debug API. Neither raw frames, targets, occupancy, nor history reach ESP-server; consequently ESP-server Dashboard cannot show live radar. Evidence: [server_client.c](../ESPS3/components/Middlewares/server_client/server_client.c), [sensor_aggregator.c](../ESPS3/components/Middlewares/sensor_aggregator/sensor_aggregator.c), and [deviceRoutes.js](../ESP-server/src/routes/deviceRoutes.js).

### P1: C52 room mapping conflicts across active source files

C52's BLE binding is `living_room`, while S3's registry is `bedroom`. The source does not resolve this discrepancy; it can label a valid C52 stream with a different room at the S3 layer. Evidence: [ESPC52 binding config](../ESPC52/components/Middlewares/radar_ble/include/radar_ble_binding_config.h) and [radar_registry.c](../ESPS3/components/Middlewares/radar_domain/radar_registry.c).

### P1: coordinate alignment versus the official App is unverified

The parser uses mm and its signed decoding; S3 defaults to no flip/rotation/offset. Source confirms internal unit consistency (`x_mm/y_mm`, `speed_cm_s`, `distance_mm`) but cannot confirm that mounting orientation and vendor App axes agree. EMA and confirmation deliberately add latency: two frames to confirm, 60% sample EMA, three frames to enter motion, and eight frames to leave motion. Evidence: [ld2450_parser.c](../ESPS3/components/radar_ld2450/ld2450_parser.c), [radar_config.h](../ESPS3/components/radar_ld2450/include/radar_config.h), [radar_spatial_state.c](../ESPS3/components/Middlewares/radar_domain/radar_spatial_state.c), and [radar_target_tracker.c](../ESPS3/components/Middlewares/radar_domain/radar_target_tracker.c).

`docs/LD2450官方解析与ESP雷达算法优化参考.md` already exists, so this audit does not create the requested fallback `docs/ld2450-official-parser-reference.md`. Its required validation format should be header, length, three target blocks (`x/y/speed/resolution`), and tail, compared against captured UART/BLE bytes and the official App at known physical positions.

### P1: zone functionality is configured but not applied to tracks

The zone map is initialized, but the local and remote spatial frame paths pass `NULL` to the tracker instead of the zone map. This makes the existing single full-room zone harmless today but prevents trustworthy per-zone output after zones are configured. Evidence: [radar_spatial_state.c](../ESPS3/components/Middlewares/radar_domain/radar_spatial_state.c).

### P2: visualizer does not receive current coordinates reliably

Current detailed position logs are disabled, remote logs lack target coordinates, and the tool has no debug-API client. Evidence: [radar_diagnostics.c](../ESPS3/components/Middlewares/radar_domain/radar_diagnostics.c) and [RadarLogParser.swift](../ESPS3-Radar-Debug/Sources/Services/RadarLogParser.swift).

### P3: stale duplicate parser path increases audit and maintenance risk

The uncompiled [radar_domain/radar_ingest.c](../ESPS3/components/Middlewares/radar_domain/radar_ingest.c) declares the same conceptual input name but parses a different flat schema. It is not a runtime defect while omitted from CMake, but it is a credible future integration hazard.

## 9. Recommended next actions

1. Capture a real GATT discovery transcript for each configured MAC: address type, selected service UUID, notify/write UUIDs, CCCD and value handles, first notification bytes, and C5 reconnect behavior. Keep the current fail-closed MAC binding.
2. Flash and monitor all three sources separately. Validate parser frames, coordinates, signs, units, and App agreement at fixed measured positions before changing flips, rotations, thresholds, or parsing logic.
3. Decide the authoritative C52 room and make the C5 binding and S3 registry agree in a dedicated change.
4. Design and implement an explicit S3-to-ESP-server radar contract before claiming dashboard support. It should distinguish raw C5 slots from S3 stable track IDs and state whether history is persisted.
5. Make visualization consume `/local/v1/radar/debug` or emit one source-qualified, stable coordinate log schema; then test interleaved S3_LOCAL/C51/C52 data.
6. Remove or quarantine the uncompiled duplicate S3 parser only in a separately approved source cleanup, after preserving any test-only purpose.

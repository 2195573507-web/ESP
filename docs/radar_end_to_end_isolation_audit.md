# Radar End-to-End Data Isolation Audit

Audit date: 2026-07-19

## Executive Result

The S3 runtime has strong source-indexed state isolation: local UART uses one
dedicated spatial context, and C51/C52 use two independent gateway slots. No
shared tracker, target buffer, latest snapshot, or sequence domain was found
between the three sources. C51/C52 fixed MACs and device mappings are distinct
in source, and S3 rejects mismatched `local_id`/`device_id` combinations.

The source/log/tool defects found in the initial audit have been repaired. The
system is still **not ready to claim complete end-to-end isolation** until each
radar receives measured per-source installation calibration and BLE hardware
acceptance is completed. None requires rewriting the spatial algorithms.

## Data Flow

The full structured data-flow diagrams and field matrix are in
[`radar_end_to_end_data_flow_audit.md`](radar_end_to_end_data_flow_audit.md).

## Three-Source Status

| Area | S3_LOCAL | C51 | C52 | Result |
| --- | --- | --- | --- | --- |
| identity registry | fixed source/device/room | ID 1 / shuttle 01 | ID 2 / shuttle 02 | Pass |
| physical peer selector | local UART only | fixed MAC | fixed MAC | Source pass; hardware pending |
| ingress pending slot | not applicable | index 0 | index 1 | Pass |
| tracker/spatial state | dedicated local instance | gateway slot 0 | gateway slot 1 | Pass |
| registry/diagnostics | slot 0 | slot 1 | slot 2 | Pass |
| coordinate configuration | shared defaults copied locally | shared defaults copied locally | shared defaults copied locally | Fail: no per-source calibration |
| API output | source-specific targets | source-specific targets | source-specific targets | Partial: room omitted in target rows |
| Mac panel/state | `.s3Local` | `.c51` | `.c52` | Partial: compact-track crosstalk defect |

## Confirmed Findings

| Risk | File and location | Evidence and impact | Minimal remediation |
| --- | --- | --- | --- |
| Fixed P1 | `ESPS3-Radar-Debug/Sources/Services/RadarLogParser.swift`, `RadarSource.swift` | Compact `track=x:y` records now retain the resolved C51/C52/S3 source. Explicit source/device conflicts are forced into UNKNOWN and cannot fall back to the most-recent source. | Regression checks cover C52 compact tracks and conflict isolation. |
| P1 pending calibration | `ESPS3/components/Middlewares/radar_domain/radar_spatial_state.c:22-86`, `radar_gateway_ingest.c:544,562,673`, `radar_local_adapter.c:252` | Spatial instances remain independent, but all start with the same compatibility-default installation values until measured mount X/Y, rotation, offsets, room bounds, and zones are supplied. | Add a source-indexed immutable installation-config provider after recording real values; do not change transform, zone, tracker, or spatial-state algorithms. |
| Fixed P2 | `ESPC52/components/Middlewares/radar_ble/include/radar_ble_binding_config.h:15` | C52 binding room now matches the S3 registry: `bedroom`. | Keep the identity/room parity check in future C51/C52 changes. |
| Fixed P2 | `ESPS3/components/Middlewares/local_http_server/radar_local_handler.c` | Debug source entries and target rows now include source, device ID, and room. The target arrays remain per-source outputs. | Add frame sequence when a target-level API contract is versioned. |
| Fixed P2 | `ESPS3/components/Middlewares/radar_ingest/radar_ingest.c`, `radar_gateway_ingest.c` | Remote receive/accept/ingest logs now carry source, device ID, room, request sequence, and frame sequence. | Retain this format for all new remote radar logs. |
| Fixed P2 | `ESPS3/components/Middlewares/radar_ingest/radar_ingest.c` | The 64-entry accepted-frame history is partitioned into two 32-entry per-source rings, preventing C51/C52 eviction of each other's forensic history. | Extend the debug API with per-source history counters if remote history is exposed later. |
| Fixed P2 | `ESPC51/ESPC52/components/Middlewares/radar_ble/radar_ble_transport.c` | Notify/control logs use discovered service/notify/write UUIDs instead of hard-coded FFF0/FFF1/FFF2 literals. | Hardware must still prove the discovered profile and CCCD subscription. |
| Corrected non-finding | `ESPS3/components/Middlewares/radar_domain/radar_log_manager.c` | `CONFIG_S3_RADAR_PER_SOURCE_LOG_SCHEDULER` defaults to enabled, so C51/C52 source-indexed detailed logs are published. | Keep the configuration enabled. |
| P3 | `ESPC51/ESPC52/components/radar_ld2450/radar_state_codec.c:45-56` | v3 carries `id` and `device_id` but not literal `source`/`room`; S3 reconstructs source and room from a trusted fixed registry. This is safe at S3 ingress but fails the requested all-stage explicit-field rule. | Version the envelope only when compatibility is approved: add `source`, `room_id`, and named timestamp fields, then validate all are consistent with the fixed mapping. |
| P3 | `ESPS3-Radar-Debug/Sources/Models/RadarSource.swift:50-86`, `RadarLogParser.swift:80-90` | The tool accepts tag/recent-source fallback and never rejects a line whose explicit source and device ID disagree. A malformed log can update the wrong room. | Treat conflicting explicit source/device IDs as UNKNOWN diagnostic; permit most-recent-source fallback only for S3 local continuation records. |

## Non-Findings: State Isolation That Is Present

- `radar_gateway_ingest.c` owns `s_slots[2]`; each slot has a full
  `radar_spatial_state_t`, rate manager, last sample, sequence, and output.
- `radar_local_adapter.c` owns a distinct local `s_spatial_state`; it explicitly
  assigns `RADAR_SOURCE_S3_LOCAL` and never routes through a C51/C52 slot.
- `radar_registry.c` owns `s_slots[RADAR_SOURCE_COUNT]`; target arrays,
  online/freshness state, recovery diagnostics, and sequence state are copied
  per source.
- The Mac tool stores `Dictionary<RadarSource, RadarRoomState>` and each
  `RadarRoomState` has its own tracks, history, freshness, and configuration.
- C51/C52 remote source tests permit same sequence numbers on different sources
  while rejecting duplicate/backward values within the relevant slot.

## C51/C52 BLE and Mapping Review

| Board | Device ID | Local ID | Exact MAC | Binding room | Verdict |
| --- | --- | ---: | --- | --- | --- |
| C51 | `sensair_shuttle_01` | 1 | `C1:BC:3C:3C:3D:60` | `living_room` | Identity source correct |
| C52 | `sensair_shuttle_02` | 2 | `8C:B1:F3:E1:15:41` | `living_room` | MAC/device correct; room wrong |

`peer_matches` compares the reversed NimBLE address bytes against one compiled
fixed MAC per board. It prevents C51 from selecting C52's configured peer and
vice versa, subject to real devices advertising the expected address. The
address type remains `ANY`, and service/notify UUIDs have not been verified on
hardware, so BLE acceptance is explicitly pending.

## S3 Local UART Review

S3 UART parsing (`radar_ld2450`/`ld2450_parser`) produces a local `radar_frame_t`
with parser-owned frame sequence and receipt timestamp. `radar_local_adapter`
passes it only to its dedicated local spatial state and updates registry source
0. No route from this adapter to C51/C52 gateway slots was found.

## Diagnostics and HTTP/API Review

`radar_diagnostics` snapshots all three registry entries and its primary
per-source transition/summary logs include source, device ID, room, target
count, online, valid age, motion age, and recovery/freshness diagnostics. This
protects against shared diagnostics state.

`/local/v1/radar/debug` iterates all three sources and reads each source's own
registry entry or local/gateway output. It does not reuse one target array for
all sources. Its JSON is incomplete as an external ownership contract because
room and full identity are not repeated on every target item.

## Protocol Consistency

The active C5 v3 -> S3 contract is `p,id,t,u,q,v`, not the older v2
`schema_version/local_id/sequence` contract retained in compatibility modules.
The v3 fields are consistent for C51/C52 `device_id`, request sequence,
frame sequence, `x_mm`, `y_mm`, velocity/speed in cm/s, and target count.

`angle`, `presence`, and `motion` are intentionally S3-produced spatial
semantics. They must not be interpreted as dropped C5 fields. `room` is a
registry-derived property and should become explicit in logs/API before being
used as a consumer-side identity assertion.

## Minimal Change Order

1. Record real mount X/Y, rotation, offsets, room bounds, and zones for all three devices.
2. Introduce a source-indexed calibration provider with those measured records,
   then wire it only at `radar_spatial_state_init` call sites.
3. Add host fixtures covering distinct calibration values, C51/C52 simultaneous
   equal sequences, and interleaved recovery/offline transitions.

The source/log/tool repairs are complete. The remaining calibration work is
configuration plumbing, not an algorithm rewrite, but needs measured device
values before a safe implementation can be selected.

## Verification

Passed in this audit:

- `idf.py -C ESPC51 -B build-radar-c51 build`
- `idf.py -C ESPC52 -B build-radar-c52 build`
- `idf.py -C ESPS3 -B build-radar-s3-isolation build`
- `sh ESPC51/components/radar_ld2450/tests/run_host_tests.sh`
- `sh ESPC52/components/radar_ld2450/tests/run_host_tests.sh`
- `sh ESPS3/components/Middlewares/radar_domain/tests/run_host_tests.sh`
- `sh ESPS3/components/radar_ld2450/tests/run_host_tests.sh`
- `sh ESPS3-Radar-Debug/script/run_parser_checks.sh`
- `swift build` in `ESPS3-Radar-Debug`

The `radar_domain` runner includes S3 v2 source-isolation and v3 adapter/latest
worker assertions. `ESPS3/components/Middlewares/radar_ingest/tests` has no
separate `run_host_tests.sh`, so its worker checks were not invoked separately.

Not validated: real C51/C52 BLE address type, GATT service/CCCD/notify behavior,
MAC association, S3 UART reception, HTTP traffic from all three devices, or
live Mac rendering. No flash or monitor was run.

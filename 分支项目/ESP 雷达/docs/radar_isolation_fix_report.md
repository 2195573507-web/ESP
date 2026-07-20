# Radar Isolation Fix Report

Date: 2026-07-19

## Root Causes

| Symptom | Root cause | Fix |
| --- | --- | --- |
| S3/C52 Mac panels showed identical targets/history | compact and diagnostic lines could inherit the most recent remote source; target values were not defensively re-scoped | remote implicit fallback now fails closed; target events are scoped to their source; tests cover interleaved coordinates and history |
| `source=0` paired with `sensair_shuttle_02` | identity fields were parsed independently and state accepted arbitrary device/room text | source/device/room mapping is fixed; explicit conflicts and unknown device/room values become `UNKNOWN` |
| `RADAR_HOME active_source=S3_LOCAL` while C52 was occupied | active source was selected from last state transition, allowing an old S3 HOLD to win | HOME recomputes from current occupancy on every read, with motion/present priority and latest-report tie break |
| source logs lacked stable device/room context | detailed logs used source/room selectively and local/person records omitted identity | registry-derived `source_id/source/device_id/room` added to detailed logs, person logs, ingest markers, and HOME |
| API source objects exposed only a shared-looking target count | `/debug` had a flat target array and numeric `targets` field inside each source | each source object now exposes its own nested `targets` array plus `target_count` |
| C5 payload had no explicit room field | room was only reconstructed from local ID at S3 | v3 codec carries `room_id`; S3 validates it against the source registry |

## Changed Files

Firmware:

- `ESPS3/components/Middlewares/radar_domain/radar_registry.c`
- `ESPS3/components/Middlewares/radar_domain/radar_log_manager.c`
- `ESPS3/components/Middlewares/radar_domain/radar_diagnostics.c`
- `ESPS3/components/Middlewares/radar_domain/radar_person_continuity.c`
- `ESPS3/components/Middlewares/radar_domain/radar_gateway_ingest.c`
- `ESPS3/components/Middlewares/radar_ingest/radar_ingest.c`
- `ESPS3/components/Middlewares/radar_ingest/tests/test_radar_ingest.c`
- `ESPS3/components/Middlewares/local_http_server/radar_local_handler.c`
- `ESPC51/components/radar_ld2450/radar_state_codec.c`
- `ESPC52/components/radar_ld2450/radar_state_codec.c`

Mac:

- `ESPS3-Radar-Debug/Sources/Models/RadarSource.swift`
- `ESPS3-Radar-Debug/Sources/Models/RadarModels.swift`
- `ESPS3-Radar-Debug/Sources/Services/RadarLogParser.swift`
- `ESPS3-Radar-Debug/Sources/Stores/RadarStateStore.swift`
- `ESPS3-Radar-Debug/Sources/Views/DashboardView.swift`
- `ESPS3-Radar-Debug/script/ParserChecks/main.swift`

Planning/audit evidence:

- `docs/radar_full_isolation_audit.md`
- `task_plan.md`
- `notes.md`

## Before / After

| Area | Before | After |
| --- | --- | --- |
| S3/C51/C52 state | mostly source-indexed in firmware, but host parser fallback could retag compact records | source-indexed firmware and Mac state with fail-closed remote source parsing |
| Device identity | registry existed but logs/API/parser could disagree | fixed source -> device -> room binding enforced at parser, store, logs, and API |
| HOME | transition-order based | real-time current presence based |
| Logs | source/room inconsistent; local/person lines omitted device | identity-bearing records across state/count/person/ingest/HOME paths |
| HTTP debug | flat target list plus numeric `targets` | independent nested targets arrays per source |
| C5 protocol | device ID plus numeric local ID | device ID plus validated room ID and existing sequence/frame timestamps |
| Mac windows | three panels existed, but source fallback could cross-feed | `.s3Local`, `.c51`, `.c52` states retain distinct targets/history/device/room |

## Verification

Passed:

- `sh ESPS3-Radar-Debug/script/run_parser_checks.sh`
- `swift build` in `ESPS3-Radar-Debug`
- `sh ESPC51/components/radar_ld2450/tests/run_host_tests.sh`
- `sh ESPC52/components/radar_ld2450/tests/run_host_tests.sh`
- `sh ESPS3/components/Middlewares/radar_domain/tests/run_host_tests.sh`
- `idf.py -C ESPC51 -B /tmp/radar-e2e-build-c51 build`
- `idf.py -C ESPC52 -B /tmp/radar-e2e-build-c52 build`
- `idf.py -C ESPS3 -B /tmp/radar-e2e-build-s3 build`
- `git diff --check`

The Mac fixture verifies S3 `(100,100)`, C51 `(200,200)`, and C52 `(300,300)`
remain in separate panels, with fixed device/room identities and independent
history lengths. The S3 host registry test verifies C52 `business_person_count=1`,
S3 `business_person_count=0`, `occupied_room_count=1`, and
`active_room=bedroom`.

Not claimed: physical BLE/UART/runtime acceptance, flashing, monitor logs, or
live Mac rendering.

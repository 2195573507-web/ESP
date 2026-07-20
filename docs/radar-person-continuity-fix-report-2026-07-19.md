# ESPS3 Radar Person Continuity Fix Report

Date: 2026-07-19

## Scope and conclusion

This change is limited to ESPS3 radar-domain processing and the separate ESPS3-Radar-Debug macOS tool. C51/C52 parsers and the C5-to-S3 protocol are unchanged. No tracker, person count, zone, or occupancy behavior was added to C5. ESP-server, Dashboard, BME690, voice, command, and Wi-Fi paths are outside this change.

This is not person identification. The resulting behavior is: 在限定时间和空间门限内，对同一人员的短时目标丢失与恢复进行业务连续性推断。

No firmware was flashed and no device monitor or hardware acceptance run was performed for this delivery.

## Audited count sources

| Field | Actual source | Meaning and count rule |
| --- | --- | --- |
| raw_target_count | radar_spatial_state_on_frame: frame->target_count | Raw LD2450 frame target entries. Not a track or person count. |
| accepted_target_count | Coordinate/domain-valid transformed targets before tracker update | Valid observations admitted to the S3 spatial tracker. Not a person count. |
| visible_track_count | radar_target_tracker_copy_active into snapshot.current_targets | Current visible confirmed motion tracks. active_track_count remains its compatibility alias. |
| confirmed_active_track_count | radar_target_tracker_confirmed_count | Diagnostic motion-track count; it includes tracker CONFIRMED and HOLD, so it must not be used as a person count. |
| history_target_count | record_stale_history circular diagnostic history | Historical HOLD tracks only. Never a current-person count. |
| visible_person_count | radar_person_continuity refresh_counts | Persons in VISIBLE; contributes to business count when the source is valid. |
| retained_person_count | radar_person_continuity refresh_counts | Persons in STILL_HOLD or DORMANT before timeout; contributes to business count only as an estimate. |
| business_person_count | visible_person_count + retained_person_count | The only business person count introduced by this change. |
| count_state | Continuity source validity and person states | OBSERVED, ESTIMATED, VACANT_INFERRED, or UNKNOWN; stale/offline/recovering input yields UNKNOWN. |

The registry keeps both representations separately. Legacy snapshot.current_target_count is populated from visible_track_count and continues to mean transport/current motion tracks. Business consumers read radar_registry_entry_t.count_summary.business_person_count with count_summary.count_state.

radar_gateway_output_t, the local adapter snapshot, /local/v1/radar/debug, and RADAR_COUNTS logging export the explicit count summary. A remote protocol payload without an S3-derived count summary receives UNKNOWN; its target count is retained only as visible_track_count, never as a person count.

## Prior ambiguity and result

Before this work, tracker allocation, HOLD, and history were useful motion diagnostics but had no independent person-continuity ownership. An old held motion segment and a newly confirmed segment could therefore be read as two current targets by a downstream UI or business reader. The macOS tool also had a legacy log fallback that equated visible tracks with business persons when a log did not contain an explicit count summary.

The following comparison is semantic/code-audit based; the old implementation did not expose a separate business-person count to replay side by side.

| Situation | Former track-oriented interpretation | New explicit result |
| --- | --- | --- |
| One person continuously walking | One visible motion track | visible_track_count=1, visible_person_count=1, business_person_count=1, OBSERVED. |
| Short loss after stop | Old tracker can retain a HOLD segment for diagnostics | visible_person_count=0, retained_person_count=1, business_person_count=1, ESTIMATED; the held track is not counted directly. |
| New track after short loss | Old held track and new confirmed track can coexist diagnostically | One-to-one reattachment preserves the old person_id; new track_id is allowed; business count remains 1. |
| History exists while one target is visible | History can coexist with current tracks | History remains separate; it changes neither visible nor business person count. |
| Legacy Debug App log without RADAR_COUNTS | Visible track count was copied into business person fields | Visible tracks remain visible, but person counts stay 0 and count_state=UNKNOWN. |

## Implementation

Data flow:

    observation -> coordinate transform -> zone -> target tracker
                -> person continuity -> spatial state / occupancy / registry

radar_person_continuity is fixed-capacity (LD2450_MAX_TARGETS, currently three persons). Each entity stores person_id, state, attached track_id, last filtered position and zone, first/last visible and attachment times, dormant start, quality, and bounded short-term velocity data. Its states are EMPTY, VISIBLE, STILL_HOLD, DORMANT, and RELEASED.

For each frame, confirmed visible motion tracks first participate in a fixed-capacity deterministic one-to-one assignment against existing persons. The assignment maximizes matches, then minimizes gated distance with a same-zone bonus. A candidate can attach only when zone continuity is acceptable and its distance from the last stable filtered position, with bounded decaying velocity, is within the reacquire gate. Thus a STILL_HOLD/DORMANT person has priority over creation of a new person.

Tentative tracks and single-frame anomalies never create a person. New person creation requires the configured number of distinct confirmed frames. Existing persons transition to STILL_HOLD, then DORMANT, and finally RELEASED at timeout. Release frees the fixed slot; a later target can receive a new person_id.

The tracker remains a motion-segment tracker. track_id may change across a short interruption and is never treated as a persistent human identifier. No LD2450 slot is used as an identity.

## Parameters

| Configuration macro | Value | Purpose |
| --- | ---: | --- |
| RADAR_CONFIG_PERSON_STILL_HOLD_MS | 1000 ms | Visible gap before DORMANT. |
| RADAR_CONFIG_PERSON_DORMANT_TIMEOUT_MS | 4000 ms | Continuity retention limit and release threshold. |
| RADAR_CONFIG_PERSON_REACQUIRE_GATE_MM | 1500 mm | Maximum spatial gate for reattachment. |
| RADAR_CONFIG_PERSON_SAME_ZONE_BONUS_MM | 250 mm | Deterministic matching preference for same zone. |
| RADAR_CONFIG_PERSON_ADJACENT_ZONE_ALLOW | 1 | Allows only adjacent-zone continuity. |
| RADAR_CONFIG_PERSON_NEW_CONFIRM_FRAMES | 2 | Confirmed frames required for a normal new person. |
| RADAR_CONFIG_PERSON_NEW_NEAR_DORMANT_CONFIRM_FRAMES | 3 | Stronger confirmation near a retained person. |
| RADAR_CONFIG_PERSON_VELOCITY_DECAY_START_MS | 400 ms | Start decaying old velocity prediction. |
| RADAR_CONFIG_PERSON_VELOCITY_DECAY_MS | 600 ms | Additional interval until velocity prediction is zero. |

## Count-state contract

| Source/person condition | Visible | Retained | Business | count_state |
| --- | ---: | ---: | ---: | --- |
| At least one VISIBLE person | Included | Only other retained persons | Included | OBSERVED |
| No visible person, STILL_HOLD/DORMANT remains before timeout | 0 | Included | Included | ESTIMATED |
| Valid source, no retained person, no unresolved observation | 0 | 0 | 0 | VACANT_INFERRED |
| Tentative/unresolved observation, stale, offline, or recovering source | 0 | 0 | 0 | UNKNOWN |

HOLD, history, allocated tracker slots, and tentative tracks never directly contribute to a person count. A retained person is a continuity estimate, not a statement that the radar currently observes a stationary body.

## Diagnostics and Debug App

State changes or rate-limited mismatches emit PERSON_CREATE, PERSON_ATTACH_TRACK, PERSON_REACQUIRE, PERSON_STILL_HOLD, PERSON_DORMANT, PERSON_RELEASE, PERSON_COUNT_CHANGE, and TRACK_COUNT_MISMATCH. Each record includes source, person ID, old/new track ID, distance, age, zone, visible/retained/business counts, count state, and reason.

RADAR_COUNTS and /local/v1/radar/debug expose raw targets, accepted targets, visible tracks, confirmed active tracks, history tracks, visible persons, retained persons, business persons, and count state independently.

The macOS Debug App parses those explicit fields and displays Raw targets, Visible tracks, Retained persons, Business persons, Count state, and History tracks independently. Its canvas receives only visibleTracks as current target glyphs. Historic data may remain a faint path in trackHistory, but it is neither drawn as a current target nor included in person counts.

## Deterministic host replay results

| # | Replay scenario | Result |
| ---: | --- | --- |
| 1 | One person continuously walking | PASS; one observed business person. |
| 2 | Walk, stop 500 ms, walk | PASS; original person_id reattached. |
| 3 | Walk, stop 1 s, walk | PASS; original person_id reattached. |
| 4 | Walk, stop 2 s and 3 s, walk | PASS; original person_id reattached. |
| 5 | Stop beyond continuity timeout, then walk | PASS; old person released and a new person_id can be created. |
| 6 | Stop with small coordinate drift / reappear within 1.5 m | PASS; filtered-position gate reattaches without a second count. |
| 7 | One leaves and another enters far away | PASS; far entrant is not merged; two persons remain distinct during retention. |
| 8 | Two people, one stops while one continues | PASS; business count remains two. |
| 9 | Two people briefly disappear and recover | PASS; two one-to-one recoveries, no collapse to one person. |
| 10 | Two people cross and slots change | PASS; deterministic matching preserves two person entities. |
| 11 | Single-frame false second/third target | PASS; no additional person is created. |
| 12 | Source stale, offline, and recovery | PASS; count state is UNKNOWN, not a certain zero. |
| 13 | History present with only one current person | PASS; history remains diagnostic only. |
| 14 | New track near dormant person / de-duplication | PASS; recovered person reuses its ID and never coexists as a duplicate business person. |

The host replay also reports sizeof(radar_person_continuity_t) = 376 bytes and a three-target maximum host CPU time of 0.002 ms; both are well below the 8 KiB and 5 ms acceptance limits. Source inspection confirms no heap allocation or new permanent task in the continuity component.

## Verification

| Command | Result |
| --- | --- |
| sh ESPS3/components/Middlewares/radar_domain/tests/run_host_tests.sh | PASS, including continuity replay, spatial/recovery, registry/ingest, and source isolation tests. |
| IDF_PYTHON_ENV_PATH=... source esp-idf/export.sh; idf.py -C ESPS3 -B build-radar-s3-person build | PASS; no new warning observed. |
| swift build -Xswiftc -warnings-as-errors in ESPS3-Radar-Debug | PASS. |
| ESPS3-Radar-Debug/script/run_parser_checks.sh | PASS, including explicit count parsing, history isolation, and legacy track-only UNKNOWN fallback. |
| ESPS3-Radar-Debug/script/build_and_run.sh --verify | PASS after correcting bundle-path process detection; verification instance was stopped afterward. |
| git diff --check | PASS. |

## Modified files

### ESPS3 radar-domain changes

- ESPS3/components/Middlewares/radar_domain/include/radar_person_continuity.h
- ESPS3/components/Middlewares/radar_domain/radar_person_continuity.c
- ESPS3/components/Middlewares/radar_domain/include/radar_spatial_types.h
- ESPS3/components/Middlewares/radar_domain/include/radar_spatial_state.h
- ESPS3/components/Middlewares/radar_domain/radar_spatial_state.c
- ESPS3/components/radar_ld2450/include/radar_config.h
- ESPS3/components/Middlewares/radar_domain/include/radar_gateway_ingest.h
- ESPS3/components/Middlewares/radar_domain/radar_gateway_ingest.c
- ESPS3/components/Middlewares/radar_domain/include/radar_registry.h
- ESPS3/components/Middlewares/radar_domain/radar_registry.c
- ESPS3/components/Middlewares/radar_domain/radar_local_adapter.c
- ESPS3/components/Middlewares/radar_domain/radar_remote_ingest.c
- ESPS3/components/Middlewares/radar_domain/radar_log_manager.c
- ESPS3/components/Middlewares/local_http_server/radar_local_handler.c
- ESPS3/components/Middlewares/CMakeLists.txt (radar source registration hunk only)

### Tests and macOS Debug App

- ESPS3/components/Middlewares/radar_domain/tests/run_host_tests.sh
- ESPS3/components/Middlewares/radar_domain/tests/test_radar_person_continuity.c
- ESPS3/components/Middlewares/radar_domain/tests/test_radar_spatial.c
- ESPS3/components/Middlewares/radar_domain/tests/test_radar_ingest.c
- ESPS3/components/Middlewares/radar_domain/tests/test_radar_gateway_ingest.c
- ESPS3-Radar-Debug/Sources/Models/RadarModels.swift
- ESPS3-Radar-Debug/Sources/Services/RadarLogParser.swift
- ESPS3-Radar-Debug/Sources/Stores/RadarStateStore.swift
- ESPS3-Radar-Debug/Sources/Views/DashboardView.swift
- ESPS3-Radar-Debug/script/ParserChecks/main.swift
- ESPS3-Radar-Debug/script/build_and_run.sh

Concurrent environment-alarm changes already present in the worktree were not modified or included in this scope.

## Hardware validation still required

- Flash ESPS3 and replay real LD2450 walking, stopping, loss, and recovery data.
- Verify real coordinates, zone transitions, 1.5 m gate behavior, and short-loss timing against the physical installation.
- Verify two real people crossing, temporary slot changes, and distant entry.
- Verify stale/offline/recovering UART behavior and the emitted diagnostic logs on device.
- Measure device-side three-target latency and memory under the production task schedule.

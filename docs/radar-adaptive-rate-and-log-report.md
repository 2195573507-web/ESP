# ESPS3 Radar Adaptive Rate and Log Report

Date: 2026-07-18

## 1. Current Problem Analysis

Before this change, `radar_rx_task` read UART bytes and fed the parser in the same task. `radar_local` then polled the latest frame every 100 ms, so parser, tracker, spatial state, snapshot publication, and diagnostics all had fixed cadence. The old tracker constants also used an 800 mm association gate, 500 ms stale period, 1500 ms delete period, 35 percent EMA, and 70 confidence minimum. This made short occlusions more likely to lose the stable ID and made the trajectory less responsive to a real move.

The UART error-recovery state machine remains unchanged. The new rate manager does not stop reading UART, reset the UART driver, alter UART pins, baud rate, or LD2450 configuration.

## 2. Modified Files

- `ESPS3/components/radar_ld2450/radar_service.c`
- `ESPS3/components/radar_ld2450/include/radar_service.h`
- `ESPS3/components/radar_ld2450/include/radar_config.h`
- `ESPS3/components/radar_ld2450/Kconfig`
- `ESPS3/components/Middlewares/CMakeLists.txt`
- `ESPS3/components/Middlewares/radar_domain/radar_local_adapter.c`
- `ESPS3/components/Middlewares/radar_domain/radar_spatial_state.c`
- `ESPS3/components/Middlewares/radar_domain/radar_target_tracker.c`
- `ESPS3/components/Middlewares/radar_domain/include/radar_spatial_types.h`
- `ESPS3/components/Middlewares/radar_domain/radar_rate_manager.c`
- `ESPS3/components/Middlewares/radar_domain/include/radar_rate_manager.h`
- `ESPS3/components/Middlewares/radar_domain/radar_log_manager.c`
- `ESPS3/components/Middlewares/radar_domain/include/radar_log_manager.h`
- `ESPS3/components/Middlewares/radar_domain/radar_diagnostics.c`
- `ESPS3/components/Middlewares/radar_domain/tests/test_radar_spatial.c`
- `ESPS3/components/Middlewares/radar_domain/tests/run_host_tests.sh`

No C5 firmware, UART GPIO, LD2450 output settings or protocol, C5/S3 data interface, Dashboard JSON schema, or ESP-server files were changed.

## 3. Processing Rate State Machine

`radar_rx_task` now continuously captures UART bytes into a bounded 1024-byte pending buffer. `radar_local` is the processing task: it consumes this buffer, runs parser/tracker/spatial operations when due, and publishes only the latest snapshot. It runs with an 8192-byte stack and reports its high-water mark.

| Mode | Entry rule | Parser | Tracker | Snapshot |
| --- | --- | ---: | ---: | ---: |
| `IDLE` | no active track for 5 seconds | 10 Hz | 5 Hz | 1 Hz |
| `DETECTING` | valid candidate, not confirmed | 10 Hz | 10 Hz | 3 Hz |
| `TRACKING` | one or more active confirmed tracks | 20 Hz | 15 Hz | 5 Hz |
| `FAST_MOVING` | confirmed velocity above 2 m/s | 20 Hz | 20 Hz | 10 Hz |
| `LOST` | active track disappears | 10 Hz | 5 Hz | 3 Hz |

`LOST` reserves the tracker identity for 3 seconds. It prevents a brief occlusion from immediately creating a new `Txxx` ID. The pre-existing UART fault backoff/reinitialization remains independent from this state machine.

## 4. Logging Strategy

`radar_log_manager` is a separate low-priority task. It receives a copied, latest-only spatial snapshot and formats logs outside `radar_local`.

| Log | IDLE | DETECTING | TRACKING | FAST_MOVING |
| --- | ---: | ---: | ---: | ---: |
| `RADAR_STATE` | 5 s | 1 s | 1 s | 1 s |
| `RADAR_TRACK` | off | 5 Hz | 10 Hz | 15 Hz |
| `RADAR_TRACKER` | 1 s | 1 s | 1 s | 1 s |

- `RADAR_STATE` includes mode, active count, selected target, coordinates, velocity, and `valid_age_ms`. A zero or future last-valid timestamp prints `valid_age_ms=unknown`, avoiding unsigned-underflow values.
- `RADAR_TRACK` contains `Txxx`, raw and filtered coordinates, velocity, match distance, and confidence.
- `RADAR_TRACKER` contains active, created, matched, deleted, stale, tentative, and velocity-outlier counters.
- `RADAR_STACK` periodically prints `radar_local` free words.
- A single pending log snapshot is retained. If a newer snapshot replaces it before the logger consumes it, `log_drop_count` increments and is emitted as `RADAR_LOG_DROP` at most once per second.
- Raw UART dumps are disabled by default. They are built only when `CONFIG_RADAR_RAW_DEBUG=1`, bounded to 256 bytes and one dump per second, and start with `RADAR_RAW_FRAME`.

Large spatial and diagnostic snapshots are static or heap-backed; the processing task has no large local snapshot/JSON/log buffers.

## 5. Tracker Parameters

- Reject invalid raw targets: zero resolution, zero confidence, `(0, 0)`, or coordinates outside the configured room bounds.
- Euclidean association gate: 1000 mm.
- New target: tentative on first observation; confirmed on the second observation only after confidence reaches 60.
- Initial confidence: 40. Matched normal observation: +20.
- Lifecycle: confirmed visible, stale after 800 ms missing, delete after 3000 ms missing.
- EMA alpha: 0.60.
- Velocity greater than 3 m/s: first two consecutive samples remain associated with confidence penalty; the third is rejected until a normal association resets the outlier streak.

The Dashboard path remains unchanged: it uses only `current_targets`, which contains active, confirmed, visible tracks. Tentative and stale tracks remain internal and are not added to the HTTP JSON payload.

## 6. Test Results

| Test | Result | Evidence |
| --- | --- | --- |
| LD2450 parser/core host tests | PASS | `sh ESPS3/components/radar_ld2450/tests/run_host_tests.sh` |
| Radar domain host tests | PASS | `sh ESPS3/components/Middlewares/radar_domain/tests/run_host_tests.sh` |
| Adaptive rate state transitions | PASS | IDLE, DETECTING, TRACKING, FAST_MOVING, LOST and 3-second protection are asserted in domain host tests |
| Five-minute simulated single-person ID stability | PASS | existing host test keeps `T001`, verifies no new ID and uses updated 800/3000 ms lifecycle |
| Simulated 3-second occlusion and velocity outliers | PASS | same ID resumes during protection; first two >3 m/s samples are scored down and the third is rejected |
| `idf.py -C ESPS3 build` | PASS | ESP-IDF 5.5.4; firmware linked and binary generated with no build warnings |
| 30-minute empty room | requires hardware verification | not run; no flash or monitor was authorized |
| 5-minute live single-person movement | requires hardware verification | not run; verify T001 remains stable |
| fast movement and 3-second occlusion | requires hardware verification | not run; verify track continuity and T001 recovery |

## 7. Resource Impact Analysis

- UART RX continues independent of processing cadence. The bounded pending buffer is 1024 bytes; overflow is counted in service diagnostics rather than blocking UART reception.
- Parser work moves from the UART reader to the adaptive processing task. At most one latest frame is published, so low-rate modes do not create a frame backlog.
- `radar_local` remains configured with an 8192-byte stack. Workspace objects are allocated once on heap/PSRAM; `RADAR_STACK` reports runtime headroom.
- Logging is latest-only and rate-limited before formatting. Normal operation therefore has no high-frequency serial printing in `radar_local`.
- The build validates compile/link integration only. Runtime CPU utilization, UART output throughput, stack headroom, and human-motion continuity still need the listed hardware tests.

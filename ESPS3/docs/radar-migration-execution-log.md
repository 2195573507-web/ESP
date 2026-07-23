# LD2450 migration execution log

Date: 2026-07-16

## Completion status

Software implementation: COMPLETE

Hardware acceptance: BLOCKED pending confirmed UART wiring, three radars, and real-device execution of HW-P1 through HW-P4.

## 2026-07-21 follow-up index

- [S3 LD2450 UART internal-memory recovery fix](development_logs/s3_radar_uart_internal_memory_fix.md): source-level repair log for bounded UART recovery, internal-memory probes, HTTP request delta probes, and PSRAM placement context. Its `idf.py build` result and hardware acceptance status are recorded independently in that log.
- [S3 DMA/network/alarm stability repair](development_logs/s3_dma_network_alarm_stability_fix.md): source-level record for capability-specific memory admission, asynchronous and coalesced external HTTP, network-worker watchdog responsiveness, environment-alarm stack remediation, and low-cost local-radar idle behavior. Build and hardware acceptance are tracked separately in that record.
- [S3 HTTP backoff/dedup follow-up](development_logs/s3_dma_network_alarm_stability_fix.md): 2026-07-22 endpoint-level monotonic overview backoff, pending/deferred/active deduplication, structured transition/completion logs, and HTTP/TLS lifecycle audit. Build-only evidence; hardware and end-to-end acceptance remain unverified.

## Implemented phases

- C5-P0/S3-P0: live-source, dirty-worktree, build, and resource baseline.
- C5-P1: removed active capture, feature, edge, runtime, resource, protocol, and CMake paths.
- C5-P2/P3: added fixed 30-byte parser, special directional decoding, UART/config transaction, presence state machine, and non-critical runtime service to C51/C52.
- C5-P4: added fixed-size latest-only `POST /local/v1/radar/state` reporting with voice/network gate.
- C5-P5: verified C51/C52 public radar source parity.
- S3-P1: removed trigger, result endpoint, protocol branch, latest cache, fusion worker/queue/lock/flush, upload, and gateway config.
- S3-P2/P3: added strict radar JSON v1 parser, three-slot registry, remote identity/session admission, and local HTTP handler.
- S3-P4: connected S3 local `radar_ld2450` service directly to `RADAR_SOURCE_S3_LOCAL` without HTTP.
- S3-P5: added independent freshness-to-UNKNOWN and 10-second bounded diagnostics.

## Added source

### C51 and C52

- `components/radar_ld2450/`: parser, presence state machine, UART adapter, configuration transaction, service, fixed-size codec, board config, and host tests.
- `components/Middlewares/sensor_domain/radar/radar_state_client.[ch]`.
- Independent `radar_board_config.h`: C51 local ID 1, C52 local ID 2.

### S3

- `components/radar_ld2450/`: same parser/UART/state core with S3-local board config.
- `components/Middlewares/radar_domain/`: protocol, registry, remote ingest, local adapter, diagnostics, and host tests.
- `/local/v1/radar/state` route and error constant.
- Registry mappings: C51=`living_room`, C52=`bedroom`, S3_LOCAL=`s3_local`.

## Deleted active source

### C51 and C52

- `components/Middlewares/sensor_domain/csi_phase_a/`
- `components/Middlewares/sensor_domain/csi_edge_detector/`
- `components/Middlewares/sensor_domain/csi_placeholder/`
- `components/Middlewares/device_protocol/envelope_builder.[ch]`

Removed shared references from CMake, orchestrator, runtime workers, event bus, backpressure/resource management, protocol constants, and WiFi configuration.

### S3

- `components/Middlewares/csi_placeholder_gateway/`
- `components/Middlewares/csi_fusion/`

Removed shared references from CMake, orchestrator, local HTTP, protocol adapter, resource manager, event bus, scheduler, sensor aggregator, network worker, server client, device stream gateway, child registry comments, and gateway configuration.

## Radar protocol and admission

- Body limit: 768 bytes.
- Exact top-level and target key sets; structured parsing through cJSON.
- Allowed local IDs: 1 and 2; allowed states: `unknown`, `vacant_inferred`, `hold`, `motion`.
- Target count and array length must match; all integer fields are range checked.
- Remote admission requires registered and online child, fixed local-ID/device mapping, matching peer IP, live resource session, and current session generation.
- Radar ingress does not call child touch/confirm, update peer identity, or restore resources.
- Same sequence/same normalized payload is a 202 no-op; same sequence/different payload and non-reboot rollback return 409.
- Clear uptime rollback permits a new report sequence without permanently rejecting a rebooted C5.

## Tests

| Test | Result |
|---|---|
| C51 parser/presence/codec host suite | PASS |
| C52 parser/presence/codec host suite | PASS |
| S3 parser/presence/codec host suite | PASS |
| S3 protocol/registry/ingest host suite | PASS |
| C51/C52 radar core and client parity | PASS |
| `git diff --check -- ESPC51 ESPC52 ESPS3` | PASS |

The S3 ingest suite covers C51/C52 isolation, forged local ID, unregistered and mismatched peer identity, oversized body, duplicate/conflicting/backward sequence, uptime rollback, invalid-body freshness behavior, remote-source expiry, and direct S3-local update.

## Negative scans

Active firmware source was scanned with build, managed component, documentation, and test directories excluded.

- Required legacy-term list: PASS, zero active matches.
- General case-insensitive CSI symbol scan: PASS, zero active matches.
- No active route or call for `/local/v1/csi/result*` or `/kernel/csi_event`.
- No active CSI-only source is present in CMake.
- The core codec host tests retain only a negative assertion that generated radar JSON does not contain the legacy label.
- `CONFIG_SOC_WIFI_CSI_SUPPORT` remains in generated config as an immutable SoC capability flag. C51/C52 runtime enable options are explicitly not set.
- C5 `/api/` scan: zero matches. Full-URL scan only finds construction of `http://<S3 local gateway IP>` and guards that reject caller-supplied complete URLs.

## Final builds and resources

Final build directory: `build-radar-final-20260716/` in each firmware root.

| Firmware | Build | Bin delta | Map image delta | Internal RAM delta | Flash/data delta |
|---|---|---:|---:|---:|---:|
| C51 | PASS | -5,696 | -5,700 | HP SRAM -4,002 | Flash -5,834 |
| C52 | PASS | -5,696 | -5,708 | HP SRAM -4,002 | Flash -5,842 |
| S3 | PASS | -23,344 | -23,348 | DIRAM -1,208 | Code -20,924; data -2,840 |

Final absolute values:

| Firmware | Bin bytes | Map image bytes | Primary internal RAM | Flash/data |
|---|---:|---:|---:|---:|
| C51 | 1,371,280 | 1,371,166 | HP SRAM 192,878 | Flash 1,242,848 |
| C52 | 1,371,280 | 1,371,166 | HP SRAM 192,878 | Flash 1,242,848 |
| S3 | 1,109,296 | 1,109,181 | DIRAM 174,167 | Code 819,866 + data 169,740 |

No new compiler warning remained after the final C51/C52 Kconfig cleanup.

## Scope audit

- `ESP-server/`: pre-existing dirty files remain exactly outside this task's edit actions; no task edit or Server start occurred.
- `managed_components/`: no task diff.
- dependency lock files: no task diff.
- Dashboard/public/database: no task edit.
- Original `ESP-111`: not accessed for writes.

## Final closeout recheck

- Re-ran all four host suites after the final reports: C51 core, C52 core, S3 core, and S3 protocol/registry/ingest all PASS.
- Re-ran `git diff --check -- ESPC51 ESPC52 ESPS3`: PASS.
- Re-ran the required legacy-term and general CSI scans with build, managed component, documentation, planning, and test paths excluded: zero active matches.
- Re-ran the exact C51/C52 `CONFIG_ESP_WIFI_CSI_ENABLED=y` scan: zero matches. The two `sdkconfig.defaults` entries are explicit `not set` directives, not an active CSI runtime path.
- Re-ran the C5 `/api/` scan: zero matches. Remaining full-URL strings are limited to local S3 gateway construction, complete-URL rejection guards, and unchanged dependency registry metadata.
- Re-ran C51/C52 radar core/client parity: PASS.
- Final build dry-runs scheduled no application source compile or link step, so the recorded final application binaries reflect the current source tree.
- Host test runners created four temporary binaries; all four were removed with `unlink` after the PASS results were recorded.
- `managed_components`, dependency lock files, `ESP-server/public`, and `ESP-server/db` remained unchanged by this task.

## Execution restrictions observed

- No flash, monitor, erase-flash, fullclean, Server start, database access, or hardware pass claim.
- Existing dirty worktree changes were not reverted.
- One attempted generated-output cleanup using `rm` was rejected by environment policy; test binaries were removed with `unlink`, and the intermediate S3 build directory was retained as generated evidence.

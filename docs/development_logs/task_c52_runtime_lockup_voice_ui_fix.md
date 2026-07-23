# C52 Runtime Lockup, VAD Voice, and Compact UI Contract Fix

**Date:** 2026-07-21  
**Status:** Initial audit recorded; implementation, source-diff attribution, and builds pending  
**Primary target:** `ESPC52` (ESP32-C5, N16R8)  
**Comparison target:** `ESPC51` (ESP32-C5, N16R8)  
**Excluded execution:** Flashing, serial monitoring, device/runtime acceptance,
network/end-to-end acceptance, and hardware validation.

## Objective

Investigate the reported C52 reset sequence without prematurely attributing it
to BME690, then make the minimum evidence-backed changes required to:

1. eliminate confirmed CPU-lockup/watchdog risks without disabling the
   watchdog or simply increasing its timeout;
2. enforce C5 local-VAD gating so no PCM is sent before a local `VOICE_START`;
3. make server-voice preparation idempotent for the same C52 owner while
   retaining explicit rejection for a different owner/session; and
4. make C52 publish the compact LCD/UI snapshot contract accepted by the C51
   reference consumer.

This record is maintained independently of the implementation. It separates
input logs and source observations from conclusions that require a final source
diff, successful target build, or hardware evidence.

## Audit Scope

The initial source search covers these C52 paths and their C51 peers:

| Area | Initial paths observed | Audit status |
| --- | --- | --- |
| Startup and voice ownership | `components/Middlewares/app_orchestrator/app_orchestrator.c`, `components/Middlewares/voice_domain/voice_chain.c` | Detailed lock/priority review pending |
| VAD and PCM gate | `components/Middlewares/mic/mic_adc_test.[ch]`, `mic_vad.[ch]`, `components/Middlewares/server_voice/server_voice_client.c` | State-machine review pending |
| PCM transport | `components/Middlewares/voice_transport/c5_audio_transport.c` | Ownership/start-stop review pending |
| BME690 | `components/Middlewares/sensor_domain/bme690/driver/bme690.c` and service/client paths | Post-init callers and bus ownership review pending |
| LCD/UI snapshot | `components/Middlewares/radar_domain/radar_home_snapshot_client.[ch]`, `components/lcd_ui/` | C51/C52 field/version/size comparison pending |
| Radar BLE scheduling | `components/Middlewares/radar_ble/`, `components/Middlewares/radar_domain/` | Exclusion boundary recorded; workload/log-rate review pending |
| Crash diagnostics | `sdkconfig.defaults`, debug/stack-monitor configuration | Config audit pending |

The source snapshot contains the relevant log strings in both C5 peer trees,
including `VOICE_LISTENING`, `VOICE_START detected`, the BME690 initialization
success log, and the compact UI mismatch log. This establishes candidate code
locations only; it does not identify a causal execution path from the reported
device reset.

## Raw Reported Runtime Evidence

The following is input evidence supplied for this task. It has not been
reproduced, flashed, or correlated with an ELF/backtrace in this documentation
pass:

```text
I (...) mic_adc_test: VOICE_START detected, request voice exclusive before server_voice PCM
W (...) voice_chain: server voice recording rejected: state=VOICE_LISTENING
W (...) mic_adc_test: server_voice prepare failed before PCM, stay VAD-only: ESP_ERR_INVALID_STATE
W (...) radar_home_ui: snapshot rejected: compact UI contract mismatch
I (...) BME690: BME690 初始化成功
ESP-ROM:esp32c5-eco2-20250121
rst:0x1a (CPU_LOCKUP)
Core0 Saved PC:0x40816aa2
0x40816aa2: panic_abort
随后再次复位：
rst:0x8 (TG1_WDT_HPSYS)
W (...) boot.esp32c5: PRO CPU has been reset by WDT.
```

The supplied radar-related lines are:

```text
RADAR_SCAN_START
RADAR_DEVICE_FOUND
RADAR_RECONNECT
```

## Preliminary Analysis and Decision Boundaries

### CPU lockup and watchdog

`CPU_LOCKUP` followed by `TG1_WDT_HPSYS` is a reset symptom, not a root-cause
classification. `panic_abort` at the saved PC is insufficient without the
exception cause, register dump, resolved backtrace, current task, and an exact
firmware ELF. BME690 initialization success only establishes ordering in the
reported log; it is not evidence that BME690 caused the reset.

Pending review must inspect post-BME690 synchronous calls, spawned tasks,
callbacks and timers; unbounded loops; task delay/yield behavior; critical
section pairing and duration; mutex/semaphore release and ordering; ISR-safe
API use; idle-task starvation; stack high-water marks; heap capability and DMA
allocation; and snapshot/queue copy sizes. Any confirmed fix must retain the
watchdog and use ordinary task context for long I2C, SPI, LCD, BLE, allocation,
or logging work.

### Voice state machine

The requested C52 semantic state machine is:

```text
VAD_ONLY -> VOICE_PREPARING -> PCM_STREAMING -> VOICE_STOPPING -> VAD_ONLY
```

Only a local VAD `VOICE_START` may enter preparation. `VOICE_LISTENING` must
be evaluated with owner, source ID, and session/generation: when these identify
the current C52 preparation, it should be an idempotent already-prepared
outcome; when they identify another owner/session, it remains an explicit busy
or owner-conflict outcome. A blanket conversion of `ESP_ERR_INVALID_STATE` to
success is not an acceptable fix.

The final implementation evidence must show rollback of resources created by a
failed prepare/start; no duplicate task, queue, socket, or PCM buffer for a
repeated start; tail handling and ownership release on `USER_SPEECH_END`; and
state-transition logs that include previous state, next state, owner, source,
session/generation, and operation result.

### LCD/UI compact snapshot

The reported `compact UI contract mismatch` is a consumer rejection, but this
pass has not yet completed the C51/C52 struct/version/producer/adapter/queue
comparison. The target behavior is a C52 compact snapshot that the current C51
consumer accepts, preferably through a shared builder/adapter. Consumer-side
contract removal or acceptance of arbitrary structure sizes is out of scope.

The pending comparison includes contract version, compact/full mode, source and
local/device IDs, room, sequence, person count/count state, radar status,
environment fields, timestamp, valid flags, string termination, zero
initialization, copy length, queue item size, `sizeof` use, component
dependencies, Kconfig, and C51/C52 compile-macro differences. Mismatch logging
must preserve the first diagnostic with expected/actual version and size plus
source/local identifiers, then rate-limit repeats.

## Radar Exclusion and Expected Offline Behavior

The LD2450 radar module is not powered. Consequently scan failure, target
absence, reconnect attempts, and exponential backoff are expected operating
conditions and are not a presumed cause of the C52 reset. This task must not:

- relax target identity matching;
- connect to arbitrary BLE devices;
- remove radar reconnect/backoff; or
- disable radar merely to make the failure disappear.

The implementation review may still limit per-device irrelevant BLE logs and
ensure scanning/backoff/callback paths do not block BME690, LCD, VAD, Wi-Fi, or
other normal tasks. The expected post-flash offline signal is low-frequency
state activity such as `RADAR_SCAN_START`, `RADAR_RECONNECT`, and
`RADAR_SOURCE_STATE state=backoff`, not an information-level line for each
unrelated advertisement.

## Known Dirty Worktree Boundary

At record creation, repository root is `/Users/zhiqin/ESP 部分开发 1`, branch
is `half`, and `HEAD` is `6c369e6` (`Publish complete workspace on half`).
`git status --porcelain` reported 935 entries: 132 modified, 787 deleted, and
39 untracked. The status includes C51, C52, S3, shared-component, build-output,
and documentation paths. It predates this documentation update and must not be
treated as this task's change list.

No existing worktree file has been reverted, cleaned, staged, committed, or
otherwise changed by this documentation pass. The two files under
`docs/development_logs/` added/updated by this record are documentation-only.
Final source-change attribution requires the primary implementation Agent to
identify its edits against this dirty baseline.

## Modification Ledger

| Category | Current result |
| --- | --- |
| Source/config/build changes by Documentation Agent | None |
| Documentation changes by Documentation Agent | This record and the development-log index entry |
| CPU lockup remediation | Pending implementation audit |
| Voice state-machine remediation | Pending implementation audit |
| UI contract synchronization | Pending C51/C52 comparison and implementation audit |
| Radar behavior change | None recorded |

## Build Ledger

No build command was run in this documentation pass. Final verification is
restricted to the project-recognized build process after the primary Agent has
reviewed and integrated its changes. The final ledger must record the exact
commands, selected target/configuration, result, warnings/errors, and whether
C51 and/or S3 were rebuilt because shared code changed.

| Target | Command | Result |
| --- | --- | --- |
| C52 | Pending final implementation | Not run by this record |
| C51 | Required if shared C5 code changes | Pending determination |
| S3 | Required if shared PCM protocol/component changes | Pending determination |

## Unvalidated Items and Post-flash Observation

No firmware has been flashed. No hardware, serial, runtime, microphone/VAD,
BLE/radar, BME690, LCD, Wi-Fi, C5-to-S3 PCM, or end-to-end validation has been
performed or claimed here.

After a successful build and a separately authorized flash, collect a resolved
backtrace and task context for any exception. Confirm normal voice transitions
semantically include:

```text
VOICE_STATE previous=VAD_ONLY next=VOICE_PREPARING
VOICE_OWNER_ACQUIRED source=C52
PCM_STREAM_START source=C52
USER_SPEECH_END
PCM_STREAM_STOP source=C52
VOICE_OWNER_RELEASED source=C52
```

The three supplied failure lines should no longer recur on the normal same-C52
path, while actual cross-owner conflicts must remain observable. Monitor heap
capabilities/largest blocks, stack high-water marks, task watchdog diagnostics,
LCD contract fields, and radar-offline log rate before declaring runtime
acceptance.

## Next Documentation Update

Update this record after each major implementation stage with: confirmed versus
rejected root-cause candidates; exact final modified files and design choices;
state and ownership transition evidence; concurrency/memory risk assessment;
and the isolated final build results. Do not convert build success into flash,
hardware, runtime, or end-to-end acceptance.

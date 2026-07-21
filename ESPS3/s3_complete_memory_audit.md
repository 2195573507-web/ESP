# ESPS3 Complete Startup Memory Audit

Date: 2026-07-20

## Executive Result

The reported CPU0 Interrupt WDT was caused by the synchronous
`heap_caps_check_integrity_all(true)` scan exceeding the old interrupt-WDT
window while comprehensive poisoning checked the 16 MiB PSRAM heap. It was not
reproduced as an application heap overwrite. `0xfefefefe` and `0xcececece`
are poisoning fill patterns and, by themselves, do not prove heap metadata
corruption or use-after-free. The final image keeps comprehensive poisoning,
all staged integrity checks, stack monitors, diagnostics, and every requested
module, while using a 5000 ms IWDT window.

## Scope and Startup Chain

Audited active sources under `ESPS3`; archived projects and generated build
directories are not part of this result. The active startup path is:

```text
app_main
  -> xTaskCreateWithCaps(gateway_startup_task, internal stack)
  -> gateway_orchestrator_start
     -> gateway_config, resource_manager, s3_scheduler_init
     -> network_worker (worker/upload/command/snapshot queues and tasks)
     -> WiFi APSTA event loop
     -> local HTTP enable
     -> radar registry -> radar ingest -> radar local -> radar RX/log tasks
     -> habit rule -> habit event reporter
     -> BME cache -> command router -> sensor aggregator
     -> radar diagnostics -> environment alarm -> scheduler workers
     -> device stream/replay -> voice proxy
```

Radar, BLE/C5 ingress, BME690, habit rules, command, HTTP, network workers,
stack monitoring, and diagnostics all remain enabled. `gateway_startup_task`
waits on a task notification until `after_task_create` is checked, so the
other-core task cannot race the first startup boundary.

## Additional Safety Repairs

The following are independently valuable memory-safety repairs found during
the full scan; they are not claimed as the primary cause of the original IWDT.

### 1. `radar_diagnostics.c`: mismatched log varargs

`log_summary()` passed `frame_fresh` between `uart_online` and the first
`uint64_t` age argument, but its `ESP_LOGI` format string had no conversion for
that value. Therefore the formatter read the following 64-bit fields from the
wrong variadic positions. On Xtensa this was undefined behavior in a hot
startup diagnostic task and could corrupt diagnostic state; the board evidence
does not identify it as the original IWDT cause.

The unused argument was removed. The registry and accepted-target counts are
now clamped before every fixed-array traversal.

### 2. `radar_service.c`: raw debug string and pending RX bounds

The bounded hexadecimal diagnostic buffer was sent to `%s` without an explicit
terminator. Its producer now terminates the string before logging. The UART
pending length is validated before `capacity - length`, and the parser copy is
bounded by its destination. A damaged counter can no longer turn into an
underflow or oversized `memcpy`.

### 3. Fixed-capacity radar counts were trusted across async snapshots

`radar_diagnostics.c`, `radar_home_snapshot.c`, `radar_log_manager.c`,
`radar_local_adapter.c`, and `habit_rule_adapter.c` now clamp source, room,
track, and target counts to the matching array capacity before indexing. This
prevents a damaged producer count from causing a second out-of-bounds access in
diagnostics, HTTP snapshots, or habit-rule selection.

`radar_spatial_state.c` also bounds writes to the local accepted-target array;
extra targets are counted as dropped instead of incrementing past
`LD2450_MAX_TARGETS`.

### 4. Task storage and ownership

The static `radar_diag` task uses file-static `StaticTask_t` and
`StackType_t[]` storage. A compile-time assertion verifies that the static
stack matches the configured byte count. Its PSRAM snapshot is freed only after
task exit is acknowledged and the task is deleted. Dynamic task parameters are
either `NULL` or process-lifetime/PSRAM-owned objects; notably `radar_local`
owns its workspace until its task stops.

Queue item sizes match their queued struct types. Network upload/command items
transfer ownership of `cJSON_malloc` buffers to their worker, which releases
them with `cJSON_free`; audit found no additional double-free or stack-address
queue payload in the active startup path.

### 5. `habit_event_reporter.c`: reporter stack exhaustion

The 3072-byte reporter task placed a 768-byte JSON buffer and a
`SERVER_CLIENT_SMALL_BODY_BYTES` (2048-byte) response buffer on its stack.
Those 2816 bytes left no safe room for call frames, logging, and HTTP client
work. They are now a PSRAM `habit_event_reporter_workspace_t`, allocated before
task creation, passed as the owned task parameter, and released on create
failure. The long-lived reporter retains the workspace for its task lifetime.

### 6. HTTP handler large local buffers

The 8 KiB HTTP server task handled a 4096-byte radar-debug response and a
2048-byte pending-command response on its stack. Both responses now allocate
PSRAM buffers for the synchronous send path and release them on every success
and error return. The radar debug handler additionally clamps local/remote
track counts before reading their fixed arrays.

## Heap and Stack Diagnostics

`heap_caps_check_integrity_all(true)` remains enabled through
`app_heap_integrity_check()`. Startup milestones log the first failure boundary:

```text
app_main_enter
after_task_create
gateway_enter / after_gateway
after_network_worker
after_wifi
after_local_http
after_radar
after_bme
```

The additional `after_network_worker` and `after_local_http` phases remain
available to narrow a failed milestone. Stack monitor and periodic heap
monitoring remain enabled.

## Definitive IWDT Root Cause

Before the repair, `CONFIG_ESP_INT_WDT_TIMEOUT_MS` used the ESP-IDF default
300 ms. Comprehensive poisoning verifies every free block, including 16 MiB of
PSRAM, inside the synchronous `app_heap_integrity_check()` call. CPU0 could
not service the interrupt WDT during that scan, producing the panic in
`multi_heap_check()` / `verify_fill_pattern()` before application tasks had
started. The repair is the configuration change below, not removal or
throttling of the integrity check:

```ini
CONFIG_ESP_INT_WDT_TIMEOUT_MS=5000
CONFIG_HEAP_POISONING_COMPREHENSIVE=y
CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y
CONFIG_COMPILER_STACK_CHECK_MODE_ALL=y
```

The final serial order is `app_main_enter`, `after_task_create`,
`gateway_enter`, and `after_gateway`; the task-notification barrier makes this
ordering deterministic on both cores.

`sdkconfig` and `sdkconfig.defaults` now select:

```text
CONFIG_HEAP_POISONING_COMPREHENSIVE=y
CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y
CONFIG_COMPILER_STACK_CHECK_MODE_ALL=y
```

ESP-IDF 5.5.4 on this target defines `StackType_t` as byte-sized, so active
`xTaskCreate*` stack-depth arguments and the static diagnostics stack are byte
counts. Cache-off startup/WiFi/UART/Radar RX stacks stay in internal 8-bit RAM;
large non-cache-critical worker stacks, history/workspaces, and queue payloads
use PSRAM where required.

## Modified Files

- `main/main.c`, `components/app_config/app_stack_monitor.h`
- `components/Middlewares/gateway_orchestrator/gateway_orchestrator.c`
- `components/Middlewares/gateway_wifi/gateway_wifi.c`
- `components/radar_ld2450/radar_service.c`
- `components/Middlewares/radar_domain/radar_diagnostics.c`
- `components/Middlewares/radar_domain/radar_home_snapshot.c`
- `components/Middlewares/radar_domain/radar_local_adapter.c`
- `components/Middlewares/radar_domain/radar_log_manager.c`
- `components/Middlewares/radar_domain/radar_spatial_state.c`
- `components/Middlewares/habit_rule_adapter.c`
- `components/habit_rule_engine/habit_rule_engine.c`
- `components/Middlewares/habit_event_reporter.c`
- `components/Middlewares/local_http_server/radar_local_handler.c`
- `components/Middlewares/local_http_server/local_http_server.c`
- `components/Middlewares/environment_alarm_reporter/environment_alarm_reporter.c`
- `components/Middlewares/network_worker/network_worker.c`
- `components/Middlewares/network_replay_worker/network_replay_worker.c`
- `components/Middlewares/runtime/s3_scheduler.c`
- `components/Middlewares/device_stream_gateway/device_stream_gateway.c`
- `components/Middlewares/voice_proxy/voice_proxy.c`
- `sdkconfig`, `sdkconfig.defaults`
- `tools/startup_reset_audit.py`

## RAM and Image Use

The worktree already contained uncommitted memory-safety changes when this
audit began, and no matching pre-audit ELF or map file exists. A numeric
pre-audit value would be invented, so the table explicitly marks that baseline
as unavailable.

The final `idf.py size` measurement is:

| Metric | Pre-audit | Final value |
| --- | ---: | ---: |
| DIRAM used | unavailable | 278759 / 341760 bytes (81.57%) |
| Static `.bss` | unavailable | 147704 bytes |
| Static `.data` | unavailable | 22416 bytes |
| IRAM used | unavailable | 16384 / 16384 bytes |
| Final image size | unavailable | 1399459 bytes |
| App binary | unavailable | `0x155b10` bytes |
| Smallest app partition free | unavailable | `0x5aa4f0` bytes (81%) |

Large radar history/workspace and network queue allocations remain PSRAM-backed,
so the repairs do not add a permanent large internal-RAM buffer. Final board
startup measured 143299 internal bytes before network-worker allocation and
21183 bytes after all workers, with a 12788-byte largest internal block.

## Verification

Passed on host with ESP-IDF 5.5.4:

```text
bash components/Middlewares/radar_domain/tests/run_host_tests.sh
bash components/radar_ld2450/tests/run_host_tests.sh
bash components/habit_rule_engine/test/run_host_tests.sh
bash components/environment_alarm_engine/test/run_host_tests.sh
git diff --check
idf.py build
idf.py size
```

The firmware build, static checks, radar domain/LD2450, habit-rule, and BME
alarm host suites pass. These prove source, ABI, buffer-boundary, and image
consistency; they do not prove peripheral or board runtime behavior.

## Device Acceptance

The final image was flashed and hash-verified on ESP32-S3 MAC
`90:e5:b1:cc:ee:40`, port `/dev/cu.usbmodem141101`, with 16 MiB PSRAM. The
reproducible harness `tools/startup_reset_audit.py` completed 30/30 RTS/DTR
hardware resets. Every run reported all six required
`HEAP_INTEGRITY ... intact=1` milestones and `gateway orchestrator startup
complete`; the raw log is `build/startup_reset_audit_final_30.log`. There were
zero `Guru Meditation`, `CORRUPT HEAP`, `HEAP_INTEGRITY_FIRST_FAILURE`, stack
overflow, or stack-canary markers.

Serial output also confirmed APSTA/SoftAP, local HTTP, Radar worker/log/RX/local
and diagnostics, habit rule/reporter, BME cache, command registry/router,
sensor aggregator, environment alarm, scheduler, replay, UDP, and voice
startup. C5/BLE registries and workers started; no peer was attached during
this run, so live peer traffic remains a separate acceptance layer.

These were USB hardware resets, not physical power-loss cold boots. This host
has no controllable USB power switch, so 30 physical cold power cycles remain
unperformed and are not claimed as passed. External STA/server acceptance is
also separate from this local startup proof.

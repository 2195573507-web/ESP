# ESP32-S3 startup memory-corruption audit

## Result

The startup crash was not fixed by disabling `radar_diag`. The active ESPS3
path keeps radar diagnostics, tracking, spatial interpretation, and all log
formats. The repair has three parts:

1. Every task creation site now runs `heap_caps_check_integrity_all(true)`
   immediately before and after the allocation. The common helper emits
   `HEAP_INTEGRITY stage=... intact=0/1`; the first failing stage is the first
   observable heap-corruption boundary.
2. `radar_diag` uses process-lifetime static ownership for
   `StaticTask_t s_task_storage` and `StackType_t s_task_stack[]`. The stack is
   internal BSS, its array size is statically asserted to equal the configured
   byte size, and its PSRAM snapshot is freed only after the task has exited and
   been deleted.
3. Diagnostic/radar count fields and the UART pending ring are revalidated at
   every copy/iteration boundary. The latent `radar_diag` summary varargs
   mismatch (an extra `frame_fresh` argument) was removed.

## Root-cause vectors found

### Diagnostic log argument mismatch

`radar_diagnostics.c::log_summary()` supplied `frame_fresh` without a matching
format conversion. When that formatter was enabled, all subsequent variadic
arguments were shifted and 64-bit values were read with the wrong type. The
formatter is retained for diagnostics but its argument list is now exact.

### Snapshot-count trust

`registry_count`, `accepted_target_count`, and `visible_track_count` came from
mutable runtime snapshots. They are now clamped to their fixed array capacities
before indexing. This prevents a corrupted count from turning a diagnostic read
into a second out-of-bounds access that obscures the original write.

### UART pending-ring invariant

The UART path now rejects a corrupted `s_pending_rx_length` before calculating
`capacity - length`, and clamps the parser copy length to the parser workspace.
It emits `RADAR_MEMORY_CORRUPTION` and resets/clamps the state instead of
turning an invalid length into a large `memcpy`.

## Task audit

ESP-IDF 5.5.4's Xtensa port defines `StackType_t` as `uint8_t`; task stack
arguments are therefore byte counts in this project. Dynamic tasks use
`xTaskCreateWithCaps`/`xTaskCreatePinnedToCoreWithCaps` with internal
`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` stacks. The sole static task uses a
`StaticTask_t` plus a `StackType_t` array with matching depth and lifetime.

| Task | Configured stack bytes | Storage/lifetime | Stop ownership |
|---|---:|---|---|
| gateway_startup | 8192 | internal dynamic | self-delete after orchestrator |
| radar_rx | 4096 | internal dynamic | signals exit, self-delete |
| radar_worker | 4096 | internal dynamic | signals exit, self-delete |
| radar_local | 8192 | internal dynamic; PSRAM workspace owned until exit | stop waits, then frees workspace |
| radar_diag | 6144 | static `StaticTask_t` + static `StackType_t[6144]`; PSRAM snapshot | stop waits, deletes, then frees snapshot |
| radar_log | 6144 | internal dynamic | stop waits for self-delete |
| habit_rule | 4096 | internal dynamic | long-lived runtime task |
| habit_event_reporter | 3072 | internal dynamic | long-lived runtime task |
| environment_alarm_reporter | 3072 | internal dynamic; PSRAM storage/queue | long-lived runtime task |
| protocol/stream/scheduler/network/upload/command/snapshot/voice/replay/UDP | existing configured values | internal dynamic | existing module lifecycle preserved |

All 20 active task creation expressions were checked. No task stack points at a
local variable, and no static task buffer is freed or reused while its task is
alive. Creation failure paths clear handles and retain existing module-degrade
behavior.

## Buffer audit

- LD2450 UART read buffer is 128 bytes; the internal pending/parser workspaces
  are each 1024 bytes and all copies are bounded by their destination.
- Radar spatial arrays are sized from `LD2450_MAX_TARGETS` (3); accepted and
  visible counts are clamped before writes/reads.
- Radar diagnostic snapshots contain owned fixed-size character arrays, not
  string pointers. The task snapshot is PSRAM-backed and has explicit admission
  and teardown checks.
- Radar log pending contexts, environment-alarm queue/storage, and task
  workspaces retain fixed-capacity ownership. Queue and ring lengths are checked
  before indexing or copying.

## Diagnostics enabled

`sdkconfig.defaults` now selects:

- `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y` (FreeRTOS method 2,
  `configCHECK_FOR_STACK_OVERFLOW=2`)
- `CONFIG_HEAP_POISONING_COMPREHENSIVE=y`

`app_heap_integrity_check()` is called before and after every task creation and
after the radar task/workspace teardown paths. These checks identify the first
bad heap boundary; a final Guru Meditation backtrace alone cannot do that.

## Verification

Executed from `ESPS3` with ESP-IDF 5.5.4:

```text
idf.py build
Project build complete.
sensair_s3_gateway.bin binary size 0x127610 bytes.
Smallest app partition is 0x700000 bytes. 0x5d89f0 bytes (84%) free.
```

The build proves compile/link/image consistency only. Flashing and monitoring
the physical ESP32-S3 is still required to capture the first runtime
`HEAP_INTEGRITY` failure (or confirm all startup stages remain intact) and to
prove UART, tracker, spatial, and server behavior on hardware.

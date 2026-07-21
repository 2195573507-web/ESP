# ESP32-C5 Startup Stack Root Cause

Date: 2026-07-20

## Root Cause

`app_startup_task` enters this path:

```text
app_startup_task -> app_orchestrator_start -> wifi_manager_init
    -> nvs_flash_init -> spi_flash_mmap
    -> esp_cache_freeze_caches_disable_interrupts
```

The task stack was allocated with `C5_MEM_PSRAM`, which maps to
`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`. During cache freeze, ESP-IDF verifies
that the current stack pointer is in internal DRAM. A PSRAM-backed current task
stack therefore fails `s_task_stack_is_sane_when_cache_frozen()` before the
NVS/flash operation can proceed.

This is an address-domain failure, not a heap corruption or an exhausted task
stack. The observed `heap_integrity result=ok`, `internal_free=141379`, and
`internal_largest=81920` support that conclusion. The 81,920-byte largest
internal block also leaves ample room for the 16,384-byte startup stack.

## Startup Stack Audit

- `app_startup_task` has no large local arrays or structures.
- Before the Wi-Fi call, `app_orchestrator_start()` keeps only
  `connected_ssid[33]`, a boolean, and scalar return values.
- `wifi_manager_init()` reaches `nvs_flash_init()` before its later Wi-Fi
  configuration object is used. Its frame is not large enough to explain a
  16 KiB task-stack failure.
- `TaskStatus_t[24]` used by `uxTaskGetSystemState()` is static BSS, not a
  startup-task local object. `vTaskList()` is not called. The full-system
  snapshot remains limited to startup lifecycle boundaries.

## Modified Files

- `ESPC51/main/main.c`
- `ESPC52/main/main.c`
- `ESPC51/components/Middlewares/app_orchestrator/app_orchestrator.c`
- `ESPC52/components/Middlewares/app_orchestrator/app_orchestrator.c`

The C51 and C52 copies remain byte-identical for these shared files.

## Stack Changes

| Item | Earlier state | Final state |
| --- | --- | --- |
| Startup stack capacity | 12,288 bytes | 16,384 bytes |
| Static-task depth argument | 12,288 `StackType_t` units (one byte on C5) | 16,384 `StackType_t` units, expressed by an explicit conversion macro |
| Startup stack backing | PSRAM | `C5_MEM_INTERNAL_CONTROL` |
| Buffer lifetime | 12 KiB PSRAM reservation after task deletion | 16 KiB internal-RAM reservation after task deletion |
| Cache-freeze address validation | none | validate first and last stack bytes with `esp_ptr_in_dram()` |
| Wi-Fi boundary observation | aggregate stack monitor | task handle, stack start/end, high-water mark, and internal-DRAM result immediately before `wifi_manager_init()` |

The 16 KiB capacity is retained rather than increased again. This task has
reasonable headroom for the startup call chain; the fix moves its existing
capacity into the memory domain required while flash caches are frozen. The
buffer is caller-owned static-task storage obtained from `c5_mem_alloc()`;
`vTaskDelete()` does not reclaim it, so the 16 KiB internal reservation is
intentional and remains for the firmware lifetime.

## Rationale And Preserved Behavior

`C5_MEM_INTERNAL_CONTROL` maps the static task stack to
`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`, so the currently running startup task
continues to have a valid stack during NVS and flash cache-freeze operations.
The failed allocation and failed address-validation paths free the owned stack
buffer and leave startup disabled with diagnostics, preserving its lifetime
rules.

The existing heap-integrity checks and stack monitor remain enabled. The new
boundary log uses only scalar locals and does not allocate heap memory. Wi-Fi,
NVS, protocol, sensor, voice, and scheduler behavior are otherwise unchanged.

## Verification Scope

- Source inspection confirms the backing allocation is now internal control
  memory and its range is validated before task creation.
- `git diff --check` passes and C51/C52 shared files compare byte-identically.
- No firmware was flashed and no device runtime claim is made here.

Device acceptance still requires booting each C5 target and checking the
`STARTUP_STACK stage=before_wifi_manager_init` record for `internal=true`, then
confirming that NVS initialization completes without the cache-freeze assert.

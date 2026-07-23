# ESP32-C5 Startup Stack Audit

Date: 2026-07-20

## Scope

- `ESPC51/main/main.c`
- `ESPC52/main/main.c`
- `ESPC51/components/Middlewares/app_orchestrator/app_orchestrator.c`
- `ESPC52/components/Middlewares/app_orchestrator/app_orchestrator.c`
- `ESPC51/components/Middlewares/wifi/wifi_manager.c`
- `ESPC52/components/Middlewares/wifi/wifi_manager.c`

## Root Cause

`app_startup_task` used a 12 KiB PSRAM-backed static stack. `wifi_manager_init()` calls NVS and Wi-Fi initialization, whose flash/cache operations can freeze caches. A task running from a PSRAM-backed stack cannot safely remain active during that cache-frozen window, producing the observed `esp_cache_freeze_caches_disable_interrupts` stack-sanity failure.

ESP32-C5 uses the RISC-V FreeRTOS port where `StackType_t` is `uint8_t`. ESP-IDF therefore accepts `xTaskCreateStatic()` stack depth in bytes for this target. The audited task allocation and depth values agree; there is no byte-versus-word multiplier error.

No `char` or `uint8_t` local array of 1024 bytes or more exists in `app_orchestrator_start()` or `app_startup_task`. Its local `connected_ssid[33]`, `wifi_config_t` in `connect_gateway_softap()`, and LCD/radar/BME snapshot values are bounded small structures. No startup JSON buffer is stack-resident in the audited path.

## Changes

- Raised `APP_STARTUP_TASK_STACK` from 12 KiB to 16 KiB for C51 and C52.
- Allocated the `app_startup_task` static stack from `C5_MEM_INTERNAL_CONTROL`, not PSRAM.
- Added startup high-water and `uxTaskGetSystemState()` logs before `app_orchestrator_start()` and before `wifi_manager_init()`; both calls share one 864-byte static status buffer.
- Added `heap_caps_check_integrity_all(true)` through the existing runtime guard before and after `wifi_manager_init()`.
- Enabled FreeRTOS Method 2 canaries, end-of-stack watchpoint, compiler stack checks for all functions, and the FreeRTOS trace facility needed by `uxTaskGetSystemState()`.

## Stack and Memory Change

| Item | Before | After |
| --- | --- | --- |
| `app_startup_task` stack | 12,288 bytes in PSRAM | 16,384 bytes in internal RAM |
| Internal dynamic heap at task creation | unchanged by startup stack | decreases by 16,384 bytes |
| PSRAM dynamic heap at task creation | decreases by 12,288 bytes | unchanged by startup stack |
| Stack-state monitor storage | none | 864 bytes internal static BSS (`TaskStatus_t[24]`) |
| Startup stack observability | high-water only | high-water plus task-state snapshot and heap-integrity checks |

The build verifies source/config integration only. Cache-freeze behavior, observed high-water marks, and disappearance of the panic require a C51/C52 flash-and-boot acceptance run.

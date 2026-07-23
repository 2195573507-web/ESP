# Development Log

## 2026-07-23 - C52 early NVS initialization for startup dependencies

**Status:** source change complete; isolated ESP-IDF build passed; no flash, monitor, or hardware acceptance
**Target:** `ESPC52`

### Problem

The C52 boot orchestrator could enter `app_orchestrator_start()` before NVS had
been explicitly initialized. WiFi manager, NimBLE/system services, and BME690
configuration paths depend on NVS, so their first startup access had no explicit
NVS-ready guarantee and could fail or leave the startup dependency chain
partially initialized.

### Root Cause

`app_startup_task` did not perform an early `nvs_flash_init()` before starting
the orchestrator. NVS recovery for `ESP_ERR_NVS_NO_FREE_PAGES` and
`ESP_ERR_NVS_NEW_VERSION_FOUND` was therefore also absent at this boundary.

### Modified files

- `ESPC52/main/main.c`
- `docs/development_log.md`

### Modification reason

Added `app_startup_init_nvs()` immediately after `app_startup_task` enters and
before `app_orchestrator_start()`. It calls `nvs_flash_init()`, erases and
retries once for the standard no-pages/new-version results, and fails closed
with a log plus task termination if initialization still fails. This preserves
the existing startup-stage design while ensuring WiFi, BLE, system service,
and BME690 users only run after NVS is ready. Radar, voice, and LCD logic and
the existing PSRAM/internal-RAM placement were left unchanged.

### Build result

- ESP-IDF: `5.5.4`
- Command: `idf.py -B build-nvs-init-c52-final build` in `ESPC52` (with the
  project Python environment and `/Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh`)
- Result: **Project build complete**
- App image: `build-nvs-init-c52-final/00_Learn.bin` size `0x23f2d0`; app
  partition free `0x2c0d30` (55%)
- **Not executed:** flash, serial monitor, hardware, or runtime acceptance.

## 2026-07-23 - C5/C51/C52 LCD-first offline-capable startup chain

**Status:** source change complete; C51/C52 isolated ESP-IDF builds passed; no flash, monitor, or hardware acceptance
**Targets:** `ESPC51`, `ESPC52` (mirrored orchestration and runtime policy)

### Modification goal

Move Wi-Fi/Gateway after local sensors and local runtime setup while keeping LCD
bootstrap bounded. BME690, Radar BLE receive/parse/track, and MIC/ADC/VAD now
start before network work; network PCM remains gated at the voice-turn boundary.

### Modified files

- `ESPC51/main/main.c`, `ESPC52/main/main.c`
- `ESPC51/components/Middlewares/app_orchestrator/app_orchestrator.c`, paired C52 file
- `ESPC51/components/Middlewares/runtime/c5_backpressure_controller.c`, paired C52 file
- `ESPC51/components/Middlewares/wifi/wifi_manager.c`, `.h`, paired C52 files
- `ESPC51/components/Middlewares/sensor_domain/bme690/service/bme_sensor_service.c`, paired C52 file
- `ESPC51/components/Middlewares/radar_ble/include/radar_ble_binding_config.h`, paired C52 file
- `ESPC51/components/Middlewares/mic/mic_adc_test.c`, paired C52 file

### Reason and architecture change

The continuation task now transitions `LCD_READY -> LOCAL_SENSOR_READY ->
LOCAL_RUNTIME_READY -> NETWORK_START -> NETWORK_READY/CLOUD_READY`, with
`OFFLINE_MODE` on bounded Wi-Fi/Gateway failure. EventBus, worker queues,
handlers, dispatcher, and the BME/system worker tasks are created before Wi-Fi.
This lets BME sampling consume local scheduler events during the bounded network
wait; HTTP upload remains blocked by the existing server communication gate.
Radar BLE binding defaults are explicitly enabled on both terminals. MIC ADC/VAD
startup no longer returns early on unstable Wi-Fi; network PCM callbacks retain
their Gateway/Wi-Fi checks.

### Risk

Build evidence does not prove boot timing, BLE attachment, BME I2C reads, ADC/VAD
capture, Wi-Fi recovery, or PCM playback. C5 local wake remains the existing
compatibility/S3 WakeNet boundary; no new WakeNet model was added. Existing
dirty build artifacts and unrelated worktree edits were preserved.

### Build result

- ESP-IDF: `5.5.4`
- C51 command: `idf.py -B /tmp/codex-final-c51-build build`
- C51 result: **Project build complete** (`1848/1848`); `00_Learn.bin` `0x22b920`,
  57% app partition free.
- C52 command: `idf.py -B /tmp/codex-final-c52-real-build build`
- C52 result: **Project build complete** (`1850/1850`); `00_Learn.bin` `0x23f020`,
  55% app partition free.
- **Not executed:** flash, serial monitor, hardware, or runtime acceptance.

## 2026-07-21 — C52 voice_chain PSRAM stack (internal-fragmentation startup fix)

**Status:** source change + `idf.py build` verification only; flash/hardware acceptance pending  
**Primary target:** `ESPC52`  
**Parity:** mirrored stack placement into `ESPC51`

### Problem

C52 voice startup could fail at `voice_chain` task creation after other long-lived
internal allocations (notably the 12,288 B `mic_adc_test` internal static stack,
dispatcher/control objects, Wi-Fi/lwIP, and remaining control heap churn).

Typical failure shape:

- `c5_mem_alloc(voice_chain_stack, C5_MEM_INTERNAL_CONTROL, 4096)` fails with
  `ESP_ERR_NO_MEM` / `C5_MEM_ADMISSION_FAIL` when `internal_largest < 4096`
  even if total `internal_free` still looks non-zero.
- Result: `VOICE_START_FAIL stage=voice_chain_stack_alloc` and no VAD/PCM
  orchestrator task.

This is fragmentation/contiguous-block pressure on internal DRAM, not a
VAD/PCM protocol bug.

### Root Cause

| Item | Evidence |
| --- | --- |
| Forced internal stack | `voice_chain_start()` allocated `VOICE_CHAIN_TASK_STACK` (4096 B) with `C5_MEM_INTERNAL_CONTROL` then `xTaskCreateStatic` |
| Contiguous admission | `c5_mem_require()` requires both free and largest block >= size |
| TCB already internal | `StaticTask_t s_voice_task_storage` is BSS; TCB does not need heap |
| DMA not on this stack | Mic/speaker DMA buffers stay on their own modules (`C5_MEM_INTERNAL_DMA` / driver-owned) |
| PSRAM stack allowed | `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`, `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y` |

### Why PSRAM stack (not lazy-init path)

`voice_chain_task` is a state/event orchestrator (queue + lease/state machine).
It does not own DMA descriptors and does not itself run flash-cache-off critical
sections. The same project already places `server_voice_rx` and other workers on
PSRAM stacks. Therefore the minimal fix is stack relocation, not voice logic
redesign and not a new lazy-init architecture.

Lazy mic start already exists as degraded mode (`VOICE_CHAIN_READY_DEGRADED`);
that path cannot help when the orchestrator task itself fails to create.

Out of scope by design: LCD, BLE, Radar, BME modules; mic DMA buffers; VAD ->
PCM streaming contract.

### Modified files

- `ESPC52/components/Middlewares/voice_domain/voice_chain.c`
- `ESPC52/components/Middlewares/voice_domain/voice_chain.h` (comment only)
- `ESPC51/components/Middlewares/voice_domain/voice_chain.c` (parity)
- `ESPC51/components/Middlewares/voice_domain/voice_chain.h` (comment only)
- `docs/development_log.md` (this entry)

### Memory change

| Resource | Before | After |
| --- | --- | --- |
| `voice_chain` task stack (4096 B) | `C5_MEM_INTERNAL_CONTROL` heap | `C5_MEM_PSRAM` heap |
| `voice_chain` TCB | internal static (`StaticTask_t`) | unchanged |
| Event queue storage | internal static buffer | unchanged |
| Mic ADC/DMA / speaker DMA | internal/DMA-capable paths | unchanged |
| Steady-state internal DRAM | -4096 B pressure from voice_chain stack | released to other control/DMA use |
| Steady-state PSRAM | +4096 B task stack | `VOICE_TASK_STACK ... source=psram` |

### Task create audit (voice domain only)

| Task | Stack placement | Action |
| --- | --- | --- |
| `voice_chain` | was internal_control; now PSRAM | fixed |
| `mic_adc_test` | internal static (cache-off safety) | left unchanged |
| `server_voice_rx` | already PSRAM | left unchanged |
| Speaker writer / DMA staging | PSRAM stack + internal DMA buffer | left unchanged |

### Build result

- Command: `idf.py -B ../build-c52-voice-chain-psram build` in `ESPC52`
- Result: **Project build complete**
- Log: `build-c52-voice-chain-psram-build.log`
- App image: `build-c52-voice-chain-psram/00_Learn.bin` size `0x227f80` (~2.16 MiB); partition free ~57%
- Compiled object: `voice_domain/voice_chain.c.obj` included in link
- **Not executed:** flash, monitor, or hardware acceptance

### Hardware verification (user)

1. Cold boot C52 and capture serial through `voice_chain_start`.
2. Expect: `MEM_ALLOC_PLAN owner=voice_chain_stack region=psram`,
   `VOICE_TASK_STACK task=voice_chain bytes=4096 source=psram`,
   `TASK_CREATE task=voice_chain`, no `voice_chain_stack_alloc_psram` failure.
3. Confirm Mic VAD starts (or degraded retry owned by voice_chain), then one
   local/LCD wake -> PCM upload -> response path.
4. Watch for stack/cache anomalies only if flash/OTA/NVS code later runs on
   this task (not expected today).
5. Do not treat host `idf.py build` as device acceptance.

## 2026-07-21 — ESP32-C5 ADC continuous failed-init crash containment

**Status:** source repair complete; isolated `idf.py build` passed for `ESPC51` and `ESPC52`; not flashed and no device/runtime acceptance claimed  
**Primary targets:** `ESPC51`, `ESPC52` `mic_adc_test`  
**Scope:** ADC continuous startup, teardown, retry admission, and task-stack placement only

### Confirmed crash root cause

On ESP-IDF v5.5.4, starting ADC continuous while the largest DMA-capable free
block is about 2944 B can make `adc_continuous_new_handle()` return
`ESP_ERR_NO_MEM`. The prior failed-initialization path could then pass a null
handle to `adc_continuous_deinit()`. The driver consequently reached
`adc_apb_periph_free()` with `s_adc_digi_ctrlr_cnt == 0` and aborted. A second
startup attempt could re-enter this inconsistent driver lifecycle and crash.

### Repair boundary

| Area | Required behavior |
| --- | --- |
| DMA admission | Before `adc_continuous_new_handle()`, check `heap_caps_get_largest_free_block(MALLOC_CAP_DMA)`; below 4096 B returns `ESP_ERR_NO_MEM` without entering the ADC driver. |
| Cleanup | Call `adc_continuous_stop()` and `adc_continuous_deinit()` only when `s_adc_handle != NULL`; failed initialization never deinitializes a null handle. |
| Lifecycle | `ADC_IDLE -> ADC_INITING -> ADC_READY`, with every failure entering `ADC_FAILED` and resetting ownership/state before another attempt. Concurrent or repeated starts cannot enter the driver during an incomplete lifecycle. |
| Voice retry | An `ESP_ERR_NO_MEM` result does not trigger an immediate retry. Retry waits for resource recovery, then repeats the DMA-largest-block admission check. |
| Stack / DMA placement | Move the `mic_adc_test` task stack to PSRAM where the configuration permits. ADC DMA buffers remain DMA-capable internal memory; PSRAM is not used for driver DMA storage. |

### Verification boundary

- `idf.py -C ESPC51 -B /tmp/espc51-adc-continuous-build build` passed.
- `idf.py -C ESPC52 -B /tmp/espc52-adc-continuous-build build` passed.
- **Not executed:** flash, monitor, ADC capture, microphone/VAD exercise, retry
  under real fragmentation, or long-duration hardware acceptance.
- A successful build will validate compilation and linkage only; it will not
  establish that the ESP-IDF driver no longer aborts on target hardware.

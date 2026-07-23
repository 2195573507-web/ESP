# C5 Memory Rebalance Report

Date: 2026-07-20

## Scope

- Active C5 targets only: `ESPC51` and `ESPC52`.
- No device was started, flashed, monitored, or reset.
- The LCD admission limits remain `free >= 30720` and `largest >= 20480` for both internal and internal-DMA heaps. The server voice response-stack allocation remains `free >= 8192` and `largest >= 8192` in its selected heap.

## Cause And Allocation Plan

| Startup owner | Internal RAM | PSRAM | Notes |
| --- | ---: | ---: | --- |
| WiFi | WiFi/lwIP plus 3 KiB reconnect stack | - | Vendor-owned WiFi allocation is not migrated. |
| BLE/radar | Controller-owned memory | BLE 2 KiB, worker 4 KiB, upload 3 KiB stacks | Existing PSRAM placement retained. |
| Dispatcher | 12 KiB stack and static control objects | - | Cache/dispatch path unchanged. |
| Voice | chain 4 KiB, Mic/I2S and DMA, speaker DMA staging | response stack 8 KiB, upload/playback buffers | Only `server_voice_rx` stack changed. |
| LCD | LVGL port 4 KiB, legacy SPI DMA 9,600 B bootstrap, LVGL draw DMA 4,800 B steady | touch 2 KiB, bootstrap/event 4/2 KiB, UI arena 96 KiB | DMA buffers remain internal and validated. |
| BME/system | static queue control objects | each deferred worker stack 8 KiB | Existing PSRAM placement retained. |

The observed `server_voice_rx` failure occurred before `xTaskCreateStatic()`: its 8 KiB stack was explicitly allocated from `C5_MEM_INTERNAL_CONTROL`, and a largest internal block of 5,376 B failed the unchanged 8,192 B requirement. The stack now uses the existing external-static-stack configuration and `C5_MEM_PSRAM`.

The LCD failure happens before LCD allocations: 14,635 B free and a 5,376 B largest block cannot pass the unchanged 30,720/20,480 admission. The former 16 KiB `app_startup` allocation was a heap-backed static stack that survived task deletion. `app_startup` is now an internal `xTaskCreateWithCaps()` task and exits through `vTaskDeleteWithCaps()`, which invokes ESP-IDF's WithCaps cleanup path for its stack and TCB. The active `esp_lvgl_port` component now likewise deletes its WithCaps task through the matching API, making its 4 KiB PSRAM stack reclaimable. The existing PSRAM LCD bootstrap retries `ESP_ERR_NO_MEM` without allocating until the real gate passes.

## Modified Files

- `ESPC51/components/Middlewares/memory/c5_memory.c`
- `ESPC52/components/Middlewares/memory/c5_memory.c`
- `ESPC51/components/Middlewares/server_voice/server_voice_client.c`
- `ESPC52/components/Middlewares/server_voice/server_voice_client.c`
- `ESPC51/main/main.c`
- `ESPC52/main/main.c`
- `ESPC51/components/Middlewares/app_orchestrator/app_orchestrator.c`
- `ESPC52/components/Middlewares/app_orchestrator/app_orchestrator.c`
- `ESPC51/components/lcd/lcd_driver.c`
- `ESPC52/components/lcd/lcd_driver.c`
- `ESPC51/components/lcd/lcd_touch.c`
- `ESPC52/components/lcd/lcd_touch.c`
- `ESPC51/components/Middlewares/radar_ble/radar_ble_runtime.c`
- `ESPC52/components/Middlewares/radar_ble/radar_ble_runtime.c`
- `ESPC51/components/Middlewares/radar_domain/radar_worker.c`
- `ESPC52/components/Middlewares/radar_domain/radar_worker.c`
- `ESPC51/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port.c`
- `ESPC52/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port.c`

## RAM Changes

| Allocation | Before | After | Internal delta | PSRAM delta |
| --- | --- | --- | ---: | ---: |
| `app_startup` 16 KiB stack | retained internal static heap allocation | internal dynamic task allocation reclaimed after task exit | +16,384 B after reaping | 0 B |
| `server_voice_rx` 8 KiB stack | internal static stack | PSRAM static stack | +8,192 B when created | -8,192 B |
| LVGL port worker 4 KiB | internal dynamic stack | PSRAM dynamic stack with WithCaps deletion | +4,096 B | -4,096 B |
| LCD touch worker 2 KiB | internal dynamic stack | PSRAM dynamic stack with WithCaps deletion | +2,048 B | -2,048 B |
| LCD SPI/LVGL DMA | internal DMA | internal DMA | unchanged | unchanged |

The startup-stack reclaim is the admission recovery mechanism for LCD. The 8 KiB response-stack migration makes the voice allocation independent of the fragmented internal largest block. All deltas are allocation-plan deltas, not device measurements.

## Admission And Fragmentation

- No threshold was reduced.
- LCD retries only after an allocation-free admission failure, so the retry itself cannot add heap fragmentation.
- The startup stack is retained through WiFi/NVS cache-freeze work and remains internal. It is released only after startup has returned through `vTaskDeleteWithCaps()`.
- The active `esp_lvgl_port` source now exits its WithCaps worker through `vTaskDeleteWithCaps()`. The LVGL worker, LCD UI data, service context, bootstrap/event stacks, and touch stack are therefore in PSRAM without an unmatched task deletion.
- `MEM_ALLOC_PLAN owner=<...> caps=<...> size=<...> region=<...>` is emitted by `c5_memory` for managed allocations and at the direct app-startup, LCD, and radar task allocation boundaries.

## Subsystem Status

| Subsystem | Source-level status | Device acceptance still required |
| --- | --- | --- |
| `server_voice_rx` | Restored: PSRAM static stack preserves its 8 KiB true allocation requirement and existing task/TCB/callback lifecycle. | Confirm the response task never runs across a cache-disabled window. |
| LCD | Recovery path added: startup stack can be reclaimed, and bootstrap retries until unchanged 30 KiB/20 KiB gates pass. DMA remains internal. | Confirm actual free/largest values pass the gate and the panel renders. |
| Radar/BLE | Preserved: PSRAM `radar_ble_rx` stop now uses `vTaskDeleteWithCaps()` so repeated start/stop does not leak its stack. | Confirm BLE scan/LD2450 runtime behavior. |
| BME | Preserved: no BME logic or allocation path changed. | Confirm sensor initialization and reporting. |
| WakeNet/voice exclusivity | Preserved: no lease, generation, Mic, I2S, or WakeNet state-machine logic changed. | Confirm wake-to-server-response cycle under target firmware. |

## Static Verification

- Source inspection confirms `server_voice_rx` uses `C5_MEM_PSRAM` in both targets while `StaticTask_t` remains internal static storage.
- Source inspection confirms all LCD DMA pointer validation remains `esp_ptr_dma_capable()` plus `esp_ptr_internal()`.
- Source inspection confirms C51/C52 counterparts are symmetric for the changed allocation behavior.
- No build was run to honor the source-only constraint; no device result is claimed.

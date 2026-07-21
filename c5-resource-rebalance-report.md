# C5 Resource Rebalance Report

## Cause

The failing C5 sequence retained the cache-sensitive 16 KiB `app_startup`
stack, then started radar/BLE tasks and reached
`c5_event_dispatcher_stack`. The observed post-radar capacity was
`internal_free=1639` and `dma_free=71`, while the dispatcher requires a
12 KiB internal stack. Dispatcher creation therefore failed deterministically.
The old startup flow logged that failure but still called WakeNet model
creation. ESP-SR can allocate an internal queue during that creation, so it
could enter `hufzip_model.c` with a NULL queue before the outer create call
could return an error.

The dominant static internal allocation is unrelated to radar: the previous
C52 map contains LVGL `work_mem_int.0` at `0x10000` bytes (64 KiB). The UI
already registers a 92 KiB PSRAM LVGL pool, making the default 64 KiB builtin
pool redundant after bootstrap.

## Rebalance

- LVGL builtin memory is reduced from 64 KiB to 2 KiB in both active C5
  configurations. The existing PSRAM UI pool remains the main LVGL allocator.
  Expected static internal release after a fresh reconfigure is 63,488 bytes.
- Radar raw ring, history, upload JSON, command JSON, wake spool, microphone
  pre-roll, and speaker ring/scratch remain in PSRAM. The LCD/I2S DMA buffers
  remain internal DMA.
- Task stacks remain internal unless their complete cache-off call chain is
  proven PSRAM-safe. This retains `app_startup`, dispatcher, radar/BLE,
  WiFi, BME/command workers, ADC and I2S stacks in internal RAM. Enabling an
  external-stack API is not proof that a task is safe during cache-disabled
  execution.

## Startup Protection

- `c5_scheduler_is_dispatcher_ready()` is the dispatcher readiness contract.
- The startup resource state machine records `dispatcher_pending`,
  `dispatcher_ready`, `voice_init_started`, and `voice_init_denied`.
- A failed dispatcher emits `C5_RESOURCE_DENIED` and `VOICE_INIT_SKIPPED`;
  WiFi, BME, BLE radar and LCD can continue, but `voice_chain_start()` and
  WakeNet model creation are not called.
- `local_wake_word_init()` independently verifies dispatcher readiness, a
  static queue, a static mutex, the `model` partition, and internal/DMA/PSRAM
  reserve capacity before calling ESP-SR. It emits `C5_RESOURCE_ADMISSION` or
  `C5_RESOURCE_DENIED`; denied admission returns without calling model create.

## Modified Files

- `ESPC51/components/Middlewares/app_orchestrator/app_orchestrator.c`
- `ESPC52/components/Middlewares/app_orchestrator/app_orchestrator.c`
- `ESPC51/components/Middlewares/runtime/c5_backpressure_controller.[ch]`
- `ESPC52/components/Middlewares/runtime/c5_backpressure_controller.[ch]`
- `ESPC51/components/Middlewares/voice_domain/voice_chain.c`
- `ESPC52/components/Middlewares/voice_domain/voice_chain.c`
- `ESPC51/components/Middlewares/wake/local_wake_word.c`
- `ESPC52/components/Middlewares/wake/local_wake_word.c`
- `ESPC51/sdkconfig` and `ESPC52/sdkconfig`
- `ESPC51/sdkconfig.defaults` and `ESPC52/sdkconfig.defaults`

## RAM Change

| Item | Before | After | Effect |
| --- | ---: | ---: | --- |
| LVGL builtin static pool | 65,536 B internal | 2,048 B internal | 63,488 B static internal released |
| LCD UI object pool | 92 KiB PSRAM | 92 KiB PSRAM | unchanged |
| Radar buffers/history/JSON | PSRAM | PSRAM | unchanged |
| DMA buffers | internal DMA | internal DMA | unchanged |
| Critical task stacks | internal | internal | unchanged for cache-off safety |

The exact free-heap delta depends on IDF/BT/LVGL allocation order and must be
read from fresh device logs. It is not inferred from the map reduction.

## Validation

### Host Build And Map

On 2026-07-20, both top-level projects completed serial `fullclean`,
reconfigure, and build with ESP-IDF 5.5.4 and its Python 3.14 environment.

| Target | Build | New `work_mem_int.0` | HP SRAM remaining |
| --- | --- | ---: | ---: |
| ESPC51 | passed | `0x800` (2,048 B) | 84,594 B |
| ESPC52 | passed | `0x800` (2,048 B) | 84,594 B |

The prior C51/C52 startup build maps both recorded `work_mem_int.0=0x10000`.
The fresh maps therefore prove a 63,488 B static-pool reduction on each C5.
C51 flash use was 2,182,518 B; C52 flash use was 2,182,516 B.

### Device Acceptance

No firmware was flashed and no reset-cycle result is claimed. Two Espressif
serial ports were detected, but only `/dev/cu.usbmodem14301` could be reliably
identified as the existing ESPC52 monitor for this workspace, and it was
already exclusively owned by that monitor. No C51-to-port binding was found.
The verification intentionally did not terminate the existing monitor or
guess a board/port mapping.

Remaining device acceptance, once both board bindings are available, is a
20-or-more reboot cycle for each target with logs proving WiFi, BME, BLE radar,
dispatcher, `C5_RESOURCE_ADMISSION`, and voice/WakeNet readiness. A fault
case with unavailable dispatcher must instead show `C5_RESOURCE_DENIED` and
`VOICE_INIT_SKIPPED` with no WakeNet panic. A successful host build or map
inspection is not device acceptance for those runtime paths.

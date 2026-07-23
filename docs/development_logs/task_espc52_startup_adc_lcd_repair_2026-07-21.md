# ESPC52 Startup, LCD and ADC Lifecycle Repair

Date: 2026-07-21

Status: ESPC52 build passed; device and integration acceptance remain pending.

## Scope And Fixed Boundaries

This task repairs the C52 application startup ordering, existing LCD/UI boot
lifecycle, compact radar snapshot parser, ADC Continuous/GDMA ownership, voice
lifecycle, and startup memory staging. It does not modify ESP-IDF, flash a
device, or claim hardware acceptance.

- C5 performs local VAD only.
- C5 sends PCM to ESPS3 only after VAD identifies speech.
- WakeNet remains an ESPS3 responsibility.
- Business producers publish copied LCD snapshots or commands; only the LCD/UI
  timer owns LVGL calls.

## Root Causes Addressed

1. `app_orchestrator_start()` originally created the LCD bootstrap only after
   the synchronous Wi-Fi, gateway, system, BME, radar, scheduler, and voice
   chain. The existing boot animation therefore could not appear while those
   modules initialized.
2. The LCD bootstrap also created deferred BME/system workers before calling
   `lcd_service_start()`, causing unnecessary initial competition with LCD DMA
   and LVGL allocations.
3. `radar_home_snapshot_client` rejected the established compact envelope when
   its `ok` field was numeric `0` or `1`, while the client accepted only JSON
   booleans. This is the source-level cause of `snapshot rejected: compact UI
   contract mismatch`; the parser now accepts both representations while still
   requiring all source and home fields.
4. ADC Continuous had overlapping start/shutdown ownership risks: task creation,
   handle configuration, ready gating, error rollback, and reconnect deinit
   could otherwise interleave. That class of lifecycle violation is consistent
   with driver failures around `gdma_disconnect` and `adc_apb_periph_free`; the
   application now serializes these transitions instead of modifying ESP-IDF.

## Implemented Startup Flow

```text
app_main
  -> internal app_startup task
  -> schedule LCD-only bootstrap on PSRAM stack
  -> wait for its first publish or cleaned-up failed attempt
  -> Wi-Fi / gateway / system service / optional speaker self-test
  -> BME service / radar BLE / dispatcher / C5 VAD voice chain
  -> latch lcd_service_mark_boot_complete()
  -> schedule phase-2 BME and system workers after app_startup exits
```

`lcd_bootstrap` now only initializes the existing LCD service, attaches the
screen command adapter, posts the initial copied snapshot, and starts the LCD
event producer. Its retry path is independent from background services. The
display remains on the existing boot animation during all later initialization.
After the voice branch reports ready, deferred, or failed, the orchestrator
latches `lcd_service_mark_boot_complete()`. `lcd_service` transfers that flag
to the UI timer, and the UI hides the boot page only after the existing minimum
boot duration. No business thread acquires an LVGL lock or invokes an LVGL API.

## LCD/UI And ESPLCD Comparison

The requested `ESPLCD` directory is not present at this workspace root. The
available project comparison is
`docs/主项目_ESPLCD_LCD迁移对比审计_2026-07-20.md`, which records the
`分支项目/ESP` ESPLCD baseline.

| Concern | ESPLCD baseline | ESPC52 migration in this task |
| --- | --- | --- |
| Panel startup | ST7789 on SPI2; legacy 9,600 B DMA line buffer before LVGL registration | Existing `lcd_driver` retains the same staged ownership and releases the legacy buffer after LVGL draw-buffer registration |
| Steady display | LVGL single 4,800 B internal-DMA draw buffer | Existing driver/service retained; no UI redesign or second display pipeline |
| UI lifecycle | Minimal service is a suitable baseline; richer C51 dashboard is not copied wholesale | Existing C52 boot animation and Home UI remain; explicit boot-complete latch makes the transition one-way and UI-task-owned |
| State ingress | UI must not perform networking or sensor I/O | `lcd_service_post_snapshot()` and command posting carry copies; the timer applies copies only |
| Excluded material | CSI, unrelated touch/UI coupling, and alternate voice ownership | Not migrated; C5 VAD/S3 WakeNet boundary remains intact |

## ADC And Voice Lifecycle

- `mic_adc_test_start()` first rejects an unstable Wi-Fi window as a retryable
  `MIC_ADC_ERR_NOT_READY`, before reserving Mic control resources.
- A start/shutdown critical-state gate prevents duplicate initialization and
  stop/deinit overlap. The Mic task is created first but waits for
  `MIC_CTRL_INIT_READY`; it cannot use the ADC handle until the main path has
  configured it and opened the gate.
- Any allocation, task creation, ADC-handle configuration, VAD, or local gate
  failure signals task abort, waits for the task to stop, then releases the
  handle, control event group, and PSRAM audio storage in ownership order.
- Reconnect release requests stop confirmation before deleting the task and only
  then call `adc_continuous_deinit()`. It clears the handle after deinit and
  returns a lifecycle error when a concurrent start/shutdown owns the state.
- `voice_chain` treats `MIC_ADC_ERR_NOT_READY` as a bounded retry condition,
  not a fatal voice-chain failure. The current C5 path stays VAD-gated; it does
  not reintroduce local WakeNet inference.

## Memory Placement And Admission

- `c5_memory` checks both total free bytes and largest contiguous block for
  every capability class; total heap alone is not accepted as DMA admission.
- LCD legacy and steady-state draw buffers remain internal DMA allocations.
  The legacy 9,600 B buffer is released once the 4,800 B LVGL draw buffer is
  registered.
- Cache-sensitive control stacks remain internal where required, including the
  app startup, dispatcher, Mic ADC, voice-chain, and I2S writer paths.
- LCD bootstrap/event and deferred-worker stacks are PSRAM-backed and are
  staged before or after the DMA-sensitive point as appropriate. Large voice,
  speaker, command, and response payloads use PSRAM; speaker DMA staging
  remains internal DMA.
- Phase-2 worker allocation no longer happens inside the LCD bootstrap, which
  avoids competing with its initial LCD/LVGL allocation window.

## Modified Source Areas

- `ESPC52/components/Middlewares/app_orchestrator/app_orchestrator.c`
- `ESPC52/components/lcd/lcd_service.h`
- `ESPC52/components/lcd_ui/lcd_service.c`
- `ESPC52/components/lcd_ui/lcd_ui.c`
- `ESPC52/components/lcd_ui/lcd_ui.h`
- `ESPC52/components/Middlewares/radar_domain/radar_home_snapshot_client.c`
- `ESPC52/components/Middlewares/mic/mic_adc_test.c`

The existing dirty implementations in `mic_adc_test.h`, `voice_chain.c`, and
`c5_memory.c` were reviewed as dependencies but were not directly edited by
this task.

## Verification Ledger

- [x] Live-source review of startup, LCD/UI, compact snapshot parser, ADC
  lifecycle, voice lifecycle, and memory admission.
- [x] `git diff --check` completed for the architecture edit before final
  integration.
- [x] Final command completed after loading ESP-IDF 5.5.4:
  `cd "/Users/zhiqin/ESP 部分开发 1/ESPC52" && idf.py build`.
  `00_Learn.bin` is `0x226bd0`; the smallest app partition is `0x500000`,
  leaving `0x2d9430` free (`57%`).
- [ ] Device acceptance: LCD timing/animation, ADC/GDMA stability, VAD, Wi-Fi,
  gateway, BLE, radar, S3 transport, and Home transition remain unverified.

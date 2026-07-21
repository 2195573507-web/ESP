# ESPLCD UI Migration Plan (Analysis Only)

Date: 2026-07-21. This is a staged recommendation, not an authorization to alter firmware. Stability of `lcd_events` is a prerequisite; no ESPLCD UI migration should begin before device evidence confirms its 4 KiB stack stays at or above 1 KiB free under stress.

## Decision

Use ESPLCD C51 as a visual reference only. Keep the current C51/C52 `lcd_driver`, `lcd_service`, `lcd_ui`, board profile, memory admission, LVGL lock, snapshot model, and wake-event queue. ESPLCD C52 is a minimal bring-up screen and offers no complete UI architecture to move.

## A. Directly Reusable Design Ideas

* Flat dark background, white/mint/amber/red semantic palette, concise environment hierarchy.
* Pixel-cat visual language, including its voice-state frame concept.
* Boot readiness presentation: display/sensor/network/audio state and bounded progress.
* Invalid BME presentation as explicit `--`, and connection-state indicators.

These are presentation requirements, to be recreated inside `lcd_ui.c` after the current snapshot has supplied values. They are not source-file copy candidates.

## B. Rewrite Before Use

| Item | Required rewrite |
| --- | --- |
| environment detail view | Add fields to the current copied `lcd_system_snapshot_t`; producer owns string/value conversion, UI only applies it on the LVGL timer. |
| voice overlay/cat motion | Keep frame resources in Flash/rodata and update only under the current UI timer/lock. Cat click continues to post `lcd_wake_event_t`, never calls voice directly. |
| boot treatment | Use current timer/lifecycle and bounded retry model; do not introduce reference globals or unmanaged timers. |
| status/navigation | Add overlays or a state enum in current `lcd_ui` rather than independent `lv_screen_load` ownership. |
| touch actions | Use `lcd_touch` cache/read callback and the current degraded path. No application I2C read or LVGL call from a worker. |

## C. Explicitly Do Not Migrate

* Reference `lcd.c`, `lcd_lvgl_service.c`, `lcd_service_*` ownership, `lvgl_port_init`, display/SPI-panel creation, or raw legacy drawing path. These conflict with current one-owner lifecycle.
* C51 direct `voice_chain_request_local_wake()` UI callback.
* CSI service, callbacks, upload, or display semantics. Current radar/home is S3-owned and C5 must remain a terminal.
* Large static internal buffers, full-frame/double DMA buffers, reference global callbacks, or a second LVGL task.
* Any change to pins, controller parameters, partition table, protocol, radar business logic, S3, or server.

## D. Current Project Advantages to Preserve

The current driver explicitly admits allocation based on total free memory and largest contiguous block, requires steady draw storage to be internal DMA-capable, and releases bootstrap storage (`components/lcd/lcd_driver.c:117-153,223-230,446-484`). Its 96 KiB PSRAM UI arena is set before display creation (`components/lcd_ui/lcd_ui.c:325-372`). Its service deletes timers and obeys the LVGL lock during stop, retaining context for retry on lock failure (`components/lcd_ui/lcd_service.c:123-160,257-297`). These are not to be replaced by ESPLCD code.

## Phased Route and Resource Gates

| Phase | Change boundary | Resource budget/gate | Verification and rollback |
| --- | --- | --- | --- |
| 0: Stabilize | no visual change; instrument existing UI | `lcd_events` configured 4,096 B, minimum remaining >=1,024 B; measure LVGL/touch high water | flash/monitor stress with voice+Wi-Fi+radar; rollback is no UI change |
| 1: Visual parity | palette/text hierarchy and cat asset mapping within current dashboard | retain one 4,800 B internal-DMA buffer; PSRAM object-pool admission must pass | source/host build then device screenshot and DMA metrics; revert individual style/asset patch |
| 2: Home enrichment | bounded environment and connectivity fields via copied view model | no new business task; all added strings have fixed maximums | malformed/expired snapshot test, device memory/stack soak; revert overlay fields |
| 3: Voice/status overlays | current UI timer and event queue only | no added `lcd_events` local arrays, LVGL calls, or waits; verify 1 KiB stack floor | queue saturation/click/voice failure tests; disable feature flag or remove overlay |
| 4: Detail pages | one overlay/page state at a time | object count and PSRAM growth measured per page; no double buffer | long-run navigation/cleanup/heap integrity/DMA largest-block test; revert the page only |

## Recommended Final Information Architecture

```
Boot readiness overlay
  -> Home comprehensive status (first priority)
       -> Voice overlay/page (second)
       -> Radar detail overlay (third, S3 snapshot only)
       -> Environment detail overlay (fourth)
       -> Network/system diagnostics overlay (fifth)
```

All page data flows through a single value-copy LCD view model. Non-LVGL tasks may update that model or post fixed ownership-clear events only. Only the LCD service/LVGL timer may create, destroy, or mutate LVGL objects while holding the port lock. Complex decisions remain on S3; C5 displays and interacts.

## Required Device Acceptance Before Any Complex Page

1. `lcd_events`, LVGL, and touch task high-water marks during simultaneous Wi-Fi, voice, radar, BME and touch activity.
2. Internal-DMA free and largest-block minimum while the current 4,800 B draw buffer is active.
3. ST7789 RGB order/inversion, 240x284 coordinate offsets, refresh quality and touch mapping.
4. Page create/delete/re-entry soak, queue overwrite behavior, voice wake failure path and no direct cross-task LVGL access.

The analysis establishes source architecture only. It does not establish that the current crash has disappeared or that the reference appearance renders correctly on a device.

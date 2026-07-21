# ESPLCD vs Current C5 LCD Comparison

Date: 2026-07-21. Compared roots: `/Users/zhiqin/ESP 部分开发/分支项目/ESP` and `/Users/zhiqin/ESP 部分开发/ESPC51`, `/Users/zhiqin/ESP 部分开发/ESPC52`. Current source is authoritative; historical build metadata only establishes that the indicated source had previously entered a build, not that this dirty worktree is flashed or stable on hardware.

## Executive Result

The current project already contains the more complete, safer LCD implementation. ESPLCD C51 supplies useful visual cues and a fuller environment/voice presentation; ESPLCD C52 is only a static bring-up view. Hardware/LVGL APIs match, but neither ESPLCD LCD service nor its direct UI-to-voice callback is a safe transplant target.

## Complete Comparison

| Comparison item | ESPLCD | Current C5 project | Difference and recommendation |
| --- | --- | --- | --- |
| LCD driver | ST7789 SPI2 raw + LVGL service | ST7789 SPI2 driver with staged cleanup | Retain current lifecycle. Do not copy `lcd.c`/service ownership. |
| Resolution/orientation | 240x284 portrait, no swap/mirror | Same board profile | Compatible, but validate physical offsets and colour on device. |
| LVGL version | LVGL 9.2.2, port 2.6.2 | Same declared versions | API-compatible; no migration is needed. |
| Draw buffer | C51 480 B; C52 4,800 B single buffer | 4,800 B single internal-DMA steady buffer | Keep current 10-line single buffer; no double buffer. |
| LVGL pool | Reference relies on its own configuration | 96 KiB PSRAM arena, 92 KiB registered LVGL pool | Current memory design is stronger (`lcd_ui.c:14-18,325-372`). |
| Page count | C51: boot overlay, dashboard, voice overlay; C52: static page | boot cover, dashboard, status overlay, command overlay | Both are one-screen overlay systems; current is functionally broader. |
| Home layout | flat environment column, net status, pixel cat, voice | dark card dashboard: AIR, SPACE/radar, HOME, voice/alarm, cat | Borrow concise hierarchy only; retain current data domains. |
| Radar information | no actual CSI/radar data is rendered | dynamic S3-owned radar/home snapshot | Current is superior and must remain S3-fed. |
| Environment | C51 BME temp/humidity/pressure/gas/air | BME air status in dashboard | ESPLCD has more fields; add only via current snapshot model. |
| Wi-Fi/server | network/gateway status dot/text | Wi-Fi and gateway snapshot text | Comparable; current producer currently only publishes ONLINE/OFFLINE. |
| Voice | cat animation and direct local-wake request | state label/cat and queue-based wake event | Keep current queue boundary; direct callback must not migrate. |
| Icons/fonts/assets | I4 cat frames; default LVGL font | two I4 cat assets; default LVGL font | Aesthetic assets may be adapted, not copy a service. |
| Theme/style | flat dark theme, no cards/shadows | dark cards, borders, rounded 6px corners | Current is denser/more dashboard-like. |
| Animation | 100 ms cat and boot progress | 100 ms cat and 3 s boot cover | Comparable; neither has page animation or gradients. |
| Touch | C51 CST816T pointer and cat hit region | touch poll task with degraded start and click paths | Current has explicit isolated task and graceful degradation. |
| Refresh | 100 ms snapshot, 1 s text rate | 100 ms LVGL service timer, state updates/limits | Both avoid per-sample redraw; keep current controls. |
| Error/offline | BME `--`, network colours, touch-degraded log | snapshot unknown/invalid text, command TTL, startup retries | Current has richer operational degradation. |
| UI object lifecycle | global objects; reference does not offer comparable stop/deinit contract | timer-first stop, LVGL lock, retry-safe cleanup | Current must not regress (`lcd_service.c:123-160,257-297`). |
| LVGL lock | port lock around creation | all external lifecycle work locked; producers never touch objects | Preserve current ownership discipline. |
| Task model | LVGL task, C51 direct UI callback | LVGL service + `lcd_bootstrap` + `lcd_events` + touch task | Current separates UI and business operations. |
| Event queue | no equivalent safe UI wake boundary | single-slot overwrite wake queue, then voice queue | Current is deliberate backpressure (`lcd_service.c:359-397`). |
| PSRAM/internal RAM | reference allocation intent does not prove region | UI arena/context/PSRAM stacks use PSRAM | Current explicitly protects C5 internal/DMA resource. |
| DMA | reference uses `MALLOC_CAP_DMA`; C51 buffer is small | explicit `INTERNAL|DMA`, pointer capability and largest-block checks | Current is safer; do not put DMA draw buffer in PSRAM. |
| Stack use | LVGL 4 KiB internal task, reference direct callbacks | LVGL 4 KiB PSRAM; `lcd_events` 4 KiB PSRAM | UI migration must not enlarge `lcd_events` call depth. |
| Current completion | reference C51 visual detail, C52 bring-up only | real driver + UI + data binding + queues + stop/fault paths | Current implementation is materially more complete; still lacks device acceptance. |

### Code and Build Evidence

Current C51/C52 `lcd`, `lcd_ui`, `app_orchestrator`, and radar snapshot clients are present in historical build metadata (`ESPC51/build-c5-voice-lcd-regression-c51/project_description.json:28-29`; matching C52 retained build metadata). Current LCD driver hardware/lifecycle starts at `components/lcd/lcd_driver.c:239`, uses the 4,800 B internal-DMA plan at `:255-260,446-484`, and uses a PSRAM LVGL task at `:392-402`. The current UI object tree is constructed at `components/lcd_ui/lcd_ui.c:224-318`; producer/value-copy and service timing are at `components/lcd_ui/lcd_service.c:97-121,163-254,310-397`.

## Current Main LCD State Machine

```
BOOT
  -> lcd_bootstrap (4 KiB PSRAM task, retry start)
  -> LCD_INIT / UI_CREATE (lvgl_port_lock held)
  -> BOOT_COVER (3 seconds)
  -> HOME_IDLE
  -> HOME_UPDATE (100 ms LCD service timer reads copied snapshot)
  -> STATUS_OVERLAY (dashboard/status click)
  -> COMMAND_OVERLAY (bounded command with TTL)
  -> HOME_IDLE

cat click -> lcd_service_request_wake_event -> capacity-one overwrite queue
          -> lcd_events -> voice queue
```

### Current Main Interface Tree

```
LVGL active screen (240x284)
├── boot_page, hidden after 3 s
├── dashboard
│   ├── SensAir C5 and Wi-Fi/GW labels
│   ├── AIR card
│   ├── SPACE/radar card
│   ├── HOME card
│   ├── clickable two-frame cat
│   └── VOICE and ALARM card
├── status_page, hidden until dashboard/status click
│   └── fixed system-status labels
└── command_overlay, hidden unless a bounded-TTL command is present
    └── wrapping command label
```

The tree is source-created, not inferred from component names (`ESPC51/components/lcd_ui/lcd_ui.c:224-318`; C52 is kept in source parity). Its dynamic values are the copied LCD snapshot, not direct BME/radar/voice task access (`lcd_service.c:97-121,310-356`).

`lcd_events` is the cross-task snapshot publisher, not an LVGL worker: 4 KiB PSRAM static stack, 100 ms cadence, 1,024 B low-water threshold, and periodic high-water logging (`components/Middlewares/app_orchestrator/app_orchestrator.c:47-57,222-291,311-359`). UI creation/timers are protected by `lvgl_port_lock`; producers only value-copy snapshots and commands under `s_data_lock` (`lcd_service.c:97-120,225-243,310-356`). `radar_home_ui` is only a log tag; the actual C5 client polls/cache-copies S3 snapshots and LCD consumes that cache (`components/Middlewares/radar_domain/radar_home_snapshot_client.c:166-205`).

## Important Five Differences

1. ESPLCD C51 exposes raw BME fields and a direct voice callback; current UI exposes S3-owned radar/home state and queue-mediated wake.
2. Current UI has a production driver/service stop path, explicit allocation admission, fault cleanup, and a PSRAM LVGL object pool.
3. ESPLCD C52 is static; it cannot substantiate claims about a complete C52 reference UI.
4. Current UI uses cards and overlays; ESPLCD uses a flatter column presentation and larger cat animation language.
5. Current C5 has a defined internal-DMA budget and existing `lcd_events` stack guard, so visual complexity must be budgeted, not copied wholesale.

## Stability Assessment

The source reduces `lcd_events` risk because it does not call LVGL, HTTP, or audio directly; it aggregates a value snapshot and posts a queue event. But source cannot prove the reported stack crash is resolved: no flash/monitor/high-water result was collected. `lcd_events` must retain at least 4,096 B configured stack and 1,024 B minimum remaining stack under pressure. Also measure DMA largest contiguous block while Wi-Fi, voice, radar and LCD are concurrent before raising visual complexity.

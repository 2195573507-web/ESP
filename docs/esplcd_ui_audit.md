# ESPLCD LCD UI Audit

Date: 2026-07-21
Scope: read-only examination of `/Users/zhiqin/ESP 部分开发/分支项目/ESP` (the ESPLCD reference). This document does not treat source or historical compile databases as device acceptance.

## Evidence Boundary

The reference contains two materially different C5 LCD implementations. ESPLCD C51 is the actual visual reference; ESPLCD C52 is a deliberately minimal static LVGL confirmation screen. CMake declares the C51 LCD service, CST816T touch and `ui/boot_screen.c` as build members (`ESPC51/components/lcd/CMakeLists.txt:1-5`, `components/ui/CMakeLists.txt:1-5`); C52 declares only its LCD service (`ESPC52/components/lcd/CMakeLists.txt:1-5`). Retained compile databases establish that older LCD sources entered a build, but the ESPLCD UI sources are dirty and some artifacts predate them. They do not prove the current C51 boot overlay was rebuilt, linked, or flashed.

Entry flow is `app_main -> app_startup_task -> app_orchestrator_start` (`ESPC51/main/main.c:25-75`). C51 starts LCD with a snapshot provider (`ESPC51/components/Middlewares/app_orchestrator/app_orchestrator.c:86-137`); C52 starts the static LCD service after voice without a provider (`ESPC52/components/Middlewares/app_orchestrator/app_orchestrator.c:179-183`). LVGL is 9.2.2 and `esp_lvgl_port` is 2.6.2 (`ESPC51/dependencies.lock:54-71`; C52 has the same declarations).

## One-Line Visual Summary

ESPLCD C51 is a portrait, dark blue-grey, sparse status dashboard with mint status accents and a tappable pixel-cat voice affordance; it is not a multi-page card dashboard or a radar-position UI.

## Physical Display and Resources

* ST7789 on SPI2, RGB565, 240x284 portrait, zero X/Y gaps, and no XY swap or X/Y mirroring (`ESPC51/components/lcd/lcd.h:28-37,120-128,259-275`; panel setup in `lcd.c:413-519`).
* It uses RGB/big-endian/byte-swap scheme B and inversion (`lcd.h:212-245`). The code alone cannot prove real-panel colour order, inversion, offsets, or touch alignment.
* C51 uses a single one-line 240-pixel LVGL draw buffer (480 B), 4 KiB priority-1 LVGL task, and 20 ms LVGL service interval (`lcd_lvgl_service.c:20-24,1277-1316`). C52 uses a 10-line single buffer (4,800 B) (`ESPC52/components/lcd/lcd_lvgl_service.c:10-14,62-105`).
* Product-specific artwork is C-source I4 pixel-cat data: dashboard 64x48, voice 128x96 and state frames (`ESPC51/components/lcd/lcd_lvgl_service.c:344-624`), plus a boot 96x72 image (`components/ui/boot_cat_image.h:5-79`). No application PNG/JPG/GIF/SquareLine project or design screenshot was found; assets under vendor `managed_components` are not application dependencies.

## C51 Actual Page Tree

```
LVGL active screen (240x284, #101820)
├── boot_root, temporary and initially visible
│   ├── 96x72 I4 boot cat
│   ├── Display / Sensor / Network / Audio labels and four 6px state dots
│   └── 160x8 progress bar
├── dashboard_root, visible after boot
│   ├── title: SensAir C5; section title: ENVIRONMENT
│   ├── TEMP / HUM / PRESS / GAS / AIR dynamic labels and AIR dot
│   ├── network dot and Online / Connecting / Offline label
│   ├── 64x48 animated, clickable cat and transparent 90x70 hit target
│   └── voice-state label
└── voice_root, initially hidden
    └── centred 128x96 state-dependent pixel cat
```

The object construction evidence is `lcd_lvgl_service.c:681-717,814-910,1153-1222` and `boot_screen.c:427-468`. These are roots/overlays on one active LVGL screen, not separate `lv_screen_load` pages.

### Boot State

The compiled C51 selection is `BOOT_SCREEN_STATUS_ONLY_TEST=1` (`components/ui/boot_screen.c:10-18`), so the unselected fuller title/status branches must not be reported as the live UI. The boot overlay is black: cat at `(72,48)`, labels at y `125/150/175/200`, dots at x `50`, progress bar `(40,240,160x8)`. Every 100 ms it checks display, sensor, network and audio; dots become mint when ready and amber while pending. It remains for at least 3 s, then exits when all services are ready or after 8 s, waits 350 ms, deletes `boot_root`, and resumes dashboard refresh (`boot_screen.c:143-245,248-289,427-488`).

### Dashboard and Voice Overlay

The dashboard has no cards. It is flat, information-sparse, and uses #101820 background, white headings, #DCE7EF text, #79D2A6 mint, #E5B75B amber and #E56B6F error colours (`lcd_lvgl_service.c:286-297,1045-1063,1159-1222`). There are no custom fonts, icons, shadows, gradients, borders, or rounded card layout.

The environment column is TEMP, HUM, PRESS, GAS and AIR. It becomes `--` when BME data is invalid. Gateway/network is mapped to Online, Connecting or Offline. The dashboard refresh timer polls snapshots every 100 ms but throttles display-data update to 1 s (`lcd_lvgl_service.c:1007-1110,1239-1252`). CSI fields may be populated by the provider, but the dashboard update never consumes them; there is therefore no actual radar presence, target count, multi-target, or target-position screen.

When voice state is not LISTEN, the dashboard is hidden and a centred cat-only full-screen overlay is shown; LISTEN reverses that (`lcd_lvgl_service.c:814-828,885-1005`). It is not a navigable speech page and has no text control.

## Interactions and Motion

* CST816T uses shared I2C0; the LVGL pointer callback maps 240x284 coordinates and mirrors Y, rejecting out-of-range points (`components/lcd/touch/touch_cst816t.c:34-89`; `lcd_lvgl_service.c:124-190`).
* The cat and transparent hit area post `voice_chain_request_local_wake()` on click (`lcd_lvgl_service.c:221-230,681-717`). There are no buttons, keys, swipe gestures, or page navigation controls.
* Cat animation runs every 100 ms: wake/recording use six frames and speaker playback toggles mouth state every four frames (`lcd_lvgl_service.c:720-812,912-1005`). Boot progress and cat motion have the periods stated above. No page-transition, gradient, numeric-roll, waveform, or breathing-light animation is implemented.
* Touch setup failure logs a degraded condition and continues with the display (`lcd_lvgl_service.c:1361-1369`).

## ESPLCD C52: Do Not Misclassify It

C52 creates only one static screen: `SensAir C5`, `LCD + LVGL ready`, and `BME / CSI / voice remain independent` on the same dark background (`ESPC52/components/lcd/lcd_lvgl_service.c:20-129`). It has no dynamic provider, touch, animation, data refresh, radar display, or page transition. It is a bring-up page, not the mature UI reference.

## ESPLCD State Machine

```
BOOT
  -> LCD_INIT (lcd_init / lcd_lvgl_service_start; LVGL port lock held while objects are created)
  -> UI_CREATE
  -> BOOT_OVERLAY (C51 only; 100 ms readiness polling)
  -> HOME_IDLE
  -> HOME_UPDATE (100 ms snapshot poll; text limited to 1 s)
  -> VOICE_OVERLAY (voice state != LISTEN)
  -> HOME_IDLE (voice state == LISTEN)
```

The boot exits at all-ready or eight-second timeout. Voice overlay visibility is the only page-like transition. `lvgl_port_lock()` surrounds registration and object construction (`lcd_lvgl_service.c:192-218,1239-1252`); state data is copied through the provider lock (`lcd_lvgl_service.c:97-101`). The reference C51 also calls voice directly from a UI click callback, a coupling that is unsuitable for the current multi-task architecture.

## Limits of This Audit

Need real hardware/screenshot evidence for panel colours, actual rendered glyphs, touch orientation/hit accuracy, animation smoothness, DMA headroom, and measured task stack high-water marks. No statement above is a flash, monitor, or long-run stress-test result.

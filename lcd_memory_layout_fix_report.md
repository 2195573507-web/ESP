# C5 N16R8 LCD Memory Layout Fix Report

## Scope

Active target: `ESPC51` and its identical C5 peer `ESPC52`.

Only the LCD allocation path was changed. WiFi, NimBLE, Radar, Voice, WakeNet,
BME, Dispatcher, and the wider memory-placement policy were not changed.

## Cause and Constraints

`LCD_MEM_ADMISSION_FAIL` is emitted only by `lcd_driver_require_memory()`.
Its admission floors remain unchanged:

| Phase | Internal/DMA free minimum | Largest-block minimum |
|---|---:|---:|
| Bootstrap | 30 KiB | 20 KiB |
| LVGL steady state | 24 KiB | 16 KiB |

The current SPI LCD path requires its submitted RGB565 buffers to remain in
internal DMA-capable memory. Although ESP32-C5 PSRAM is DMA-capable, this
SPI-panel path does not submit `SPI_TRANS_DMA_USE_PSRAM`; moving the active
LVGL buffer to PSRAM would require an internal bounce allocation per flush.

## Applied Fix

The legacy manufacturing fill buffer now has the same size as the actual
10-line RGB565 transfer stripe:

| Allocation | Before | After | Caps | Lifetime |
|---|---:|---:|---|---|
| Legacy fill DMA buffer | 9,600 B | 4,800 B | internal + DMA + 8-bit | Panel-ready until LVGL handoff, failure cleanup, or stop |
| LVGL partial draw buffer | 4,800 B | 4,800 B | internal DMA | LVGL display lifetime |
| LVGL task stack | 4,096 B | 4,096 B | PSRAM + 8-bit | LVGL port lifetime |

`lcd_driver_fill_legacy()` already fills and submits one 10-line stripe at a
time. Therefore 4,800 B is sufficient for every legacy transfer, and
`spi_bus_config_t.max_transfer_sz` follows the same bound.

This reduces the known simultaneous LCD pixel-DMA peak during LVGL handoff
from 14,400 B to 9,600 B, without moving any DMA buffer, lowering an
admission threshold, or removing LVGL, touch, framebuffer/draw buffering, or
SPI DMA.

The active UI/service context and task-stack policy remain PSRAM-backed where
already configured. The static LCD wake queue remains internal `.bss` and has
no runtime heap allocation.

## Modified Files

- `ESPC51/components/lcd/lcd_board_profile.h`
- `ESPC51/components/lcd/lcd_driver.c`
- `ESPC52/components/lcd/lcd_board_profile.h`
- `ESPC52/components/lcd/lcd_driver.c`

## Verification

- `git diff --check` passed.
- C51/C52 `lcd_driver.c` files are byte-identical.
- C51/C52 `lcd_board_profile.h` files are byte-identical.
- Agent review found no transfer-size, lifecycle, DMA-capability, or
  threshold-regression issue.

No build, firmware flash, monitor session, or device startup was run. Actual
LCD startup and rendering on the C5 hardware remain the required device-level
acceptance check.

# C5 N16R8 LCD Admission Alignment Report

## Scope

This change is limited to the LCD admission calculation in the paired active
targets `ESPC51` and `ESPC52`. It does not modify WiFi, BLE/NimBLE, radar,
voice, WakeNet, BME, dispatcher, or any PSRAM placement policy.

## Previous Admission Logic

`lcd_driver_require_memory()` was called before bootstrap allocation and after
the LVGL handoff. Both calls applied one fixed largest-block threshold to the
internal and DMA heaps:

| Stage | Required free | Previous required largest block |
| --- | ---: | ---: |
| `bootstrap` | 30 KiB | 20 KiB |
| `lvgl_steady` | 24 KiB | 16 KiB |

`LCD_MEM_ADMISSION_FAIL` logged those function parameters as
`required_free` and `required_largest`. The 20 KiB and 16 KiB values were not
derived from an LCD allocation and remained after the stripe size became
4,800 B.

## DMA Allocation Audit

| Owner | Exact request and caps | Lifetime |
| --- | --- | --- |
| SPI bus descriptors | 48 B total when LCD owns SPI2; `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA` | `spi_bus_initialize()` through `spi_bus_free()` |
| Legacy fill stripe | `LCD_LEGACY_DMA_BYTES = 240 * 10 * 2 = 4,800 B`; `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT` | panel-ready state until LVGL draw buffer validation, or cleanup |
| LVGL draw buffer | `LCD_LVGL_DRAW_BYTES = 240 * 10 * 2 = 4,800 B`; LVGL requests DMA memory and the driver verifies internal DMA capability | LVGL display lifetime |

The legacy and LVGL stripes briefly overlap during handoff, so their known
application pixel payload peak is 9,600 B. `double_buffer=false`; no second
draw buffer, rotation buffer, full-frame buffer, or flush-time DMA pixel
allocation exists. LVGL submits the persistent draw buffer directly to the
panel.

The 96 KiB UI arena, 2 KiB service context, and LVGL task stack are PSRAM or
non-DMA allocations and remain outside this DMA admission model.

## Applied Alignment

`lcd_driver_require_memory()` now receives `required_dma_block` instead of a
fixed `largest_min`:

| Stage | Required largest block after change | Source of truth |
| --- | ---: | --- |
| `bootstrap` | `LCD_LEGACY_DMA_BYTES` = 4,800 B | the next legacy DMA stripe allocation and SPI transfer maximum |
| `lvgl_steady` | `LCD_LVGL_DRAW_BYTES` = 4,800 B | the verified active LVGL DMA draw-buffer contract |

The admission still rejects when either internal or DMA largest free block is
smaller than that actual required DMA block. It also retains the existing
30 KiB bootstrap and 24 KiB steady-state free-capacity gates for both internal
and DMA-capable heaps. Address-capability checks remain unchanged: the legacy
buffer must be internal and DMA-capable, and the LVGL buffer must have the
same properties. No DMA requirement was moved to PSRAM.

The failure log now names the value precisely as `required_dma_block`.

## C51/C52 Consistency

The active `components/lcd` and `components/lcd_ui` source trees are
byte-identical between C51 and C52. The identical `lcd_driver.c` adjustment
was applied to both targets; no device-specific LCD difference was introduced.

## Verification Performed

- `git diff --check -- ESPC51/components/lcd/lcd_driver.c ESPC52/components/lcd/lcd_driver.c`
- byte comparison of the two updated `lcd_driver.c` files
- source audit of the LCD DMA allocations, allocation lifetimes, and LVGL
  display configuration

Per request, no build, flash, monitor, server, or runtime verification was
performed. Hardware startup and rendering remain device-level acceptance work.

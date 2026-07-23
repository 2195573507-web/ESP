# C5 N16R8 LCD Dynamic Admission Requirement Report

## Scope And Constraints

Active targets: `ESPC51` and `ESPC52`, which remain byte-identical for the
LCD driver. This audit and change are limited to LCD admission in
`components/lcd/lcd_driver.c`.

No Voice, Radar, WiFi, or PSRAM-placement policy was changed. In particular,
the existing LCD SPI/LVGL DMA contract remains internal DMA; this work does
not move DMA buffers to PSRAM.

## Agent Findings

| Role | Result |
|---|---|
| Agent1, threshold provenance | `required_free=30720` came from the historical fixed `30 KiB` bootstrap floor, not from a current LCD allocation. The only file-history introduction was commit `6c369e64`; the historical plan also paired it with fixed 20 KiB largest-block guidance. |
| Agent2, allocation inventory | The exact bootstrap DMA allocation is one RGB565 stripe: `240 * 10 * 2 = 4800 B`, with `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT`. SPI/panel control allocations are SDK-internal and have no safe project-source size to turn into a fixed reserve. |
| Agent3, dynamic design | Admission must derive `required_free` as the sum of pending requests with identical caps and `required_largest` as their maximum size. Bootstrap and LVGL steady state each have one known 4800 B request. |
| Agent4, DMA boundary | Legacy and LVGL buffers retain internal-DMA caps and runtime `esp_ptr_dma_capable` plus `esp_ptr_internal` validation. LVGL remains `buff_dma=true`, `buff_spiram=false`; touch is I2C with a PSRAM task stack. |
| Agent5, implementation | Replaced fixed LCD admission floors with allocation-plan admission in both C5 drivers. |
| Agent6, review | Found that bootstrap admission initially preceded SPI/panel allocations; it was moved directly before `heap_caps_calloc`. Final review found no issues against the pre-implementation baseline. |

## Dynamic Requirement

`lcd_driver_admit_plan()` receives requests shaped as `{ owner, size, caps }`.
For each caps group it queries the same caps heap and derives:

```
required_free    = sum(request.size)
required_largest = max(request.size)
```

The current plans are:

| Stage | Pending allocation | Requirement | Caps | Admission placement |
|---|---|---:|---|---|
| bootstrap | `heap_caps_calloc` legacy stripe | 4800 B free, 4800 B largest | internal + DMA + 8-bit | immediately before the allocation |
| LVGL steady | `lvgl_port_add_disp` draw buffer | 4800 B free, 4800 B largest | internal + DMA + 8-bit | immediately before display creation |

The steady value is calculated from `display_cfg.buffer_size * sizeof(uint16_t)
* (double_buffer ? 2 : 1)`, rather than copied from a fixed value. The
bootstrap value is `LCD_LEGACY_DMA_BYTES`, which is the same one-stripe
allocation used by `heap_caps_calloc` and the SPI maximum transfer size.

The old `30 KiB`, `24 KiB`, `20 KiB`, and `16 KiB` admission constants and
`lcd_driver_require_memory()` have been removed from both active LCD drivers.
The runtime `LCD_RUNTIME_DMA_LARGEST_WARN` remains diagnostic-only and does
not reject startup.

## Boundary Confirmation

- Bootstrap DMA allocation stays `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA |
  MALLOC_CAP_8BIT` and must pass both DMA-capable and internal-pointer checks.
- LVGL still uses `buff_dma=true` and `buff_spiram=false`; its active draw
  buffer must also pass DMA-capable and internal-pointer checks.
- SPI stays on `SPI_DMA_CH_AUTO` with a 4800 B maximum transfer size.
- Touch performs I2C reads; its task stack, LCD service context, and UI arena
  are outside this internal-DMA admission path.
- A pre-existing LVGL task-stack PSRAM setting was already present in the dirty
  workspace at the start of this task. It was neither changed nor reverted,
  respecting the no-PSRAM-policy-change boundary.

## Supplied Runtime Snapshot

For the supplied snapshot (`internal_free=33771`, `dma_free=24559`), a
4800 B dynamic admission would not reject solely on total free space. The
actual admission is intentionally evaluated after SPI/panel setup, so the
device must still provide an at-that-point 4800 B largest internal-DMA block.

## Static Verification

- `git diff --check -- ESPC51/components/lcd/lcd_driver.c ESPC52/components/lcd/lcd_driver.c` passed.
- `cmp -s ESPC51/components/lcd/lcd_driver.c ESPC52/components/lcd/lcd_driver.c` returned zero.
- Source scan found no remaining fixed 30/24 KiB LCD admission constants or
  `lcd_driver_require_memory` references in either active LCD component.
- Final Agent6 review confirmed both admission calls are immediately before
  their real allocation calls and the DMA/internal contracts remain unchanged.

Per request, no build, flash, monitor, startup, or runtime validation was
performed. Hardware acceptance remains a cold-start LCD check that captures
the post-panel `LCD_MEM_ADMISSION_FAIL` fields, if any, and confirms display
rendering.

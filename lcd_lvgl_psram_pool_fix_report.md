# LCD LVGL PSRAM Pool Registration Fix Report

## Scope

This repair changes only the C51 and C52 LVGL builtin-memory configuration and
the diagnostics immediately surrounding their existing PSRAM pool registration.
It does not change the 96 KiB UI arena, its 4 KiB offset, LCD DMA size, internal
RAM admission, draw buffer, touch, SPI DMA, radar, voice, Wi-Fi, or BME code.

## Evidence And Root Cause

Both `ESPC51/components/lcd_ui/lcd_ui.c` and
`ESPC52/components/lcd_ui/lcd_ui.c` allocate `LCD_UI_ARENA_BYTES = 96 KiB` in
PSRAM, then register the region beginning at `arena + 4 KiB`:

```
pool_addr = arena + 4 KiB
pool_size = 96 KiB - 4 KiB = 92 KiB = 94,208 bytes
```

The LVGL 9.2.2 builtin allocator calls `lv_tlsf_add_pool()` from
`lv_mem_add_pool()`. LVGL passes the configuration limit to TLSF as:

```
TLSF_MAX_POOL_SIZE = LV_MEM_SIZE + LV_MEM_POOL_EXPAND_SIZE
```

The prior defaults selected a 2 KiB builtin LVGL pool and left the expansion
size at the LVGL default of 0 KiB. That limit cannot describe the 92 KiB
external PSRAM pool, so registration produced `LCD_UI LVGL PSRAM pool
registration failed`.

## Applied Change

Both `ESPC51/sdkconfig.defaults` and `ESPC52/sdkconfig.defaults` now set:

```
CONFIG_LV_MEM_SIZE_KILOBYTES=2
CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=63
```

This is a size-limit configuration, not a new heap allocation. The 2 KiB
builtin bootstrap allocation is unchanged. The 63 KiB expansion gives
`TLSF_MAX_POOL_SIZE = 2 KiB + 63 KiB = 65 KiB`. In the LVGL 9 TLSF mapping,
65 KiB has `ceil_log2 = 17`, producing a 128 KiB `block_size_max`; that accepts
the unchanged 92 KiB external pool. With 62 KiB expansion, the total is exactly
64 KiB and remains in the smaller 64 KiB maximum range, so it rejects the pool.
Therefore 63 KiB is the strict minimum configuration change.

The rejected 8 KiB bootstrap plus 84 KiB expansion alternative was based on an
incorrect fixed-control-size assumption. LVGL TLSF derives its maximum block
from the configured size's logarithmic index; the source-level arithmetic above
is the basis for the final 2 KiB plus 63 KiB configuration.

Immediately before and after the unchanged `lv_mem_add_pool()` call, both LCD
UI implementations now emit:

```
LVGL_MEM_INIT_STAGE before_pool pool_addr=<...> pool_size=<...> psram_free=<...>
LVGL_MEM_INIT_STAGE after_pool pool_addr=<...> pool_size=<...> psram_free=<...>
```

The `after_pool` log is issued before the NULL check, so it is present on both
success and registration failure. Pool preparation remains before display/UI
creation, after LVGL initialization as established by the existing lifecycle.

## C51/C52 Parity

The targeted configuration values and `lcd_ui_prepare_lvgl_pool()` pool/logging
blocks are byte-identical between C51 and C52. Identity-specific configuration
outside this scope was preserved.

## Verification Boundary

Static verification only: whitespace validation, targeted configuration/log
searches, and C51/C52 parity comparison. No build, flash, monitor, server, or
runtime/device validation was performed by request. Runtime acceptance still
requires observing successful pool registration and the new stage logs on the
target device.

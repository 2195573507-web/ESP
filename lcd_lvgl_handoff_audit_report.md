# C5 N16R8 LCD LVGL Handoff Audit

Date: 2026-07-20

Scope: read-only audit of the active ESPC51 and ESPC52 LCD/LVGL handoff.
No product code, configuration, DMA placement, thresholds, radar, voice, or
WiFi files were changed. No build, flash, monitor, or runtime validation was
performed.

## Result

The LVGL register-stage watchdog has a deterministic LCD/LVGL root cause:
the 2 KiB LVGL builtin bootstrap allocator cannot allocate the enabled Span
snippet stack. The resulting `LV_ASSERT_MALLOC` takes the default
`while(1)` handler on `taskLVGL`; CPU0 idle subsequently misses the 5-second
task watchdog.

The reported frames are consistent with this path:

`lv_init -> lv_draw_buf_init_handlers -> lv_span_stack_init -> lv_malloc ->
LV_ASSERT_MALLOC -> while(1)`.

`width_to_stride` is an adjacent normal LVGL initialization frame, not the
blocking operation or an indication of a draw-buffer geometry error.

## Findings

### P0: Bootstrap heap OOM becomes an unyielding taskLVGL loop

- Both targets select LVGL builtin malloc with `CONFIG_LV_MEM_SIZE_KILOBYTES=2`,
  `CONFIG_LV_USE_SPAN=y`, and `CONFIG_LV_SPAN_SNIPPET_STACK_SIZE=64`:
  [ESPC51 sdkconfig](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/sdkconfig:2801),
  [ESPC51 sdkconfig](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/sdkconfig:3040),
  with byte-identical C52 settings.
- LVGL creates its TLSF control data inside that same 2 KiB array before the
  first allocatable pool is recorded:
  [lv_mem_core_builtin.c](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/managed_components/lvgl__lvgl/src/stdlib/builtin/lv_mem_core_builtin.c:71).
  The allocator control alone is about 680 B on this 32-bit target, leaving
  substantially less than the nominal 2048 B for allocations.
- The enabled Span path allocates a 64-entry snippet stack during `lv_init`:
  [lv_init.c](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/managed_components/lvgl__lvgl/src/lv_init.c:167),
  [lv_span.c](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/managed_components/lvgl__lvgl/src/widgets/span/lv_span.c:94).
  A 32-bit snippet is about 28 B, so the requested object is about
  `64 * 28 + 4 = 1796 B`, which cannot fit in the remaining bootstrap pool.
- Allocation failure is fatal in this configuration:
  [lv_assert.h](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/managed_components/lvgl__lvgl/src/misc/lv_assert.h:71)
  maps it to the default infinite-loop handler in
  [lv_conf_internal.h](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/managed_components/lvgl__lvgl/src/lv_conf_internal.h:1014).
- The 92 KiB UI PSRAM pool arrives too late to help. `lcd_service_start()`
  invokes `lcd_driver_register_lvgl()` first, then only later calls
  `lcd_ui_start()`; the latter registers the PSRAM pool:
  [lcd_service.c](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/components/lcd_ui/lcd_service.c:177),
  [lcd_ui.c](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/components/lcd_ui/lcd_ui.c:328).

### Cleared: draw buffer geometry and DMA placement

- The active profile is 240 x 284, ten RGB565 lines. This is 2400 pixels and
  exactly 4800 B:
  [lcd_board_profile.h](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/components/lcd/lcd_board_profile.h:36),
  [lcd_board_profile.h](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/components/lcd/lcd_board_profile.h:70).
- The port receives pixel count `2400`, RGB565, one buffer, partial mode,
  hres 240, vres 284, and internal DMA flags:
  [lcd_driver.c](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/components/lcd/lcd_driver.c:379).
  `esp_lvgl_port` multiplies by RGB565's two bytes per pixel before registering
  the buffer:
  [esp_lvgl_port_disp.c](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c:270),
  [esp_lvgl_port_disp.c](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c:331).
- LVGL stride is `240 * 16 / 8 = 480 B`; ten rows are 4800 B. The driver
  independently verifies the resulting active buffer size and internal-DMA
  capability:
  [lcd_driver.c](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/components/lcd/lcd_driver.c:426).

### Cleared: persistent no-yield loop and watchdog configuration

- `taskLVGL` enters `lv_init()` before notifying the registering task:
  [esp_lvgl_port.c](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port.c:87),
  [esp_lvgl_port.c](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port.c:222).
- Its normal loop waits on an event group and unconditionally executes
  `vTaskDelay(1)`. There is no LCD application or port-level busy loop to
  modify:
  [esp_lvgl_port.c](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port.c:245).
- C51 and C52 both enable a five-second task watchdog checking CPU0 idle:
  [ESPC51 sdkconfig](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/sdkconfig:1811).
  No LCD/LVGL source registers, resets, disables, or otherwise manipulates it.
  A watchdog workaround would hide the allocator assertion and is rejected.

### Stack review

- `lcd_lvgl_port_stack=4096` is 4096 bytes, passed directly to
  `xTaskCreateWithCaps`, and remains correctly located in PSRAM:
  [lcd_driver.c](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/components/lcd/lcd_driver.c:364),
  [esp_lvgl_port.c](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port.c:87).
- It is below the port's 7168 B default:
  [esp_lvgl_port.h](/Users/zhiqin/ESP%20%E9%83%A8%E5%88%86%E5%BC%80%E5%8F%91/ESPC51/managed_components/espressif__esp_lvgl_port/include/esp_lvgl_port.h:64).
  Static evidence does not prove stack exhaustion, and increasing it cannot
  resolve the established `LV_ASSERT_MALLOC` infinite loop. Do not make a
  stack-only change for this failure.

## Minimal Repair Recommendation

Disable unused LVGL Span in both C51 and C52, preserving the 2 KiB bootstrap
pool, the confirmed 4800 B internal-DMA buffer, all admission rules, and the
PSRAM LVGL task stack:

`CONFIG_LV_USE_SPAN=n`

Repository search found no application `lv_span_` or `lv_spangroup` use outside
the managed LVGL component and build artifacts. This removes the failing
startup allocation without relocating DMA, changing an internal-RAM admission
threshold, or changing radar, voice, or WiFi behavior. Apply the equivalent
configuration parity update to the active C51 and C52 configuration sources.

A later, separate runtime measurement may justify raising only
`.task_stack` from 4096 to the port default 7168 in the two `lcd_driver.c`
files. That is not part of this root-cause repair and should not be folded into
it without stack watermark evidence.

## Scope Review

C51 and C52 active LCD driver, board profile, and LCD service sources are
byte-identical. The recommended change is limited to the LVGL feature setting
for those two C5 configurations. It does not change radar, voice, WiFi, DMA
placement, internal-RAM thresholds, or draw-buffer parameters.

## Verification Boundary

This is static source/configuration proof only. No compilation, flashing,
monitoring, or device runtime test was performed, as requested. Hardware
acceptance after the configuration repair remains unverified.

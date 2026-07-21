# C5 N16R8 LVGL Span Fix Report

Date: 2026-07-20

## 1. Modified Files

The repair changes only the LVGL Span configuration, with C51/C52 parity.

| Target | Active configuration | Durable configuration default | Change |
| --- | --- | --- | --- |
| ESPC51 | `ESPC51/sdkconfig:3040` | `ESPC51/sdkconfig.defaults:24` | `CONFIG_LV_USE_SPAN=n` |
| ESPC52 | `ESPC52/sdkconfig:3040` | `ESPC52/sdkconfig.defaults:24` | `CONFIG_LV_USE_SPAN=n` |

`sdkconfig` is the active resolved configuration. `sdkconfig.defaults` now
pins the same value for a future clean configure or `menuconfig` regeneration.
Both projects ignore `sdkconfig` (`ESPC51/.gitignore:34` and
`ESPC52/.gitignore:34`), so the defaults entries are the repository-persistent
part of this repair.

No `sdkconfig.old` exists under either active C5 project. Existing build
generated configuration files still contain the previous enabled value, for
example `ESPC51/build-c5-radar-psram-c51/config/sdkconfig.h:1289` and
`ESPC52/build-c5-memory-final-c52/config/sdkconfig.h:1289`. They are derived
artifacts and were deliberately not edited or regenerated.

## 2. Reason for the Change

The LVGL Kconfig symbol defaults to enabled when the minimal profile is not
selected (`managed_components/lvgl__lvgl/Kconfig:1089-1095`). With Span
enabled, `lv_init()` allocates the 64-entry Span snippet stack before the
application registers its PSRAM UI pool. The 2 KiB builtin LVGL bootstrap pool
cannot satisfy that allocation, and the enabled malloc assertion enters the
default `while(1)` handler. `taskLVGL` then prevents CPU0 idle from servicing
the task watchdog.

The application source has no `lv_span_` or `lv_spangroup` consumer outside
managed LVGL sources and build artifacts. Disabling the unused widget removes
only the failing startup allocation and preserves the previously confirmed
display handoff.

## 3. Scope and Invariants

No radar, voice, WiFi, LCD DMA, internal-RAM admission, or PSRAM allocation
source was modified. No watchdog workaround was added.

The following C51/C52 values remain matched and unchanged by this repair:

| Invariant | C51 / C52 value | Evidence |
| --- | --- | --- |
| LVGL builtin pool | 2 KiB | `sdkconfig:2812` |
| DMA draw buffer | 240 x 10 RGB565, single buffer, 4800 B | `components/lcd/lcd_board_profile.h:36-41,70-73`; `components/lcd/lcd_driver.c:379-420` |
| LVGL task | 4096 B, priority 1, PSRAM stack | `components/lcd/lcd_driver.c:364-372` |
| PSRAM admission settings | always internal 16384, reserve 32768 | `sdkconfig:1721-1723` |

The active C51/C52 LCD driver, board profile, and LCD service sources remain
byte-identical. Static source search found zero application matches for
`lv_span_` or `lv_spangroup` after excluding managed components and generated
build artifacts.

## 4. Follow-up Verification Commands

The following commands are intentionally not run in this change. They are
the next validation layer after the configuration repair:

```sh
cd "/Users/zhiqin/ESP 部分开发/ESPC51"
idf.py build

cd "/Users/zhiqin/ESP 部分开发/ESPC52"
idf.py build
```

After a build, verify that each generated `config/sdkconfig.h` no longer
defines `CONFIG_LV_USE_SPAN`, then perform the separately authorized device
boot and LCD registration test. No build, flash, device start, or monitor was
performed for this task.

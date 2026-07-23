# ESPC52 LCD Bootstrap Task Watchdog Repair

Date: 2026-07-21

Status: ESPC52 build passed; flash, monitor, and hardware acceptance remain
pending. This record is scoped to the ESPC52 early LCD-startup Task Watchdog
repair.

## Symptom

Observed boot log:

```text
TASK_CREATE task=lcd_bootstrap stack=4096 source=psram_static
Task watchdog got triggered:
- IDLE (CPU 0)
CPU 0: app_startup
app_orchestrator_start
components/Middlewares/app_orchestrator/app_orchestrator.c:557
vTaskDelay
```

The target is single-core ESP32-C5. `app_startup` was runnable at a priority
above IDLE0 while waiting for the LCD bootstrap's first attempt to finish. At
the reported source revision, the loop at `app_orchestrator.c:557` used
`vTaskDelay(pdMS_TO_TICKS(1U))`. With the project configured at a 100 Hz
FreeRTOS tick rate, one millisecond converts to zero ticks. `vTaskDelay(0)` is
only a yield and does not block the task until a tick, so the startup loop
repeated and starved IDLE0. Task WDT then correctly reported IDLE0 and the
running `app_startup` task.

## Original Call Chain

```text
app_main
  -> xTaskCreateWithCaps(app_startup_task, internal stack, priority 3)
  -> app_startup_task
  -> app_orchestrator_start
  -> create lcd_bootstrap
  -> while (LCD first attempt has not completed)
       vTaskDelay(pdMS_TO_TICKS(1U))
```

The LCD bootstrap was logged as a 4,096-byte PSRAM-backed static task. That
placement is unsuitable as the default for early LCD/SPI/GPIO/display-driver
initialization without a complete external-stack safety audit.

## Implemented Repair

The repair preserves LCD-first boot animation while separating it from the
subsequent hardware/business startup phase.

```text
app_main
  -> create app_startup (internal dynamic stack, priority 3)
  -> schedule lcd_bootstrap (internal static 4,096-byte stack, priority 1)
  -> schedule app_orchestrator continuation (internal dynamic 16,384-byte stack, priority 3)
  -> app_startup returns/releases its startup stack promptly
  -> lcd_bootstrap starts LCD service and boot animation
  -> lcd_bootstrap publishes READY or FAILED through an EventGroup
  -> continuation blocks once on READY/FAILED with a 30,000 ms timeout
  -> continuation starts Wi-Fi, gateway, system, BME, radar, scheduler and voice
  -> LCD UI continues displaying module state, then performs its existing Home transition
```

`app_orchestrator_start()` now creates the static EventGroup, clears only the
previous DONE bits, schedules the continuation, schedules `lcd_bootstrap`, and
returns. The continuation at `app_orchestrator.c:526` calls
`xEventGroupWaitBits()` once for READY or FAILED, with `pdFALSE` for both clear
and wait-all plus `c5_block_ticks(C5_LCD_BOOTSTRAP_WAIT_MS)`. The result bits
are durable, so READY/FAILED cannot be lost when the LCD task publishes before
the continuation begins waiting. READY, FAILED, and timeout all produce one
`BOOT_STAGE stage=orchestrator state=continuing ...` log and then invoke the
remaining startup chain. There is no LCD state-flag polling loop in
`app_startup` or `app_orchestrator_start()`.

LCD retry is bounded: retryable errors use a delay clamped to at least one tick,
with a finite attempt limit and a final FAILED publication. No Task WDT policy,
timeout, IDLE monitoring, or manual WDT-reset change is part of this repair.

The continuation sets `s_orchestrator_startup_complete` immediately before its
first `lcd_service_mark_boot_complete()` attempt. If the bounded wait timed out
and LCD subsequently becomes ready while the continuation is still finishing,
the LCD task observes that latch and makes the same one-way boot-complete call.
This closes the late-READY/UI-home-transition race without adding a wait or a
second business-startup path.

## Task Placement And Lifecycle

| Task | Priority | Stack | Source | Lifecycle expectation |
| --- | ---: | ---: | --- | --- |
| `app_startup` | 3 | 16,384 bytes | internal dynamic reclaimable | Schedules first phase, then exits through `vTaskDeleteWithCaps()` |
| `lcd_bootstrap` | 1 | 4,096 bytes | internal static | Performs only bounded LCD bootstrap/retry, records high-water mark, signals result, calls `vTaskDelete(NULL)` |
| `app_orchestrator` continuation | 3 | 16,384 bytes | internal dynamic reclaimable | Blocks once for a bounded EventGroup result, owns later module initialization, logs high-water mark, calls `vTaskDeleteWithCaps(NULL)` |

`lcd_bootstrap` must not have a high-priority busy loop. Its retry waits use an
effective block duration, and the high-water mark is logged on its ready/failed
exit so the 4,096-byte allocation can be assessed from runtime evidence rather
than increased blindly.

## Required Logs

The startup path records concise non-polling events:

```text
BOOT_STAGE stage=lcd_bootstrap state=start
BOOT_STAGE stage=lcd_bootstrap state=ready elapsed_ms=...
BOOT_STAGE stage=lcd_bootstrap state=failed err=...
BOOT_STAGE stage=orchestrator state=continuing
```

The task-create and terminal bootstrap logs also carry priority, stack source,
and stack high-water mark.

## Changed Files

Modified documentation and implementation scope:

- `ESPC52/components/Middlewares/app_orchestrator/app_orchestrator.c`
- `ESPC52/components/Middlewares/app_orchestrator/app_orchestrator.h`
- `docs/development_logs/c52_lcd_boot_wdt_fix.md`
- `docs/development_logs/README.md`

No unrelated application business logic, Task WDT policy, flash settings, or
hardware configuration is in scope.

## Risks And Rollback

- A continuation task must not be created twice or outlive its EventGroup; its
  creation/error path must publish an observable failure and release resources.
- The EventGroup must remain valid until the LCD task and its receiver cannot
  reference it. Static storage avoids allocation failure but does not remove
  lifecycle requirements.
- LCD failure must show/log a clear degraded state and must never leave startup
  indefinitely waiting for a panel that did not initialize.
- A READY result which arrives after the continuation's wait timeout must still
  reach the existing boot-complete transition. The startup-complete latch is
  set before the primary transition call so the late LCD task owns that retry.
- A rollback should revert only the task handoff/EventGroup and internal LCD
  bootstrap stack changes together. Do not retain the zero-tick polling wait.

## Verification Ledger

- [x] Reported watchdog trace and original zero-tick polling mechanism recorded.
- [x] Live source review confirms `app_startup` priority 3 and stack creation
  with `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`.
- [x] `lcd_bootstrap` now uses a 4,096-byte internal static stack at priority 1
  and logs start, ready/failed, elapsed time, source, priority, and high-water mark.
- [x] `app_orchestrator` continuation uses an internal dynamic 16,384-byte
  stack at priority 3 and blocks once on the static EventGroup for at most 30 s.
- [x] LCD retry remains finite (maximum five attempts), blocks through
  `c5_block_ticks()`, and publishes a FAILED result after its terminal path.
- [x] Source search after the repair found no `while (!s_lcd_bootstrap...)` or
  `vTaskDelay(pdMS_TO_TICKS(1U))` synchronous LCD wait in this orchestrator.
- [x] Build only, after loading ESP-IDF 5.5.4:
  `source /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh` followed by
  `cd "/Users/zhiqin/ESP 部分开发 1/ESPC52" && idf.py build`.
  The build passed; `00_Learn.bin` is `0x2277a0`, leaving 57% free in the
  smallest application partition.
- [ ] Flash, monitor, and all hardware validation are explicitly not executed.

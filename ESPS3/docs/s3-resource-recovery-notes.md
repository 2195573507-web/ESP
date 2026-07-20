# S3 Resource Recovery Notes

## Baseline

- Full build passes, but static D/IRAM is `307399 B` with `34361 B` remaining.
- Large internal BSS includes BME cache (`28032 B`), alarm engine and staged events
  (`60592 B`), alarm reporter buffers (`17536 B`), and radar log snapshots
  (`22536 B`).
- Production startup has 27 fatal `ESP_ERROR_CHECK(...init/start...)` calls.
- Ordinary application/HTTP task stacks total about `142336 B`; before Wi-Fi start,
  startup has already requested about `89088 B` of application task stacks.
- `s3_runtime_ingress_t` is `4328 B`; current generic 8-bit allocation does not
  guarantee PSRAM placement.

## Functional Contract

Home AI, environment alarms, radar source state and continuity, BME caching/replay,
voice, local HTTP, network upload/command workers, scheduler priorities, Smart Home,
and device stream behavior must remain present and compatible.

## Final Evidence

- Final ESP-IDF 5.5.4 build passes; firmware is `0x132b40` and the application
  partition remains 83% free.
- Static DIRAM fell from `307399 B` to `159615 B`; remaining DIRAM rose from
  `34361 B` to `182145 B`.
- `.bss` fell from `200600 B` to `52816 B`. The exact `147784 B` reduction is
  retained state moved from static internal RAM to explicit runtime storage.
- Production source scan finds no remaining `ESP_ERROR_CHECK` startup aborts.
- Environment alarm, Home AI, radar-domain, LD2450 core, and Smart Home host
  suites pass. `git diff --check -- ESPS3` passes.
- No flash, monitor, Server launch, or hardware/runtime acceptance was performed.

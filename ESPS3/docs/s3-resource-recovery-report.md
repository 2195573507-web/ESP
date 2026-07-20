# S3 Resource Recovery Report

Status: source implementation, host regression, and ESP-IDF build complete.

## Outcome

All 33 startup module steps remain present. Startup failures now produce
`STARTUP_MODULE_RESULT` and `STARTUP_MEMORY_CHECK` diagnostics, gate only true
dependents, and continue independent modules. The final production scan has no
startup `ESP_ERROR_CHECK` abort path, including the NVS recovery erase branch.

Large BME, alarm, radar, command, Home AI history, network, scheduler, and replay
storage uses explicit capability-aware PSRAM allocation. Six high-volume
network/scheduler queues use internal `StaticQueue_t` controls with PSRAM item
storage. Cache-off startup and NVS/SPIFFS-writing worker stacks, Wi-Fi/lwIP
resources, mutex/semaphore controls, and task TCBs remain internal.

Selected command/radar buffers and radar/voice task stacks retain guarded internal
fallbacks to preserve functionality if PSRAM allocation fails. Resource
constructors changed in this recovery pass release their owned partial tasks,
queues, files, and buffers on failure; platform subservices without a complete
teardown API are not claimed as transactionally rolled back. Capacities,
priorities, cadence, retry policy, backpressure, protocols, and algorithms were
not reduced.

## Memory Comparison

| Metric | Baseline | Final | Change |
| --- | ---: | ---: | ---: |
| Static DIRAM used | 307399 B | 159615 B | -147784 B (-48.1%) |
| Static DIRAM remaining | 34361 B | 182145 B | +147784 B |
| `.bss` | 200600 B | 52816 B | -147784 B (-73.7%) |
| Firmware binary | `0x130790` | `0x132b40` | +9136 B |

The final application partition remains 83% free.

## Functional Preservation

- Home AI room, rule, virtual-device, history, event, emergency, voice-session,
  and voice-router initialization remains enabled.
- Environment alarm capacities, histories, observer callback, FIFO, retry/ack,
  and Home AI observer semantics remain unchanged.
- Radar source/room/home state, person continuity, ingest history, diagnostics,
  local UART recovery, rate management, and log compatibility remain enabled.
- BME cache capacity, Smart Home, wake prompt, voice proxy, local HTTP, network
  upload/command workers, replay, scheduler, and device stream remain enabled.

## Verification

- ESP-IDF 5.5.4 full build and final incremental relink: PASS.
- Environment alarm host suite (6 programs): PASS.
- Home AI host suite, including five history scenarios: PASS.
- Radar-domain, radar-person-continuity, and LD2450 core host suites: PASS.
- Smart Home gateway host suite: PASS.
- 33 startup steps, production startup abort scan, WithCaps delete pairing scan,
  and `git diff --check -- ESPS3`: PASS.
- Radar ingest, local radar, Wi-Fi, emergency observer, and resource-manager
  failure paths have scoped rollback coverage; host failure injection is not
  available for these FreeRTOS/platform branches.

No device was flashed or monitored. Wi-Fi, UART/LD2450, BME690, voice, local HTTP,
server retry, PSRAM runtime behavior, and full startup remain hardware acceptance
items rather than conclusions from this build.

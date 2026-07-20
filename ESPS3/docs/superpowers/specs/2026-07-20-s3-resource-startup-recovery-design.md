# S3 Resource And Startup Recovery Design

## Goal

Make the current ESPS3 firmware complete startup under bounded internal RAM without
removing or weakening Home AI, environment alarms, radar, voice, networking, local
HTTP, device streaming, BME caching, replay, or server-facing behavior.

## Invariants

- Preserve every current module, route, protocol, queue policy, algorithm, and data
  contract.
- Keep Wi-Fi/lwIP, FreeRTOS control objects, UART/DMA resources, and cache-off task
  stacks in internal RAM.
- Place large retained histories, snapshots, JSON workspaces, applicable queue
  storage, and cache-safe worker stacks explicitly in PSRAM.
- A failed optional module logs its result and must not reboot unrelated modules.
  Constructors changed by this recovery pass roll back owned partial dynamic
  resources where a teardown path exists.
- Scheduler ingress queues are prepared before producers can publish work. Runtime
  workers start in dependency order and voice starts last.

## Architecture

Startup is split into small dependency-aware phases: core state and queue
preparation, connectivity, local ingress, sensors, analysis/dispatch, reporting,
and voice. Each module result is recorded together with internal free/largest and
PSRAM free memory. Dependent modules are gated, while independent modules continue.

Resource constructors changed in this recovery pass unwind owned dynamic resources
where the underlying platform provides a teardown path. Large buffers use explicit
capability-aware allocation so placement does not depend on generic heap preference.

## Verification

- Scan production sources for direct startup `ESP_ERROR_CHECK(...init/start...)`,
  ordinary task creation where explicit capabilities are required, and large
  internal BSS objects.
- Run environment alarm, Home AI, radar, and Smart Home host suites.
- Run a complete ESP-IDF 5.5.4 build and compare current map usage with the
  pre-change `307399 B` static D/IRAM baseline.
- Do not claim Wi-Fi, UART, BLE, BME, voice, HTTP, or full startup acceptance without
  a later flash/monitor run.

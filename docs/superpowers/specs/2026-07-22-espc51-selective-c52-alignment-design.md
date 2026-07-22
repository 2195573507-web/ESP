# ESPC51 Selective C52 Alignment

## Goal

Bring ESPC51 to the C52 LCD, environmental snapshot, startup, and diagnostics
baseline while retaining C51's physical-terminal identity and BLE radar binding.

## Scope

Synchronize the C52 implementation into the corresponding C51 components for:

- LCD bootstrap scheduling, bounded EventGroup continuation, boot-complete latch,
  and deferred-worker ordering.
- LCD snapshot fields and UI/assets for temperature, humidity, pressure, gas,
  IAQ, and network status.
- BME690 service snapshot fields, worker diagnostics, and internal-control stack
  placement.
- Radar HOME snapshot parsing of numeric `ok=0/1` values and the transport
  robustness changes that are independent of terminal identity.

Do not change:

- C51 device ID, local ID, alias, server identity, or radar board identity.
- C51 BLE radar enablement, `living_room` binding, fixed radar MAC, or source
  attribution.
- Existing equivalent C51/C52 voice, Wi-Fi/S3 gateway, I2C, IIS, and main-entry
  implementations.
- ESPS3, ESP-server, managed components, or generated build artifacts.

## Design

For implementation files whose behavior must match C52, use C52 as the source
of truth and synchronize the matching C51 file. Identity-bearing configuration
files remain C51-owned. The radar BLE transport is synchronized only where its
compile-time behavior remains controlled by C51's existing enabled binding
configuration.

The startup boundary remains non-blocking: LCD initialization reports READY or
FAILED through persistent EventGroup bits, the continuation waits at most 30
seconds, and independent services proceed in degraded mode after failure or
timeout. All LVGL updates remain owned by the LCD UI task.

## Error Handling

Task creation, LCD startup, BME startup, and radar snapshot parsing preserve
their C52 bounded/error-reporting behavior. C51 continues operating when the
LCD fails or times out. No identity fallback may remap C51 data to C52.

## Verification

- Confirm source parity for the selected implementation files.
- Confirm identity and C51 radar binding configuration remain intentionally
  different.
- Build ESPC51 with ESP-IDF 5.5.4 in an isolated build directory.
- Do not flash, monitor, or claim hardware acceptance.

## Acceptance Criteria

- C51 compiles with the C52 LCD/environment/startup behavior.
- C51 retains `sensair_shuttle_01`, local ID 1, `living_room`, its current
  fixed BLE MAC, and enabled BLE radar default.
- The remaining C51/C52 source differences are identity configuration and any
  intentionally excluded generated artifacts only.

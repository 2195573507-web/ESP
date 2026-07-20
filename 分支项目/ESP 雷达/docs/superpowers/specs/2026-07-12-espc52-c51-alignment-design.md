# ESPC52-C51 Alignment Design

## Goal

Make ESPC52 match ESPC51's current runtime behavior while retaining the
configuration values that identify ESPC52 as the second C5 terminal.

## Scope

Only `ESPC52` firmware sources are changed. ESPS3, ESP-server, frontend, and
ESPC51 are not modified.

## Alignment Changes

- Add the IIS DMA heap diagnostic used by ESPC51 before I2S channel creation.
- Add `spiffs` to ESPC52's middleware component requirements.
- Restore the shared voice timeout aliases and PSRAM-first, internal-heap
  fallback used for the local voice upload buffer.
- Restore the shared voice request timeout macro in ESPC52's protocol-common
  header.
- Replace ESPC52's streaming wake-prompt implementation with ESPC51's
  SPIFFS-temporary-file playback flow, including its cleanup and diagnostics.

## Required C52 Differences

The following remain C52-specific and are explicitly excluded from copying:

- `SERVER_COMM_DEVICE_ID=sensair_shuttle_02`
- `TERMINAL_CONFIG_DEFAULT_DEVICE_ID=sensair_shuttle_02`
- `TERMINAL_CONFIG_DEFAULT_LOCAL_ID=ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52`
- `TERMINAL_CONFIG_DEFAULT_ALIAS=SensaiShuttle02`
- CSI link `S3_TO_C52` and local report ID `2`
- Hardware, board, and SDK configuration differences required by C52.

## Error Handling

The copied wake-prompt path must close the HTTP stream, remove its temporary
file, and return an error so the existing short-beep fallback remains active.
Voice buffer allocation must report failure only after both PSRAM and internal
heap allocation attempts fail.

## Verification

1. Compare C51 and C52 source trees while excluding generated build output;
   only the documented identity and hardware differences may remain.
2. Run `git diff --check -- ESPC51 ESPC52`.
3. Build ESPC51 and ESPC52 independently with the installed ESP-IDF 5.5.4
   Python environment. No flashing is part of this work.

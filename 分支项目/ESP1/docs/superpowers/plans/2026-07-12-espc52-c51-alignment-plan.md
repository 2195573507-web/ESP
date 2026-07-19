# ESPC52-C51 Alignment Plan

## Goal

Align ESPC52 runtime behavior with ESPC51 while preserving C52 identity,
hardware, and SDK configuration differences.

## Phases

- [x] Phase 1: Inspect C51/C52 differences and agree on scope.
- [x] Phase 2: Write and approve the alignment design.
- [x] Phase 3: Mirror the approved C51 runtime changes into ESPC52.
- [x] Phase 4: Verify remaining source differences and whitespace.
- [x] Phase 5: Build ESPC51 and ESPC52 without flashing.
- [x] Phase 6: Record results and deliver.

## Change Set

1. Synchronize IIS heap diagnostics in `components/BSP/IIS/iis.c`.
2. Add the `spiffs` middleware dependency.
3. Synchronize voice timeout compatibility and PSRAM allocation fallback.
4. Synchronize the shared voice request timeout macro.
5. Synchronize wake-prompt temporary SPIFFS spool playback.

## Guardrails

- Change only `ESPC52` firmware source files.
- Preserve all documented C52 identity and CSI link values.
- Do not flash hardware or create a git commit.
- Do not alter ESPS3, ESP-server, frontend, ESPC51, or generated build files.

## Results

- The post-change source comparison retains only C52 identity values, its CSI
  link/report ID, and editor settings.
- `git diff --check -- ESPC51 ESPC52` passed.
- ESPC51 and ESPC52 both completed `idf.py build` with ESP-IDF 5.5.4.

# Radar End-to-End Isolation Audit Plan

## Goal

Audit and repair source/device/room/track isolation across ESPS3, ESPC51,
ESPC52, and ESPS3-Radar-Debug without changing coordinate transforms, zone
maps, tracker core algorithms, ESP-server, BME, voice, or command modules.

## Phases

- [x] Establish current source-of-truth paths and identity registry
- [x] Repair firmware identity-bearing diagnostics and real-time HOME aggregation
- [x] Harden Mac parser/state source and room binding
- [x] Add cross-source regression fixtures and tests
- [x] Run firmware/Mac verification and write audit/fix reports

## Status

Current phase: complete; all requested software checks passed. Hardware/runtime
acceptance remains explicitly pending.

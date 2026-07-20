# Task Plan: C5 CSI Feature Schema Refactor

## Goal

Make ESPC51 and ESPC52 stable CSI feature generators that emit one aligned, structured feature frame per ready CSI frame.

## Constraints

- Do not change I/Q to amplitude conversion.
- Do not change P30-P70 variance-ranked subcarrier selection.
- Do not change Hampel/median/EWMA filtering behavior.
- Do not change calibration convergence timing or logic.
- Do not change motion score formulas or move fusion logic onto C5.
- Do not output raw CSI, I/Q, phase, selected subcarriers, or subcarrier-level data.
- Do not change the current time model or add ping/tick mechanisms.

## Phases

- [x] Phase 1: Inspect live C51/C52 CSI feature and publish paths.
- [x] Phase 2: Confirm C51/C52 CSI source parity before edits.
- [x] Phase 3: Add structured feature-frame schema on C5.
- [x] Phase 4: Keep S3 parsing compatible with the structured C5 schema without changing fusion.
- [x] Phase 5: Run parity, boundary scans, and build/static validation.

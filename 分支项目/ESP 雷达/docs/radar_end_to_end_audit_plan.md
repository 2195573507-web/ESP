# Radar End-to-End Data Isolation Audit Plan

## Goal

Audit the S3_LOCAL, C51, and C52 LD2450 data paths, including the macOS
debug tool. Document evidence, risks, and minimal remediation without
rewriting the spatial algorithms.

## Phases

- [x] Establish scope, current worktree boundary, and prior contract evidence.
- [x] Inspect C51/C52 identity, BLE, parsing, and result serialization.
- [x] Inspect S3 local/remote ingest, adapters, spatial state, diagnostics, and HTTP API.
- [x] Inspect macOS parser, store, visual separation, and fixtures.
- [x] Compare field contracts and source-isolation tests.
- [x] Write the two requested audit reports.
- [x] Run S3/C51/C52 IDF and macOS SwiftPM builds, then record results.

## Audit Boundary

Existing uncommitted files are the audit target. This audit does not revert or
rewrite them. Core spatial algorithm files are read-only in this phase.

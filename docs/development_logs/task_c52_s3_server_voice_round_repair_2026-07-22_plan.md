# Task Plan: C52/S3/Server Voice Round Repair

## Goal

Repair the specified wake-to-answer voice transaction without expanding into device validation.

## Progress

- [x] Read task evidence and establish forbidden-change boundaries.
- [x] Identify active C52, S3, and Server source roots; preserve existing dirty changes.
- [x] Start parallel workstreams and create the task development record.
- [x] Document the PCM Transport + WakeNet source mapping: FIFO-owned sequence lifecycle, the
  16000 Hz mono pcm16le contract, session reset/clean behavior, and model-frame reassembly.
- [x] Review workstream modifications and resolve cross-owner conflicts.
- [x] Add complete source-level root-cause and state-chain evidence to the record.
- [x] Run isolated C52 and S3 `idf.py build` plus minimal Server static validation.
- [x] Complete final diff/diagnostic review and report validation limits.

## Current Phase

Complete: source/build validation only; hardware/runtime acceptance remains intentionally out of
scope.

## Validation Boundary

No flashing, serial monitor, microphone capture, real WakeNet detection, real server turn, or
end-to-end voice test will be run in this task. The PCM Transport + WakeNet documentation is based
on current source inspection only; it is not runtime or hardware validation.

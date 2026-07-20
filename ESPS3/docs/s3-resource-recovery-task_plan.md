# S3 Resource Recovery Task Plan

## Goal

Resolve S3 allocation and initialization failures quickly with parallel agents and
zero functional reduction.

## Phases

- [x] Audit current startup chain, map, task stacks, queues, and failure paths.
- [x] Approve the capability-aware staged-startup design.
- [x] Parallel implementation: startup, alarm/BME, network/runtime.
- [x] Integrate radar, voice, Home AI cleanup, and resolve cross-module issues.
- [x] Run static scans, host suites, full build, and map comparison.
- [x] Publish final resource and functionality-preservation report.
- [x] Converge post-audit Wi-Fi, radar, emergency, resource, and radar-ingest
  rollback paths.
- [x] Re-run full build, size/map comparison, and all key host suites after the
  rollback convergence pass.

## Boundaries

- Do not revert existing uncommitted work.
- Do not modify C51, C52, ESP-server, Dashboard, or the macOS radar tool.
- Do not flash, erase, monitor, or start a Server.
- Do not commit unless explicitly requested.

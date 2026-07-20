# ESP-server Backend Audit Plan - 2026-06-09

## Goal
Audit the ESP-server backend broadly, fix confirmed backend issues, avoid frontend changes, and update Codex memory.

## Scope
- In scope: backend JavaScript, backend docs, scripts, tests, route contracts, storage, jobs, command/device/memory services, runtime validation.
- Out of scope: `public/`, dashboard frontend assets, firmware, real deployment, remote push, and unrelated worktree cleanup.

## Phases
- [x] Phase 1: Confirm scope, existing worktree state, and memory constraints.
- [ ] Phase 2: Inventory backend code paths and current uncommitted changes.
- [x] Phase 3: Audit route contracts, data validation, database writes, background jobs, and service boundaries.
- [x] Phase 4: Run syntax checks, tests, smoke regression, and boundary checks.
- [x] Phase 5: Fix confirmed backend defects.
- [ ] Phase 6: Re-run verification, write audit report, and update memory.

## Progress Notes
- Started from branch `codex/esp-server-backend-refactor`.
- Existing uncommitted backend/doc/script changes were present before this audit and must be preserved.
- `public/` and `db/database.db` are protected boundaries for this task.

# Task Plan: ESP-server Missing Backend Interfaces

## Goal
Complete the backend API surfaces required by the frontend for real logs, alarms, smart-home state/control, natural-language commands, cleanup, and SSE events in `/Users/zhiqin/ESP-111/ESP-server`.

## Constraints
- Do not edit `public/`.
- Preserve existing routes and compatibility.
- Prefer unified `/api/*` envelope responses.
- Do not return mock data; return empty arrays or explicit unavailable state when no real data exists.
- Ensure/migrate SQLite tables at startup.
- Verify with syntax checks, tests, curl smoke, and `public/` diff proof.

## Phases
- [x] Phase 1: Read task attachment and current backend state.
- [x] Phase 2: Implement missing persistence and route surfaces.
- [x] Phase 3: Wire event logging and SSE broadcasts.
- [x] Phase 4: Run required verification.
- [x] Phase 5: Audit every requirement and close out.

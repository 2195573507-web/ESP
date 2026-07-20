# ESP-111 API Boundary v1 Refactor Plan

## Goal

Converge ESP-server and ESPS3 onto `docs/api-boundary-v1.md`: unified gateway trust, gateway-device binding, command ownership/state semantics, event log source of truth, mock isolation, frontend legacy inventory, and ESPS3-only Server-facing `/api/*` traffic.

## Constraints

- Do not modify ESPC51 or ESPC52 firmware.
- Do not change frontend UI; endpoint-string migration and dependency scans in `ESP-server/public` are allowed for this task.
- Do not add non-v1 external APIs.
- Server is the business truth.
- ESPS3 is the only Server-facing gateway.
- Preserve unrelated dirty worktree changes.

## Workstreams

- [x] Read boundary document and current project/memory context.
- [ ] GatewayAuth Agent: implement gateway auth + device binding checks for Server write/poll/ack APIs.
- [ ] Command Agent: converge command state semantics and ACK ownership.
- [ ] Event Agent: route voice/command/device/smart-home event writes through `event_logs`.
- [ ] Mock Agent: isolate mock data with `source=mock`, prevent mock success/persistence.
- [x] Frontend Agent: migrate `public` dashboard reads to `/api/dashboard/v1/*` and report legacy dependencies without UI redesign.
- [ ] ESPS3 Agent: ensure ESPS3 is sole `/api/*` gateway and C5 remains `/local/v1/*`.
- [ ] Integration pass: reconcile non-overlapping patches, run backend/firmware-safe verification.
- [ ] Final report with changed files, tests, residual risks, and legacy inventory.

## Status

Started 2026-06-17. Current phase: first agent wave plus independent frontend/ESPS3/mock boundary passes.

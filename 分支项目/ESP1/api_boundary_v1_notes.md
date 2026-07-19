# ESP-111 API Boundary v1 Notes

## Initial Findings

- `docs/api-boundary-v1.md` is the active boundary source for this task.
- `ESP-server` is the only nested git repo; the integration root is not a git repo.
- `ESP-server/public` is not a UI-change surface. Endpoint-string migration and scans only.
- `ESPC51/` and `ESPC52/` are explicitly out of edit scope.
- Current server routes already include dashboard v1, device v1, command, smart-home v1, event/log v1, and legacy `/sensor`/`/asr`/`/llm`.
- Current backend worktree was dirty before this run; existing dirty files must not be treated as necessarily produced by this task.

## Boundary Spec Highlights

- Gateway write/poll/ack APIs need gateway authentication and gateway-device binding.
- ACK must validate command existence, device ownership, gateway ownership, allowed status, and idempotency.
- v1 command state vocabulary is `created`, `queued`, `dispatched`, `acknowledged`, `succeeded`, `failed`, `expired`, `rejected`.
- `event_logs` should be the unified event fact source for voice, command, device, and smart-home events.
- Public dashboard reads must move from legacy `/sensor/latest`, `/asr/latest`, `/llm/latest` to `/api/dashboard/v1/*`.
- Mock/fallback data must be explicit `source=mock` and must not enter real success state or durable success semantics.

## Frontend Boundary Pass

- Runtime `ESP-server/public` fetches now use:
  - `/api/dashboard/v1/sensors/latest`
  - `/api/dashboard/v1/asr/latest`
  - `/api/dashboard/v1/llm/latest`
- Post-migration scan found no runtime public calls to legacy `/sensor`, `/asr`, `/llm`, forbidden `/api/device`, `/api/commands`, `/api/smart-home`, or `/local/v1`.
- UI layout and controls were not changed.

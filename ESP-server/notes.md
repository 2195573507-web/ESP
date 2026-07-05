# Notes: ESP-server Missing Backend Interfaces

## Current Findings
- The task attachment requires logs/alarms, smart-home status/control, natural-language command persistence, cleanup APIs, and expanded SSE event types.
- `public/` currently has no diff.
- Existing uncommitted backend work already adds partial `event_logs`, `eventRoutes`, unified envelope helper, and SSE service. These should be extended rather than replaced.
- The existing event log route currently has GET endpoints only; POST, cleanup, specific response shapes, and smart-home APIs still need implementation.
- Prior project memory says backend verification should use temporary SQLite for smoke where possible and separately prove `public/` and real DB boundaries.


# ESP-server Backend Audit Report - 2026-06-09

## Scope
This audit covered the backend in `/Users/zhiqin/Projects/ESP/ESP-server`: route contracts, input validation, SQLite migrations, soft-delete behavior, memory jobs, command/device routes, voice/text LLM paths, admin-protected user-data deletion, dependency security, regression coverage, and protected-file boundaries.

The frontend boundary was preserved. No `public/` files were changed, and the real `db/database.db` was not modified.

## Fixed Issue

### Invalid calendar dates accepted by memory/job APIs

Affected paths:
- `POST /api/memory/daily`
- `GET /api/memory/daily`
- `POST /api/jobs/daily-summary/run`
- `POST /api/jobs/weekly-profile/run`
- `GET /api/jobs/memory`

Problem:
- These paths accepted any string matching the `YYYY-MM-DD` shape.
- Invalid calendar dates such as `2026-02-31` could pass validation.
- Daily and weekly job date math could then normalize the invalid date into a different real date, producing an incorrect aggregation window or target memory date.

Fix:
- Added `src/utils/date.js` with strict `YYYY-MM-DD` calendar validation.
- Reused it in memory storage, memory routes, and memory jobs.
- Added smoke-regression assertions for invalid calendar dates.
- Updated API docs to state that date fields must be valid calendar dates.

## Audit Results

### Route and API Contracts
- Machine API 404 behavior remains JSON-only for API-like paths.
- Voice body parser handling remains separated from JSON body parsing.
- Memory, command, device, record, sensor, and user-data routes preserve existing response shapes while adding soft-delete filtering.

### Database and Migration Safety
- Legacy schema migration is covered by smoke regression.
- New `deleted_at`, `delete_reason`, and `updated_at` columns are added through the existing migration helper.
- Unique-index creation still skips unsafe legacy duplicate rows rather than breaking startup.
- Soft-delete filtering was checked across memory, agent state, command history, latest record, sensor, device status, and prompt context reads.

### User-Data Deletion
- Admin token protection is enforced for summary, preview, delete, and deletion-run listing.
- Delete requires `confirm: "DELETE"`.
- Soft delete is transactional.
- Regression coverage confirms rollback when a table update fails.
- Protected tables such as `device_capabilities` are not deleted by user-data scopes.
- Audit-log soft delete can hide prior audit runs while keeping the current run visible.

### Memory Jobs
- Daily and weekly jobs read only non-deleted input rows.
- Duplicate daily/weekly summary generation is skipped unless `force` is true.
- `dry_run` returns computed stats without writing memory/job records.
- Date handling is now strict and covered by tests.

### Dependency Security
- `npm audit --registry=https://registry.npmjs.org --omit=dev --json` reported 0 vulnerabilities.
- The local npm mirror did not support the audit endpoint, so the audit was rerun against the official npm registry.

## Verification
- `node --check server.js`: passed.
- Full JS syntax sweep over `src`, `scripts`, and `server-time-sync`: passed.
- `npm test` / `node scripts/smoke-regression.js`: passed against a temporary SQLite database.
- `git diff --check`: passed.
- `npm audit --registry=https://registry.npmjs.org --omit=dev --json`: 0 vulnerabilities.
- `git diff -- public db/database.db`: no output.
- `git status --short -- public db/database.db`: no output.

## Residual Risk
- The audit did not perform live calls to real Volc/OpenAI-compatible upstream services; smoke regression uses mock LLM and `VOICE_TURN_MOCK=1`.
- Existing uncommitted backend changes were present before this audit and were preserved rather than reverted.

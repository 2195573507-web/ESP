# ESP-server Backend Audit Notes - 2026-06-09

## Working Boundaries
- Do not modify `public/`.
- Do not mutate or stage the real `db/database.db`.
- Treat existing uncommitted changes as prior context, not as disposable state.

## Evidence Log
- Initial git status showed existing modifications in backend docs, scripts, `server.js`, and backend `src/` files.
- Memory constraints emphasize backend-only work, temp-DB smoke tests, `node --check`, `npm test`, full JS syntax sweeps, `git diff --check`, and `git diff -- public db/database.db`.
- Verification passed: `node --check server.js`, full JS syntax sweep, `npm test`, `git diff --check`, official-registry `npm audit`, and `git diff -- public db/database.db`.

## Findings
- Confirmed issue: memory date inputs only checked the `YYYY-MM-DD` shape. Invalid calendar dates such as `2026-02-31` could pass validation and then be normalized by JavaScript date math in daily/weekly jobs. Fixed with a shared strict date utility and smoke-regression coverage for memory daily filters, daily summary jobs, weekly profile jobs, and memory job filters.

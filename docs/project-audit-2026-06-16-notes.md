# ESP-111 Project Audit Notes - 2026-06-16

## Scope Baseline

- Integration root: `/Users/zhiqin/ESP-111`.
- The integration root is not a git repository.
- `ESP-server` is its own git repository and has pre-existing dirty/untracked backend files at audit start.
- Existing root-level `task_plan.md` and `notes.md` are prior backend integration artifacts, not this audit's notes.

## Hard Boundaries

- ESPS3 must own server-facing `/api/...` traffic.
- C5 firmware must stay on local ESPS3-facing `/local/v1/...` traffic.
- CSI must remain summary-only; raw CSI must not be uploaded to the backend.
- Smart-home ACK must not report success when no real device exists.
- Voice activity must not block heartbeat, snapshot, logs, or core telemetry.
- `ESP-server/public` and `ESP-server/db/database.db` are out of scope for modification.

## Findings Log

Findings will be promoted to the final report only after file/line evidence is verified.

### Candidate Findings - Security Agent Pasteur

- High: Backend binds with `app.listen(PORT)` and most machine write APIs lack unified authentication; only user-data deletion has explicit admin token enforcement.
- High: ESPS3 server uplink uses hardcoded plaintext `http://124.221.162.188:3000` and sends identity headers without TLS or request signing.
- High: SoftAP password and local identity constants are hardcoded; ESPS3 local HTTP allowlist checks identify device IDs but do not authenticate per-device secrets.
- Medium: Firmware production security options appear disabled in sdkconfig: secure boot, NVS encryption, flash encryption, and debug access protections.
- Medium: Logs/DB retain LLM and voice text previews; masking covers API keys but not user prompt/transcript content.
- Low: `cors` dependency exists but no explicit CORS/security middleware is configured in `server.js`.
- Low: `tools/csi-debug-web/package-lock.json` resolves packages from `registry.npmmirror.com`, while main server uses npmjs.

### Candidate Findings - C5 Agent Mendel

- High: C5 NVS configuration can override `gateway_ssid`, `gateway_pass`, and `gateway_ip`; communication rejects absolute endpoints but does not independently prove the configured base host is the ESPS3 SoftAP.
- Medium: C51/C52 CSI defaults diverge: C51 enables CSI service by default while C52 disables it; upload behavior also differs.
- Low: `app_time_sync` builds an absolute URL but the shared C5 HTTP layer rejects absolute `http(s)://` endpoints, so enabling time sync may fail.
- Low: protocol declares `rn` room-name field but C5 BME/CSI payloads currently send `rid` without `rn`.

### Main-Thread Candidate Findings

- High: ESPS3 defaults `GATEWAY_CONFIG_ENABLE_CSI_TRIGGER=1` and `GATEWAY_CONFIG_ENABLE_CSI_RESULT_INGEST=1`, while multiple docs/checklists say CSI is placeholder/default-disabled or not uploaded by default.
- Medium: C5 voice exclusive mode pauses heartbeat/status/command poll/BME and ordinary local gateway HTTP; S3 may continue snapshots/logging, but terminal-origin telemetry is intentionally suppressed during voice turns.

### Candidate Findings - ESPS3 Agent Hegel

- High: `command_router_ack()` can mark a C5 command ACK as locally accepted during `voice_busy` and return `ESP_OK` without calling `server_client_ack_command()`, so C5 sees success while Server may never receive the ACK.
- Medium: BME/CSI ingest and dashboard snapshot uploads are one-shot best-effort; failed Server forward is recorded in `offline_policy` but does not create an outbox or make C5 retry.
- Medium: smart-home no-device ACK correctly reports `status=failed`/`applied=false`, but ACK upload failures are ignored by the poll loop and have no retry path.
- Low: gateway event reports are not voice-gated, but a global 10s rate limit plus no retry can drop dense system/voice/command events.
- Low: command ACK endpoint path interpolates `command_id` without URL encoding.

### Candidate Findings - Backend Agent Wegener

- High: ESP-server mounts most write/control routers without auth; user-data deletion is the only route family with explicit admin token middleware.
- High: log cleanup and event deletion routes can physically delete audit logs without admin middleware.
- High: command ACK and smart-home ACK are keyed by `command_id` only and are not bound to the claiming device/gateway identity.
- Medium: `all_user_data` deletion does not cover smart-home and natural-language command tables even though those tables hold user/device command history.
- Medium: `/api/voice/prompt/config` can persistently modify voice prompt/TTS config without auth.
- Low: legacy `/sensor` can accept arbitrary `payload_type` into raw payload fields and may pollute records outside the v1 ingest enum.

### Candidate Findings - Build Agent Lagrange

- Medium: ESPS3 manifest/lock only declares `idf`, but `managed_components/` contains extra ESP-SR/DSP/cJSON component directories, creating stale dependency audit noise.
- Medium: C5 manifests allow `idf >=4.1.0` while lockfiles resolve IDF 5.5.4 and target `esp32c5`; a clean dependency resolution without lock may select an unsupported range.
- Medium: `sdkconfig` is ignored but currently carries target and partition-critical state; clean rebuilds depend on external `set-target`/defaults discipline.
- Medium: existing build artifacts contain absolute local paths and compile-time date metadata; they are not reproducibility proof.
- Medium: `terminal_config` is compiled through `server_comm`, blurring component ownership.
- Low: C51/C52 both produce `00_Learn.bin`, which is easy to mix up in release/flash artifacts.
- Low: C5 factory-only partitioning has no OTA/rollback reserve.

### Candidate Findings - Supply Chain Agent Pasteur

- Medium: `tools/csi-debug-web` lockfile resolves from `registry.npmmirror.com` while ESP-server resolves from npmjs, splitting supply-chain source policy.
- Medium: native addon dependency chains (`sqlite3`, serialport bindings) execute install scripts or use prebuilt artifacts during install.
- Medium: `prebuild-install` is marked no longer maintained in the ESP-server dependency chain.
- Low: `cors` is declared but unused, increasing dependency surface and suggesting a CORS policy that is not actually applied.
- Low: `npm start` touches real runtime DB/cache, while smoke tests use a temp DB; docs/CI should avoid using `npm start` as verification.

### Candidate Findings - Voice Agent Carson

- High: SoftAP password is a shared source constant reused by ESPS3 and both C5 terminals; possession of source/firmware reveals the local network credential.
- High: voice routes are mounted without auth; `/api/voice/turn` accepts large raw PCM and `/api/voice/prompt/config` can be persistently modified.
- High: HTTP TTS reads the full upstream response into memory before parsing, so oversized or malformed TTS responses can exhaust Node heap before validation.
- Medium: realtime WebSocket audio accumulates fragments/deltas before `Buffer.concat()` and lacks practical total-byte caps.
- Medium: C5 voice busy mode pauses heartbeat/status/command poll/BME/local gateway HTTP, which conflicts with the desired invariant that voice turns not block heartbeat/snapshot/log/reporting.

### Second-Round Findings - Reliability/Contracts/Docs

- High: `command_router_ack()` returns `ESP_OK` even when the Server ACK upload fails in normal non-voice cases; `offline_policy` records failure but command queue state is already terminal/local-success.
- High: No project-level CI/release/build script was found to cleanly build ESPS3/ESPC51/ESPC52; docs use manual commands and devcontainer defaults to floating `latest`.
- High: Build docs hardcode a local ESP-IDF path and omit the known bundled IDF Python env fallback required in this environment.
- High: CSI default docs say trigger/ingest/service are default-off, but current ESPS3 and C51 macros default them on.
- Medium: heartbeat/status runtime uses POST `/local/v1/health` instead of dedicated canonical `/heartbeat` and `/status` routes, while S3 registers all three.
- Medium: docs say C51/C52 are logically identical/runtime-config-only, but current compile-time identity/CSI behavior differs.
- Medium: old S3 migration docs still describe ESPS3 as an old C5-copy/esp32c5 project without a prominent historical status marker.
- Medium: deployment/local-test docs and curl examples can write the real default DB unless `ESP_SERVER_DB_PATH` is set to a temp database.
- Medium: smart-home failed ACK and sensor/CSI ingest have usable idempotency keys (`command_id`, `device_id/payload_type/seq`) but no saved payload/outbox for retry.
- Medium: `offline_policy` is a global last-result health flag, not a per-path delivery ledger, so later success can hide earlier undelivered ACK/ingest/log failures.

## Verified Positive Findings

- No direct ESPS3 local route exposes `/api/...` to C5; `local_http_server` route registration is `/local/v1/...`, and server-facing endpoints are centralized in `server_client`.
- Smart-home gateway does not fake success when no real device is attached; it builds a failed ACK with `applied=false`.
- Backend SQL searches found parameterized query usage in the reviewed command/event/user-data paths; dynamic identifiers in user-data deletion use `quoteIdentifier`.
- `npm audit --registry=https://registry.npmjs.org --omit=dev --json` for `ESP-server` returned 0 production vulnerabilities.

## Command Log

- `rg --files` inventory excluding common binary/image/map artifacts.
- `find ... AGENTS.md/README/package/CMake/sdkconfig` boundary scan.
- `git -C ESP-server status --short` showed existing backend dirty state.
- Narrow contract/security/build searches are in progress.
- Created `docs/project-audit-2026-06-16-task-plan.md`, `docs/project-audit-2026-06-16-notes.md`, and `docs/project-audit-2026-06-16-report.md` as audit artifacts only.

## Consolidation Pass - Report Draft 1

- Promoted the first verified set of findings into `docs/project-audit-2026-06-16-report.md`.
- Report now covers: auth/identity gaps, ACK binding, firmware secrets/transport, delivery outbox gaps, CSI default drift, voice memory/auth, C5 NVS gateway target, Dashboard mock/live contract drift, user-data deletion scope gaps, temp-DB documentation risk, sdkconfig production security, and test coverage gaps.
- Pascal frontend/public agent returned a completed pass. Key promoted findings: S3 page and Dashboard rely on mock/legacy data; mock smart-home appliances may be displayed as operational state; C51/C52 tabs do not bind to device-specific reads.
- Main-thread verification confirmed `ESP-server/public` and `ESP-server/db/database.db` remain untouched by diff/status checks.
- Final follow-up: merge the last completed Kant/Einstein/Newton/Pascal/Carson/Wegener returns into this report, then stop. No new audit goal is requested for this thread.

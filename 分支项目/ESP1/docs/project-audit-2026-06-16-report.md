# ESP-111 Project Audit Report - 2026-06-16

## Executive Summary

This is a read-only, multi-agent audit of `/Users/zhiqin/ESP-111`. It covers ESPS3 gateway firmware, ESPC51/ESPC52 terminal firmware, ESP-server backend routes/services/DB contracts, Dashboard frontend behavior, build/release configuration, test coverage, and documentation drift.

The largest risks are identity/authentication gaps on ESP-server write APIs, hardcoded plaintext network credentials and server URL in firmware, local-success semantics that can hide failed Server delivery, CSI/default behavior diverging from the docs, and Dashboard mock data that can look like real system state.

No implementation, frontend asset, or real database change was made by this audit. Only audit files under `docs/` were written. Static evidence does not prove runtime, hardware, or deployment behavior.

## Scope And Verification Boundary

- Integration root: `/Users/zhiqin/ESP-111`.
- `/Users/zhiqin/ESP-111` is not a git repository; `ESP-server` is its own git repository.
- Audit artifacts written: `docs/project-audit-2026-06-16-task-plan.md`, `docs/project-audit-2026-06-16-notes.md`, and this report.
- `ESP-server/public` and `ESP-server/db/database.db` were not modified; `git -C ESP-server status --short -- public db/database.db` and `git -C ESP-server diff -- public db/database.db` returned no output.
- No firmware build, server runtime, hardware flash, or end-to-end device test was run in this audit pass.
- `npm audit --registry=https://registry.npmjs.org --omit=dev --json` was run for `ESP-server` and reported 0 production vulnerabilities.

## High Severity Findings

### H1. ESP-server write/control APIs mostly lack authentication and gateway identity binding

Evidence:
- `ESP-server/server.js:120-131` mounts LLM, command, device ingest, dashboard, smart-home, event, memory, agent-state, record, and sensor routers without a common auth middleware.
- `ESP-server/server.js:256` calls `app.listen(PORT)` without an explicit host binding.
- `ESP-server/src/routes/userDataRoutes.js:32-48` and `ESP-server/src/services/userDataService.js:629-649` show user-data routes are the one reviewed family with explicit admin-token enforcement.
- `ESP-server/src/routes/commandRoutes.js:115-187` exposes capabilities, enqueue, pending, and ACK flows without caller auth.
- `ESP-server/src/routes/smartHomeRoutes.js:31-99` exposes smart-home state/control/pending/ACK without caller auth.
- `ESP-server/src/routes/eventRoutes.js:42-58`, `86-96`, `135-158`, and `173-176` expose log write, cleanup/delete, and SSE subscribe flows without auth.
- `ESP-server/src/routes/memoryRoutes.js:237-317` exposes conversation/memory/profile/correction/job-run writes and reads without auth.
- `ESP-server/src/routes/agentStateRoutes.js:239-435` exposes environment profile, experience/relation memory, reminders, emergency, CSI behavior, LCD status, and LCD display writes without auth.

Impact:
Any network client reaching the service can spoof ESPS3/C5 metadata, enqueue or ACK commands, mutate smart-home state, upload device records, write or delete logs, and subscribe to event streams. This conflicts with the intended ESPS3-only Server-facing gateway boundary.

Recommendation:
Add a gateway identity layer before all device-origin write APIs: token/HMAC/mTLS or deployment-specific equivalent. Bind `gateway_id` and allowed child `device_id` to the authenticated principal. Split route roles into public read, dashboard read, gateway write, device write, and admin delete scopes.

### H2. Device identity is trusted from body/header/query strings

Evidence:
- `ESP-server/src/services/deviceMetadata.js:145-193` accepts `device_id` from body aliases, headers, query, or fallback input.
- `ESP-server/src/routes/deviceRoutes.js:102-122` sends `/api/device/v1/ingest` body/headers/query into ingest services.
- `ESP-server/src/services/dashboardService.js:611-624` accepts gateway snapshot bodies into the in-memory/latest dashboard state after normalization, without tying the body to an authenticated gateway.
- `ESPS3/components/Middlewares/server_client/server_client.c:126-144` sets identity headers, but there is no signing/TLS client identity shown in the firmware client.

Impact:
Server state can be polluted by forged `device_id`/`gateway_id`. C5 direct-to-Server calls or external callers can bypass ESPS3 child registry, local command compression, and offline policy.

Recommendation:
Stop using raw request metadata as authority. Server should derive gateway identity from auth, maintain `gateway_id -> child device_id` allowlists, and reject body/header/query identities that do not match the authenticated gateway.

### H3. Command and smart-home ACKs are not bound to claiming device/gateway

Evidence:
- `ESP-server/src/routes/commandRoutes.js:175-187` ACKs by `:command_id`.
- `ESP-server/src/commands/queue.js:371-383` updates `command_queue` where `command_id=?` and status is `queued` or `dispatched`, with no `device_id`, `gateway_id`, or dispatch-owner condition.
- `ESP-server/src/commands/queue.js:295-341` makes dispatched commands visible again after timeout, but returns no lease or attempt token for the ACK path.
- `ESP-server/src/routes/smartHomeRoutes.js:81-99` claims by query `gateway_id` and ACKs by `:command_id`.
- `ESP-server/src/services/smartHomeService.js:324-327` only filters by gateway when `filters.gateway_id` is truthy, so an empty gateway query reaches all queued commands.
- `ESP-server/src/services/smartHomeService.js:340-366` claims queued commands and may set empty `gateway_id` from query.
- `ESP-server/src/services/smartHomeService.js:407-424` updates any smart-home command by `command_id`, with no gateway binding and no terminal-state guard.

Impact:
An unauthenticated, wrong, or missing gateway can claim or complete commands it does not own. Smart-home ACKs can overwrite commands even if they were never dispatched to that gateway or are already terminal.

Recommendation:
Make pending/ACK identity come from gateway auth, not query/body. Require `gateway_id` or filter empty values explicitly to `gateway_id=''`. Use update conditions such as `WHERE command_id=? AND gateway_id=? AND status='dispatched'`. Define idempotent duplicate ACK behavior or return 409 for invalid state transitions.

### H3a. Command and natural-language writes lack idempotency keys

Evidence:
- `ESP-server/src/routes/commandRoutes.js:145-157` enqueues every POST `/api/commands` request.
- `ESP-server/src/commands/queue.js:201-227` generates a fresh command ID and inserts a new `command_queue` row for each enqueue.
- `ESP-server/src/services/naturalLanguageCommandService.js:57-93` creates a new `nlcmd_*` row for each natural-language request.
- `ESP-server/src/db/voiceTurns.js:58-80` inserts voice turn records with `request_id` stored as data, not as a uniqueness constraint.

Impact:
Client retries, repeated clicks, or replayed requests can generate duplicate device commands, duplicate natural-language intents, and duplicate voice turns. Once natural-language commands are connected to execution, this can become repeated physical action.

Recommendation:
Add request-level idempotency keys for command, natural-language, and voice turn writes. Store them with unique constraints and return the prior result on safe retry. For command polling, add a lease/attempt token and require it on ACK.

### H4. Firmware hardcodes plaintext Server URL and WiFi credentials

Evidence:
- `ESPS3/components/Middlewares/gateway_config/gateway_config.h:72-73` hardcodes `http://124.221.162.188:3000`.
- `ESPS3/components/Middlewares/gateway_config/gateway_wifi_credentials.h:26-29` contains a concrete STA credential entry.
- `shared_components/esp111_protocol_common/include/esp111_protocol_common.h:36-38` hardcodes SoftAP SSID/password/IP.
- `ESPC51/components/Middlewares/terminal_config/terminal_config.h:46-55` uses the shared SoftAP defaults.
- `ESPC51/components/Middlewares/terminal_config/terminal_config.c:99-111` allows NVS to override `gateway_ssid`, `gateway_pass`, and `gateway_ip`.

Impact:
Extracting source or firmware exposes local network credentials and the production Server endpoint. ESPS3 -> Server traffic is plaintext HTTP and not protected against interception or replay by the reviewed client.

Recommendation:
Move secrets and deployment endpoints out of source. Use provisioning/NVS/secure storage, rotate the exposed credentials, require HTTPS or private transport, and add request authentication/signing.

### H5. ACK/ingest delivery failures can be hidden by local-success semantics

Evidence:
- `ESPS3/components/Middlewares/command_router/command_router.c:384-388` marks the local command entry ACKED before Server upload.
- `ESPS3/components/Middlewares/command_router/command_router.c:455-462` returns `ESP_OK` when voice busy skips command ACK upload.
- `ESPS3/components/Middlewares/command_router/command_router.c:467-483` records the Server result but still returns `ESP_OK`.
- `ESPS3/components/Middlewares/sensor_aggregator/sensor_aggregator.c:567-579` logs failed dashboard snapshot upload as deferred, but no durable outbox is shown.
- `ESPS3/components/Middlewares/sensor_aggregator/sensor_aggregator.c:625-639` posts BME/CSI to Server and records offline policy, but no retry ledger/payload persistence is shown.

Impact:
C5 can see local success while Server never receives the ACK/ingest/snapshot. Later successes can make the global offline state look healthy even when individual deliveries were lost.

Recommendation:
Introduce a durable per-path outbox or retry ledger for command ACK, smart-home ACK, BME/CSI ingest, snapshots, and key events. Local responses should distinguish "accepted by S3" from "committed to Server" when Server delivery is required.

### H6. CSI defaults are enabled in code while docs say default-off

Evidence:
- `ESPS3/components/Middlewares/gateway_config/gateway_config.h:36-42` defaults CSI trigger and result ingest to `1`.
- `ESPS3/components/Middlewares/gateway_config/gateway_config.c:60-61` applies those booleans.
- `ESPS3/components/Middlewares/csi_placeholder_gateway/csi_placeholder_gateway.c:113-128` forwards CSI summary into the aggregator when ingest is enabled.
- `ESPS3/components/Middlewares/sensor_aggregator/sensor_aggregator.c:625-637` forwards CSI result kinds to `/api/device/v1/ingest`.
- `ESPC51/components/app_config/app_main_config.h:35-42` enables C51 CSI service by default.
- `ESPC52/components/app_config/app_main_config.h:35-42` disables C52 CSI service by default.
- `docs/esp-111-minimal-integration-checklist.md:13`, `63-64`, and `175-193` say CSI is default-off and not uploaded.
- `docs/esp-111-firmware-architecture.md:18-19`, `58-69`, and `100` say CSI is default-off/reserved.
- `docs/wifi-csi-esp111-implementation-plan.md:7` and `263-270` say default trigger/ingest/upload are off.

Impact:
Operators following docs will assume no CSI trigger/ingest/upload, while ESPS3 and C51 defaults can enable summary ingestion. This is especially sensitive because CSI scope should remain summary-only and explicitly controlled.

Recommendation:
Make code and docs agree. Prefer default-off for production unless explicitly enabled, and add a firmware test or config check that asserts raw CSI is never uploaded and summary upload requires an explicit flag.

### H7. Voice and TTS paths expose large unauthenticated processing and memory pressure

Evidence:
- `ESP-server/src/routes/voiceRoutes.js:596-598` mounts `/api/voice/turn` and prompt config GET/PUT without auth.
- `ESP-server/src/routes/voiceRoutes.js:148-151` uses a raw parser with `readVoiceTurnMaxBytes()`.
- `ESP-server/src/voice/turnConfig.js:5-9` defaults `VOICE_TURN_MAX_BYTES` to 4 MiB and concurrency to 1.
- `ESP-server/src/voice/chain.js:237-268` reads the full HTTP TTS upstream body with `arrayBuffer()` before validation.
- `ESP-server/src/voice/chain.js:149-210` accumulates realtime audio chunks and then `Buffer.concat`s them.
- `ESP-server/src/voice/realtimeSocket.js:121-176` grows an internal buffer from incoming WebSocket data with no practical total byte cap.

Impact:
Unauthenticated callers can force expensive voice-chain work or memory growth. A malicious or malformed upstream TTS/WebSocket response can consume heap before validation or normalization.

Recommendation:
Authenticate voice APIs, lower and role-gate body limits, enforce streaming response size caps, cap realtime frame/message/total PCM bytes, and fail before buffering the full upstream response.

### H8. Local short device IDs can wrap and collide

Evidence:
- `ESPS3/components/Middlewares/protocol_adapter/protocol_adapter.c:335-349` casts JSON `id` to `uint8_t` and accepts any numeric value that survives `cJSON_IsNumber`.
- `ESPS3/components/Middlewares/local_http_server/local_http_server.c:340-363` parses `id` with `atoi()` and then casts to `uint8_t`.

Impact:
Values like `257` can become `1`. A client on the SoftAP can therefore use out-of-range numbers to impersonate C51/C52 or other local IDs and pull commands / report state as the wrong child.

Recommendation:
Require exact integers and validate the allowed set explicitly, not by modulo wrap. Reject negatives, decimals, trailing characters, and overflow.

## Medium Severity Findings

### M1. C5 gateway target can be changed by NVS without proving it is ESPS3

Evidence:
- `ESPC51/components/Middlewares/terminal_config/terminal_config.c:99-111` loads `gateway_ssid`, `gateway_pass`, and `gateway_ip` from NVS.
- `ESPC51/components/Middlewares/server_comm/server_comm_config.c:18-27` builds base URL from `terminal_config_get_gateway_ip()`.
- `ESPC51/components/Middlewares/server_comm/server_comm_config.c:85-91` rejects absolute `http://` or `https://` endpoints but not an arbitrary configured gateway IP.

Impact:
C5 code rejects explicit absolute Server URLs, which is good, but a changed gateway IP can redirect the local client to a non-ESPS3 host implementing `/local/v1/*`.

Recommendation:
Pin the expected ESPS3 SoftAP/BSSID/certificate/token or require a local gateway handshake before accepting the configured target.

### M2. Dashboard and S3 pages can present mock state as operational reality

Evidence:
- `ESP-server/public/app.js:43-89` defines mock sensor/ASR/LLM/history/alert/system-log data.
- `ESP-server/public/app.js:476-492` falls back to mock data on empty/error responses.
- `ESP-server/public/app.js:522-533` always returns mock history/alerts/system logs.
- `ESP-server/public/app.js:1329-1360` uses alert logs to populate header state.
- `ESP-server/public/app.js:1738-1762` mixes dynamic alerts with mock logs.
- `ESP-server/public/pages/s3.js:1-37` defines S3/C51/C52 gateway, sensor, and appliance mock data.
- `ESP-server/public/pages/s3.js:371-396` renders the full S3 page from mock data by default.
- `ESP-server/src/services/dashboardService.js:1135-1178` builds fallback overview with `mockAppliances()`.

Impact:
Operators can see warning/danger alerts, online states, or smart-home appliances even when data is missing, failed, or mocked. This can mask outages and conflicts with the "do not fake smart-home success" boundary.

Recommendation:
Require explicit demo mode for mock data. In production UI, show `unknown`, `offline`, `unavailable`, or `no snapshot` for missing data. Treat `source:"mock"` and `mock:true` as non-operational and do not count them as online or controllable.

### M2a. Smart-home mock appliances can still read as real devices

Evidence:
- `ESP-server/src/services/dashboardService.js:1174-1178` injects `mockAppliances()` into dashboard fallback overview.
- `ESP-server/public/app.js:101-109` and `1424-1537` make smart-home rows and toggles appear available when a later real toggle method exists, even though the default state is synthetic.
- `ESP-server/public/pages/s3.js` was reported by Pascal as still using mock/live mixed state on the S3 page.

Impact:
When no real appliance exists, mock appliances can still be perceived as controllable or online. That violates the boundary against fake smart-home success.

Recommendation:
Hide or clearly segregate mock appliances in production-facing views. If a real smart-home backend is absent, keep the control surface disabled and visibly unavailable.

### M3. Public Dashboard still uses legacy reads instead of Dashboard v1 contracts

Evidence:
- `ESP-server/public/app.js:495-516` reads `/sensor/latest`, `/asr/latest`, and `/llm/latest`.
- `ESP-server/public/app.js:522-529` has no real history/alert endpoint integration.
- `ESP-server/src/routes/dashboardRoutes.js:107-127` exposes Dashboard v1 latest/history routes.
- `ESP-server/src/services/dashboardService.js:1031-1076` and `1121-1212` already support `query.device_id` for latest/history/overview reads.
- `ESP-server/public/app.js:2093-2151` tab handling is frontend-only; C51/C52 views do not drive a `device_id` query.
- `ESP-server/public/index.html:22-39` defines S3/C51/C52 anchors, but not a device-id mapping.

Impact:
Dashboard v1 envelope and device filtering can drift from what users actually see. C51/C52 tabs can show global latest data from the wrong device.

Recommendation:
Create one Dashboard v1 API client for public pages. Bind S3/C51/C52 tabs to explicit `device_id`/gateway queries and render v1 error envelopes directly.

### M3a. Command IDs and local ACK paths are not URL-safe

Evidence:
- `ESPS3/components/Middlewares/server_client/server_client.c:267-272` and `321-326` interpolate `command_id` directly into REST paths.
- `ESPC51/components/Middlewares/command_domain/system_command/system_server_client.c:606-612` does the same for the local ACK path.

Impact:
If a command ID contains `/`, `?`, `#`, `%`, or similar characters, the request path can be truncated or reinterpreted. That can route an ACK to the wrong resource or create hard-to-debug command failures.

Recommendation:
Restrict command IDs to a URL-safe character set and encode path segments on both C5 and S3 before sending them.

### M3b. S3 command ACK JSON is string-interpolated instead of escaped

Evidence:
- `ESPS3/components/Middlewares/command_router/command_router.c:424-447` extracts arbitrary `error_code` and `message` strings from the local ACK body and interpolates them into `server_ack` with `snprintf`.

Impact:
An ACK body containing quotes, backslashes, or control characters can break the JSON payload or inject unexpected fields into the Server ACK. This can happen even when the business state is valid.

Recommendation:
Build `server_ack` with `cJSON` or escape every interpolated string. Keep machine error codes enum-like rather than accepting arbitrary strings.

### M4. User-data deletion omits smart-home and natural-language command tables

Evidence:
- `ESP-server/src/db/userDataDeletion.js:88-129` and `330-355` define deletion policies/scopes.
- `ESP-server/src/db/smartHome.js:21-51` defines `smart_home_commands` and `natural_language_commands`.
- `ESP-server/docs/api.md:1267-1275` lists `device_history` without smart-home or natural-language command tables.

Impact:
`all_user_data` can leave user/device command history behind even though the docs describe broad device history deletion.

Recommendation:
Add smart-home and natural-language command tables to the appropriate deletion scopes, or explicitly document why they are retained and expose a separate retention/deletion scope.

### M4a. Soft-deleted logs and snapshots can remain readable

Evidence:
- `ESP-server/src/db/userDataDeletion.js:225-234` includes `dashboard_snapshots` and `event_logs` in deletion policies.
- `ESP-server/src/services/eventLogService.js:142-149` lists event rows without a `deleted_at IS NULL` predicate.
- `ESP-server/src/services/eventLogService.js:223-228` also lists mapped events without a `deleted_at IS NULL` predicate.
- `ESP-server/src/routes/eventRoutes.js:37-40`, `81-84`, and `114-133` expose alarms/system/voice event reads on that service.
- `ESP-server/src/services/dashboardService.js:540-567` reads dashboard snapshots without filtering `deleted_at`; `readLatestDashboardSnapshot` can also serve the in-memory latest snapshot before DB state is reloaded.

Impact:
After a user-data soft delete, API reads can still return event payloads and dashboard snapshot payloads that were intended to be hidden. After hard delete, an in-process dashboard snapshot cache may briefly expose old payload data.

Recommendation:
Make all read paths filter `deleted_at IS NULL` by default. Invalidate the in-memory dashboard snapshot when deletion affects `dashboard_snapshots`. Add tests proving deleted event/snapshot data no longer appears in logs, voice events, overview, latest, or history.

### M4b. Soft-deleted device status can be revived by later activity

Evidence:
- `ESP-server/src/services/deviceStatusService.js:155-179` updates `device_status`, explicitly setting `deleted_at=NULL`, and matches by `device_id` without `deleted_at IS NULL`.
- `ESP-server/src/services/deviceStatusService.js:284-302` does the same for `device_module_status`.
- `ESP-server/src/services/deviceStatusService.js:508-515` read paths only return undeleted rows.

Impact:
After a `device_history` soft delete, any later S3 snapshot, heartbeat, command poll/ACK, voice prompt, or ingest refresh can make the soft-deleted status row visible again.

Recommendation:
Do not silently clear `deleted_at` in update paths. Either update only active rows and insert a new lifecycle record for new activity, or define a device-generation model that makes revival explicit.

### M4c. Dashboard snapshot ingest can duplicate recent events

Evidence:
- `ESP-server/src/services/dashboardService.js:629-643` inserts every dashboard snapshot.
- `ESP-server/src/services/dashboardService.js:694-715` writes each `recent_voice_events` and `recent_commands` item as a new event log.
- `ESP-server/src/services/eventLogService.js:150-159` defaults to a generated event ID when no stable event ID is supplied.

Impact:
If ESPS3 retransmits overlapping snapshots, Dashboard event counts and recent lists can grow duplicates.

Recommendation:
Derive stable event IDs from source plus command/voice IDs and timestamps, then upsert or ignore duplicates. Add a smoke case for repeated snapshot ingest.

### M4d. Log cleanup dry-run still writes a cleanup event

Evidence:
- `ESP-server/docs/api.md:1611-1614` says `dry_run=true` only counts and does not delete.
- `ESP-server/src/services/eventLogService.js:268-309` still records and broadcasts a `logs_cleanup` event for dry runs.

Impact:
Operators may use dry-run expecting no database writes, but it mutates `event_logs`. That weakens audit semantics and complicates retention proofs.

Recommendation:
Either make dry-run truly read-only or document that dry-run writes an audit event. Prefer recording dry-run audits in a dedicated admin audit table rather than the same event log being counted/cleaned.

### M5. Runtime and deployment docs can write the real database during tests

Evidence:
- `ESP-server/.env.example:3-4` documents `ESP_SERVER_DB_PATH` as optional and says normal deployments use `db/database.db`.
- `ESP-server/server-time-sync/README.md:80-101` instructs `npm start` and POST `/sensor`.
- `ESP-server/docs/deploy-branches.md:96-100` says first deploy runs `npm install` then `npm start`.
- `ESP-server/scripts/smoke-regression.js:743-762` correctly uses a temp DB for smoke testing.

Impact:
Manual verification commands can mutate the real deployment database if users forget to set `ESP_SERVER_DB_PATH`.

Recommendation:
Make docs distinguish smoke verification from production startup. Prefer a documented temp-DB command for local tests, and warn before examples that write `/sensor` or command APIs.

### M6. Firmware production security options are disabled in sdkconfig

Evidence:
- `ESPS3/sdkconfig:493-494`, `2032`, and `2305` show secure boot, NVS encryption, and flash encryption not enabled.
- `ESPC51/sdkconfig:583-584`, `2162`, and `2420` show the same pattern.
- `ESPC52/sdkconfig:583-584`, `2162`, and `2420` show the same pattern.
- UART console remains enabled in `ESPS3/sdkconfig:1329-1340`, `ESPC51/sdkconfig:1436-1446`, and `ESPC52/sdkconfig:1436-1446`.

Impact:
The current sdkconfig profile is appropriate for development, but not for protecting shipped credentials, firmware, or NVS values.

Recommendation:
Create explicit dev and production sdkconfig profiles. Production should evaluate secure boot, flash encryption, NVS encryption, and console restrictions.

### M6a. Local JSON construction on C5 is not consistently escaped

Evidence:
- `ESPC51/components/Middlewares/terminal_config/terminal_config.c:99-111` loads `gateway_ssid`, `gateway_pass`, and `gateway_ip` from NVS.
- `ESPC51/components/Middlewares/sensor_domain/bme690/server_client/bme_server_client.c:52`, `ESPC51/components/Middlewares/sensor_domain/csi_placeholder/csi_server_client.c:70`, and `ESPC51/components/Middlewares/server_comm/gateway_link.c:426` embed those values into JSON strings.

Impact:
If a stored value contains quotes, backslashes, or control characters, the generated JSON can be malformed or injectable.

Recommendation:
Use a JSON builder everywhere or validate stored strings to a narrow safe charset before serialization.

### M6b. BME `v` arrays are length-checked but not type-checked

Evidence:
- `ESPS3/components/Middlewares/protocol_adapter/protocol_adapter.c:168-200` only checks array length before reading `valueint` and `valuedouble`.

Impact:
String/null/object elements can be coerced to zero or defaults, producing silent sensor corruption.

Recommendation:
Validate each element with `cJSON_IsNumber` before reading and enforce numeric ranges.

### M7. Voice observability and partial-playback semantics are ambiguous

Evidence:
- `ESPS3/components/Middlewares/server_client/server_client.h:77` documents unknown/chunked voice response length as `-1`, but `ESPS3/components/Middlewares/server_client/server_client.c:471-481` treats negative `esp_http_client_fetch_headers()` as a fetch-header error.
- `ESPS3/components/Middlewares/server_client/server_client.c:360-388` reads streamed voice response chunks until response completion without a maximum response byte cap.
- `ESPC51/components/Middlewares/server_voice/server_voice_client.c:413-444` and `ESPC52/components/Middlewares/server_voice/server_voice_client.c:413-444` accept successful HTTP status and stream playback, but do not validate audio headers, maximum/minimum bytes, or odd trailing PCM bytes as failures.
- `ESPS3/components/Middlewares/server_client/server_client.c:496-505` streams voice response and returns an error only after read completion.
- `ESP-server/src/routes/voiceRoutes.js:543-559` logs ASR text previews and error context.
- `ESP-server/src/db/voiceTurns.js:5-24` stores voice turn status, byte counts, and `raw_json`.
- `ESP-server/src/routes/voiceRoutes.js:154-168` can return `409 VOICE_DEVICE_BUSY` or `429 VOICE_SERVER_BUSY`.
- `ESPS3/components/Middlewares/offline_policy/offline_policy.c:42-44` maps both 409 and 429 to local `voice_busy`.
- `ESP-server/docs/api.md:228-307` documents voice validation/config/upstream failures but not the 409/429 busy responses.

Impact:
If a voice response begins streaming to C5 and fails mid-way, the user may hear partial audio while logs only show a generic failure. Legitimate chunked Server audio can be rejected by S3 despite the header contract saying unknown length is allowed, while oversized or malformed PCM can be streamed without a firm cross-hop byte/format contract. Request IDs and stage-level failure details may not be enough to reconstruct C5 -> S3 -> Server timing without correlating multiple logs.

Recommendation:
Add a voice turn ID/trailer or content-length verification across C5, S3, and Server. Allow chunked/unknown-length responses only with explicit total-byte and timeout caps. Validate content type/audio metadata and reject empty, odd-byte, or oversized PCM unless the API explicitly defines those cases. Record bytes played, content length, stage, and close reason on both sides. Update API docs with 409/429 retry/backoff semantics and the S3-to-C5 `voice_busy` mapping.

### M8. Command-domain documentation mixes C5 device commands and smart-home commands

Evidence:
- `docs/esp-111-firmware-architecture.md:171-175` documents S3 -> Server command paths as `/api/commands/*`.
- `docs/esp-111-minimal-integration-checklist.md:122-145` validates the C5 local command router via `/api/commands/pending`.
- `docs/esp-backend-api-integration-plan.md:49-51` defines smart-home pending/ACK as `/api/smart-home/v1/*`.
- `ESPS3/components/Middlewares/server_client/server_client.c:235-254` implements smart-home pending, while `ESPS3/components/Middlewares/server_client/server_client.c:285-337` implements C5 device command pending/ACK.

Impact:
Future integration can send smart-home `target_id/action/params` commands into the C5 device command queue, whose semantics are display/system-device commands and capability-checked by `device_id`.

Recommendation:
Split the docs into two command domains: C5 device command router (`/api/commands/*`) and smart-home gateway (`/api/smart-home/v1/*`). Give each its own lifecycle, ACK status model, and tests.

### M9. Memory jobs can be force-replayed into additional records

Evidence:
- `ESP-server/src/memory/store.js:580-617` creates a fresh `memory_job_runs` row with a generated job ID.
- `ESP-server/src/jobs/memoryJobs.js:307-390` only skips existing daily summaries when `force` is false, then writes a new job and daily memory row.
- `ESP-server/src/jobs/memoryJobs.js:568-666` does the same for weekly summaries and also writes candidate profile/experience/relation records.

Impact:
Repeated forced job runs can create multiple summaries and downstream candidates for the same date/week. This may be intended for manual recomputation, but it lacks a superseded/versioned model and can confuse deletion/audit summaries.

Recommendation:
Define job replay semantics. Use a stable job key plus version/superseded status, or explicitly mark prior candidates as superseded when `force=true`.

## Low Severity / Hygiene Findings

- `ESP-server/package.json:15` declares `cors`, but no CORS middleware is configured in `ESP-server/server.js`. Remove it or define the intended policy.
- `ESPC51/CMakeLists.txt:14` and `ESPC52/CMakeLists.txt:14` both use project name `00_Learn`, increasing release artifact mix-up risk.
- `ESPC51/partitions.csv:4` and `ESPC52/partitions.csv:4` are factory-only layouts; no OTA/rollback reserve is visible.
- `ESPS3/components/Middlewares/server_client/server_client.c:267-272` and `321-326` interpolate `command_id` into URL paths without URL encoding.
- `docs/esp-111-gateway-migration-notes.md:7-8` still contains historical statements that ESPS3 was a C5 copy and hardcodes a local ESP-IDF path; it needs a clearer stale/historical marker.

## Positive Findings

- ESPS3 local HTTP route registration uses `/local/v1/*`, not `/api/*`: `ESPS3/components/Middlewares/local_http_server/local_http_server.c:448-460`.
- C5 HTTP config rejects absolute `http://` and `https://` endpoints: `ESPC51/components/Middlewares/server_comm/server_comm_config.c:85-91`.
- Reviewed CSI conversion paths carry summary fields, not raw CSI arrays, into Server ingest: `ESPS3/components/Middlewares/protocol_adapter/protocol_adapter.c:216-256`.
- Smart-home ESPS3 gateway path does not fake success when no real appliance is attached; this was verified in agent findings and should be preserved.
- Reviewed SQL paths generally use parameterized queries; dynamic user-data deletion identifiers use `quoteIdentifier` checks in `ESP-server/src/db/userDataDeletion.js:51-57`.
- `ESP-server/scripts/smoke-regression.js:743-762` uses a temp SQLite DB and temp voice prompt paths for smoke tests.
- `npm audit --registry=https://registry.npmjs.org --omit=dev --json` returned 0 production vulnerabilities for `ESP-server`.

## Test Coverage Gaps

Existing smoke tests cover many happy-path flows and some validation, but the audit found missing or inverted coverage:

- No failing tests for unauthenticated command, smart-home, dashboard write, voice prompt config, log cleanup/delete, event stream, or device ingest writes.
- No tests proving command ACK and smart-home ACK must match the claiming gateway/device and dispatched state.
- No tests for command/natural-language/voice idempotency keys, duplicate request replay, or command lease/attempt mismatch.
- No firmware-level tests asserting C5 cannot be redirected away from ESPS3 through NVS gateway IP.
- No static/build check that CSI defaults in code match docs.
- No test for Dashboard production mode refusing mock fallback data.
- No TTS/realtime tests for oversized upstream body, oversized WebSocket frames, or total PCM caps.
- No firmware voice tests for chunked/unknown-length Server responses, odd trailing PCM bytes, empty audio, or oversized playback payloads.
- No durable delivery/outbox tests for ACK/ingest/log retry semantics.

## Agent Coverage

- Hegel: ESPS3 gateway forwarding, offline policy, command ACK reliability.
- Mendel: ESPC51/ESPC52 boundary, NVS gateway target, CSI parity.
- Wegener: ESP-server route/auth/DB/command contracts.
- Carson: voice pipeline, prompt cache, voice observability.
- Pasteur: security configuration, secrets, supply chain.
- Lagrange: build/release reproducibility and docs drift.
- Pascal: Dashboard/public frontend and mock/live contract drift.
- Kant: DB/schema/retention and user-data deletion coverage.
- Einstein: firmware memory safety/protocol parsing.
- Newton: docs/API contract consistency.

## Recommended Remediation Order

1. Lock down ESP-server identity/auth for all write/control routes, starting with device ingest, commands, smart-home, voice, and log deletion.
2. Rotate/remove hardcoded WiFi credentials and switch Server traffic away from plaintext unauthenticated HTTP.
3. Fix ACK ownership and state transitions for command and smart-home flows.
4. Decide and enforce CSI default-off/default-on policy in code, docs, and tests.
5. Replace best-effort delivery with a durable outbox or explicit degraded-delivery contract.
6. Remove production Dashboard mock fallback and migrate public reads to `/api/dashboard/v1/*`.
7. Add temp-DB-only verification docs and broaden smoke tests around negative security cases.

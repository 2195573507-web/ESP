# ESP-server Complete Project Documentation

Generated: 2026-06-13

This document is written as a takeover handoff. It assumes the next developer or AI agent has only this file and the repository contents.

## 1. Executive Summary

`ESP-server` is a Node.js/Express server for an ESP32-C5 environmental/voice device system. It serves a static dashboard from `public/`, accepts ESP device telemetry and voice audio, stores operational data in SQLite, proxies LLM requests to a Volc/Doubao-compatible gateway, maintains a command queue for devices, and exposes memory/agent-state/privacy APIs for higher-level assistant behavior.

The application is not a compiled frontend app. It is a single Node runtime:

- Backend entrypoint: `server.js`
- Static frontend: `public/index.html`, `public/styles.css`, `public/app.js`
- Database: SQLite at `db/database.db` by default
- Runtime dependencies: `express`, `sqlite3`, `dotenv`, `cors`
- Test command: `npm test` or `npm run test:smoke`

Important takeover state:

- `package-lock.json` is tracked by Git but currently deleted in the worktree. Restore or regenerate it before any dependency-sensitive deployment.
- `node_modules/` exists locally but is ignored.
- `.env` is ignored. Use `.env.example` as the configuration template.
- Chinese frontend and prompt strings are UTF-8. On Windows PowerShell, use `Get-Content -Encoding UTF8` or Node file reads to avoid mojibake.
- `cors` is declared in `package.json` but is not currently used in `server.js`.

## 2. Repository Map

```text
ESP-server/
  server.js                         Express app entrypoint and route mounting.
  package.json                      npm scripts and dependency declarations.
  .env.example                      Server, LLM, voice, command defaults.
  db/database.db                    Local SQLite database file.
  public/
    index.html                      Static dashboard DOM.
    styles.css                      Dashboard styling and responsive layout.
    app.js                          Browser state, polling, rendering, mocks.
  server-time-sync/
    timeSync.js                     /api/time routes and timing helpers.
    README.md                       Time sync usage notes.
  src/
    agent/stateStore.js             Environment, experience, relation, reminders, emergency, CSI, LCD state.
    commands/schema.js              Command whitelist and payload validation.
    commands/queue.js               Device capability registry and command lifecycle.
    db/*.js                         SQLite connection, migrations, table creation, inserts.
    jobs/memoryJobs.js              Daily and weekly memory summarization jobs.
    llm/*.js                        LLM text client and structured JSON parsing.
    memory/store.js                 Conversation turns, daily memory, profiles, corrections, job runs.
    routes/*.js                     Express route modules.
    services/*.js                   Device ingest, status, dashboard, context, user data services.
    utils/*.js                      Env, date, logging helpers.
    voice/*.js                      Voice HTTP contract, ASR/LLM/TTS chain, prompt cache, WebSocket client.
  scripts/
    smoke-regression.js             Broad integration smoke regression using temp DB and mock LLM.
  docs/
    api.md                          Detailed API reference.
    *.md                            Roadmaps, deployment, project memory, migration plans.
```

## 3. System Architecture Diagram

```text
                    +---------------------------+
                    |        Browser UI         |
                    | public/index/styles/app   |
                    +-------------+-------------+
                                  |
                                  | GET /dashboard, /sensor/latest,
                                  | /asr/latest, /llm/latest
                                  v
+-------------+        +----------+-----------+        +----------------+
| ESP device  | -----> | Express server.js    | -----> | SQLite database |
| BME/voice/  | HTTP   | middleware + routes  | SQL    | db/database.db  |
| command     | <----- | services + helpers   | <----- | all tables      |
+------+------+        +----------+-----------+        +----------------+
       |                          |
       |                          | HTTPS/WSS
       |                          v
       |              +-----------+------------+
       |              | Volc/Doubao AI gateway |
       |              | chat, realtime ASR/TTS |
       |              +------------------------+
       |
       | Command lifecycle:
       | POST capabilities -> poll pending -> ack result
       v
+------+----------------+
| Device command worker |
+-----------------------+
```

### Backend Route Layer

```text
server.js
  createVoiceRouter() before express.json()
    POST /api/voice/turn       raw PCM
    GET  /api/voice/prompt     raw PCM prompt
    GET  /api/voice/prompt-cache
  express.json()
  static public/
  GET / -> /dashboard
  GET /dashboard -> public/index.html
  createLlmTextRouter()
  createStructuredLlmRouter()
  createCommandRouter()
  createDeviceRouter()
  /api/dashboard/v1 -> createDashboardRouter()
  createMemoryRouter()
  createAgentStateRouter()
  createUserDataRouter()
  createRecordRouter()
  createSensorRouter()
  /api/time -> createTimeSyncRouter()
  JSON 404 for machine API paths
  JSON error handler
```

Voice is mounted before `express.json()` because `/api/voice/turn` needs raw PCM bytes rather than JSON parsing.

## 4. Backend Architecture

### Boot Sequence

1. `server.js` loads `.env` via `dotenv`.
2. Creates the Express app.
3. Opens SQLite using `createDatabase(__dirname)`.
4. Builds promise helpers `dbRun` and `dbAll`.
5. Mounts routes and middleware.
6. Runs startup migrations/table creation:
   - `ensureRecordTables`
   - `ensureSensorTimingColumns`
   - `ensureDeviceStatusTables`
   - `ensureVoiceTurnsTable`
   - `ensureCommandTables`
   - `ensureMemoryTables`
   - `ensureAgentStateTables`
   - `ensureUserDataDeletionTables`
7. Starts HTTP listener on `PORT` or `3000`.
8. Handles `SIGTERM` and `SIGINT` by closing HTTP server and SQLite.

### Persistence Model

The project uses self-migrating SQLite modules. Each `src/db/*.js` file creates its tables if missing and uses `ensureTableColumns` to add missing columns to older schemas. Unique indexes are created when safe; if duplicate legacy rows exist, `ensureUniqueIndex` logs a warning and skips that index.

Default database path:

```text
db/database.db
```

Override:

```dotenv
ESP_SERVER_DB_PATH=/path/to/test-or-prod.sqlite
```

### Core Backend Layers

```text
routes/       HTTP parsing, status codes, response envelopes.
services/     Business logic and read/write models.
db/           SQLite schema creation, migration helpers, low-level inserts.
commands/     Command validation, capability checks, queue lifecycle.
llm/          Text client, structured output prompt and parser.
voice/        PCM HTTP contract, ASR -> LLM -> TTS chain, prompt cache.
memory/       Long-term profile and conversation memory store.
agent/        Agent state APIs for environment, reminders, emergency, LCD, CSI.
jobs/         Manual memory summary jobs.
utils/        Common env parsing, date validation, log masking.
```

## 5. All Modules and Dependencies

### npm Dependencies

| Dependency | Used for | Notes |
| --- | --- | --- |
| `express` | HTTP server, routers, body parsers, static serving | Express 5 style package. |
| `sqlite3` | SQLite database access | Callback API wrapped in promises by `src/db/sqlite.js`. |
| `dotenv` | Loads `.env` at startup | Required by `server.js`. |
| `cors` | Declared dependency | Not currently used in `server.js`. |

### Node Built-ins Used

| Built-in | Main use |
| --- | --- |
| `path`, `fs` | Database directory creation, prompt cache files, static file paths. |
| `crypto` | UUIDs, prompt cache checksums, WebSocket keys. |
| `http`, `https` | Minimal WebSocket transport for realtime ASR/TTS. |
| `os`, `child_process`, `assert` | Smoke tests only. |

### Root Modules

| File | Responsibilities | Depends on |
| --- | --- | --- |
| `server.js` | App boot, route mounting, migration boot, shutdown handling. | All route modules, DB table modules, time sync. |
| `server-time-sync/timeSync.js` | `/api/time/*`, server time snapshots, upload delay helpers. | `deviceMetadata`, `deviceStatusService`. |

### Route Modules

| File | Endpoints | Depends on |
| --- | --- | --- |
| `src/routes/deviceRoutes.js` | Device v1 ingest, gateway state, status/context/latest sensor. | `sensorBme690Service`, `csiMotionService`, `dashboardService`, `deviceStatusService`, `deviceContextService`. |
| `src/routes/sensorRoutes.js` | Legacy `/sensor` write/read. | `timeSync`, `deviceStatusService`, `deviceMetadata`. |
| `src/routes/recordRoutes.js` | Legacy ASR/LLM record write/read. | Raw sqlite `db`. |
| `src/routes/dashboardRoutes.js` | `/api/dashboard/v1/*` read layer. | `dashboardService`. |
| `src/routes/llmTextRoutes.js` | `/api/llm/text`. | `textClient`, `llmPromptContextService`, logging. |
| `src/routes/structuredLlmRoutes.js` | `/api/llm/structured`. | `textClient`, `structuredOutput`, `commands/queue`, `llmPromptContextService`. |
| `src/routes/voiceRoutes.js` | Voice turn and prompt cache. | `voice/*`, `deviceMetadata`, `deviceStatusService`, `db/voiceTurns`. |
| `src/routes/commandRoutes.js` | Capability registration, queue, ack, history. | `commands/queue`, `commands/schema`, `deviceMetadata`, `deviceStatusService`. |
| `src/routes/memoryRoutes.js` | Conversation, memory, jobs. | `memory/store`, `jobs/memoryJobs`, date utils. |
| `src/routes/agentStateRoutes.js` | Agent state, LCD, CSI, emergency, reminders. | `agent/stateStore`, `commands/queue`. |
| `src/routes/userDataRoutes.js` | Privacy summary, deletion, reserved export. | `services/userDataService`. |

### Service Modules

| File | Responsibilities |
| --- | --- |
| `deviceMetadata.js` | Reads v1 metadata from body, headers, query; computes valid upload delay. |
| `deviceStatusService.js` | Upserts `device_status` and `device_module_status`, online thresholds, delay stats. |
| `sensorBme690Service.js` | Validates v1 BME690 envelope, maps payload into `sensor_records`, computes fallback air quality. |
| `csiMotionService.js` | Validates `csi.motion`, updates status, records latest in dashboard memory aggregate. |
| `dashboardService.js` | Dashboard read models, gateway snapshot in memory, CSI merge, legacy fallback overview. |
| `deviceContextService.js` | Produces LLM/device context from status, modules, latest BME, air quality. |
| `llmPromptContextService.js` | Adds device context text to LLM prompts for text/structured/voice paths. |
| `userDataService.js` | Admin-token protected user data summary, preview, soft/hard delete, audit runs. |

### Database Modules

| File | Tables or helpers |
| --- | --- |
| `sqlite.js` | Opens SQLite and wraps `db.run`, `db.all`. |
| `migrations.js` | Add-column migration and unique-index helpers. |
| `records.js` | `asr_records`, `llm_records`. |
| `sensorRecords.js` | `sensor_records` plus sensor indexes. |
| `deviceStatus.js` | `device_status`, `device_module_status`. |
| `commands.js` | `device_capabilities`, `command_queue`. |
| `voiceTurns.js` | `voice_turns` and insert helper. |
| `memory.js` | `conversation_turns`, `daily_memory`, `long_term_profile`, `memory_corrections`, `memory_job_runs`. |
| `agentState.js` | `environment_profile`, `experience_memory`, `relation_memory`, `reminder_rules`, `reminder_records`, `emergency_events`, `csi_behavior_events`, `lcd_status`. |
| `userDataDeletion.js` | `data_deletion_runs`, soft-delete columns/policies on many tables. |
| `upsert.js` | update-then-insert helper with unique constraint retry. |

### Command Modules

| File | Responsibilities |
| --- | --- |
| `commands/schema.js` | Whitelisted command definitions and validation. |
| `commands/queue.js` | Capability normalization, queue insert, pending claim, ack, history. |

Current command whitelist:

```text
device.noop
voice.set_volume              payload: { volume: integer 0..100 }
sensor.set_upload_interval    payload: { interval_ms: integer 1000..3600000 }
display.show_text             payload: { text: string <=120, ttl_ms: 1000..60000 default 5000 }
alert.play_tone               payload: { tone: confirm|warning|error, duration_ms: 100..10000 optional }
```

### LLM and Voice Modules

| File | Responsibilities |
| --- | --- |
| `llm/textClient.js` | Reads gateway env, validates text request, calls chat completions with timeout. |
| `llm/structuredOutput.js` | JSON-only structured prompt, robust JSON extraction and command list parsing. |
| `voice/http.js` | Voice content type, audio format, request validation, PCM/error response helpers. |
| `voice/gatewayConfig.js` | Reads and validates ASR/chat/TTS gateway config. |
| `voice/turnConfig.js` | Reads voice timeout, body size, concurrency, mock flag. |
| `voice/chain.js` | ASR WebSocket -> LLM text -> TTS WebSocket/HTTP. |
| `voice/mockTurn.js` | Mock sine-wave PCM for voice turn and prompt. |
| `voice/promptCache.js` | File-backed prompt PCM cache under `cache/voice_prompts` by default. |
| `voice/realtimeSocket.js` | Minimal WebSocket client over Node `http`/`https`. |
| `voice/realtimeEvents.js` | Parses ASR/TTS realtime events. |
| `voice/ttsAudio.js` | Normalizes PCM/WAV/JSON base64 TTS outputs and detects silence. |
| `voice/errors.js` | Stage-aware error objects. |
| `voice/payloadUtils.js` | Base64 and payload decoding helpers. |
| `voice/gatewayHeaders.js` | Authorization and optional resource ID headers. |

### Frontend Modules

| File | Responsibilities |
| --- | --- |
| `public/index.html` | Static shell: sidebar, topbar, metric cards, chart canvas, alerts/logs, command modals, smart-home panel. |
| `public/styles.css` | Full responsive dashboard styles, dark/light themes, mobile sidebar, modals. |
| `public/app.js` | Browser state, polling, endpoint fetches, mock fallbacks, chart canvas, UI rendering, command modal behavior. |

## 6. Data Storage Schema Summary

The server is the schema authority. It creates missing tables and columns on startup.

| Table | Purpose |
| --- | --- |
| `sensor_records` | Legacy and v1 BME690 readings, device metadata, raw JSON, air quality fields. |
| `asr_records` | Legacy ASR text records. |
| `llm_records` | LLM prompt/response records for latest display and history. |
| `device_status` | One row per device, last seen, online basis, upload delay stats, reboot count. |
| `device_module_status` | One row per device/module type, module last seen and delay stats. |
| `device_capabilities` | Registered commands supported by each device. |
| `command_queue` | Queued/dispatched/completed/failed device commands. |
| `voice_turns` | Diagnostics for `/api/voice/turn`, including bytes, timings, errors. |
| `conversation_turns` | Conversation turns and structured command references. |
| `daily_memory` | Daily and weekly memory summaries. |
| `long_term_profile` | Long-term user/profile facts. |
| `memory_corrections` | User corrections to memory/profile data. |
| `memory_job_runs` | Daily/weekly memory job audit records. |
| `environment_profile` | Environment profile facts. |
| `experience_memory` | Experience memories. |
| `relation_memory` | Relation triples. |
| `reminder_rules` | Reminder definitions. |
| `reminder_records` | Reminder event records. |
| `emergency_events` | Emergency event records. |
| `csi_behavior_events` | CSI behavior event records, not raw CSI. |
| `lcd_status` | Last LCD/display state per device. |
| `data_deletion_runs` | Privacy deletion preview/delete audit records. |

Soft delete:

- `ensureUserDataDeletionTables` adds `deleted_at`, `delete_reason`, `updated_at` to many data tables.
- Most read paths include `deleted_at IS NULL`.
- `device_capabilities` is protected from user-data deletion.

## 7. Data Flow Diagrams

### 7.1 New BME690 Device Ingest

```text
ESP BME module
  POST /api/device/v1/ingest
  body: schema_version=1, payload_type=sensor.bme690, payload readings
        |
        v
deviceRoutes.js validates payload_type
        |
        v
sensorBme690Service.validateBmeEnvelope()
sensorBme690Service.normalizeAirQuality()
        |
        +--> INSERT sensor_records
        |
        +--> refreshDeviceActivity()
               UPDATE/INSERT device_status
               UPDATE/INSERT device_module_status(module_type=sensor.bme690)
        |
        v
201 device envelope with id, upload delay, air quality
```

### 7.2 Legacy Sensor Ingest

```text
Old ESP/script
  POST /sensor
  body: temperature, humidity, pressure, gas_resistance, device_id, esp_time_ms
        |
        v
sensorRoutes.normalizeSensorBody()
timeSync.buildSensorTimingFields()
        |
        +--> INSERT sensor_records legacy columns
        |
        +--> optional refreshDeviceActivity(module_type=sensor.bme690)
        |
        v
JSON { ok:true, success:true, id, timing fields }
```

### 7.3 Dashboard Read Flow

```text
Browser public/app.js
  every 3000 ms:
    GET /sensor/latest
    GET /asr/latest
    GET /llm/latest
        |
        v
legacy routes read SQLite
        |
        v
frontend normalizeSensor/buildMetrics/render cards/chart/logs
        |
        v
if empty/fetch failed, frontend falls back to mock values
```

Dashboard v1 exists server-side but current `public/app.js` does not use it.

### 7.4 Dashboard v1 Server Read Flow

```text
GET /api/dashboard/v1/overview
        |
        v
dashboardService
  if latestGatewaySnapshot exists:
      clone snapshot, merge latest in-memory CSI motion
  else:
      read latest sensor, device status, sensor history from SQLite
      synthesize fallback gateway/devices/history model
        |
        v
{ ok:true, server_time_ms, data, error:null }
```

### 7.5 LLM Text Flow

```text
Client/ESP
  POST /api/llm/text { text, device_id, session_id }
        |
        v
llmTextRoutes.readLlmTextRequest()
        |
        v
llmPromptContextService.buildLlmPrompt()
  deviceContextService.getDeviceContext()
    reads device_status, module_status, latest BME
        |
        v
llm/textClient.requestLlmText()
  POST gateway /v1/chat/completions
        |
        v
INSERT llm_records(prompt=user text, response=reply)
        |
        v
JSON reply with id/model/server_time_ms
```

### 7.6 Structured LLM + Command Flow

```text
Client/ESP
  POST /api/llm/structured { text, device_id, target_device_id }
        |
        v
build device-context prompt
buildStructuredPrompt()
        |
        v
gateway chat completions
        |
        v
parseStructuredLlmOutput()
        |
        +--> INSERT llm_records
        |
        +--> for each parsed command:
               normalize command against whitelist
               require target device capabilities
               INSERT command_queue if valid
        |
        v
JSON { text, commands, rejected_commands, structured }
```

### 7.7 Command Queue Lifecycle

```text
Device boot/capability phase:
  POST /api/devices/capabilities
    -> upsert device_capabilities
    -> refresh module command.capabilities

Command creation:
  POST /api/commands or /api/llm/structured or /api/lcd/display
    -> whitelist validation
    -> device capability check
    -> INSERT command_queue status=queued

Device polling:
  GET /api/commands/pending?device_id=...
    -> queued or timed-out dispatched rows selected
    -> rows marked status=dispatched
    -> refresh module command.poll

Device ack:
  POST /api/commands/:command_id/ack { status: completed|failed }
    -> update command_queue terminal status
    -> refresh module command.ack
```

### 7.8 Voice Turn Flow

```text
ESP microphone
  POST /api/voice/turn
  headers:
    Content-Type: audio/L16; rate=16000; channels=1
    X-Audio-Format: pcm_s16le_mono_16k
    X-Device-Id: ...
  body: raw PCM s16le mono 16k
        |
        v
voiceRoutes raw parser and validateVoiceTurnRequest()
        |
        +--> refreshDeviceActivity(module_type=voice.turn)
        |
        +--> concurrency guard per device and server
        |
        +--> if VOICE_TURN_MOCK=1:
        |       streamMockVoiceTurn() returns sine PCM
        |
        +--> else runVoiceTurnChain():
                ASR realtime WebSocket
                LLM chat completions with device context
                TTS realtime WebSocket or HTTP
        |
        +--> INSERT voice_turns diagnostic row
        |
        v
200 raw PCM response or JSON error
```

### 7.9 Voice Prompt Cache Flow

```text
ESP speaker/wake flow
  GET /api/voice/prompt-cache?prompt_key=wake_ack_zh&device_id=...
        |
        v
readPromptCache(prompt_key)
        |
        +--> hit: return cached PCM with X-Prompt-Cache: hit
        |
        +--> miss and VOICE_TURN_MOCK=1:
        |       generate mock prompt PCM, write cache, return miss
        |
        +--> miss and real TTS configured:
                requestVoiceTts("I am here, speak" equivalent text)
                validate non-silent PCM
                write cache
                return miss
        |
        +--> TTS failure with stale cache: return stale
        |
        v
raw PCM response
```

### 7.10 User Data Deletion Flow

```text
Admin client with X-Admin-Token or Bearer token
        |
        v
GET /api/user-data/summary
POST /api/user-data/delete/preview
POST /api/user-data/delete { confirm:"DELETE" }
        |
        v
userDataService policiesForScope()
        |
        +--> preview: count rows, insert data_deletion_runs request_type=preview
        |
        +--> delete: BEGIN IMMEDIATE
              insert running audit row
              soft-delete or hard-delete scoped tables
              update audit row completed
              COMMIT
        |
        v
JSON result
```

## 8. Device Communication Flow

### Unified Device Metadata v1

JSON envelope requests place metadata in the body. Raw PCM and GET requests place metadata in headers. The server reads body, headers, query, and explicit fallback device IDs through `readDeviceMetadata`.

Body fields:

```json
{
  "schema_version": 1,
  "device_id": "esp32-c5-whole-001",
  "device_type": "esp32c5_env_voice_node",
  "firmware_version": "0.1.0",
  "request_seq": 123,
  "esp_uptime_ms": 12345678,
  "esp_time_ms": 1780732142207,
  "time_synced": true,
  "payload_type": "sensor.bme690",
  "payload": {}
}
```

Header equivalents:

```http
X-Schema-Version: 1
X-Device-Id: esp32-c5-whole-001
X-Device-Type: esp32c5_env_voice_node
X-Firmware-Version: 0.1.0
X-Request-Seq: 123
X-Esp-Uptime-Ms: 12345678
X-Esp-Time-Ms: 1780732142207
X-Time-Synced: true
X-Payload-Type: voice.turn
```

Rules:

- `device_id` means whole device, not a module.
- `sensor_id` or `module_id` belongs inside `payload`.
- Server computes `server_recv_ms`, `server_time_iso`, and `upload_delay_ms`.
- `upload_delay_ms` is only valid when `time_synced=true`, `esp_time_ms` is finite, and calculated delay is `0..60000`.
- BME module status can be offline while whole device remains online from voice, command, prompt, or time ping traffic.

### Payload Types Currently Handled

| payload_type/module_type | Endpoint | Storage/status effect |
| --- | --- | --- |
| `sensor.bme690` | `POST /api/device/v1/ingest`, legacy `/sensor` | Inserts `sensor_records`, updates device and BME module status. |
| `csi.motion` | `POST /api/device/v1/ingest` | Updates device/module status and in-memory dashboard CSI occupancy. |
| `gateway.dashboard_snapshot` | `POST /api/device/v1/gateway-state` | Stores latest dashboard gateway snapshot in process memory only. |
| `voice.turn` | `POST /api/voice/turn` | Inserts `voice_turns`, updates device and voice.turn module status. |
| `voice.prompt` | `GET /api/voice/prompt[-cache]` | Updates device and voice.prompt module status. |
| `command.capabilities` | `POST /api/devices/capabilities` | Upserts `device_capabilities`, updates status. |
| `command.poll` | `GET /api/commands/pending` | Marks commands dispatched, updates status. |
| `command.ack` | `POST /api/commands/:id/ack` | Marks commands completed/failed, updates status. |
| `time.ping` | `POST /api/time/ping` | Keeps latest ping in memory, updates status. |

## 9. Frontend Architecture

The frontend is a static dashboard. There is no bundler, framework, compile step, or module system. Express serves `public/` directly.

### UI Structure

`public/index.html` contains:

- Sidebar with brand and online-device card.
- Topbar with search, online status, alert bell, theme toggle, user menu.
- Metric cards for temperature, humidity, air quality, ESP status.
- Main chart canvas.
- Alert panel.
- Alert logs table.
- System logs panel.
- Command control panel with custom/fetch/calibrate/reinitialize/clear-log buttons.
- Smart-home control panel.
- Log modal, custom-command modal, command-confirm modal, smart-home-confirm modal.

### Browser State

`public/app.js` maintains a single `dashboardState` object:

```text
sensor, asr, llm, metrics, history, alertLogs, systemLogs,
commandLogs, operationLogs, smartHomeDevices, sources
```

Timers:

- `DASHBOARD_REFRESH_INTERVAL_MS = 3000`
- `ESP_DELAY_REFRESH_INTERVAL_MS = 1000`

Current actual network calls:

```text
GET /sensor/latest
GET /asr/latest
GET /llm/latest
```

Current mock-only frontend functions:

- `fetchHistoryData()` returns `mockHistoryData`.
- `fetchAlertLogs()` returns `mockAlertLogs`.
- `fetchSystemLogs()` returns `mockSystemLogs`.
- `getNaturalLanguageSubmitMethod()` returns `null`.
- `getDeviceOperationMethod()` returns `null`.
- `getSmartHomeStatusMethod()` and `getSmartHomeToggleMethod()` return `null`.

The backend already has `/api/dashboard/v1/*`, `/api/commands`, and `/api/llm/structured`, but the current frontend does not use them.

### Frontend Rendering Flow

```text
DOMContentLoaded
  initThemeToggle()
  initChartRangeSelector()
  initLogModals()
  bindCommandButtons()
  initCommandControls()
  initSmartHomeControls()
  bindMobileSidebar()
  updateDashboard()
  startDashboardTimers()

updateDashboard()
  Promise.all(fetch sensor, asr, llm, history, alert logs)
  normalizeSensor()
  buildMetrics()
  buildDynamicAlertLogs()
  buildSystemLogs()
  renderMetricCards()
  renderMainChart()
  renderAlertSummary()
  renderAlertLogs()
  renderSystemLogs()
  renderStatusHeader()
  renderSourceDebug()
```

### Frontend Known Limitations

- Hard-coded legacy URLs are scattered in fetch functions. Existing docs recommend a central `API_CONFIG`, but the current file does not implement one.
- Dashboard v1 API is not consumed yet.
- History chart uses frontend mock data instead of `/sensor/history` or `/api/dashboard/v1/sensors/history`.
- Alert and system logs are mostly derived/mock. There is no backend alert-log API.
- Command buttons do not call backend command APIs except "fetch current data" using `/sensor/latest`.
- Smart-home controls are disabled because there is no backend smart-home state/toggle API.
- Air quality/gas display logic can conflate `aqi`, `air_quality`, and `gas_resistance`; future work should keep gas resistance and air quality separate.

## 10. API Specifications

### Common API Behavior

Machine API paths return JSON on 404:

```json
{ "ok": false, "error": "Not found" }
```

Unexpected route errors return JSON:

```json
{ "ok": false, "error": "Internal server error" }
```

Dashboard v1 endpoints use:

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {},
  "error": null
}
```

Dashboard v1 errors use:

```json
{
  "ok": false,
  "server_time_ms": 1780000000000,
  "data": null,
  "error": { "code": "ERROR_CODE", "message": "readable message" }
}
```

### Static Frontend

| Method | Path | Description |
| --- | --- | --- |
| GET | `/` | Redirects to `/dashboard`. |
| GET | `/dashboard` | Serves `public/index.html`. |
| GET | `/* static` | Serves files from `public/`. |

### Device v1 APIs

| Method | Path | Request | Success | Errors/notes |
| --- | --- | --- | --- | --- |
| POST | `/api/device/v1/ingest` | JSON envelope, `payload_type` must be `sensor.bme690` or `csi.motion`. | Device envelope with `data`; BME returns 201, CSI returns 202. | 400 for unsupported type or invalid payload. |
| POST | `/api/device/v1/gateway-state` | Gateway snapshot with `payload_type=gateway.dashboard_snapshot`, `schema_version=2`. | 202 device envelope with gateway id and counts. | Stored in process memory only. |
| GET | `/api/device/v1/status?device_id=` | Optional `device_id`. | `{ ok:true, status, server_time_ms }`. | Empty `device_id` reads latest known device. |
| GET | `/api/device/v1/modules/status?device_id=` | Optional `device_id`. | `{ ok:true, modules, server_time_ms }`. | Module online threshold is 30000 ms. |
| GET | `/api/device/v1/context?device_id=` | Optional `device_id`. | `{ ok:true, context, server_time_ms }`. | Used by LLM prompt context. |
| GET | `/api/device/v1/sensors/latest?device_id=` | Optional `device_id`. | `{ ok:true, sensor, server_time_ms }`. | Returns mapped latest BME or `{}` for no data. |

#### BME690 v1 Request

```json
{
  "schema_version": 1,
  "device_id": "esp32-c5-whole-001",
  "device_type": "esp32c5_env_voice_node",
  "firmware_version": "0.1.0",
  "request_seq": 123,
  "esp_uptime_ms": 12345678,
  "esp_time_ms": 1780732142207,
  "time_synced": true,
  "payload_type": "sensor.bme690",
  "payload": {
    "sensor_id": "bme690_01",
    "temperature_c": 29.57,
    "humidity_percent": 30.29,
    "pressure_hpa": 986.26,
    "gas_resistance_ohm": 35164,
    "air_quality_score": 72,
    "air_quality_level": "moderate",
    "air_quality_confidence": "low",
    "air_quality_source": "esp"
  }
}
```

Required:

- `schema_version` numeric `1`
- `payload_type` exactly `sensor.bme690`
- non-empty `device_id`
- `payload.temperature_c`, `payload.humidity_percent`, `payload.pressure_hpa`, `payload.gas_resistance_ohm`

Air quality note: the score is a relative ESP/BME690 estimate. It is not national AQI and does not represent PM2.5, PM10, or CO2.

#### CSI Motion v1 Request

```json
{
  "schema_version": 1,
  "device_id": "esp32-c5-whole-001",
  "payload_type": "csi.motion",
  "payload": {
    "occupancy": { "state": "occupied" },
    "motion_score": 0.8,
    "variance": 12.3,
    "rssi": -55,
    "sample_count": 20,
    "updated_at": 1780732142207
  }
}
```

Allowed occupancy states: `unknown`, `vacant`, `occupied`.

### Legacy Sensor and Record APIs

| Method | Path | Request | Success | Notes |
| --- | --- | --- | --- | --- |
| POST | `/sensor` | Flat JSON readings. | `{ ok:true, success:true, id, timing fields }`. | Legacy write. Still updates BME module status when possible. |
| GET | `/sensor/latest` | None. | Latest row object or `{}`. | Current frontend uses this. |
| GET | `/sensor/history?limit=` | Optional limit default 50 max 500. | Array old-to-new. | Current frontend does not actually call this. |
| POST | `/asr` | `{ text }`, max 4000 chars. | `{ ok:true, success:true, id }`. | Legacy ASR write. |
| POST | `/llm` | `{ prompt, response }`, each max 4000 chars. | `{ ok:true, success:true, id }`. | Legacy LLM write. |
| GET | `/asr/latest` | None. | Latest row or `{}`. | Current frontend uses this. |
| GET | `/llm/latest` | None. | Latest row or `{}`. | Current frontend uses this. |

### Time APIs

Mounted under `/api/time`.

| Method | Path | Request | Success | Notes |
| --- | --- | --- | --- | --- |
| GET | `/api/time/now` | None. | `{ ok:true, server_time_ms, server_time_iso }`. | Server time snapshot. |
| GET | `/api/time/status` | None. | `{ ok:true, server_time_ms, server_time_iso, latest_ping }`. | `latest_ping` is in-memory and lost on restart. |
| POST | `/api/time/ping` | `{ device_id, esp_send_ms, esp_uptime_ms }`. | Ping record with estimated one-way delay. | Updates status as module `time.ping`. |

### LLM APIs

| Method | Path | Request | Success | Errors/notes |
| --- | --- | --- | --- | --- |
| POST | `/api/llm/text` | `{ text, device_id?, session_id? }`. `text` max 4000. | `{ ok:true, text, id, model, server_time_ms }`. | 400 invalid body/text. 503 missing key. 504 timeout. 502 upstream/parse/empty reply. |
| POST | `/api/llm/structured` | `{ text, device_id?, target_device_id?, session_id? }`. | `{ ok:true, text, chat, commands, rejected_commands, structured, id, model, server_time_ms }`. | Uses device context and command queue. Invalid commands are rejected per command, not whole request. |

LLM env source priority:

- API key: `VOLC_GATEWAY_API_KEY` then `LLM_API_KEY`
- Base URL: `VOLC_GATEWAY_HTTP_BASE_URL` then `LLM_BASE_URL` then default
- Chat path: `VOLC_GATEWAY_CHAT_PATH` then `LLM_CHAT_PATH`
- Model: `VOLC_GATEWAY_CHAT_MODEL` then `LLM_MODEL`

### Voice APIs

| Method | Path | Request | Success | Errors/notes |
| --- | --- | --- | --- | --- |
| POST | `/api/voice/turn` | Raw PCM body. Headers must include `Content-Type: audio/L16; rate=16000; channels=1` and `X-Audio-Format: pcm_s16le_mono_16k`. | Raw PCM with same content type. | 415 bad content type/audio format. 400 empty/odd PCM. 413 body too large. 409 device busy. 429 server busy. |
| GET | `/api/voice/prompt` | Optional `prompt_key`, `device_id`, `refresh=1`. | Raw PCM prompt. | Compatibility alias for prompt cache behavior. |
| GET | `/api/voice/prompt-cache` | Optional `prompt_key`, `device_id`, `refresh=1`. | Raw PCM prompt with `X-Prompt-Cache: hit|miss|stale`. | Default cache key `wake_ack_zh`. |

Voice turn success headers:

```http
Content-Type: audio/L16; rate=16000; channels=1
X-Audio-Format: pcm_s16le_mono_16k
Cache-Control: no-store
X-Content-Type-Options: nosniff
```

Voice prompt success headers:

```http
Content-Type: audio/L16; rate=16000; channels=1
X-Prompt-Key: wake_ack_zh
X-Prompt-Cache: hit
X-Audio-Format: pcm_s16le_mono_16k
X-Sample-Rate: 16000
X-Channels: 1
Cache-Control: public, max-age=86400
```

### Command APIs

| Method | Path | Request | Success | Errors/notes |
| --- | --- | --- | --- | --- |
| GET | `/api/commands/whitelist` | None. | `{ ok:true, commands }`. | Lists allowed command schemas. |
| POST | `/api/devices/capabilities` | `{ device_id, protocol_version?, capabilities:{ commands:[] } }`. | `{ ok:true, device_id, protocol_version, capabilities, server_time_ms }`. | Unknown commands are ignored. Updates status. |
| GET | `/api/devices/:device_id/capabilities` | Path device id. | `{ ok:true, device_id, protocol_version, capabilities, last_seen_at, updated_at }`. | 404 if absent. |
| POST | `/api/commands` | `{ name, target_device_id, payload, reason? }`. | 201 `{ ok:true, command }`. | Requires whitelist and device capabilities. |
| GET | `/api/commands/pending?device_id=&limit=` | Required device id. | `{ ok:true, commands, server_time_ms }`. | Marks rows dispatched. Timed-out dispatched rows reappear. |
| POST | `/api/commands/:command_id/ack` | `{ status:"completed"|"failed", result?, error_code?, error_message? }`. | `{ ok:true, status, command_id, server_time_ms }`. | 400 invalid status, 404 not accepted. |
| GET | `/api/commands/history?device_id=&limit=` | Optional filters. | `{ ok:true, commands }`. | Limit default 50 max 200. |

`COMMAND_DISPATCH_TIMEOUT_MS` controls redispatch delay, default 60000.

### Dashboard v1 APIs

Mounted under `/api/dashboard/v1`.

| Method | Path | Query | Data payload |
| --- | --- | --- | --- |
| GET | `/api/dashboard/v1/overview` | optional `device_id` | Gateway/dashboard snapshot model or fallback model with `gateway`, `devices`, `home_summary`, `history`, `recent_voice_events`, `recent_commands`. |
| GET | `/api/dashboard/v1/sensors/latest` | optional `device_id` | Latest mapped BME sensor row or `null`. |
| GET | `/api/dashboard/v1/sensors/history` | optional `device_id`, `limit` | Array of mapped BME sensor rows old-to-new. |
| GET | `/api/dashboard/v1/devices/:device_id/history` | path device id, optional `limit` | Sensor history for one device. |
| GET | `/api/dashboard/v1/asr/latest` | none | Latest ASR row or `null`. |
| GET | `/api/dashboard/v1/llm/latest` | none | Latest LLM row or `null`. |
| GET | `/api/dashboard/v1/time/status` | none | Time status wrapped for dashboard. |
| GET | `/api/dashboard/v1/device/status` | optional `device_id` | Whole-device status object. |
| GET | `/api/dashboard/v1/modules/status` | optional `device_id` | `{ modules: [...] }`. |

`limit` must be a positive integer. Max is 500. Invalid limit returns `400 DASHBOARD_BAD_LIMIT`.

### Memory APIs

| Method | Path | Purpose |
| --- | --- | --- |
| POST | `/api/conversation/turns` | Create conversation turn. |
| GET | `/api/conversation/turns` | List turns by optional `device_id`, `session_id`, `limit`. |
| POST | `/api/memory/daily` | Create daily/weekly memory row. |
| GET | `/api/memory/daily` | List daily/weekly memory by date/type/limit. |
| POST | `/api/memory/profile` | Upsert long-term profile. |
| GET | `/api/memory/profile` | List profiles by status/category/limit. |
| POST | `/api/memory/corrections` | Apply memory correction. |
| POST | `/api/jobs/daily-summary/run` | Run daily summary job. |
| POST | `/api/jobs/weekly-profile/run` | Run weekly profile job. |
| GET | `/api/jobs/memory` | List memory job runs. |

Daily and weekly jobs are manually triggered HTTP endpoints. There is no scheduler in this repo.

### Agent State APIs

| Method | Path | Purpose |
| --- | --- | --- |
| POST/GET | `/api/environment/profile` | Upsert/list environment profiles. |
| POST/GET | `/api/memory/experience` | Create/list experience memories. |
| POST/GET | `/api/memory/relation` | Create/list relation memories. |
| POST/GET | `/api/reminders/rules` | Create/list reminder rules. |
| POST/GET | `/api/reminders/events` | Create/list reminder events. |
| POST/GET | `/api/emergency/events` | Create/list emergency events. |
| POST/GET | `/api/csi/behavior` | Create/list CSI behavior events. |
| POST/GET | `/api/lcd/status` | Upsert/list LCD status. |
| POST | `/api/lcd/display` | Queue `display.show_text` command and update LCD status. |

### User Data and Privacy APIs

All require admin authorization:

```http
X-Admin-Token: <USER_DATA_DELETE_TOKEN or ADMIN_TOKEN>
Authorization: Bearer <same token>
```

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/api/user-data/summary` | Count data by public scope. |
| POST | `/api/user-data/delete/preview` | Preview affected rows and write preview audit run. |
| POST | `/api/user-data/delete` | Execute soft/hard delete. Requires `{ confirm:"DELETE" }`. |
| GET | `/api/user-data/deletion-runs` | List preview/delete audit runs. |
| GET | `/api/user-data/export` | Reserved, currently returns 501. |

Valid scopes include:

```text
summaries
profiles
memory
conversations
device_history
jobs
all_user_data
```

Valid modes:

```text
soft_delete
hard_delete
```

## 11. Environment Variables

### Server and Database

| Variable | Default | Purpose |
| --- | --- | --- |
| `PORT` | `3000` | HTTP server port. |
| `ESP_SERVER_DB_PATH` | `db/database.db` | Optional override database path. |

### LLM Text and Structured APIs

| Variable | Default | Purpose |
| --- | --- | --- |
| `LLM_API_KEY` | empty | Server-side chat gateway API key. |
| `LLM_BASE_URL` | `https://ai-gateway.vei.volces.com` | Chat gateway base URL. |
| `LLM_CHAT_PATH` | `/v1/chat/completions` | Chat completions path. |
| `LLM_MODEL` | `Doubao-Seed-1.6-flash` | Chat model. |
| `LLM_TIMEOUT_MS` | `30000` | Chat request timeout and voice LLM timeout. |

`VOLC_GATEWAY_API_KEY`, `VOLC_GATEWAY_HTTP_BASE_URL`, `VOLC_GATEWAY_CHAT_PATH`, and `VOLC_GATEWAY_CHAT_MODEL` override the corresponding `LLM_*` values for current code paths.

### Command Queue

| Variable | Default | Purpose |
| --- | --- | --- |
| `COMMAND_DISPATCH_TIMEOUT_MS` | `60000` | Dispatched commands become pollable again after this timeout if not acked. |

### Voice Turn

| Variable | Default | Purpose |
| --- | --- | --- |
| `VOICE_TURN_MOCK` | unset/false | Set `1` to return mock PCM and avoid external ASR/LLM/TTS. |
| `VOICE_TURN_TIMEOUT_MS` | `45000` | Total turn timeout. |
| `VOICE_TURN_MAX_CONCURRENT` | `1` | Global concurrent voice turns. |
| `VOICE_TURN_MAX_BYTES` | `4194304` | Raw PCM request max body bytes. |

### Volc Gateway Voice/Chat

| Variable | Default | Purpose |
| --- | --- | --- |
| `VOLC_GATEWAY_API_KEY` | empty | API key for ASR/chat/TTS chain. |
| `VOLC_GATEWAY_WS_BASE_URL` | `wss://ai-gateway.vei.volces.com` | Realtime WebSocket base URL. |
| `VOLC_GATEWAY_HTTP_BASE_URL` | `https://ai-gateway.vei.volces.com` | HTTP base URL. |
| `VOLC_GATEWAY_REALTIME_PATH` | `/v1/realtime` | ASR realtime path. |
| `VOLC_GATEWAY_CHAT_PATH` | `/v1/chat/completions` | Chat completions path. |
| `VOLC_GATEWAY_ASR_MODEL` | `bigmodel` | ASR realtime model. |
| `VOLC_GATEWAY_ASR_FORMAT` | `pcm` | Must describe raw PCM. |
| `VOLC_GATEWAY_ASR_CODEC` | `raw` | Must describe raw PCM. |
| `VOLC_GATEWAY_ASR_SAMPLE_RATE` | `16000` | Must be 16000. |
| `VOLC_GATEWAY_ASR_BITS` | `16` | Must be 16. |
| `VOLC_GATEWAY_ASR_CHANNELS` | `1` | Must be 1. |
| `VOLC_GATEWAY_WS_AUDIO_CHUNK_BYTES` | `32000` | PCM chunk size sent over ASR WebSocket. |
| `VOLC_GATEWAY_CHAT_MODEL` | `Doubao-Seed-1.6-flash` | Chat model used in voice chain. |
| `VOLC_GATEWAY_TTS_MODEL` | empty | Required when `VOICE_TURN_MOCK` is not `1`. |
| `VOLC_GATEWAY_TTS_VOICE` | empty | Required when `VOICE_TURN_MOCK` is not `1`. |
| `VOLC_GATEWAY_TTS_PATH` | empty unless model set | Realtime or HTTP TTS path. |
| `VOLC_GATEWAY_TTS_SAMPLE_RATE` | `16000` | Must be 16000. |
| `VOLC_GATEWAY_TTS_FORMAT` | `pcm_s16le_mono_16k` | Required TTS output format. |
| `VOLC_GATEWAY_USE_RESOURCE_ID` | false | Enables ASR resource ID header. |
| `VOLC_GATEWAY_ASR_RESOURCE_ID` | `volc.bigasr.sauc.duration` | ASR resource ID when enabled. |
| `VOLC_GATEWAY_TTS_USE_RESOURCE_ID` | false | Enables TTS resource ID header. |
| `VOLC_GATEWAY_TTS_RESOURCE_ID` | empty | TTS resource ID when enabled. |

TTS aliases also read by code:

```text
LLM_MODEL_TTS
LLM_GATEWAY_TTS_MODEL
TTS_MODEL
LLM_TTS_VOICE
TTS_VOICE
TTS_VOICE_TYPE
VOICE_TYPE
VOLC_GATEWAY_TTS_REALTIME_PATH
LLM_TTS_PATH
TTS_PATH
TTS_URL
TTS_ENDPOINT
```

### Voice Prompt Cache

| Variable | Default | Purpose |
| --- | --- | --- |
| `VOICE_PROMPT_CACHE_DIR` | `cache/voice_prompts` | Optional prompt PCM cache directory. |

### User Data Admin

| Variable | Default | Purpose |
| --- | --- | --- |
| `USER_DATA_DELETE_TOKEN` | empty | Admin token for privacy APIs. |
| `ADMIN_TOKEN` | empty | Fallback admin token for privacy APIs. |

If neither admin token is set, user-data APIs return `503 USER_DATA_ADMIN_TOKEN_NOT_CONFIGURED`.

## 12. Build Process

There is no TypeScript, bundler, transpiler, or frontend build pipeline.

Install dependencies:

```bash
npm install
```

Start server:

```bash
npm start
```

Run smoke regression:

```bash
npm test
```

or:

```bash
npm run test:smoke
```

Syntax-check useful files:

```bash
node --check server.js
node --check public/app.js
node --check src/services/llmPromptContextService.js
node --check src/routes/voiceRoutes.js
```

Smoke tests:

- Use temporary database paths.
- Start a local mock LLM server.
- Verify legacy routes, device v1 routes, command queue, voice errors, memory jobs, privacy delete, and migration behavior.
- Clean up temp directories after completion.

## 13. Deployment Process

The project docs describe two collaboration branches:

- `api`: backend deployment branch.
- `ui`: frontend deployment branch.

Production deployment should not overwrite the whole server directory blindly because `public/`, backend files, `.env`, and `db/database.db` have different ownership/risk.

### Backend Deployment

Deploy backend files:

```text
server.js
package.json
package-lock.json
server-time-sync/
src/
scripts/
docs/
```

Handle `db/` only after a production backup and explicit migration plan.

Example production flow:

```bash
cd /opt/ESP-server
git fetch origin api
git checkout origin/api -- server.js package.json package-lock.json server-time-sync src scripts docs
npm install
pm2 restart esp-server
pm2 logs esp-server --lines 100
```

Systemd alternative:

```bash
sudo systemctl restart esp-server
sudo journalctl -u esp-server -n 100 -f
```

### Frontend Deployment

Deploy only:

```text
public/index.html
public/styles.css
public/app.js
```

Example:

```bash
cd /opt/ESP-server
git fetch origin ui
git checkout origin/ui -- public/
pm2 restart esp-server
```

### Required Production Checks

Before deployment:

```bash
git status --short
npm test
```

After deployment:

```bash
curl http://localhost:3000/api/time/status
curl http://localhost:3000/sensor/latest
curl http://localhost:3000/dashboard
```

Before any database-changing deployment:

1. Stop or quiesce writes if possible.
2. Back up `db/database.db`.
3. Start server once to run migrations.
4. Check logs for `[db:migration] skip unique index ...` warnings.
5. Run smoke checks against a non-production clone when possible.

## 14. Current Implementation Status

Implemented:

- Express server boot and graceful shutdown.
- Static dashboard served from `public/`.
- SQLite self-migration/table initialization.
- Legacy sensor, ASR, and LLM record APIs.
- Unified device v1 BME690 ingest.
- CSI motion lightweight ingest to status/dashboard memory.
- Gateway dashboard snapshot ingest to process memory.
- Device and module status with online thresholds.
- Upload delay stats with valid sample filtering.
- Time sync endpoints and status refresh.
- LLM text proxy with device context injection.
- Structured LLM endpoint with command queue integration.
- Command whitelist, capabilities, queue, polling, ack, and history.
- Voice turn raw PCM contract, mock mode, and real ASR -> LLM -> TTS chain scaffolding.
- File-backed voice prompt cache.
- Dashboard v1 backend read layer.
- Conversation/memory/profile/correction APIs.
- Daily and weekly memory job endpoints.
- Agent-state APIs for environment, reminders, emergency, CSI behavior, LCD status/display.
- User-data summary, preview, soft-delete, hard-delete, audit run APIs.
- Broad smoke regression suite.

Partially implemented:

- Dashboard v1 backend exists, but frontend still reads legacy endpoints.
- CSI motion is lightweight occupancy/status only; raw CSI capture/classification is not implemented.
- LCD/display command is queued and status is stored, but hardware LCD driver is not implemented in this repo.
- Voice real chain exists but requires correct external gateway config and credentials; mock mode is default in `.env.example`.
- Smart-home UI exists but backend status/toggle APIs are absent.
- Alert UI exists but backend alert-log persistence/API is absent.
- Memory jobs are manually triggered, not scheduled.

Not implemented/reserved:

- `/api/user-data/export` returns 501.
- Backend authentication for most APIs. Only user-data routes enforce admin token.
- Rate limiting, request signing, device authentication, or CORS policy.
- Production-grade migration versioning/rollback.
- Persistent storage for dashboard gateway snapshot, latest CSI occupancy, or time ping. These are in-memory and lost on restart.
- Frontend API client abstraction.

## 15. Missing Features

Highest-impact missing features:

1. Frontend migration to `/api/dashboard/v1/*`.
2. Real backend alert/event log APIs to replace frontend mock alert logs.
3. Smart-home backend status and control APIs, or removal of disabled UI until supported.
4. Natural-language command submission from dashboard to `/api/llm/structured` or `/api/commands`.
5. Device authentication and admin/API auth beyond user-data deletion.
6. Persistent dashboard snapshot and CSI occupancy state across restarts.
7. Scheduled daily/weekly memory jobs.
8. Full user-data export implementation.
9. Production deployment lockfile restoration/regeneration.
10. End-to-end validation with real ESP hardware and real Volc ASR/TTS credentials.

Device-side gaps reflected by server docs:

- CSI raw/features pipeline is reserved, not complete.
- LCD hardware driver is reserved, not complete.
- BME690 air quality is relative and should not be labeled as national AQI.

## 16. Technical Debt

1. `package-lock.json` is currently deleted in the worktree but referenced by deployment docs.
2. Route modules mix callbacks and promise helpers; legacy routes still use raw callback `db.run/db.get/db.all`.
3. Migrations are ad hoc add-column/table checks rather than versioned migrations.
4. Unique index creation is skipped if legacy duplicates exist, which can leave important tables without uniqueness.
5. In-memory process state exists for `latestPingRecord`, `latestDashboardSnapshot`, and latest CSI motion.
6. Most APIs are unauthenticated.
7. No rate limiting on voice, LLM, command, or deletion preview endpoints.
8. `cors` dependency is unused.
9. Frontend has hard-coded URLs instead of a central API config/client.
10. Frontend mock data can hide backend integration failures.
11. Dashboard command and smart-home buttons are present but mostly no-op.
12. Some docs still describe branch-specific ownership rules that may not match all current work.
13. Error shapes differ across legacy, device v1, dashboard v1, voice, and user-data APIs.
14. LLM prompt text is embedded in source strings; changes require code edits and careful UTF-8 handling.
15. No OpenAPI/JSON schema source of truth for APIs.
16. No automated browser/UI tests.
17. No formal production config validation at startup. Missing voice/LLM config errors happen on request.
18. The minimal WebSocket client is custom code; it should be heavily tested if realtime voice becomes production-critical.

## 17. Suggested Next Development Milestones

### Milestone 1: Stabilize Repository and CI Basics

- Restore or regenerate `package-lock.json`.
- Add a CI script that runs:
  - `node --check server.js`
  - `node --check public/app.js`
  - `npm test`
- Add a short `README.md` with setup, env, run, test, and dashboard URL.

### Milestone 2: Migrate Frontend to Dashboard v1

- Add a single frontend API client/config in `public/app.js`.
- Replace current calls:
  - `/sensor/latest` -> `/api/dashboard/v1/sensors/latest` or `/overview`
  - mock history -> `/api/dashboard/v1/sensors/history`
  - `/asr/latest` -> `/api/dashboard/v1/asr/latest`
  - `/llm/latest` -> `/api/dashboard/v1/llm/latest`
- Keep legacy fallback only as explicit compatibility mode.
- Separate gas resistance display from air quality score/level.

### Milestone 3: Make Dashboard Controls Real or Honest

- Wire custom natural-language input to `/api/llm/structured`.
- Wire supported manual commands to `/api/commands`.
- Show command queue history via `/api/commands/history`.
- Keep unsupported smart-home controls disabled or remove them until backend/device support exists.

### Milestone 4: Security Baseline

- Add API token/device token middleware for write endpoints.
- Decide CORS policy and either configure `cors` or remove the dependency.
- Add request size limits for JSON APIs.
- Add rate limits for LLM, voice, and command creation.
- Add audit logging for command creation and privileged operations.

### Milestone 5: Persist Runtime State

- Persist dashboard gateway snapshots or derive overview entirely from DB.
- Persist CSI motion latest state if occupancy matters after restart.
- Persist time ping samples if latency history matters.
- Consider adding `device_latency_samples` for p95/windowed latency.

### Milestone 6: Production Voice Integration

- Validate real ASR realtime config.
- Validate real TTS model/voice/path and output format.
- Add integration tests around upstream failure bodies.
- Add monitoring for `voice_turns` status/error counts.
- Consider replacing the custom WebSocket implementation with a maintained library if policy allows.

### Milestone 7: Memory and Privacy Completion

- Implement `/api/user-data/export`.
- Add scheduled daily/weekly memory jobs.
- Add review/activation flows for candidate profiles/memories.
- Add admin UI or CLI for deletion preview/export/delete.

### Milestone 8: Device Protocol Hardening

- Add request signing or device tokens.
- Add idempotency support using `request_seq` plus device id for ingest.
- Add duplicate/retry handling for command ack.
- Add formal JSON schema tests for device v1 payloads.

## 18. Takeover Checklist

When continuing development:

1. Run `git status --short` and account for the deleted `package-lock.json`.
2. Use UTF-8 reads for frontend/prompt files on Windows.
3. Do not modify `db/database.db` unless the task explicitly requires database data changes.
4. Prefer temp DB via `ESP_SERVER_DB_PATH` for tests.
5. Run `npm test` before and after backend changes.
6. Keep legacy endpoints stable unless the task explicitly deprecates/removes them.
7. Update API documentation before changing endpoint fields.
8. Treat frontend and backend ownership separately unless the user asks for full-stack changes.
9. Never label BME690 relative air quality as national AQI.
10. Keep raw PCM `/api/voice/turn` mounted before JSON body parsing.


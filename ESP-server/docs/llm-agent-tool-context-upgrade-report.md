# LLM Agent Tool and Context Upgrade

## Scope

This upgrade changes only the current `ESP-server` repository. It does not modify ESP32 firmware projects, device protocols, or radar, voice, BME, and CSI ingestion paths.

## Architecture Change

The two existing LLM HTTP entry points remain unchanged:

- `POST /api/llm/text`
- `POST /api/llm/structured`

They now use `src/agent/agentRunner.js`. Each OpenAI-compatible completion request contains:

1. A fixed `system` message loaded from `src/prompts/esp-home-agent-system-prompt.txt`.
2. A separate `system` message containing JSON from `contextBuilder`.
3. The registered OpenAI tool schemas.
4. The user's original message.

The OpenAI-compatible Chat Completions API has no standard top-level `context` field. The dynamic context is therefore a distinct system message, never concatenated into the fixed system prompt. It includes only `home_location`, persisted `device_capabilities`, and `available_tools`; it does not include live sensor, weather, household, or device status values.

`agentRunner` supports up to three tool rounds. On a model `tool_calls` response it validates the named tool and JSON arguments, runs the server handler, appends a `role: tool` result, and requests the final answer. A handler failure is returned to the model as `{ "success": false, "error": "..." }`; no fallback data is fabricated.

The legacy voice chain intentionally remains on its existing text-completion path. The upgrade applies to the requested Agent endpoints without changing voice transport behavior.

## Prompt Design

The independent prompt defines the ESP Home AI Agent role and requires tools for weather, family state, environment/sensors, and device status. It explicitly prohibits guessing, stale-data presentation, and completing failed tool results.

## Tool Registry and Data Sources

`src/agent/defaultToolRegistry.js` registers tools in one shape: `name`, `description`, `parameters`, and `handler`.

| Tool | Data source | Failure behavior |
| --- | --- | --- |
| `weather_query` | OpenWeather current weather plus forecast API | Missing key, missing home coordinates, invalid input, timeout, HTTP, and network failures return `success: false`. |
| `home_state_query` | `device_status` records | Returns unavailable when no device status exists; device online status is explicitly not treated as occupancy. |
| `sensor_query` | Fresh `sensor_records` through `deviceContextService` | Returns unavailable for absent or stale data. |
| `device_status_query` | `device_status` and `device_module_status` | Returns unavailable when no device status exists. |

OpenWeather uses `OPENWEATHER_API_KEY` and `OPENWEATHER_TIMEOUT_MS`; no key is stored in source. The startup check logs a warning when the key is missing, and `weather_query` fails closed.

## Home Location

`home_location` is a single-row SQLite table (`id = 1`) with `country`, `province`, `city`, `district`, `latitude`, `longitude`, and `timezone`. Its migration runs from `server.js` through `ensureHomeLocationTables()`.

New APIs use the existing standard envelope:

- `GET /api/settings/home-location`
- `POST /api/settings/home-location`

The POST endpoint validates coordinate bounds and IANA time zones. The dashboard now provides a `#settings` / Home Location page to load and save those fields.

## Modified Files

- `.env.example`
- `server.js`
- `src/llm/textClient.js`
- `src/routes/llmTextRoutes.js`
- `src/routes/structuredLlmRoutes.js`
- `src/prompts/esp-home-agent-system-prompt.txt`
- `src/agent/agentRunner.js`
- `src/agent/contextBuilder.js`
- `src/agent/defaultToolRegistry.js`
- `src/agent/toolRegistry.js`
- `src/agent/homeTools.js`
- `src/agent/weatherQuery.js`
- `src/db/homeLocation.js`
- `src/services/homeLocationService.js`
- `src/routes/settingsRoutes.js`
- `public/index.html`
- `public/app.js`
- `public/styles.css`
- `scripts/smoke-regression.js`

## Test Results

`npm test` passed on 2026-07-19. The smoke suite uses temporary SQLite and a local mock upstream. It verifies:

- Prompt file loading.
- Registry contents and OpenAI schema projection.
- Home location persistence and HTTP CRUD.
- Weather query success and missing-key failure.
- LLM system/context/tools request construction.
- A two-request `weather_query` tool-calling loop with tool result forwarding.
- Existing regression coverage for command parsing, device ingestion, and other server routes.

An additional temporary-server HTTP check confirmed that `/dashboard#settings` serves the Home Location navigation and form, and that `GET /api/settings/home-location` returns the configured envelope shape. Browser pixel automation was not run because the local Python runtime does not include Playwright.

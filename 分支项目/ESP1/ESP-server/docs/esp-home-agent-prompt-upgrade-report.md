# ESP Home AI Agent Prompt Upgrade Report

Date: 2026-07-19

## Scope

Only `ESP-server` was modified. No ESP32-S3, C5, radar, voice implementation, or Tool implementation was changed.

## Prompt Location

The fixed agent rules are stored in `prompts/esp_home_agent_system.md`. The file contains the ESP Home AI Agent role, real-time data truthfulness rules, tool-selection rules, source boundaries, future device-control safeguards, and concise fact-based response rules.

## Loading Flow

`src/services/llmPromptContextService.js` reads and caches the fixed Markdown prompt. `buildLlmPrompt()` retains its existing return contract (`ok`, `context`, `prompt`, and `error` on degradation) and builds one compatible text payload with these labeled sections:

1. `System Prompt`: fixed rules from the Markdown file.
2. `Runtime Context`: current device context from `deviceContextService`.
3. `User Message`: the original user input.

The existing text, structured, and voice consumers continue to pass the same `prompt` string to the existing LLM client. The fixed System Prompt directs tool selection; the server does not register, invoke, emulate, or otherwise change any Tool implementation. Runtime device data is therefore not written into the fixed System Prompt file.

## Modified Files

- `prompts/esp_home_agent_system.md`
- `src/services/llmPromptContextService.js`
- `scripts/test-esp-home-agent-prompt.js`
- `package.json`
- `docs/esp-home-agent-prompt-upgrade-report.md`

## Tests

Run `npm run test:esp-home-agent-prompt` for the focused local checks:

- system prompt file loading
- required rule/tool content
- normal knowledge questions are explicitly instructed not to call tools
- weather questions are explicitly instructed to call `weather_query`
- household-presence questions are explicitly instructed to call `home_state_query`
- final prompt section order and context-failure fallback

This report records prompt-level behavior only. It does not claim that a Tool was executed, because this change intentionally leaves Tool registration and execution unchanged.

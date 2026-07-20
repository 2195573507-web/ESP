# ESP Home AI Agent Voice Tool Chain Audit

Audit date: 2026-07-20  
Scope: current top-level `ESPC51`, `ESPS3`, and `ESP-server` source only. `archive/`, `分支项目/`, and build products were excluded. No code, configuration, database, or running service was changed.

## Conclusion

The actual C5 voice route reaches ASR, an ordinary LLM completion, and TTS, then returns PCM to C5. It **does not enter the Agent/tool loop**. The server does contain a real `weather_query` implementation, schema, dispatcher, OpenWeather client, and tool-result-to-LLM loop, but that loop is only used by `/api/llm/text` and `/api/llm/structured`, not `/api/voice/turn`.

For the spoken request `现在天气怎么样`, the current voice route is category **A: the voice chain never enters the Tool system**. It is not B/C/D:

- `weather_query` exists and is registered for the Agent.
- `weather_query` can be passed to an LLM by the Agent route.
- The voice route calls `requestLlmText()`, which explicitly supplies `[]` as the tools argument, so the LLM never receives the schema and no dispatcher can be reached.
- Consequently, there is no voice-route tool result that could fail to return to the LLM.

This is a source-level conclusion. No C5/S3 device, ASR/LLM/TTS provider, OpenWeather endpoint, or running server was exercised.

## Actual End-to-End Route

```text
C5 ADC microphone
  mic_adc_test_task()
    -> mic_adc_pcm_convert_sample()
    -> mic_adc_voice_stream_push_sample()
    -> voice_chain_server_voice_append_pcm()
    -> server_voice_client_append_pcm()
    -> server_voice_client_finish_turn()
      HTTP POST /local/v1/voice/turn (PCM)

S3 local HTTP
  voice_proxy_handle_turn()
    -> voice_proxy_process_reserved_turn()
    -> server_client_post_voice_turn()
      HTTP POST /api/voice/turn (same PCM, streamed response)

ESP-server
  POST /api/voice/turn -> handleVoiceTurn()
    -> runVoiceTurnChain()
      -> requestVoiceAsr()       (Volc ASR WebSocket)
      -> requestVoiceTurnLlm()   (ordinary chat completion, no tools)
      -> requestVoiceTts()       (Volc TTS WS or HTTP)
    <- raw PCM response

S3
  stream_to_httpd() streams response PCM to C5

C5
  server_voice_response_process()
    -> server_voice_play_response_chunk()
    -> audio_player stream / speaker playback
```

## C5 Evidence

| Stage | File and function | Input | Output / evidence |
|---|---|---|---|
| Microphone acquisition | `ESPC51/components/Middlewares/mic/mic_adc_test.c`, `mic_adc_test_task()` | ADC continuous samples | `adc_continuous_read()` at lines 1726-1731, parsing at 1755-1759, and sample iteration at 1765-1791. |
| PCM conversion | `.../mic_adc_pcm.c`, `mic_adc_pcm_convert_sample()` | ADC raw sample | Signed `int16_t` PCM; contract documents 16 kHz mono at lines 124-152. Call site is `mic_adc_test.c:1775-1776`. |
| VAD/voice handoff | `.../mic_adc_test.c`, `mic_adc_voice_stream_activate()` / `mic_adc_voice_stream_push_sample()` | VAD start, pre-roll and live PCM | Calls registered backend append callback for pre-roll and live chunks (`:1053-1099`, `:1202-1287`). |
| Voice orchestration | `ESPC51/components/Middlewares/voice_domain/voice_chain.c`, `voice_chain_start()` | Registered microphone callbacks | Registers `prepare_cb`, `append_pcm_cb`, `finish_cb` with `mic_adc_test_set_voice_stream_ops()` at lines 1053-1063; these resolve to `server_voice_client_*`. |
| Upload buffer and local request | `.../server_voice/server_voice_client.c`, `server_voice_client_append_pcm()` / `server_voice_client_finish_turn()` | `int16_t` PCM | Appends to `upload_buf` at 680-704, then posts it through `server_comm_http_post_raw_fixed_stream_begin()` at 707-813. The C5 sends after recording ends; it is not a live chunked upload to S3. |
| C5 target and protocol | `.../server_voice/server_voice_protocol.h`; `.../esp111_protocol_common.h` | PCM body | Target `/local/v1/voice/turn`; `Content-Type: audio/L16; rate=16000; channels=1`; `X-Audio-Format: pcm_s16le_mono_16k` (`server_voice_protocol.h:10-28`, protocol header lines 145-171). |
| Device context | `.../device_protocol/device_protocol_metadata.c`, `device_protocol_prepare_metadata()` | Local device/gateway/time state | Adds `X-Device-Id`, `X-Gateway-Id`, device type, firmware version, request sequence, uptime/time-sync, payload type `voice.turn`, and optional `X-Room-Id` at lines 48-100. The voice client copies this metadata and adds `X-Audio-Format` at `server_voice_client.c:724-739`. |
| Response playback | `.../server_voice/server_voice_client.c`, `server_voice_response_process()` / `server_voice_play_response_chunk()` | S3-returned raw PCM | Reads the local HTTP response and sends chunks to the speaker player. The response worker is notified immediately after upload at lines 804-813; its documented module role is PCM playback, not ASR/LLM/TTS. |

## S3 Evidence

`ESPS3/components/Middlewares/local_http_server/local_http_server.c:1099-1114` binds the C5 endpoint `/local/v1/voice/turn` to `voice_proxy_handle_turn()`.

`ESPS3/components/Middlewares/voice_proxy/voice_proxy.c` performs the following:

1. `voice_proxy_handle_turn()` validates `X-Device-Id`, enforces the C5 allowlist and reserves a single turn (`:461-514`).
2. `voice_proxy_process_reserved_turn()` receives the entire C5 PCM body (`read_pcm_body()`), then calls `server_client_post_voice_turn()` (`:306-360`).
3. Its callback `stream_to_httpd()` writes every Server response chunk straight to the C5 HTTP response (`:209-228`).

The S3-to-Server request is an HTTP POST to `/api/voice/turn`, created in `ESPS3/components/Middlewares/server_client/server_client.c:1782-2036`. It preserves the audio type/format and sends `X-Device-Id`, `X-Gateway-Id`, and optional `X-Gateway-Token` (`:1876-1897`).

S3 does **not** perform ASR, call an LLM, invoke tools, or make local intent decisions in this route. This is both borne out by the calls above and stated by the public module contract in `voice_proxy.h:6-9`; the implementation contains only admission control, byte buffering/forwarding, status/error mapping, and response streaming.

## ESP-server Voice Route

| Function | File | Input | Output / behavior |
|---|---|---|---|
| HTTP entry | `src/routes/voiceRoutes.js`, `handleVoiceTurn()` | Raw PCM request from S3 | Validates content type, format, body and device metadata; dispatches `runVoiceTurnChain()` at lines 375-541. Route registration is `POST /api/voice/turn` at line 596. |
| ASR entry | `src/voice/chain.js`, `requestVoiceAsr()` | PCM buffer | Opens the configured ASR WebSocket, sends session update, base64 PCM append events, commits audio, and returns final transcript (`:54-134`). |
| ASR text extraction | `src/voice/realtimeEvents.js`, `parseAsrRealtimeEvent()` | Realtime JSON event | Extracts `transcript`/`text`; `requestVoiceAsr()` accepts it only when final (`chain.js:78-111`). |
| Prompt assembly | `src/voice/chain.js`, `requestVoiceTurnLlm()` -> `src/services/llmPromptContextService.js`, `buildLlmPrompt()` | ASR text plus `deviceId` | Builds a device/environment context and appends `用户：<ASR text>` (`chain.js:137-152`; prompt service `:78-98`). |
| Voice LLM entry | `src/voice/chain.js`, `requestVoiceTurnLlm()` | Assembled prompt | Calls `requestLlmText()` directly, not `runAgentConversation()` (`:145-152`). |
| Actual LLM HTTP request | `src/llm/textClient.js`, `requestLlmText()` -> `requestLlmChat()` | One `role:user` message | `requestLlmText()` passes `[]` for tools (`:288-295`); `requestLlmChat()` only includes a `tools` field when the array is non-empty (`:200-231`). |
| TTS entry | `src/voice/chain.js`, `requestVoiceTts()` | LLM reply text | Chooses realtime WebSocket for `ws/wss`, otherwise HTTP TTS; normalizes output to PCM (`:324-330`). |
| Ordered chain | `src/voice/chain.js`, `runVoiceTurnChain()` | PCM and device ID | ASR -> ordinary LLM -> TTS, then returns PCM (`:333-373`). |

The server consumes `X-Device-Id` in `src/voice/http.js:44-60`, and uses it for voice activity/context lookup. This is device context for prompt construction and status tracking; it does not make the voice turn Agent-enabled.

## Agent and Tools: Real but Disconnected from Voice

### Agent entry and prompt

The actual Agent entry is `ESP-server/src/agent/agentRunner.js:33-76`, `runAgentConversation()`. It loads `src/prompts/esp-home-agent-system-prompt.txt`, builds system/dynamic context, supplies `toolRegistry.openAiTools()` to `requestLlmChat()`, processes tool calls for at most three rounds, and appends each tool result as a `role: tool` message.

Only these server routes call it in current source:

- `src/routes/llmTextRoutes.js:25-81`, `POST /api/llm/text`.
- `src/routes/structuredLlmRoutes.js:51` (structured LLM API).

`server.js:125` mounts the voice router separately from the Agent text routers at lines 150-151. No source call connects `runVoiceTurnChain()` or `requestVoiceTurnLlm()` to `runAgentConversation()`.

### weather_query

| Item | Real implementation |
|---|---|
| Existence and registration | `src/agent/defaultToolRegistry.js:15-50` registers `weather_query` with `handler: weatherQuery`. |
| Tool schema | Same file, lines 18-30: OpenAI function schema with optional `location: string`. `src/agent/toolRegistry.js:22-31` converts it to the `tools` request field. |
| Dispatcher | `src/agent/toolRegistry.js:33-54`, `invoke(name, rawArguments, context)`: locates the handler, parses JSON arguments, and returns its result or an explicit failure object. |
| Execution | `src/agent/agentRunner.js:59-68`: invokes the dispatcher for each returned `tool_call`. |
| OpenWeather call | `src/agent/weatherQuery.js:78-139`: calls `/data/2.5/weather` then `/data/2.5/forecast` through `fetchOpenWeather()` (`:35-56`, `:106-128`). It fails closed without `OPENWEATHER_API_KEY` (`:13-21`, `:78-82`). |
| Result to LLM | `agentRunner.js:25-30` serializes the result as `{ role: "tool", tool_call_id, content: JSON.stringify(result) }`; the next loop round calls the LLM with that message (`:46-68`). |

Therefore `weather_query` is a working server-side Agent capability in source, but it is **not registered with the LLM request generated by the voice route**.

## Trace: “现在天气怎么样”

This trace describes what current code will request, not a claim that an external provider succeeded.

| Hop | File: function | Input | Output |
|---|---|---|---|
| 1 | `ESPC51/.../mic_adc_test.c: mic_adc_test_task()` | Microphone ADC readings | Converted PCM samples, pre-roll and VAD-driven recording. |
| 2 | `ESPC51/.../mic_adc_test.c: mic_adc_voice_stream_push_sample()` | PCM after wake/VAD | Calls backend append callback in recording state. |
| 3 | `ESPC51/.../voice_chain.c: voice_chain_server_voice_append_pcm()` -> `server_voice_client_append_pcm()` | PCM chunks | C5 upload buffer. |
| 4 | `ESPC51/.../server_voice_client.c: server_voice_client_finish_turn()` | Completed PCM buffer and metadata headers | POST `/local/v1/voice/turn` to S3. |
| 5 | `ESPS3/.../local_http_server.c: voice_proxy_handle_turn()` | C5 HTTP request | Queued S3 voice proxy job. |
| 6 | `ESPS3/.../voice_proxy.c: voice_proxy_process_reserved_turn()` -> `server_client_post_voice_turn()` | Same PCM and device ID | POST `/api/voice/turn` to ESP-server. |
| 7 | `ESP-server/src/routes/voiceRoutes.js: handleVoiceTurn()` | PCM, `X-Device-Id`, gateway headers | Validated request and invocation of `runVoiceTurnChain()`. |
| 8 | `ESP-server/src/voice/chain.js: requestVoiceAsr()` | PCM | Final ASR text, expected to be `现在天气怎么样` for this trace. |
| 9 | `ESP-server/src/voice/chain.js: requestVoiceTurnLlm()` | ASR text plus database-derived device context | One ordinary LLM prompt via `requestLlmText()`. No system Agent prompt, no tools schema, no `tool_calls` loop. |
| 10 | `ESP-server/src/voice/chain.js: requestVoiceTts()` | Ordinary LLM textual reply | 16 kHz mono PCM. |
| 11 | `ESPS3/.../voice_proxy.c: stream_to_httpd()` | Server PCM response chunks | Streams PCM back through the original C5 HTTP response. |
| 12 | `ESPC51/.../server_voice_client.c: server_voice_response_process()` | S3-returned PCM | Speaker playback, then voice chain returns to listening. |

The trace stops before `weather_query`: there is no executable edge from hop 9 to `runAgentConversation()`, `toolRegistry.openAiTools()`, `toolRegistry.invoke()`, or `weatherQuery()`.

## Classification and Remediation Boundary

**Classification: A.** The voice chain does not enter Tool system.

The immediate structural cause is the direct call in `src/voice/chain.js:145-152` to `requestLlmText()`, whose implementation in `src/llm/textClient.js:288-295` calls `requestLlmChat(..., [], ...)`. The only code that sends tool schemas and executes tool calls is `runAgentConversation()`, and the voice route never calls it.

No remediation was implemented, per the read-only scope. Any future change should preserve the existing voice-specific device-context prompt and make an explicit decision on how the Agent result, tool timeout/error behavior, and the voice turn's total timeout interact before replacing the direct LLM call.

## Verification Limits

The audit used static source traversal and call-site searches only. It does not prove that:

- the C5/S3 devices are reachable or microphone/VAD wakes on hardware;
- `VOLC_GATEWAY_*` configuration, ASR, chat, and TTS endpoints work;
- `OPENWEATHER_API_KEY` and home location are configured or OpenWeather succeeds;
- the deployed server matches this checkout; or
- the LLM would choose a tool if the Agent route were invoked.

The workspace was already dirty before this report was added; no pre-existing changes were modified or reverted.

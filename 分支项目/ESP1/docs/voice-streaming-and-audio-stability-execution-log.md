# Voice Streaming And Audio Stability Execution Log

This file records only the implementation work requested by the voice streaming plan. It does not replace the user-maintained implementation plan.

## Local Verification Boundary

- Do not start ESP-server, Express, background services, or network listeners.
- Do not contact production, real databases, devices, or the Volcengine gateway without explicit later approval.
- Allowed evidence: source audit, `node --check`, pure offline unit tests, C51/C52/S3 compilation without flash, and parity/static checks.
- The existing `npm test` BME690 v3 failure is an unrelated baseline record and is intentionally not repaired in this voice task.

## P0 - Offline Fixture And Protection Tests

Status: complete (local offline gate)

Modified files:

- `ESP-server/test/voice/fixtures/volc-tts-realtime-sanitized.json`
- `ESP-server/test/voice/voice-streaming-offline.test.js`
- `docs/voice-streaming-and-audio-stability-execution-log.md`

Verification target:

- The artificial fixture proves a session acknowledgement, five supported audio aliases, first audio before completion, and a byte-exact buffered PCM reference.
- The unit test also preserves rejection of odd-length PCM and unsupported audio containers.

Remaining risk:

- The fixture proves parser/consumer behavior only. A real Volcengine canary, live HTTP first-write timing, and C5 playback remain deployment-environment validation.

Rollback point:

- Remove the P0 fixture/test files only; no production behavior has changed.

Verification result:

- `node --check test/voice/voice-streaming-offline.test.js`
- `node test/voice/voice-streaming-offline.test.js` -> `PASS`
- `git diff --check` and `git -C ESP-server diff --check`

## P1 - C51/C52 Resident Playback Resources

Status: complete (local source and build gate)

Modified files:

- `ESPC51/components/Middlewares/speaker/speaker_player.c`
- `ESPC51/components/Middlewares/speaker/speaker_player.h`
- `ESPC51/components/BSP/IIS/iis.h`
- Matching `ESPC52` files under the same paths

Implementation:

- `audio_player_init()` now creates the persistent IIS/DMA, ringbuffer, synchronization objects, scratch allocation, and one writer task before Mic/VAD starts.
- Per-turn playback only advances a generation, enables IIS, drains PCM, and returns to `READY_DISABLED`; the writer owns the IIS disable transition.
- Abort no longer deletes resources or directly disables IIS from a non-owner task. It fails closed if the writer does not acknowledge within the existing bounded wait.
- Prompt/beep playback now reuses the same stream lifecycle. `server_voice_rx` remains per-turn and C5 upload semantics are unchanged.

Verification result:

- C51/C52 P1 source files are byte-identical.
- Static check: one `speaker_iis_writer` creation site and `iis_deinit()` only on initialization rollback.
- `idf.py build` passed for `ESPC51` and `ESPC52`; no flash or device interaction occurred.

Remaining risk:

- Continuous voice, abort, Wi-Fi disconnect, wake prompt/beep, actual DMA/heap values, writer HWM, and first-chunk-to-I2S latency require device validation.

Rollback point:

- Revert the paired `speaker_player.{c,h}` and IIS header documentation changes; no C5 protocol or server-facing route changed.

## P2a - Offline Realtime TTS Streaming Consumer

Status: complete (local source and offline gate; streaming remains disabled by default)

Modified files:

- `ESP-server/src/voice/pcmAligner.js`
- `ESP-server/src/voice/pcmQueue.js`
- `ESP-server/src/voice/ttsStream.js`
- `ESP-server/src/voice/realtimeEvents.js`
- `ESP-server/src/voice/realtimeSocket.js`
- `ESP-server/src/voice/turnConfig.js`
- `ESP-server/src/voice/http.js`
- `ESP-server/src/voice/chain.js`
- `ESP-server/src/routes/voiceRoutes.js`
- `ESP-server/test/voice/fixtures/volc-tts-realtime-sanitized.json`
- `ESP-server/test/voice/voice-streaming-offline.test.js`
- `ESP-server/test/voice/voice-streaming-pipeline.test.js`
- `docs/voice-streaming-and-audio-stability-execution-log.md`

Implementation:

- `VOICE_TTS_STREAMING_ENABLED` defaults to off. HTTP TTS and flag-off WebSocket TTS stay buffered; the existing one-shot `input_text.append` plus `input_text.done` request contract remains unchanged.
- Flag-on WebSocket TTS reuses the existing session/auth/model/event source and emits aligned PCM16 through a bounded queue. It does not implement P2b text-incremental TTS.
- A small raw-PCM prefix gate rejects WAV, JSON, and encoded/container signatures before the first response write. The output route omits `Content-Length`, honors write backpressure, and records generated PCM bytes separately from bytes passed to `res.write()`.
- Completion aliases remain compatible for response/audio completed events while `tts_session.completed` is not treated as audio completion. Client abort, queue overflow, provider failure, and partial response errors terminate the upstream source once; a partial PCM response is ended without appending a JSON error body.
- The offline tests use only sanitized fixture data, in-memory queues, and fake response/WebSocket objects. They do not create listeners or contact a provider.

Verification result:

- `node --check` passed for all changed P2a voice source and test files.
- `node test/voice/voice-streaming-offline.test.js` -> `PASS`.
- `node test/voice/voice-streaming-pipeline.test.js` -> `PASS`.
- The streaming consumer and the actual buffered WebSocket fallback consume the same sanitized fixture and produce byte-identical final PCM.
- The pipeline test covers PCM sample alignment, odd tail, empty audio, raw-container rejection, completion aliases, bounded queue overflow, idempotent abort, writer backpressure, partial-write accounting, and streaming flag default/on/off parsing.
- `git diff --check` and `git -C ESP-server diff --check` passed. Static scope review found no diffs under `ESP-server/public/**`, database/schema, Dashboard UI/API, package manifests, or non-voice service paths.

Remaining risk and deployment-environment validation:

- No ESP-server process, HTTP route listener, real Volcengine request, canary, production database, S3/C5 connection, or device test was run.
- The sanitized fixture proves consumer behavior and response-write ordering only. A real canary must still prove the live gateway emits raw PCM16 mono 16 kHz audio before provider completion, honors abort promptly, and remains byte/format compatible.
- The prefix gate intentionally delays the first write by up to 12 provider bytes so it can reject containers safely. Its actual latency impact and live format coverage require canary evidence.

Rollback point:

- Keep `VOICE_TTS_STREAMING_ENABLED` unset or false to select buffered output immediately. Revert only the listed P2a voice source/test files to remove the dormant streaming implementation; no C5/S3 protocol, public asset, schema, or non-voice route rollback is required.

## P3 - S3 EOF, Disconnect, And Metrics Hardening

Status: complete (local source, pure-test, and compile gate)

Modified files:

- `ESPS3/components/Middlewares/server_client/server_client.c`
- `ESPS3/components/Middlewares/server_client/server_client_voice_eof.h`
- `ESPS3/components/Middlewares/server_client/server_client_voice_eof_test.c`
- `ESPS3/components/Middlewares/voice_proxy/voice_proxy.c`
- `docs/voice-streaming-and-audio-stability-execution-log.md`

Implementation:

- Known `Content-Length` completion now requires exact byte equality. An overread is rejected before forwarding that chunk, and a zero-read before the declared length is an incomplete response failure.
- Chunked and unknown-length bodies now complete only when `esp_http_client_is_complete_data_received()` reports completion. Incomplete zero-reads retain the existing 20 ms bounded wait and are counted as `repeated_zero_reads`.
- A downstream `httpd_resp_send_chunk()` failure returns from the callback immediately, closes the current upstream client once, and prevents further upstream reads. `esp_http_client_cancel_request()` was intentionally not used in this synchronous path because ESP-IDF 5.5.4 closes then reconnects that client context.
- Voice-only summaries now include chunks, known-length mismatch, repeated zero reads, downstream close, and close result. A zero-length HTTP chunk is sent only on the successful response path; a partial upstream failure is not marked as a clean chunked completion.
- The fixed C5 upload `Content-Length`, S3 request cache, 1024-byte response buffer, local route contract, mutex/busy ownership, scheduler, and non-voice paths remain unchanged.

Verification result:

- Native pure C test: `server_client_voice_eof_test.c` compiled with `cc -std=c11 -Wall -Wextra -Werror` and reported `voice eof offline tests: PASS`.
- `idf.py build` passed in `ESPS3` with ESP-IDF 5.5.4. No flash, monitor, network connection, or device access occurred.
- Static review confirmed callback failure stops additional reads, only the successful branch emits `httpd_resp_send_chunk(req, NULL, 0)`, and P3 does not touch C5 fixed-length upload or non-voice local routes.
- `git diff --check` and `git -C ESP-server diff --check` passed; C51/C52 P1 files remain byte-identical.

Remaining risk and deployment-environment validation:

- No live ESP-server chunked response, real Volcengine stream, S3/C5 HTTP turn, downstream disconnect, busy-release trace, or device playback test was executed.
- The pure test covers EOF decisions, not ESP-IDF socket behavior, callback-close timing, terminator wire behavior, or hardware recovery. Those require deployment-environment validation.
- The existing 60 s voice deadline remains unchanged; the separate `VOICE_TOTAL_TIMEOUT_MS` macro drift is outside P3 scope.

Rollback point:

- Revert only the listed P3 server-client/voice-proxy helper and test files. This restores the prior EOF and partial-response behavior without changing C5, ESP-server, public assets, schema, or non-voice contracts.

## Deployment Canary And P4 - Pending Human Execution

Status: not executed by design

Blocking authorization boundary:

- Local work is explicitly prohibited from starting ESP-server, opening an HTTP listener, contacting the Volcengine gateway, accessing production data, or connecting to S3/C5 devices.
- `VOICE_TTS_STREAMING_ENABLED` must remain unset or false until the real canary passes. P4 must not begin before that authorization and evidence exist.

Required deployment-environment gates:

- Real Volcengine canary: compare buffered and streaming use of the same short request; prove first audio event and first response write precede provider completion; verify raw PCM16 little-endian, 16 kHz, mono, non-empty/even bytes, abort, provider error, empty/wrong-format handling, listener cleanup, and flag-off rollback.
- S3/C5 end-to-end: prove chunked Server response reaches S3 then both C51/C52 playback before final completion; exercise downstream disconnect, known/unknown EOF, terminator/busy release once, and no continued upstream consumption after C5 disconnect.
- C5 non-voice and stability regression: continuous turns, abort, Wi-Fi disconnect/reconnect, wake prompt/beep, Mic/VAD ordering, player/IIS DMA memory/HWM, first-chunk-to-I2S latency, BME, CSI, heartbeat, status, command ACK, event, and register paths.

Stop/rollback rule:

- Any canary, end-to-end, memory, compatibility, or non-voice regression failure keeps the streaming flag off and stops further rollout. The local rollback points in P1, P2a, and P3 remain independent.

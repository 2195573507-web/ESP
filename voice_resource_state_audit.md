# C5 Resource State And Voice Chain Audit

Date: 2026-07-20

Scope: the live `ESPC51` and `ESPC52` C5 firmware sources. Archive and branch-project copies were excluded. `VOICE_EXCLUSIVE` remains the background-resource lease; it does not represent ownership of Mic DMA or speaker I2S.

## Result

One race was found and fixed in both live `voice_chain.c` files.

Before the fix, `voice_chain_prepare_for_server_voice_start()` changed the resource manager to `QUIESCING` and waited for HTTP/BME/worker acknowledgements. A gateway-loss callback in that interval observed the new generation from the manager, but `s_voice.lease` was not populated until `c5_resource_manager_begin_voice()` returned. The terminal event was therefore rejected as stale by the voice task. The callback could then continue into wake acknowledgement even though the gateway had been lost.

The fix has two parts:

1. A lock-protected `wake_prepare_in_progress` gate holds terminal cleanup until the Mic callback has either queued the local-wake event or failed. This prevents a terminal handler from releasing/restarting the Mic while the original Mic task is still issuing `server_prepare` and `MIC_PAUSE`.
2. The Mic callback rechecks `gateway_link_can_start_voice_turn()` immediately after `begin_voice()` returns. A loss during `QUIESCING` now enters the existing terminal-error path under the published `VOICE_EXCLUSIVE` lease. It does not attempt resource release from the Mic task itself.

The release path and `VOICE_EXCLUSIVE` semantics are unchanged.

## State Transition Audit

There is no symbol named `c5_resource_state_transition()` in either live target. The state-transition implementation is the internal `c5_resource_set_state()` in `c5_resource_manager.c`; the public transition owners are below.

| Transition / operation | Live call owner | Evidence |
| --- | --- | --- |
| `STANDBY -> QUIESCING` | `c5_resource_manager_begin_voice()` | `ESPC51/.../runtime/c5_resource_manager.c:190-229` and matching C52 file |
| `QUIESCING -> VOICE_EXCLUSIVE` | `c5_resource_manager_begin_voice()` after HTTP, BME, and worker acknowledgements | C51 `:231-257` |
| `QUIESCING -> ERROR -> STANDBY` | begin failure rollback | C51 `:259-276` |
| `VOICE_EXCLUSIVE -> RELEASING` | `c5_resource_manager_release_voice()` | C51 `:371-409` |
| `RELEASING -> RELEASING` | response/speaker release failure retry | C51 `:411-437` |
| `RELEASING -> STANDBY` | response abort/wait, speaker I2S release, BME/workers/HTTP resume complete | C51 `:439-465` |
| acquire / release callers | `voice_chain_prepare_for_server_voice_start()` and `voice_chain_release_voice_resources()` | C51 `voice_chain.c:384-405,558-626` |
| state checkpoint callers | wake-ack playback and server-response playback use generation-checked `note_phase()` / `set_audio_phase()` | C51 `voice_chain.c:751-805`; `server_voice_client.c:342-406,795-810` |

Generation checks protect response callbacks and terminal events. `c5_resource_manager_lease_is_current()` only accepts the current generation while the manager is in `VOICE_EXCLUSIVE`; phase updates also accept `RELEASING` so release logging cannot mutate a newer lease.

## DMA And I2S Audit

`MIC_DMA_RELEASED` is emitted only after the Mic task has acknowledged pause, its audio cache is cleared, and `mic_adc_test_release_for_speaker()` has stopped/deinitialized ADC resources:

`voice_chain_quiesce_mic_for_speaker()` -> `mic_adc_test_pause()` -> `mic_adc_test_wait_paused()` -> `mic_adc_test_clear_audio_cache()` -> `mic_adc_test_release_for_speaker()` -> `MIC_DMA_RELEASED`.

Evidence: `ESPC51/components/Middlewares/voice_domain/voice_chain.c:751-782`; the C52 flow is equivalent. `mic_adc_test_release_for_speaker()` maps to stop, task acknowledgement, task deletion, ADC deinit, EventGroup deletion, and buffer/stack release in `mic_adc_test.c:2003-2098`.

There are no literal `MIC_DMA_ALLOCATED`, `I2S_INIT`, or `I2S_DEINIT` log tokens in the live targets. Their functional sites are:

| Requested token | Actual ownership point |
| --- | --- |
| `MIC_DMA_ALLOCATED` | `mic_adc_test_start()` creates/configures ADC continuous DMA and creates the Mic task; its session generation changes only after the new task starts. |
| `I2S_INIT` | `audio_player_init()` calls `iis_init()` before allocating the speaker ring/staging and persistent writer task. |
| `I2S_DEINIT` | `audio_player_release_session()` calls `iis_deinit_timed()` only when no speaker stream is open. It runs before C5 resumes BME/workers/HTTP. |

Server PCM opens I2S only after `voice_chain_server_playback_start_sink()` has completed Mic quiesce. The response callback checks response generation, lease generation, and current lease before it opens/writes the stream (`server_voice_client.c:342-406`).

## Wake Ack, Server Prepare, And Mic Resume

The normal order is:

1. Mic VAD callback acquires `VOICE_EXCLUSIVE`, pauses non-voice work, performs the lightweight synchronous `server_voice_client_prepare_async()`, requests Mic pause, then queues local wake acknowledgement.
2. The voice task waits for Mic pause acknowledgement, releases Mic DMA, plays wake ack/I2S, releases the speaker session, recreates Mic ADC/VAD, and opens the recording window.
3. On server reply or failure, the terminal event is generation checked. It performs Mic cleanup, resource release in audio-first order, then recreates Mic ADC/VAD and returns to listening.

`server_prepare` itself does not start a separate worker; it briefly changes the server client from `IDLE` to `PREPARING` and back. The response worker is created at client initialization and receives work only after upload completion. `mic_adc_test_resume()` is not used by `voice_chain`; recovery recreates Mic ADC/VAD after resource release, so a stale pause bit cannot resume an old ADC handle.

The new gate closes the only observed overlap between a terminal gateway event and the prepare callback. It also makes the `QUIESCING` gateway-loss window explicit and recoverable.

## Verification

- `git diff --check`: passed.
- Static call scan completed for both live C5 targets, including all `c5_resource_manager_*` transition/phase calls and all requested DMA/I2S tokens.
- ESPC51: isolated `idf.py -B build-voice-resource-audit-c51 build` completed; `00_Learn.elf` and `00_Learn.bin` are present.
- ESPC52: its isolated configured build compiled `esp-idf/Middlewares/CMakeFiles/__idf_Middlewares.dir/voice_domain/voice_chain.c.obj` successfully.
- No flash, erase, monitor, gateway connection, or board-level wake/response stress test was run. Hardware scheduling and I2S/ADC coexistence therefore remain device-validation work.

## Parity Note

During this audit, the two live `voice_chain.c` files contained unrelated concurrent changes and are not byte-identical outside this fix (task-handle lifecycle and heap-protection code differ). The added wake-prepare gate and post-quiesce gateway recheck are present in both files with the same behavior; no unrelated C51/C52 differences were overwritten.

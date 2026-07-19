# P1/P2 Sensor Ingress And Voice Resource Arbitration Execution Report

## Scope

This report covers only the 2026-07-13 firmware work:

- P1: fail-fast `ESPS3 /local/v1/sensor` ingress under contention.
- P2: mirrored C51/C52 voice resource arbitration.

No ESP-server process was started. No files under `ESP-server/public`, the database,
`managed_components`, or `archive` were modified for this task. No flash, monitor,
erase, or fullclean operation was run. Existing backend worktree changes were preserved.

## Root Cause And Changes

P1 had two pre-enqueue waits that could use `portMAX_DELAY`: resource-session snapshot
and runtime event-bus admission. This could leave the C5 sensor client waiting for its
own long HTTP timeout rather than receiving a local overload result.

The sensor handler now creates one 100 ms deadline across both waits. Each stage receives
only the remaining budget; a timed admission failure returns `ESP_ERR_TIMEOUT`, maps to
HTTP 503, and leaves aggregation, cache work, and S3-to-Server upload asynchronous. The
`SENSOR_INGRESS` diagnostic separately rate-limits success and failure records, while
normal sensor requests no longer emit two generic debug telemetry records each time.

P2 previously had independent business gates but no single resource owner. A voice turn
could overlap background HTTP/reconnect work, audio cleanup failure could resume background
work before I2S/DMA/session resources were released, and terminal callbacks used a lossy
queue plus mutable Mic generation.

The new manager owns one generation lease and moves through:

```text
STANDBY -> QUIESCING -> VOICE_EXCLUSIVE -> RELEASING -> STANDBY
                    \-> ERROR -> rollback -> STANDBY
```

Quiesce order is normal HTTP admission, BME pause ACK, CSI callback/worker pause ACK, then
background-worker ACK. Any failure rolls completed steps back in reverse order. Release first
aborts and waits for the response receiver, drains/deinitializes speaker/I2S/DMA and frees
session buffers, then resumes CSI, BME, workers, and finally normal HTTP admission.

If response shutdown or audio release times out, the lease and gate remain active in
`RELEASING`; voice-chain retains the generation and posts a reliable release-retry event.
It does not restart Mic or restore background work early.

The response worker freezes its originating lease before clearing its per-turn state and
passes that value to done/error callbacks. Terminal dispatch validates the supplied lease,
so a delayed old response callback cannot be relabeled as, or terminate, a newer turn.

## Modified Firmware Areas

P1:

- `ESPS3/components/Middlewares/local_http_server/local_http_server.c`
- Timed resource/event-bus helper seams in `resource_manager`, `s3_event_bus`, and
  `s3_scheduler` provide the bounded admission boundary.

P2, mirrored under both `ESPC51` and `ESPC52`:

- `components/Middlewares/runtime/c5_resource_manager.{c,h}`
- `components/Middlewares/runtime/app_runtime.{c,h}`
- `components/Middlewares/runtime/c5_runtime_workers.{c,h}`
- `components/Middlewares/sensor_domain/csi_placeholder/csi_service.{c,h}`
- `components/Middlewares/server_comm/server_comm_http.{c,h}` and `gateway_link.{c,h}`
- `components/Middlewares/voice_domain/voice_chain.c`
- `components/Middlewares/server_voice/server_voice_client.{c,h}`
- `components/Middlewares/speaker/speaker_player.{c,h}` and `components/BSP/IIS/iis.{c,h}`
- Supporting Mic, BME, CMake, and orchestration seams required for pause/ACK ownership.

The mirrored common-file set is:

```text
components/BSP/IIS/iis.{c,h}
components/Middlewares/CMakeLists.txt
components/Middlewares/app_orchestrator/app_orchestrator.c
components/Middlewares/command_domain/system_command/system_server_client.c
components/Middlewares/memory/c5_memory.{c,h}
components/Middlewares/mic/mic_adc_test.{c,h}
components/Middlewares/runtime/app_runtime.{c,h}
components/Middlewares/runtime/c5_resource_manager.{c,h}
components/Middlewares/runtime/c5_runtime_workers.{c,h}
components/Middlewares/sensor_domain/bme690/server_client/bme_server_client.c
components/Middlewares/sensor_domain/csi_placeholder/csi_service.{c,h}
components/Middlewares/server_comm/gateway_link.{c,h}
components/Middlewares/server_comm/server_comm_http.{c,h}
components/Middlewares/server_voice/server_voice_client.{c,h}
components/Middlewares/speaker/speaker_player.{c,h}
components/Middlewares/voice_domain/voice_chain.c
```

## State And Lock Order

```text
Acquire:
normal HTTP gate -> wait normal HTTP idle -> BME pause/wait -> CSI pause/wait
-> worker quiesce/wait -> VOICE_EXCLUSIVE -> recording/upload -> response/playback

Release:
response abort/wait -> speaker drain/session release/I2S deinit -> CSI resume
-> BME resume while HTTP stays closed -> worker resume -> reopen normal HTTP -> STANDBY
```

The resource-manager lock protects only lease state and is never held while a bounded
operation waits. Persistent coordinator/response/writer tasks remain blocked in standby but
do not hold per-turn upload buffers, speaker ring/scratch, I2S channel, or DMA session
resources.

## Diagnostics

Relevant runtime markers:

- `SENSOR_INGRESS`
- `C5_RESOURCE_STATE`
- `VOICE_START_STAGE`
- `SPEAKER_PLAYER_READY`
- `IIS_DMA_CHECK`
- `C5_MEM`

Required phases include `quiesce_begin`, `quiesce_complete`, `before_recording`,
`after_upload_buffer_alloc`, `after_upload_buffer_free`, `before_i2s_init`,
`after_i2s_init`, `playback_complete`, `after_i2s_deinit`, and `release_complete`.

## Verification

| Check | Result |
|---|---|
| `ESPS3 idf.py build` | PASS |
| `ESPC51 idf.py build` | PASS |
| `ESPC52 idf.py build` | PASS |
| Top-level `git diff --check` | PASS |
| Nested `ESP-server` `git diff --check` | PASS |
| C51/C52 modified common-file comparison | PASS; no unexpected differences |
| New C5 `/api/`, absolute Server URL, or raw CSI upload scan | No matching added source lines |
| New `portMAX_DELAY` in S3 sensor ingress diff | No matching added line |

## Hardware Validation Still Required

No hardware values are claimed by this report. Validate on both C51 and C52:

1. Hold the S3 resource-session lock and event-bus lock separately; confirm sensor ingress
   returns HTTP 503 near the single 100 ms budget and C5 enters its existing non-2xx backoff.
2. Confirm normal sensor ingress still returns 202 and does not wait for aggregation or Server
   upload.
3. At standby, verify `SPEAKER_PLAYER_READY` and `IIS_DMA_CHECK` do not show an allocated
   playback session/I2S/DMA. Record the `C5_RESOURCE_STATE` heap and DMA baseline.
4. During a wake turn, verify BME, CSI, workers, health/register, heartbeat, status, command
   poll, and ordinary HTTP do not begin after lease acquisition.
5. Verify Mic pauses before waiting for the response, upload memory is freed before playback,
   and response/I2S/session resources are released before background resume.
6. Repeat normal, timeout, response abort, and gateway-link-loss turns. Confirm release retry
   clears the lease and `s_non_voice_paused` once resources become releasable, and confirm
   internal largest-block does not steadily decline.

## Residual Risk

The static builds prove compilation and boundary wiring, not live timing, driver cleanup,
heap fragmentation, DMA availability, callback scheduling, or reconnect behavior. Existing
standby coordinator/writer task stacks remain deliberately resident; only per-turn playback
session resources are guaranteed absent by source. A device baseline must establish the
acceptable fixed standby heap/DMA budget before rollout.

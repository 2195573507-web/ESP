# Resource Lifecycle and Pipeline Repair

- Date: 2026-07-22
- Scope: `ESPC51`, `ESPC52`, and `ESPS3` resource lifecycle, capability-memory admission, VAD-gated voice, BLE radar lifecycle, remote-radar freshness, and staged rollback. `ESP-server` is intentionally unchanged.
- Status: source repairs and three isolated ESP-IDF builds are recorded below. No firmware has been flashed and no hardware, serial, Wi-Fi, BLE, radar, microphone, LCD, WakeNet, or Server end-to-end acceptance is claimed.

## Problem and Decision

The preceding [resource and pipeline audit](resource_and_pipeline_audit_20260722.md) established that C5 voice ownership needed a VAD recording-window boundary, C5 BLE shutdown did not previously release the complete NimBLE lifecycle, and S3 needed stricter admission and rollback behavior. The repair keeps C5 responsible for mic/VAD and PCM upload only; WakeNet remains on S3 and Server proxying remains after a WakeNet hit.

The implementation does not use larger watchdog timeouts, unbounded retries, or a blanket PSRAM migration as a substitute for ownership. Cache-off and DMA-sensitive paths remain subject to their existing Internal/DMA requirements. Large non-DMA voice bodies and temporary WakeNet tail storage are admitted from PSRAM instead of falling back to Internal RAM.

## Source Evidence and Changes

### C51/C52 BLE radar

- `radar_ble_transport` now has a lifecycle state (`UNINITIALIZED`, `INITIALIZING`, `RUNNING`, `STOPPING`, `STOPPED`, `FAILED`) and tracks the NimBLE port, callout, and in-flight GAP callbacks.
- Start accepts only one active initialization, enters `FAILED` on binding or `nimble_port_init` failure, and routes a failed initialized port through stop before retry.
- Stop first atomically acquires `STOPPING` so a concurrent caller cannot issue a second cleanup. It then gates callbacks, stops the backoff callout, cancels scanning, requests link termination, and waits only until the bounded 2500 ms deadline for GAP callbacks. A tracked stop task runs the NimBLE stop/deinit; a timeout retains resources while cleanup continues and start returns `ESP_ERR_INVALID_STATE` until it reaches `STOPPED`.
- The `radar_ble_ms_to_ticks()` and runtime helper clamp non-zero millisecond waits to at least one tick. This avoids the C5 100 Hz `pdMS_TO_TICKS(1)` zero-tick failure mode.
- `radar_ble_runtime_start()` rolls the radar domain and PSRAM task back when transport start fails; `radar_ble_runtime_stop()` stops transport before deleting the runtime task and domain.

Primary evidence: `ESPC51/components/Middlewares/radar_ble/radar_ble_transport.c:39-70`, `:614-748`, and `radar_ble_runtime.c:79-132`; C52 is behaviorally identical.

### C5 voice lifecycle

- `voice_chain` keeps a non-zero tick helper for the one-millisecond wake-prepare wait and shares the same state/lease/retry behavior across C51/C52.
- `server_voice_client` rejects reentrant turns, requires an active resource lease before allocating the upload buffer, accepts PCM only while streaming, frees the upload buffer after the request is submitted, and exposes cancellation/abort paths for playback and HTTP-stream shutdown.
- The response task stack and PCM upload path remain PSRAM-oriented. The C5 voice path remains VAD-window gated; it does not own WakeNet.

Primary evidence: `ESPC51/components/Middlewares/voice_domain/voice_chain.c:111-115,1129-1143` and `ESPC51/components/Middlewares/server_voice/server_voice_client.c:635-776,799-813`.

### S3 memory and WakeNet boundary

- `voice_proxy` admits the received PCM body only from PSRAM and records Internal, DMA, and PSRAM free/minimum/largest-block telemetry around the allocation. A PSRAM admission failure returns an error rather than consuming a large Internal allocation.
- `audio_wake_gateway` records the same capability snapshots before model create, after create, after detect, and after destroy. The per-request model is still destroyed on every exit path. A sub-chunk PCM tail is now zero-padded in PSRAM and evaluated once instead of being silently discarded.
- Existing S3 worker changes use staged creation markers and failure cleanup for queues, mutexes, PSRAM storage, and worker tasks; Wi-Fi and alarm ownership remain constrained by module-owned resources only.

Primary evidence: `ESPS3/components/Middlewares/voice_proxy/voice_proxy.c:144-181`, `ESPS3/components/Middlewares/audio_wake_gateway/audio_wake_gateway.c:41-125`, and `ESPS3/components/Middlewares/network_worker/network_worker.c:2531-2657`.

### Remote radar state

The S3 radar path retains source-specific state and separates an unavailable/disabled local device from a disconnected remote device. Freshness must be derived from the source update clock rather than from target count so an empty but reporting radar does not become offline. This area requires device-side observation of transition and recovery logs before it can be marked accepted.

## C51/C52 Parity Regression

Direct current-source comparison found:

| Surface | Result |
| --- | --- |
| `radar_ble_transport.c` | byte-identical |
| `voice_chain.c` | byte-identical |
| `server_voice_client.c` | byte-identical |
| `radar_ble_runtime.c` | only `source=C51` versus `source=C52` log identity strings differ |
| BLE/runtime and voice tick helpers | both use the same `ticks == 0 ? 1 : ticks` conversion where one-millisecond waits are used |

No C51/C52 parity code edit was required by this regression pass.

## Build Evidence

| Target | Evidence | Meaning and limit |
| --- | --- | --- |
| ESPS3 | `idf.py -C ESPS3 -B /tmp/esp-fix-esps3-build build` completed successfully in a clean isolated directory | Compiles and links the current S3 repair set; it is not a runtime or device test. |
| ESPC51 BLE | `idf.py -C ESPC51 -B /tmp/esp-fix-espc51-build build` completed successfully; `/tmp/esp-fix-espc51-build/00_Learn.bin` was generated | Compiles and links the current C51 BLE repair; it is not a runtime or device test. |
| ESPC52 | `idf.py -C ESPC52 -B /tmp/esp-fix-espc52-build build` completed successfully; `/tmp/esp-fix-espc52-build/00_Learn.bin` was generated | Compiles and links the current C52 repair set; it is not a runtime or device test. |

## Risks and Required Hardware Verification

1. Exercise repeated BLE start/stop/restart while scanning, connecting, connected, and receiving notifications. Confirm scan cancellation, callout cancellation, disconnect, host stop, port deinit, and restart on both C5 boards.
2. Stress the stop callback deadline and concurrent stop calls. Source serializes `STOPPING` and preserves resources during asynchronous cleanup, but only hardware can confirm NimBLE callback/host timing and repeated restart behavior.
3. Capture capability telemetry at S3 boot, voice-body admission, WakeNet create/detect/destroy, Wi-Fi/BLE coexistence, and after repeated HTTP requests. Check free and largest contiguous Internal/DMA/PSRAM blocks and task high-water marks.
4. Verify C5 VAD rejects idle PCM, accepts a complete spoken VAD segment, and the S3 zero-padded tail does not regress WakeNet behavior or latency.
5. Stop remote C51/C52 reporting without disconnecting it, then verify the S3 status transitions to stale after the configured timeout and returns online only on the next valid frame. Confirm LCD and HTTP views do not log the same transition repeatedly.
6. Force each S3 init stage to fail where feasible and verify reverse-order cleanup, repeat init/stop, and no destruction of shared IDF global objects.

## Rollback

Revert only the scoped C5 BLE lifecycle, C5/S3 voice admission, S3 radar freshness, and S3 rollback commits/files together. Do not retain a partial rollback that restores C5 PCM upload while removing S3 WakeNet gating. Before shipping a rollback, rebuild C51, C52, and S3 in isolated build directories; then repeat the hardware lifecycle checks above.

# ESP Backend API Integration Notes

## Findings

- `ESP-server/docs/api.md` already defines the needed backend routes: gateway-state, `sensor.bme690`, `csi.motion`, logs, smart-home pending/ack, wake prompt cache, and voice turn.
- No backend change is required so far. `ESP-server/public` remains out of scope.
- Existing ESPS3 modules already cover most boundaries:
  - `server_client`: Server-facing HTTP.
  - `sensor_aggregator`: dashboard snapshot and ingest mapping.
  - `command_router`: C5 local command queue.
  - `voice_proxy`: `/local/v1/voice/turn` proxy.
  - `wake_prompt_cache_gateway`: Server prompt config/cache to local PCM.
  - `csi_placeholder_gateway`: CSI summary boundary.
- Existing snapshot code still adds mock appliances. This conflicts with the current requirement to avoid fake real smart-home state and should be removed from firmware snapshots.
- Existing S3 command routing uses old `/api/commands/*`; smart-home P1 needs `/api/smart-home/v1/commands/pending` and `/ack` handled by S3 directly. With no real smart-home hardware attached, ACK must be `failed`.
- Existing S3 non-voice paths skip during `voice_busy`; the current requirement asks voice turn not to block heartbeat/snapshot/log. Periodic snapshot/log work should run in a separate task and not depend on voice completion.

---

# P1/P2 Execution Notes (2026-07-13)

## Baseline

- The top-level repository is `main...origin/main [ahead 1]` and already contains
  unrelated/uncommitted C5, S3, ESP-server, and documentation work. It must be
  preserved and is not attributed to this task.
- The nested `ESP-server` repository has its own untracked `notes.md` and
  `task_plan.md`; it is out of scope and will not be edited or started.
- Both top-level and nested-repository `git diff --check` passed before this task's
  changes.
- Existing `docs/voice-streaming-and-audio-stability-implementation-plan.md` assumes
  startup-resident speaker I2S/DMA. This is intentionally not used as the P2 source of
  truth because the approved P2 plan requires those resources to remain absent in
  standby.

## Execution order

1. Trace and repair only the S3 `/local/v1/sensor` HTTP ingress blocking path. A normal
   accepted request must remain asynchronous and return `202`; bounded ingress timeout
   must return `503` rather than forcing the C5 client to reach its own `408` timeout.
2. Build and statically verify S3 before modifying C5 P2 runtime behavior.
3. Add a C51/C52 mirrored resource owner with explicit `STANDBY`, `QUIESCING`,
   `VOICE_EXCLUSIVE`, `RELEASING`, and `ERROR` states. Keep BME 408 diagnosis separate
   from voice-resource diagnosis.
4. Record all build/static evidence and mark heap, DMA, timing, and repeated-turn
   behavior as hardware validation unless actually observed on a device.

## P1 Result

- The live S3 `/local/v1/sensor` handler was already asynchronous after ingress; the
  source of the C5-visible delay was two pre-enqueue `portMAX_DELAY` mutex waits:
  resource-session snapshot and event-bus admission.
- Added HTTP-only 100 ms timed APIs for those two waits. Generic event-bus and resource
  lifecycle callers retain their existing blocking/reliable semantics.
- Successful sensor ingress keeps `202 Accepted`. A timed resource/session or bus-lock
  failure returns `ESP_ERR_TIMEOUT`, maps to `503`, and lets the existing C5 BME
  non-2xx backoff handle the retry.
- Added a rate-limited `SENSOR_INGRESS` diagnostic with route, identity, body length,
  receive duration, lock/enqueue duration, safely sampled event-bus depth, heap/DMA
  values, and explicit stages including `invalid_content_length`, `recv_timeout`,
  `peer_closed`, `partial_body`, `validation_failure`, `resource_session_lock_timeout`,
  `event_bus_lock_timeout`, and `enqueue_failure`.
- `ESPS3` build passed with ESP-IDF v5.5.4. Actual 100 ms timing, heap/DMA values, and
  C5 retry traces remain hardware validation.

## P2 Result

- Added mirrored `c5_resource_manager` ownership for the whole voice turn. It quiesces
  normal HTTP, BME, CSI, and background workers in that order; it only grants recording
  after every bounded ACK succeeds.
- Normal HTTP now tracks in-flight admission. Gateway reconnect health/register work no
  longer bypasses the voice gate; a blocked reconnect is deferred without manufacturing a
  link-failure count.
- Release is audio-first and fail-closed. If response shutdown or I2S/session release times
  out, the manager remains in `RELEASING`, retains the lease, and voice-chain schedules a
  generation-checked retry. CSI, then BME, then workers, then normal HTTP resume only after
  audio release succeeds.
- Terminal done/error/link-abort delivery is reliable even when the ordinary voice queue is
  full. Terminal events carry the voice lease rather than mutable Mic generation, and a link
  transition in standby does not trigger a spurious voice abort. The response task freezes
  its originating lease before clearing local state and passes that value to done/error
  callbacks, so a delayed old callback cannot terminate a newer turn.
- Phase checkpoints now include wake-ack/response I2S initialization, playback completion,
  upload buffer allocation/free, and `after_i2s_deinit`. Actual heap, DMA, latency, repeated
  turn, and timeout recovery values remain hardware validation.
- Final local builds passed for ESPS3, ESPC51, and ESPC52 with ESP-IDF v5.5.4. Top-level and
  nested `ESP-server` `git diff --check` passed; the existing backend worktree changes were
  preserved and no backend service was started.

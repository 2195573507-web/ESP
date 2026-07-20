# ESPS3 C5 Resource Lifecycle Notes

## Requirements Snapshot

- States: `ACTIVE`, `GRACE`, `RELEASED`, `RESTORING`.
- AP station disconnect enters `GRACE` and immediately releases real-time resources.
- Identity/config/history remain retained during and after grace.
- Any valid register, heartbeat, sensor, or CSI result may confirm identity and restore.
- AP station connect alone must never restore resources.
- Grace is 20 seconds; heartbeat timeout remains 30 seconds.
- CSI suspend clears sample count, validity, quality, and weight; restore restarts warmup.
- `CSI_LATEST` and `CSI_FUSION_TELEMETRY` are silent without an active session.

## Findings

- `AP_STADISCONNECTED` currently discards MAC/AID before `network_worker`, so
  `child_registry_mark_all_link_lost()` cannot identify one C5.
- Existing `LINK_LOST` is intentionally online-ish for 20 seconds. CSI trigger
  and command polling therefore continue unless they use a separate resource gate.
- Protocol, CSI, and network events use different queues. Restore must reject an
  ingress whose `rx_time_ms` predates the current GRACE transition.
- CSI owns a per-link latest cache plus fusion samples and the scheduler event-bus
  latest cache. All three must be cleared or gated on suspend.
- BME replay is implemented in `network_replay_worker`, not in the aggregator.
  Replaying only the global oldest record would let a suspended device either
  retry or block another active device.
- `server_client` performs 2/5/10/30 second retries. Per-device requests need a
  cancellation predicate so release stops future attempts and backoff promptly.

## Selected Design

- Keep identity status in `child_registry`; keep resource session state solely in
  the new `resource_manager`.
- Add per-peer command, sensor, CSI gateway, and CSI fusion gates. Restore order is
  peer IP, command, sensor, CSI warmup, then ACTIVE.
- Propagate AP station MAC to `network_worker` through a new compatible API and
  persist MAC/IP/device mappings after validated ingress. An unmapped disconnect
  must never release every allowlisted peer; a fallback is allowed only when the
  SoftAP has no remaining stations and exactly one session can be identified.
- Retain sensor history, last values, BME cache, registry/config, and command queue.
  Suspend only their live producers/consumers and retry paths.

## Final P1 Closure

- CSI suspend must clear only ingress at or before the disconnect cutoff (plus
  legacy zero-timestamp entries), preserving post-disconnect CSI that can confirm
  identity while the session is in GRACE.
- Device release must actively cancel an in-flight command poll or sensor retry,
  not merely prevent later attempts after `esp_http_client_perform()` returns.
- The HTTP cancellation registry must serialize handle unregister/cleanup with
  `esp_http_client_cancel_request()` so release never dereferences a stale client.

## Final Audit Findings

- `resource_manager` consistently acquires the lifecycle operation mutex before
  its state mutex and releases the state mutex before cross-component suspend or
  restore calls. Release retry follows the same order.
- The legacy CSI result API still has no peer-IP argument. Its signature remains
  compatible, while real network ingress must use the timestamp/peer-aware path;
  a null-IP legacy call can only confirm a peer already mapped in the registry.
- The command ACK enqueue path now uses a zero-tick send while holding the queue
  mutation mutex, so a full command queue cannot block resource release forever.
- `/sensor` ingress validates `PROTOCOL_ADAPTER_MESSAGE_SENSOR_BME690` before
  identity confirmation; a status envelope posted to that URI cannot restore.
- `AP_STACONNECTED` does not restore resource sessions. Restore is driven only by
  validated register, heartbeat, sensor, or CSI ingress after the disconnect cutoff.
- `CSI_LATEST` and `CSI_FUSION_TELEMETRY` are gated by ACTIVE resource sessions;
  RESTORING can consume CSI for warmup but does not print those ACTIVE-only logs.
- `server_client_cancel_peer_requests()` scopes cancellation to command polling
  and device ingest/replay requests. Gateway state, voice, CSI event uploads, and
  command ACK HTTP requests remain outside the cancellation registry.

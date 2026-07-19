# ESPS3 C5 Resource Lifecycle Progress

## 2026-07-10

- Started task and recorded immutable scope boundaries.
- Detected pre-existing dirty changes in all three firmware projects and ESP-server.
- Will preserve all pre-existing changes and edit only ESPS3 files required by this task.
- Completed parallel audits of session/network, CSI, command, and sensor retry paths.
- Selected an independent per-peer resource state machine with timestamped restore
  validation and MAC-based disconnect targeting.
- Resumed from the existing partial implementation without reverting any user changes.
- Re-read the task-local design, plan, and live ESPS3 diffs; started focused audits of
  session/registry wiring, CSI teardown/warmup/log gating, and command/sensor retry cancellation.
- Serialized release/restore side effects and moved stale-signal checks ahead of peer mapping.
- Kept sessions in RESTORING until the fifth distinct CSI warmup tick commits ACTIVE.
- Preserved the legacy one-argument CSI result API and added timestamp/peer-aware extensions.
- Made AP disconnect delivery high priority, removed unsafe release-all fallback, and added
  final per-peer CSI trigger/session checks at the UDP send boundary.
- Prevented passive status and voice completion from extending or reviving identity state.
- `git diff --check -- ESPS3` passed before the full build.
- Resumed final closure from the persisted plan and audited live code against the
  remaining P1 list: CSI cutoff-aware queue cleanup, unmapped disconnect isolation,
  and active cancellation of device-scoped HTTP requests.
- Replaced the obsolete release-all fallback in the recorded design with a
  zero-station plus uniquely identifiable-session fallback requirement.
- Audited the lifecycle lock order and identity-confirm path. Cross-component
  resource side effects run without the resource-manager state mutex held.
- Rechecked two earlier review findings against current source: nonblocking command
  ACK enqueue and strict BME690 sensor-kind validation are already present.
- Implemented and reviewed unmapped AP disconnect isolation: MAC mapping remains
  primary; fallback requires a successful zero-station snapshot plus exactly one
  live session, otherwise the event is deferred without releasing another peer.
- Added cutoff-aware CSI cleanup across the event-bus STATE slot, fusion worker
  queue, and gateway suspend path. Only zero/invalid timestamps or ingress at or
  before the session cutoff are removed; later CSI remains eligible to restore.
- Added S3 monotonic `rx_time_us` to the CSI latest diagnostic cache so cutoff
  comparisons never mix the C5 payload clock with S3 uptime.
- Updated resource-manager release and retry paths to pass the current session
  cutoff, and rerun idempotent cleanup on every valid AP disconnect generation.
- Added device-scoped active HTTP request registration and cancel generations for
  command polling, realtime BME upload, and BME replay. Resource release now calls
  `server_client_cancel_peer_requests()` and retries any incomplete cancellation;
  unrelated gateway, voice, CSI, and ACK requests remain unregistered.
- Completed final live-source review of resource manager, Wi-Fi AP disconnect,
  command/sensor gates, CSI suspend/restore/warmup, cutoff-aware queue cleanup,
  and server-client device-scoped request cancellation.
- `git diff --check -- ESPS3 s3_resource_lifecycle_task_plan.md
  s3_resource_lifecycle_notes.md s3_resource_lifecycle_progress.md` passed.
- First `idf.py build` attempt failed before compilation because `idf.py` was not
  on PATH in the noninteractive shell. Loaded
  `/Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh` and reran successfully.
- Final ESPS3 build passed. Generated `build/sensair_s3_gateway.bin`, size
  `0x10c670`; smallest app partition remaining `0x5f3990` bytes, about 85% free.

# C5 Session Resource Lifecycle Design

## Scope

Implement device-level C5 resource lifecycle management in ESPS3. ESP-server,
ESPC51, and ESPC52 remain unchanged. Existing public interfaces stay available.

## State Model

Each allowlisted device has one resource session state:

- `ACTIVE`: command, sensor, and CSI resources may run.
- `GRACE`: identity may recover for 20 seconds, but all live resources are off.
- `RELEASED`: grace expired or heartbeat timed out; resources remain off.
- `RESTORING`: a validated post-disconnect identity signal is restoring resources.

AP station disconnect changes the mapped device from ACTIVE to GRACE and releases
resources immediately. A validated register, heartbeat, sensor, or CSI result with
a receive timestamp at or after the current GRACE start may restore the session.
AP station connect and status-only traffic never restore it.

## Disconnect Targeting

The Wi-Fi callback forwards station MAC/AID and callback time to `network_worker`.
Validated ingress records the peer IP and the SoftAP station MAC in
`child_registry`. On disconnect, `network_worker` resolves MAC to one device and
releases that session. If mapping is unavailable, it logs the fallback and applies
the existing release-all/link-lost behavior.

## Resource Operations

Release is idempotent and runs outside the resource-manager mutex:

1. Suspend command polling and local command delivery for the peer.
2. Suspend new sensor upload and cancel queued/retry work while retaining cache.
3. Suspend CSI trigger, ingest processing, latest diagnostics, and fusion link.
4. Clear the matching event-bus/latest upload diagnostics.

Restore updates peer IP first, changes to RESTORING, restores command and sensor
resources, restores CSI with a per-link five-sample warmup, then changes to ACTIVE.
CSI suspend clears sample count, validity, quality, and fusion weight.

## Queue And Retry Safety

All consumers re-check peer resource state. Ingress received before the current
disconnect cannot restore the session. Sensor and command HTTP retry loops use a
cancellation predicate checked before attempts and during backoff. BME replay
selects the oldest record belonging to an ACTIVE device, avoiding head-of-line
blocking while retaining suspended-device records.

## Logging

Every state transition emits `SESSION_STATE_CHANGE`. Release and successful
restore emit `RESOURCE_RELEASE` and `RESOURCE_RESTORE` with device and reason.
`CSI_LATEST` and `CSI_FUSION_TELEMETRY` are not emitted when no ACTIVE session
exists.

## Verification

Review touched paths to prove ESP-server/ESPC51/ESPC52 are unchanged, then run
`idf.py build` from the ESPS3 project using the configured ESP-IDF 5.5.4 toolchain.

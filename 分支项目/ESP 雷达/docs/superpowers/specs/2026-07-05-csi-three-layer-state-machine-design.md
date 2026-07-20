# ESP-111 CSI Three-Layer State Machine Design

## Goal

Refactor ESP-111 CSI handling from an occupancy/result pipeline into a three-layer state system:

- C5 terminals sense and compress WiFi CSI into low-dimensional features.
- ESPS3 is the only CSI decision and fusion source.
- ESP-server stores, serves, and streams S3 facts without signal processing.

The canonical CSI state vocabulary is `IDLE`, `MOTION`, and `HOLD`. The old `unknown`, `vacant`, and `occupied` occupancy vocabulary is removed from active CSI paths instead of being preserved as a compatibility contract.

## Current Anchors

Live code inspection on 2026-07-05 shows these active surfaces:

- `ESPC51` and `ESPC52` share `components/Middlewares/sensor_domain/csi_phase_a` and `csi_placeholder`.
- Current C5 CSI code uses fixed selected subcarriers, Hampel filtering, a 32-frame window, and C5-side occupancy/motion score generation.
- `ESPS3/components/Middlewares/protocol_adapter` maps local CSI payloads to `csi.motion`.
- `ESPS3/components/Middlewares/csi_placeholder_gateway` receives C5 CSI summaries and forwards them through `sensor_aggregator`.
- `ESPS3/components/Middlewares/sensor_aggregator` builds dashboard snapshots and is the natural place to publish S3-owned CSI state facts.
- `ESP-server/src/services/csiMotionService.js` and `dashboardService.js` currently validate and merge old occupancy-shaped CSI data, mostly in memory and snapshot JSON.

## Architecture

### C5 Edge Sensing

C5 handles raw CSI only inside the WiFi CSI callback and local feature code. It does not output raw CSI, I/Q arrays, subcarrier arrays, selected-subcarrier lists, or C5-side state decisions.

Lifecycle:

1. `INIT`: CSI service configured but no features emitted.
2. `CALIBRATION`: 5 to 10 seconds after CSI start.
3. `RUN`: selected subcarriers and baseline are frozen until restart or explicit recalibration.

Calibration collects CSI amplitude, RSSI, and per-frame energy. It computes:

- `baseline[k]`: mean amplitude per subcarrier.
- `E0`: mean frame energy across calibration frames.
- `sigma0`: standard deviation of frame energy.
- `noise_floor`: MAD-derived robust noise estimate.
- `selected_subcarriers`: 20 to 40 subcarriers selected after removing DC and guard carriers, then keeping subcarriers whose variance lies in the P30 to P70 band.

RUN processing:

1. Convert I/Q to amplitude.
2. Subtract baseline: `H_clean[k] = amplitude[k] - baseline[k]`.
3. Compute temporal delta: `delta[k] = H_clean[k] - previous_H_clean[k]`.
4. Apply EWMA smoothing with alpha in the 0.2 to 0.3 range.
5. Emit only:
   - `device_id`
   - `link_id`
   - `frame_energy`
   - `variance`
   - `rssi`
   - `timestamp`

Hampel filtering, raw CSI caching, and subcarrier-level upload are removed from active code paths. Debug-only placeholders must return unsupported or be compile-disabled; they must not be reachable runtime upload paths.

### S3 Gateway Decision

S3 accepts feature streams from C51, C52, and any local C5 identity exposed by the existing local protocol. It does not accept raw CSI, I/Q arrays, phase, or subcarrier arrays.

Per-link tracking:

- Maintain the latest feature frame for each link.
- Use EWMA and/or a 16-frame window for energy statistics.
- Keep per-link `mean_energy`, `variance`, `rssi`, freshness, and quality flags.

Scoring:

- Compute a normalized score from energy variation and energy level:
  `motion_score = norm(variance_energy) + lambda * norm(mean_energy_delta)`.
- Clamp `motion_score` to `[0, 1]`.
- Keep thresholds configurable in `gateway_config`, with conservative defaults.

Fusion:

- Fuse link scores by weighted average:
  `fused_score = w1*C51 + w2*C52 + w3*C5`.
- Ignore stale or missing links by renormalizing active weights.
- If no feature link is fresh, keep state `IDLE` with score 0 and mark CSI unavailable in diagnostics.

State machine:

- Canonical states: `IDLE`, `MOTION`, `HOLD`.
- `IDLE -> MOTION`: fused score >= `T_high` for N consecutive updates.
- `MOTION -> HOLD`: fused score drops below `T_high` but remains above `T_low`, or after a short no-motion hold timeout.
- `HOLD -> IDLE`: fused score <= `T_low` for N consecutive updates or hold timeout expires.
- Any stale-input condition must not produce artificial motion.

S3 publishes the canonical fact model:

```json
{
  "device_id": "sensair_shuttle_01",
  "link_id": "S3_TO_C51",
  "state": "IDLE",
  "frame_energy": 12.34,
  "variance": 0.42,
  "rssi": -51,
  "motion_score": 0.18,
  "timestamp": 1783260000000
}
```

For fused home-level state, `device_id` is the S3 gateway or configured fused CSI device, and `link_id` is `fused`.

### ESP-server State Center

ESP-server is only a state receiver, store, API provider, and SSE broadcaster. It does not calculate CSI variance, smoothing, filtering, subcarrier logic, or motion score.

Server validation accepts only the canonical fact model:

- `device_id`
- `link_id`
- `state`
- `frame_energy`
- `variance`
- `rssi`
- `motion_score`
- `timestamp`

Valid `state` values are `IDLE`, `MOTION`, and `HOLD`.

Server persistence adds a dedicated CSI state table or equivalent durable table with those columns plus server receive metadata. `raw_csi` and `subcarrier_data` are not created, accepted, or exposed.

SSE and API output use the same model. Dashboard code reads from the canonical model and shows:

- `motion_score` trend
- `frame_energy` trend
- state timeline

Old raw CSI visualization and old occupancy-specific semantics are removed from active dashboard paths.

## Data Flow

```text
WiFi CSI raw callback
  -> C5 local amplitude/baseline/delta/EWMA feature extraction
  -> C5 /local/v1/csi/result feature frame
  -> S3 protocol_adapter feature validation
  -> S3 csi gateway/fusion state machine
  -> S3 server_client POST csi.motion canonical fact
  -> ESP-server validate/store/broadcast
  -> Dashboard trend and timeline views
```

## Error Handling

- C5 calibration with too few frames keeps the service in calibration or emits no feature frame.
- C5 invalid CSI frames are dropped locally.
- C5 upload failures wait for the next feature frame; raw CSI is never retried.
- S3 rejects CSI payloads containing raw CSI or subcarrier arrays.
- S3 stale links are excluded from fusion rather than treated as motion.
- Server rejects malformed state facts and records only accepted facts.
- Server-side rejection does not cause C5 to fake success or local motion.

## Scope Boundaries

- C5 never calls ESP-server `/api/...` routes.
- ESPS3 remains the only server-facing gateway.
- Server-side CSI code must remain free of signal-processing algorithms.
- `ESP-server/public` may be edited only for the dashboard CSI visualization required by this goal.
- Existing BME690, voice, command, smart-home, heartbeat, and time-sync behavior must not be regressed by CSI work.

## Implementation Plan

1. Update shared protocol constants and local CSI payload shape.
2. Refactor C51/C52 CSI structs and feature code for calibration, baseline subtraction, temporal delta, EWMA, and feature-only output.
3. Update C51/C52 runtime service to use dynamic subcarrier selection and remove Hampel/window/presence state generation from active runtime.
4. Add S3 CSI fusion/state-machine module and wire it from local CSI ingress.
5. Update S3 server-facing serialization to publish canonical `csi.motion` facts.
6. Add server durable CSI state persistence, validation, API/SSE output, and dashboard data shaping.
7. Update dashboard UI to show score, energy, and state timeline without raw CSI views.
8. Run C51/C52 parity checks, C firmware compile/syntax checks where available, server JS syntax/tests/smoke checks, and boundary scans for raw/subcarrier/server-processing drift.

## Verification Gates

Required evidence before completion:

- `rg` proves no active upload path emits `raw_csi`, `subcarrier_data`, I/Q arrays, or selected-subcarrier arrays.
- `rg` proves server CSI service has no smoothing/filtering/subcarrier/motion-score computation.
- C51 and C52 CSI source files remain in parity except intentional identity/config differences.
- C5 CSI tests or compile checks cover calibration selection and feature extraction.
- S3 tests or compile checks cover score fusion, hysteresis, and stale-link behavior.
- Server validation accepts canonical `IDLE/MOTION/HOLD` facts and rejects old occupancy-only payloads.
- Server database migration creates durable canonical CSI storage.
- Dashboard API/SSE output includes canonical CSI facts.
- Frontend no longer depends on raw CSI or subcarrier data.
- Existing backend smoke or regression tests pass, or any environment limitation is documented with the strongest available substitute check.

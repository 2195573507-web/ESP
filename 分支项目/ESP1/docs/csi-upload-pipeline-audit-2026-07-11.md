# ESP-111 CSI Upload Pipeline Audit

- Audit date: 2026-07-11
- Method: source-only audit of the current working tree, including uncommitted changes.
- Scope: ESPC51, ESPC52, ESPS3, and the nested `ESP-server` checkout.
- Excluded: no source changes, build, flash, packet capture, or runtime measurement.

## Executive Conclusion

The active data path is summary-only and layered correctly: raw I/Q exists only transiently in each C5 callback and feature worker; C5 sends compact feature JSON over HTTP to ESPS3; ESPS3 coalesces by link, fuses at 100 ms, and holds only the newest canonical event for Server upload. ESP-server accepts only canonical CSI v2 and stores the fused fact, not raw CSI or C5 feature vectors.

The normal steady-state upper bounds are:

- C5 report: at most 10 HTTP feature reports/s per terminal, subject to a callback arriving, calibration completing, gateway readiness, voice state, and backpressure.
- ESPS3 local receive: at most about 20 accepted HTTP reports/s from C51+C52 under that C5 limit. The controlled UDP trigger attempts 20 sends/s per active C5, but this is not a measured CSI callback rate.
- ESPS3 fusion: one 100 ms tick, nominally 10 fusion finalizations/s while an active session exists; ingress is latest-per-link, so not every received C5 frame is necessarily fused.
- ESPS3 to ESP-server: at most 1/s for `MOTION` and `HOLD`, 1/2 s for `IDLE`, with a newest-only cache. A state transition is marked due immediately, but a preceding attempt inside the same rate window can still defer it.

These are source-derived cadence limits, not hardware-verified observations. Actual callback, receive, fusion, and upload counts require timestamps from a running system.

## Complete Pipeline

```text
C5 Wi-Fi RX packet / CSI callback
  -> copy newest I/Q sample only (64 pairs maximum; overwrite older pending sample)
  -> C5 event bus and CSI worker
  -> amplitude conversion, calibration, selected-subcarrier feature extraction
  -> edge detector: EMA + 32-sample window + local IDLE/MOTION hint
  -> HTTP POST /local/v1/csi/result
  -> ESPS3 local_http_server validation and scheduler ingress
  -> per-C51/C52 latest-only CSI fusion worker queue
  -> csi_placeholder_gateway latest diagnostic cache
  -> csi_fusion 100 ms tick, confidence-weighted two-link state machine
  -> canonical v2 serializer and network_worker newest-only upload cache
  -> HTTP POST /kernel/csi_event
  -> ESP-server validation, immediate runtime cache update, low-priority persistence queue
  -> SQLite csi_motion_events + event_logs, dashboard in-process state, SSE event
  -> frontend overview polling / CSI-history database reads
```

## Frequency And Retention Table

| Stage | Source-derived frequency | Data volume / shape | Cached | Discarded | Failure handling |
| --- | --- | --- | --- | --- | --- |
| S3 UDP CSI trigger -> C5 | configured 50 ms, attempt 20/s per live C5 | `"ping trigger csi"`, 16 bytes | no | skipped when voice busy or scheduler is under hard pressure | UDP enqueue failure is logged; no delivery retry contract |
| C5 Wi-Fi CSI callback | packet-driven; no fixed Hz in source | transient I/Q, max 64 pairs; converted to amplitude | one pending sample | overwrite newest pending sample on callback burst | callback only queues work; invalid/empty CSI is ignored |
| C5 feature process | event-driven; worker processes newest callback sample | low-dimensional feature frame | two latest feature slots | old unread feature invalidated | process/report suppressed when C5 is not allowed to run |
| C5 -> S3 report | configured minimum 100 ms, at most 10/s per C5 | JSON in a 512-byte builder buffer; fields below | one latest feature only | a newer feature replaces an unread one | one HTTP POST, 5 s timeout; no C5 retry or local historical queue |
| S3 local HTTP ingress | bounded by C5 reports, normally <=20/s aggregate | validated compact CSI JSON | scheduler/event queues | 503 on queue/memory/state failure; per-link queue coalescing | C5 receives 202 only after accepted enqueue |
| S3 fusion worker | nominal 100 ms, 10 ticks/s | C51/C52 latest feature states | 16-item CSI worker queue plus one latest item per link | queued same-link ingress coalesced; stale/resource-generation items dropped | queue-full drops, flush request is coalesced and retried by scheduler state |
| S3 -> ESP-server | MOTION/HOLD <=1/s; IDLE <=0.5/s | canonical v2 JSON: schema, trace, tick, links, state, confidence, timestamp | one newest JSON generation | previous latest body is freed; no history/replay | no retry of same generation; next fused event replaces it; invalid Server payload is discarded |
| ESP-server ingestion | receives S3 rate above | strict canonical v2, seven top-level fields | runtime cache plus in-memory low-priority queue | queue is not bounded/coalesced in current code | response is 202 after memory update/queueing; worker retries a failed batch in memory |
| ESP-server persistence/UI | worker every 500 ms, batches up to 100 | `csi_motion_events`, event log, SSE fact | SQLite history and process-memory latest state | none by CSI service itself; process restart loses unflushed queue/cache | transaction rollback requeues the whole failed batch |

## ESPC51 / ESPC52

### 1. Capture And Trigger

`app_orchestrator_start()` starts CSI after Wi-Fi is stable and the local gateway is ready (`ESPC51/components/Middlewares/app_orchestrator/app_orchestrator.c:98`). `MAIN_ENABLE_CSI_SERVICE=1` by default (`ESPC51/components/app_config/app_main_config.h:35`). The C52 counterpart is mirrored.

`csi_service_rx_cb()` is registered with `esp_wifi_set_csi_rx_cb()` and is the capture callback (`ESPC51/components/Middlewares/sensor_domain/csi_placeholder/csi_service.c:170`, `:203`). The callback:

1. reads `wifi_csi_info_t` supplied by Wi-Fi receive processing;
2. skips the first two I/Q pairs when `first_word_invalid` is set;
3. copies at most 64 I/Q pairs, RSSI, and `esp_timer_get_time()/1000` into one pending slot;
4. overwrites that slot if it was still valid; and
5. enqueues `C5_EVENT_CSI_READY` only.

This proves callback/worker separation, but it also means the raw callback count is not inferable as a fixed number per second. It follows received Wi-Fi packets. S3 independently sends a UDP CSI trigger every configured 50 ms to each online active C5 (`ESPS3/components/Middlewares/gateway_config/gateway_config.h:44`, `ESPS3/components/Middlewares/csi_placeholder_gateway/csi_placeholder_gateway.c:933`, `ESPS3/components/Middlewares/runtime/s3_scheduler.c:1724`); that creates a nominal 20/s traffic stimulus, not a guaranteed one-to-one CSI callback count.

Wi-Fi CSI is enabled with HE legacy/HT20/SU acquisition where supported, or LLTF+HTLTF, LTF merge and channel filtering on the non-HE configuration (`csi_service.c:182`). `esp_wifi_set_csi(true)` finalizes enablement (`:215`). The capture code does not enable promiscuous mode.

### 2. C5 Edge Processing

Raw CSI is never transmitted. It is copied transiently into the pending I/Q slot, converted to amplitudes `sqrt(I^2+Q^2)`, and handed to the feature processor (`csi_capture.c:15`; `csi_service.c:72-129`). No I/Q, phase, amplitude arrays, selected subcarriers, or raw CSI fields are placed in the C5 HTTP body.

Feature extraction details (`csi_feature.c`):

- Calibration defaults: 7 s duration, at least 50 frames, 2 s variance convergence, epsilon 0.75, RSSI >= -82 dBm (`:403-418`). The initializer enforces at least 5 s calibration and 50 frames (`:420-450`).
- Guard/DC subcarriers are excluded. Candidates are ranked from the 30th-70th calibration-variance percentile with stability weighting. The configured selection is 20-40 out of no more than 64 input subcarriers (`csi_types.h:23-26`, `csi_feature.c:256-350`).
- For each selected carrier: subtract calibration baseline, derive inter-frame delta, then apply EWMA with alpha 0.25. `frame_energy` is the mean absolute smoothed delta; `variance` is the population variance of those magnitudes; `cv=sqrt(variance)/frame_energy` (`csi_feature.c:510-557`).
- `quality=1/(1+variance+EWMA(mean_delta))`, clamped to [0,1]. `rssi` is copied from Wi-Fi CSI metadata. The pre-edge feature hint is IDLE/HOLD/MOTION at variance 80/260 or CV 0.08/0.22 (`csi_feature.c:112-130`, `:548-562`).

The edge detector is the published local decision. It retains a 32-sample rolling feature window and applies a second EMA (alpha 0.25). Its motion score is `0.45*normalized_variance + 0.35*normalized_cv + 0.20*relative_energy_delta`; variance normalizes from 80 to 260 and CV from 0.08 to 0.22 (`csi_edge_detector.c:108-145`). It enters MOTION at score >=0.55 sustained 300 ms and returns to IDLE at <=0.35 sustained 500 ms (`csi_edge_detector.h:22-28`, `csi_edge_detector.c:148-200`). Confidence combines quality, window fill, and state separation (`csi_edge_detector.c:283-300`).

### 3. C5 -> S3 Protocol

CSI uses HTTP, not the legacy UDP stream: `server_comm_http_post_json(ESP111_PROTOCOL_ROUTE_CSI_RESULT, ...)` posts to `POST /local/v1/csi/result` with a 5 s timeout (`csi_server_client.c:25-30`, `:114-130`). The C5 URL builder only permits `/local/v1` paths and constructs `http://<terminal_config.gateway_ip>`; it rejects complete Internet URLs (`server_comm_config.c:18-26`, `:79-122`).

The current default compact JSON is actually:

```json
{
  "id": 1,
  "device_id": "sensair_shuttle_01",
  "lid": "S3_TO_C51",
  "t": 123456,
  "state": "IDLE|MOTION",
  "motion_score": 0.0,
  "confidence": 0.0,
  "quality": 0.0,
  "rssi": -60,
  "energy": 0.0,
  "variance": 0.0,
  "cv": 0.0
}
```

The serializer is at `ESPC51/components/Middlewares/device_protocol/envelope_builder.c:168-233`. `id` is a static local identity (`1` for C51, `2` for C52), not a sequence. `t` is the local C5 monotonic uptime timestamp from `esp_timer_get_time()/1000`, not wall-clock Unix time. S3 derives an internal sequence as the low 32 bits of `t` (`ESPS3/components/Middlewares/protocol_adapter/protocol_adapter.c:783-788`). C51 uses `S3_TO_C51`; C52 uses `S3_TO_C52`; this is the observed intended paired difference in their client files.

Important implementation/doc drift: `CSI_OUTPUT_ENABLE_DEBUG_METRICS` defaults to `0` and its comment says energy/variance/CV are omitted (`app_main_config.h:60-63`), but the `#else` serializer still includes all three fields (`envelope_builder.c:213-231`). The active source therefore sends those metrics.

Reporting is event-driven but rate-limited. The CSI worker processes each accepted event, while report runs only when `now >= next_report`; default report interval is 100 ms and may be increased by gateway-not-ready, voice, queue, or CPU backpressure (`c5_runtime_workers.c:102-124`, `c5_backpressure_controller.c:253-279`). There is no per-frame sequence, retry loop, offline spool, or historical CSI buffer. A failed HTTP POST is logged and lost; a newer feature replaces the prior feature.

## ESPS3 Receive And Fusion

### 1. HTTP Entry And Parsing

`local_http_server` registers `POST /local/v1/csi/result` to `csi_result_handler` (`ESPS3/components/Middlewares/local_http_server/local_http_server.c:875-938`, `:1151-1165`). It parses and validates the local envelope first, then stores body, receive time, and peer IP in `s3_runtime_ingress_t` and returns `202 Accepted` only when scheduler enqueue succeeds (`:635-744`).

`protocol_adapter` accepts compact edge JSON, validates `id`, `device_id`, `lid`, timestamp, state/motion_score/confidence, quality/RSSI and feature metrics, and recursively rejects raw signal fields including `raw_csi`, I/Q, phase, subcarrier arrays/matrices, amplitudes, and selected-subcarrier fields (`protocol_adapter.c:195-306`, `:704-822`). Only `S3_TO_C51` and `S3_TO_C52` are accepted internal links (`:135-140`).

The ingress is routed to a dedicated CSI fusion worker, not the normal protocol worker (`s3_scheduler.c:1638-1656`). Its queue depth is 16, budget is 12 items per yield, and it coalesces queued ingress by C51 or C52 latest state key (`s3_scheduler.c:72-85`, `:1124-1195`, `:1198-1245`). Resource-generation stale frames are also dropped (`:667-701`). Therefore the receive boundary can accept more frames than the fusion step preserves.

`csi_placeholder_gateway` keeps a separate diagnostic latest snapshot per link with counters and last timestamp. This cache is RAM-only and is overwritten on every accepted feature (`csi_placeholder_gateway.c:470-508`); its 10-second diagnostic logger does not add a history store (`:872-930`).

### 2. Fusion

Fusion uses a 100 ms tick (`csi_fusion.h:29-31`), and the scheduler requests a flush on that cadence while any active session exists (`s3_scheduler.c:1733-1740`). Each feature is assigned to a timestamp tick. A late frame for an already finalized tick is ignored; a newer tick finalizes the former tick (`csi_fusion.c:597-666`). Restored links require five distinct warmup ticks before participating (`csi_fusion.c:649-658`). A sample older than 3 s is not fresh (`csi_fusion.c:16`, `:195-206`).

For C51 and C52, the per-link score is a blend of C5 motion score (70%) and metrics score (30%); metrics score combines energy, variance, and CV. Each link's weight is quality * RSSI factor * freshness. RSSI maps -90 to -45 dBm and freshness decays linearly over 3 s (`csi_fusion.c:16-32`, `:70-135`, `:499-545`). No fixed C51/C52 preference exists: both use the same formula, so the effective weight changes per feature.

State transitions are:

- IDLE -> MOTION: fused score >=0.62 for five fusion ticks, nominally 500 ms.
- MOTION -> HOLD: score no longer >=0.62.
- HOLD -> IDLE: fused score <=0.30 for 20 ticks, nominally 2 s.
- renewed motion immediately returns HOLD/MOTION to MOTION.

The state machine is at `csi_fusion.c:400-447`; generated fact contains `schema_version=2`, trace ID, tick ID, active links, fused state, fused motion score, confidence, and tick timestamp (`:499-594`).

### 3. S3 -> ESP-server Upload

When fusion produces a valid fact with active sessions, `publish_fusion_outputs()` calls `sensor_aggregator_handle_csi_fact()` (`csi_placeholder_gateway.c:716-753`). That function stores the latest RAM fact, builds canonical v2 JSON, and passes ownership to `network_worker_submit_server_json(NETWORK_WORKER_SERVER_JSON_CSI_EVENT, ...)` (`sensor_aggregator.c:1076-1134`).

The canonical Server body is:

```json
{
  "schema_version": "v2",
  "trace_id": "csi-v2-...",
  "tick_id": 0,
  "links": ["link_0", "link_1"],
  "fused_state": "IDLE|MOTION|HOLD",
  "confidence": 0.0,
  "timestamp_ms": 0
}
```

`protocol_adapter_build_csi_event_v2_json()` emits exactly these seven top-level fields (`protocol_adapter.c:1472-1523`). Internal `S3_TO_C51`/`S3_TO_C52` are converted by output position to `link_0`/`link_1` (`:1417-1469`), because the Server validator requires that exact array-index form.

Semantic loss at this boundary: S3 computes a fused `motion_score`, but the canonical v2 Server payload carries only `confidence`; it has no `motion_score`, energy, variance, CV, RSSI, or per-link metrics. This is deliberate in the current serializer, but it means Server-facing CSI is a fused state/confidence fact rather than a full fusion telemetry record.

`network_worker` does not queue CSI JSON on its general upload queue. It replaces one `s_latest_csi_json` cache entry and increments a generation (`network_worker.c:1002-1039`, `:2539-2558`). It polls this latest value every 250 ms, but sends only after `LINK_STABLE`, active sessions, and cadence gates (`:82-84`, `:2091-2130`). Cadence is 1,000 ms for MOTION and HOLD, 2,000 ms for IDLE (`:106-116`, `:973-1000`). A new state is logically due immediately; however `s_last_csi_attempt_ms` still blocks another attempt inside the current interval (`:1041-1092`). Thus a state transition has no strict immediate-send guarantee.

The transport function is `server_client_post_csi_event_json()` and endpoint is `POST /kernel/csi_event` (`server_client.c:938-960`). CSI uses the telemetry HTTP slot. It does not retry the same body: on failure the next fused fact must replace it; an invalid Server link/schema response explicitly clears the latest item (`network_worker.c:1095-1162`, `:1988-2130`). S3 has no persistent CSI history or offline replay spool. This is intentional latest-state behavior.

## ESP-server Receive, Persistence, And UI

### 1. Route And Validation

The route is `POST /kernel/csi_event` in `ESP-server/src/routes/deviceRoutes.js:207-276`, mounted directly in `ESP-server/server.js:157-170`. It is protected by gateway authentication and gateway binding before payload validation.

`validateCanonicalCsiEventV2()` requires exactly seven top-level keys, `schema_version:"v2"`, a non-empty trace ID, non-negative integer tick ID, positive integer timestamp, confidence in [0,1], state in IDLE/MOTION/HOLD, and a non-empty links array whose item `i` is literally `link_i` (`ESP-server/src/services/csiMotionService.js:22-130`). It does not accept C5 raw fields, feature metrics, or raw CSI at this endpoint.

### 2. Processing Model

The response is deliberately asynchronous:

1. On successful validation, the route immediately updates `runtimeStateCache` with the fused fact (`deviceRoutes.js:241-248`).
2. It enqueues `persistCanonicalCsiEventV2()` as a low-priority in-memory persistence job and returns HTTP 202 (`:249-264`).
3. The worker runs every 500 ms, up to 100 jobs in one SQLite transaction. A failed transaction rolls back and requeues its full batch in memory (`persistenceWorker.js:7-109`; `persistenceQueue.js:22-76`). The current CSI queue has no capacity limit and no CSI-specific coalesce/drop rule.
4. The persistence job updates device activity, writes `csi_motion_events`, updates dashboard service's process-memory latest CSI map, writes an `event_logs` CSI entry, and then broadcasts SSE event `csi_motion` (`csiMotionService.js:200-240`).

`csi_motion_events` stores the fused row plus the canonical body in `raw_json`; it stores `frame_energy`, `variance`, and RSSI as `NULL` because canonical v2 does not send those fields (`csiMotion.js:47-89`, `csiMotionService.js:158-170`). The Server does not persist raw CSI or C5 per-link features.

There is also a current field-name semantic mismatch: `prepareCanonicalCsiEventV2()` assigns the database/API `motion_score` field from the canonical `confidence` value (`csiMotionService.js:115-129`, `:158-170`). Therefore a Server dashboard/history `motion_score` is not the ESPS3 fused motion score; it is confidence unless the contract is extended.

Consequences: the immediate overview state is memory-first; `/api/dashboard/v1/csi/history` is SQLite-only and can lag the 202 by one worker interval or queue delay; SSE is emitted only after the persistence job succeeds. A process restart before flush loses the unpersisted low-priority queue and runtime-cache update.

### 3. Frontend Sources

- `GET /api/dashboard/v1/overview` reads `runtimeStateCache` first and falls back to SQLite only on a cache miss (`dashboardService.js:1505-1517`). The runtime cache overlays the latest fused CSI into the dashboard snapshot (`runtimeStateCache.js:238-314`).
- `GET /api/dashboard/v1/csi/history` reads persisted `csi_motion_events`, ordered ascending after selecting the latest requested rows (`dashboardRoutes.js:167-180`, `dashboardService.js:1420-1445`, `csiMotion.js:72-89`).
- The SSE endpoint `/api/events/v1/stream` exists and `csi_motion` is broadcast after persistence (`eventRoutes.js:188-192`, `eventStreamService.js:69-84`), but the current public dashboard has no `EventSource` consumer. It polls `/api/dashboard/v1/overview`; the S3 page refreshes every 3 s (`public/pages/s3.js:108-114`, `public/app.js:104-105`, `:2695-2704`).

## Direct Answers

### How many CSI data items does C5 produce per second?

There are two different answers:

- Raw callback samples: no fixed source-defined rate. They are Wi-Fi RX packet-driven. S3 attempts a 50 ms UDP trigger, so the controlled stimulus is nominally 20/s per C5, but retransmissions, other Wi-Fi traffic, callback policy, voice/load gating, and loss prevent treating this as a measured sample count.
- C5 -> S3 feature reports: at most 10/s per C5 under the default 100 ms report interval. Actual can be zero during calibration, weak RSSI, no callback, voice activity, non-ready gateway, or backpressure; intermediate features are overwritten.

### How many does S3 receive per second?

For normal C5 reporting, source-derived maximum is about 20 HTTP CSI reports/s across C51 and C52 (10/s each). It is not a promise: local HTTP can reject when queues/resources are unavailable, and the CSI worker intentionally coalesces pending same-link messages.

### How many times does S3 fuse per second?

Nominally 10 fusion flushes/s, from `CSI_FUSION_TICK_MS=100`. It finalizes each tick at most once. Only fresh latest C51/C52 samples participate, and worker/scheduler pressure can delay a flush, so 10/s is a design target rather than a real-time guarantee.

### How many times does S3 upload ESP-server per second?

At most 1/s for fused MOTION or HOLD, 0.5/s for fused IDLE, with only the newest canonical event retained. State changes are prioritized by the due predicate but can still wait up to the last-attempt rate gate (plus the 250 ms worker poll and stable-link gate).

### Is current CSI upload wasteful?

No, relative to a raw/frame-by-frame design. The chain deliberately reduces roughly up-to-20 local feature reports/s to 10 fusion ticks/s and then to 0.5-1 Server event/s. C5 raw and full feature histories are not uploaded. The remaining cost is one canonical JSON and one database/event-log row per 1-2 seconds; the current Server persistence queue is unbounded, so a sustained SQLite outage could grow memory rather than coalesce CSI jobs. That is a backend reliability risk, not a high-frequency network waste.

### Would lowering S3 -> Server frequency affect detection?

Lowering only the Server upload cadence does not change C5 feature extraction or the 100 ms S3 fusion/state machine, so it does not change detection decisions. It does reduce persisted timeline granularity, delays dashboard/SSE updates, and can delay visibility of state transitions unless transitions are granted a separate immediate-send path. The present code already makes the detection/upload separation explicit; any further reduction should retain a bounded transition-latency requirement.

## Audit Limits And Recommended Runtime Evidence

This audit does not claim hardware-realized rates. To verify them without changing behavior, collect existing timestamps/counters from:

- C5: `CSI_TX` / `CSI_REPORT_HTTP` logs and pending overwrite count exposure.
- S3: `CSI_RX`, `CSI_FUSION_EDGE`, `CSI_LATEST`, `CSI_FUSION_EMPTY`, S3 event-bus CSI coalesce/drop statistics, and final payload logs.
- Server: API latency lines for `/kernel/csi_event`, persistence-worker queue logs, SQLite row timestamps, and SSE client observation.

The relevant comparison must distinguish callback count, accepted HTTP count, coalesced fusion count, canonical upload count, and committed SQLite count. A single frontend refresh count cannot establish the CSI sampling rate.

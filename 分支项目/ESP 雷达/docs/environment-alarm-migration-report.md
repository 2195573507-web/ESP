# BME690 Environment Alarm Migration Report

## 1. Scope And Safety Boundary

This migration implements the BME690 environment-alarm path in `ESPS3` only.
No C51, C52, `ESP-server`, server configuration, database, radar protocol,
voice, command, SoftAP, STA, existing BME cache, or existing BME upload
contract was changed. No flash, monitor, server start, production request, or
database operation was performed.

The checkout was already dirty before this work. Existing unrelated radar and
macOS debugger changes were preserved. The environment changes are additive
and do not reset, clean, or replace them.

## 2. Audit Summary

### Real BME V3 Flow

1. C51 and C52 BME690 source trees are byte-identical in the audited
   `sensor_domain/bme690` directories.
2. Both report BME V3 through the local S3 route with temperature, humidity,
   pressure, gas resistance, nested `air_quality`, `boot_id`, request/remote
   sequence metadata, `sample_time_ms`, and `time_synced`.
3. The nested real fields used here are `score`, `level`, `gas_ratio`,
   `stability_score`, and `sensor_state`. S3 does not synthesize READY or a
   stability value.
4. `sensor_aggregator_handle_envelope()` first keeps the existing BME
   snapshot/cache/upload path and then passes the already parsed envelope to
   the environment adapter.

### ESP1 Reference And Differences

The permitted source is
`分支项目/ESP1/ESPS3/components/environment_alarm_engine`. Its core rules,
bounded histories, device slots, and event API were copied. ESP1's direct
integration was not copied because it used remote `envelope->seq` as a
permanent ingest sequence and forced `sensor_state=READY` plus
`stability_score=1.0f`. Current ESPS3 instead consumes the real V3 fields and
maintains S3-local sequence state.

### Server Contract

`POST /api/logs/v1/alarms` is gateway-authenticated and returns HTTP 201 on
creation. The current route is tolerant of omitted application fields, using
fallback values, but S3 supplies `device_id`, `level`, `title`, `message`,
`acknowledged`, `room_id`, `room_name`, `source`, and object `payload` for a
meaningful compatible alarm. The route has no request-level idempotency field;
`alarm_id` and `dedup_key` therefore remain inside `payload`.

## 3. Files

### Copied From ESP1 And Adapted

- `ESPS3/components/environment_alarm_engine/CMakeLists.txt`
- `ESPS3/components/environment_alarm_engine/include/environment_alarm_engine.h`
- `ESPS3/components/environment_alarm_engine/src/environment_alarm_config.c`
- `ESPS3/components/environment_alarm_engine/src/environment_alarm_history.c`
- `ESPS3/components/environment_alarm_engine/src/environment_alarm_events.c`
- `ESPS3/components/environment_alarm_engine/src/environment_alarm_engine.c`
- `ESPS3/components/environment_alarm_engine/src/environment_alarm_rules.c`
- Component README, design notes, and host-test sources under the same
  component.

### New ESPS3 Files

- `ESPS3/components/Middlewares/environment_alarm_adapter/environment_alarm_adapter.c`
- `ESPS3/components/Middlewares/environment_alarm_adapter/environment_alarm_adapter.h`
- `ESPS3/components/Middlewares/environment_alarm_adapter/environment_alarm_sequence.c`
- `ESPS3/components/Middlewares/environment_alarm_adapter/environment_alarm_sequence.h`
- `ESPS3/components/Middlewares/environment_alarm_reporter/environment_alarm_delivery.c`
- `ESPS3/components/Middlewares/environment_alarm_reporter/environment_alarm_delivery.h`
- `ESPS3/components/Middlewares/environment_alarm_reporter/environment_alarm_reporter.c`
- `ESPS3/components/Middlewares/environment_alarm_reporter/environment_alarm_reporter.h`
- `ESPS3/components/environment_alarm_engine/test/test_environment_alarm_acceptance_host.c`
- `ESPS3/components/environment_alarm_engine/test/test_environment_alarm_delivery_host.c`

### Modified Existing ESPS3 Files

- `ESPS3/components/Middlewares/CMakeLists.txt`: additive source/include and
  `environment_alarm_engine` dependency entries. Existing radar dependencies
  remain present.
- `ESPS3/components/Middlewares/gateway_orchestrator/gateway_orchestrator.c`:
  non-fatal adapter initialization and structured init failure log.
- `ESPS3/components/Middlewares/sensor_aggregator/sensor_aggregator.c`:
  adapter call after the pre-existing BME cache/upload work.
- `ESPS3/components/Middlewares/network_worker/network_worker.c` and `.h`:
  additive high-priority environment-alarm JSON type and completion callback.
- `ESPS3/components/environment_alarm_engine/test/run_host_tests.sh`: hosts
  adapter, reporter, delivery, and acceptance targets.
- Component README/design and the four `docs/environment-alarm-*` reports.

### Explicitly Unchanged

- `ESPC51/**`
- `ESPC52/**`
- `ESP-server/**`
- `radar_domain`, `radar_ingest`, `radar_ld2450`, LD2450 BLE/UART, coordinate,
  zone, tracker, and spatial source code
- `gateway_event_reporter` behavior and its global rate limiter
- BME cache manager and original BME upload payload/ownership behavior
- Voice, wake word, microphone/playback, command, Wi-Fi, SoftAP, and STA
  behavior

## 4. Final Data Flow

`C51/C52 BME V3 -> protocol_adapter -> sensor_aggregator (existing cache/upload) -> environment_alarm_adapter -> environment_alarm_engine -> environment_alarm_reporter FIFO -> high-priority network_worker item -> server_client_post_alarm_json -> POST /api/logs/v1/alarms`

The adapter and reporter are separate from `sensor_aggregator`: the ingress
path only maps and submits a bounded sample; it never builds HTTP JSON or
waits for network I/O.

## 5. Rule Set And State Gates

The engine retains 13 rule types:

1. high temperature
2. low temperature
3. fast temperature change
4. high humidity
5. low humidity
6. fast humidity change
7. air-quality warning
8. air-quality critical
9. air-quality deterioration
10. pollution spike
11. environment unstable
12. sensor degraded
13. critical environment composite

WARMUP and UNKNOWN allow only independently valid temperature/humidity rules.
READY enables air-quality, trend, pollution, stability, and composite rules
when their actual fields are valid. DEGRADED may produce the sensor-degraded
rule but cannot be treated as READY for air-quality data. Missing, NaN,
infinite, out-of-range, or invalid-enum fields gate only their dependent rules
and do not stop the original BME pipeline.

## 6. Boot, Sequence, And Time Strategy

- The adapter has one ingress state per C5: last boot id, remote sequence,
  local ingest sequence, timestamps, duplicate/out-of-order/wrap/restart, and
  invalid/missing counters.
- A changed `boot_id` resets only that device's engine state and remote-seq
  comparison. It does not remove events already owned by the reporter FIFO.
- Same boot plus identical remote sequence is dropped as a duplicate. Small
  backwards movement is dropped as out-of-order. `uint32_t` modular comparison
  accepts a real wrap and increments the wrap counter.
- If boot id is absent, a compatibility restart heuristic requires a 60-second
  receive silence plus a backwards small remote sequence. This is a documented
  fallback, not a C5 protocol change.
- `local_ingest_seq` advances only after a newly accepted S3 sample. It is the
  engine's monotonic ingest key.
- Rule timing uses ESPS3 monotonic time. A C5 timestamp is sent only when
  `time_synced` is true; otherwise payload time is null and marked
  `s3_monotonic` instead of fabricating wall time.

## 7. Event, Queue, And Concurrency Semantics

The engine's 64-event FIFO and the reporter's 24-event pending FIFO are
different ownership layers. `peek` copies a prefix. The reporter deep-copies
each accepted event, then acknowledges only that contiguous prefix. A payload
failure, reporter-not-ready state, or full reporter FIFO leaves the engine
event unacknowledged. The reporter task retries engine draining when it frees
pending capacity, so old events do not require another BME sample to move.

The reporter sends only the FIFO head, retaining active/recovered ordering.
It has one completion queue of depth four and an eight-entry fixed dead-letter
record. Short mutex-protected sections protect adapter state and reporter FIFO;
no lock is held across JSON construction or HTTP. The reporter task is
`tskIDLE_PRIORITY + 1` with a configured 3072-byte stack. No new timer is
created and no environmental static storage is placed in PSRAM.

`alarm_id` is generated per activation cycle from engine boot nonce, device,
rule, cycle, local sequence, and available boot id. `dedup_key` includes
device, rule, state, alarm id, and event sequence. The legacy global 10-second
`gateway_event_reporter_alarm()` limiter is not used for this path.

## 8. Delivery Policy

`network_worker` classifies environment alarms as high priority, can evict only
existing low-priority telemetry to make room, and returns a final per-event
completion after an HTTP attempt or terminal local drop. The reporter removes
only confirmed 2xx outcomes. HTTP 408, 429, 5xx, transport errors, unavailable
link, and local queue pressure retain the head and use bounded exponential
backoff. Other 4xx results, plus locally known invalid payload/argument/size
errors, enter the fixed dead-letter record and release the head.

The delay function is `1000 ms << min(retry_count, 6)` capped at 60000 ms.
Remote completion increments the count before scheduling, so its first retry
is 2000 ms; a local submission fallback schedules 1000 ms. This policy is
tested without making a real HTTP request.

## 9. Baseline, Build, And Resource Delta

The pre-change isolated build was recorded before the environment component
was integrated. The final isolated build uses ESP-IDF 5.5.4 and
`build-environment-final`. The checkout also contains unrelated dirty radar
work; the table is a reproducible whole-worktree baseline-to-final comparison,
not an assertion that every byte belongs exclusively to this migration.

| Metric | Baseline | Final | Delta |
| --- | ---: | ---: | ---: |
| Firmware binary | 1,156,192 (`0x11a460`) | 1,183,632 (`0x120f90`) | +27,440 |
| Flash non-RAM | 1,033,126 | 1,060,330 | +27,204 |
| Flash code | 856,154 | 878,446 | +22,292 |
| Flash rodata | 176,716 | 181,628 | +4,912 |
| DIRAM data | 21,820 | 22,060 | +240 |
| DIRAM BSS | 111,472 | 194,512 | +83,040 |
| DIRAM text | 84,707 | 84,707 | 0 |
| DIRAM used / total | 217,999 / 341,760 | 301,279 / 341,760 | +83,280 used |
| DIRAM remaining | 123,761 | 40,481 | -83,280 |
| IRAM used / total | 16,384 / 16,384 | 16,384 / 16,384 | 0 |

Final map highlights are the 54,192-byte engine runtime, 6,400-byte engine
staging area, 9,984-byte reporter pending FIFO, 6,400-byte reporter drain
array, and 1,024-byte reporter JSON buffer. These bounded arrays account for
the expected DIRAM growth. Runtime FreeRTOS queue/mutex/task allocations and
stack high-water marks require on-device inspection; they are not inferred as
passed from the host build.

## 10. Verification Results

- Environment host suite: PASS, including core rules, all 13 rule families,
  state gates, C51/C52 isolation, boot/remote-seq cases, queue ack/full, and
  delivery classification/backoff.
- `radar_ld2450` host suite: PASS.
- `radar_domain` suite: one initial `test_radar_spatial.c:269` assertion did
  fail, then five consecutive full reruns passed without an environment source
  change. This is a pre-existing radar-test nondeterminism risk, not accepted
  as a proof of radar hardware behavior.
- C51/C52 BME source-directory parity check: PASS (exit 0).
- ESPS3 clean build: PASS. Link map contains the environment engine and all
  required radar components.
- Compiler warnings: none observed in the final build. ESP-IDF printed its
  normal ESP-TEE target notice; it is not a compiler warning.
- No standalone BME envelope, sensor-aggregator, gateway-event-reporter, or
  network-worker host test script exists in this checkout. The clean build
  compiles and links those paths, but this is not runtime acceptance.

## 11. Remaining Hardware Validation And Risks

Not performed: actual C51/C52 BME data capture, warmup/READY/DEGRADED
transitions, C5 reboot and remote-sequence behavior over transport, S3 queue
pressure, real server authentication/persistence, 429/503 retry observation,
flash, monitor, and per-room calibration.

The largest residual functional risk is server-side idempotency. Stable
`alarm_id` and `dedup_key` are delivered in the payload, but the current route
does not deduplicate them. A transport failure after server acceptance can
therefore create a duplicate until the server gains an input-level idempotency
constraint. Thresholds are inherited from ESP1 defaults and must not be
treated as calibrated for the current rooms.

See `docs/environment-alarm-calibration.md`,
`docs/environment-alarm-api-payload.md`, and
`docs/environment-alarm-test-report.md` for the operational details.

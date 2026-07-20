# Environment Alarm Test Report

## Executed Tests

| Test | Input / assertion | Result | Source |
| --- | --- | --- | --- |
| Core engine host | high-temperature debounce, hysteresis, recovery, WARMUP AQ gate, DEGRADED recovery, C51/C52 isolation, NaN/Inf/missing field gate | PASS | `test_environment_alarm_host.c` |
| Acceptance engine host | high/low temperature, high/low humidity, fast changes, AQ warning/critical/deterioration, pollution escalation/recovery, unstable, DEGRADED, composite, UNKNOWN gate, reset isolation, alarm id, timestamp rollback | PASS | `test_environment_alarm_acceptance_host.c` |
| Sequence host | duplicate, new, out-of-order, and `uint32_t` wrap classification | PASS | `test_environment_alarm_adapter_host.c` |
| Real V3 adapter host | nested `air_quality`, real READY/WARMUP, boot reset, duplicate/out-of-order/wrap counters, missing stability field | PASS | `test_environment_alarm_adapter_ingest_host.c` |
| Delivery policy host | 2xx, 408, 429, 500, 503, transport timeout, permanent 400/401/local invalid payload, 60 s cap | PASS | `test_environment_alarm_delivery_host.c` |
| Reporter ownership host | no ack before reporter init, ack after FIFO copy, full FIFO retains engine events, ack bound protection | PASS | `test_environment_alarm_reporter_host.c` |
| LD2450 host | core parser tests | PASS | `ESPS3/components/radar_ld2450/tests/run_host_tests.sh` |
| Radar domain host | protocol, registry, ingest, spatial/recovery, source isolation, continuity replay | PASS after five consecutive reruns | `ESPS3/components/Middlewares/radar_domain/tests/run_host_tests.sh` |
| ESPS3 isolated build | all application components compile and link | PASS | `idf.py -C ESPS3 -B build-environment-final build` |
| C51/C52 BME directory parity | byte-for-byte comparison of audited BME source directories | PASS | `git diff --no-index -- ESPC51/.../bme690 ESPC52/.../bme690` |

Run the environment suite from the repository root:

```sh
sh ESPS3/components/environment_alarm_engine/test/run_host_tests.sh
```

## Coverage Notes

The environment tests assert event type/state/content, active state, queue
depth, acknowledgement bounds, device isolation, sequence counters, and
delivery decision rather than merely checking return values. They cover the
13 rule types across the core and acceptance targets.

The reporter host test uses a bounded host stub rather than a real FreeRTOS
worker/HTTP client. It proves engine-to-reporter ownership and full-queue
behavior. The delivery host test proves the pure classification/backoff policy.
It does not prove an actual authenticated server request, asynchronous callback
delivery under Wi-Fi loss, or database persistence.

## Intermediate Observations

- The first run after extracting `environment_alarm_delivery.c` failed at host
  link time because the reporter target did not yet include the new source.
  The host script was corrected; the final complete suite passes.
- The first AQ-deterioration acceptance sample sequence did not satisfy the
  real 60-second debounce. The test was corrected to use 30-second contiguous
  samples; production thresholds were not weakened.
- One initial radar-domain invocation failed at
  `test_radar_spatial.c:269`. Five immediate full reruns passed with no
  environment source change. This is retained as a radar-test flakiness risk,
  not hidden as unconditional deterministic evidence.

## Not Executed

| Check | Status | Reason / required follow-up |
| --- | --- | --- |
| Physical C51 BME690 V3 capture | not executed | requires C51 hardware and monitoring evidence |
| Physical C52 BME690 V3 capture | not executed | requires C52 hardware and monitoring evidence |
| C5 reboot/sequence behavior over transport | not executed | host synthetic envelope only; validate boot id and local sequence on device |
| Server HTTP 201 persistence | not executed | server was not started and no production/test-server request was authorized |
| HTTP 429/503 real retry | not executed | host classification only; inject on an authorized test server |
| Queue/stack/heap watermarks | not executed | requires flashed ESPS3 and runtime diagnostics |
| BME envelope/sensor-aggregator/gateway-event/network-worker standalone tests | not executed | no corresponding executable host test scripts were found in this checkout |
| Flash and monitor | not executed | explicitly prohibited by task constraints |

## Warning And Static Checks

- Environment host targets use `-Wall -Wextra -Werror`: PASS.
- Final ESP-IDF build completed without compiler warnings observed.
- `git diff --check -- ESPS3 docs`: final check is required after all document
  edits and is recorded in the final handoff.
- The final link map contains `environment_alarm_engine`, its adapter/reporter
  objects, and the existing `radar_domain`, `radar_ingest`, and `radar_ld2450`
  objects.

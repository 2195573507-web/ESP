# C5/S3 Code Smell Audit Notes

## Scan Baseline

- Working directory: `/Users/zhiqin/ESP-111`.
- Top-level git status already contained many modified C5/S3 files before this report was written.
- Excluded generated and vendor code from scale metrics: `build/`, `managed_components/`, `.vscode/`, `.devcontainer/`.
- In-scope project-owned C/H/CMake/config files under ESPC51, ESPC52, ESPS3: 254 files.
- In-scope project-owned C/H line count: 56,346 lines.

## Large Files

Largest project-owned C/H files after exclusions:

- `ESPS3/components/Middlewares/runtime/s3_scheduler.c`: 2,094 lines.
- `ESPS3/components/Middlewares/network_worker/network_worker.c`: 2,036 lines.
- `ESPC51/components/Middlewares/mic/mic_adc_test.c`: 1,833 lines.
- `ESPC52/components/Middlewares/mic/mic_adc_test.c`: 1,833 lines.
- `ESPS3/components/Middlewares/protocol_adapter/protocol_adapter.c`: 1,431 lines.
- `ESPC51/components/Middlewares/server_comm/server_comm_http.c`: 1,341 lines.
- `ESPC52/components/Middlewares/server_comm/server_comm_http.c`: 1,341 lines.
- `ESPS3/components/Middlewares/sensor_aggregator/sensor_aggregator.c`: 1,120 lines.
- `ESPC51/components/Middlewares/sensor_domain/bme690/driver/bme690.c`: 1,100 lines.
- `ESPC52/components/Middlewares/sensor_domain/bme690/driver/bme690.c`: 1,100 lines.

## Current Diff

- `git diff --stat -- ESPC51 ESPC52 ESPS3` reported 66 modified tracked files with 4,749 insertions and 1,062 deletions.
- Untracked firmware files total 3,107 lines, mostly new C5 runtime bus/worker files and S3 runtime/cache/replay files.
- The audit therefore covers an unstable in-progress firmware state, not a clean release baseline.

## C51/C52 Parity

- ESPC51/ESPC52 have 99 paired project-owned paths.
- 97 paired files are byte-identical.
- Only two paired files differ:
  - `components/Middlewares/server_comm/server_comm_config.h`
  - `components/Middlewares/terminal_config/terminal_config.h`
- The differences are expected identity/local-id/alias values, but the duplication model means common logic is copied rather than shared.

## S3 Function Size Spot Check

Longest functions found by lightweight brace counting:

- `network_worker.c`: `perform_server_json` 70 lines, `network_worker_init` 67 lines, `evaluate_state` 67 lines, `enqueue_csi_latest_upload_if_needed` 61 lines, `enqueue_upload_work_item` 53 lines.
- `s3_scheduler.c`: `s3_scheduler_tick` 79 lines, `process_ingress` 78 lines, `s3_scheduler_enqueue_event` 75 lines, `process_event` 65 lines, `process_stream_ingress` 60 lines.
- `protocol_adapter.c`: `protocol_adapter_fill_compact_csi_result_payload` 107 lines, `protocol_adapter_fill_csi_v2_payload` 106 lines.

## Boundary Checks

- C5 `/api/` scan found no direct Server API route construction. C5 HTTP config rejects full `http://` and `https://` endpoints and only allows `/local/v1`.
- S3 contains Server-facing routes as expected.
- Raw/subcarrier CSI rejection is present in S3 protocol/gateway code, but there are still compatibility and deprecated-stream rejection paths in runtime and stream code.

## Security/Configuration Notes

- Shared protocol header still contains default SoftAP SSID and password literals.
- S3 gateway config still contains a default public Server base URL literal.
- Auth token default is an empty string unless supplied by build-time override/config.

## Main Audit Direction

The codebase is not uniformly bad. The clearest risks are:

- S3 runtime/network files have too much ownership in one translation unit.
- C51/C52 are effectively cloned projects.
- Queue/coalescing/retry code is complex and manually memory-managed.
- Security/config defaults are still firmware literals.
- Placeholder/deprecated compatibility vocabulary remains visible and should be aggressively bounded.

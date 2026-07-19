# S3 Network Worker Notes

## Current Source Findings
- `gateway_wifi.c` currently does STA connect/reconnect and child registry link-lost updates inside the Wi-Fi event callback.
- `local_http_server.c` already keeps POST handlers mostly enqueue-only, but lifecycle is started directly by `gateway_orchestrator_start()`.
- `csi_placeholder_gateway.c` starts its trigger and fusion tick tasks at init time; they need worker-controlled enablement before producing CSI/upload work.
- `device_stream_gateway.c` has UDP listener/sender tasks that wait on `gateway_wifi_is_net_ready()`, but the readiness bit is updated directly from the Wi-Fi callback and lacks the required 3-second stability gate.
- ESPS3 does not currently have the C5 `gateway_link` module; the worker will own a local gateway link state enum for S3 network state reporting.

## Verification
- Wi-Fi callback boundary grep shows no HTTP, CSI, child registry, malloc, memcpy, PSRAM, cJSON, or timer work in `gateway_wifi.c` callbacks; STA connect remains only in the worker-callable function outside callback context.
- `NETWORK_WORKER_STABLE_GATE_MS` is 3000 ms.
- `idf.py build` passed for `ESPS3` and generated `build/sensair_s3_gateway.bin`.

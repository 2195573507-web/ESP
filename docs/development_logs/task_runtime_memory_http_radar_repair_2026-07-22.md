# Runtime Memory, HTTP, and Radar Repair

- Date: 2026-07-22
- Scope: ESPS3 internal/DMA fragmentation diagnostics, local HTTP server session visibility, dashboard-overview backoff, S3 UART radar lifecycle, and ESPC52 BLE radar enablement.
- Evidence boundary: source inspection and isolated ESP-IDF builds only. No flash, serial monitoring, Wi-Fi, HTTP, BLE, UART, radar, or long-duration hardware acceptance is claimed.

## Observed Runtime Evidence

The supplied S3 logs retain about 10-11 KiB Internal free after first sensor ingress but reduce its largest block to roughly 2.7 KiB; DMA largest similarly falls as low as about 1.1 KiB. This is a fragmentation/capability-admission problem, not evidence that simply reducing an admission threshold is safe. Local HTTP also logs peer resets and `ESP_ERR_HTTP_FETCH_HEADER`, `ESP_ERR_HTTP_WRITE_DATA`, and `ESP_ERR_HTTP_EAGAIN`; static source alone cannot attribute them to a particular peer or socket leak.

## Allocation Differential Audit

The following dynamic, long-lived or request-sized buffers are outside Internal RAM in the live source:

| Owner | Allocation / size class | Capability | Lifetime |
| --- | --- | --- | --- |
| local HTTP JSON request | `req->content_len + 1` | PSRAM | handler receive through enqueue transfer/failure cleanup |
| local ingress event | `sizeof(s3_runtime_ingress_t)` | PSRAM | scheduler ownership until worker release |
| command-pending response | 2048 B | PSRAM | one response |
| radar request / home / debug response | request-sized, 1536 B, 4096 B | PSRAM | one handler response |
| BME cache storage and JSON copy | object-sized / payload-sized | PSRAM | cache replacement and shutdown |
| alarm reporter storage and serialized body | object-sized / payload-sized | PSRAM | reporter lifecycle / queue completion |
| dashboard and command queued copies | payload-sized | PSRAM | worker completion, cancellation, or drop |
| Wi-Fi scan records | `ap_count * sizeof(wifi_ap_record_t)` | PSRAM | one scan |

HTTPD, lwIP, Wi-Fi, driver, ISR, UART driver control, and DMA-facing objects remain Internal. The current source has no remaining application-owned 512, 748, 1024, 1090, 1332, 1716, 2048, 2804, 3072, or 4096-byte Internal temporary buffer on the audited request/cache paths; cJSON and ESP-IDF transport-private allocation remains a hardware measurement risk.

## Implemented Changes

- `local_http_server` records throttled `HTTPD_SESSION` snapshots at request cleanup or failure. They include active HTTPD sessions from `httpd_get_client_list`, configured `max_open_sockets`, socket-descriptor count, handler activity, and started/finished/failed request counters. The configured limit remains four; it was not enlarged to conceal a leak. `LOCAL_HTTP_MEMORY_DIAGNOSTICS` defaults to zero; enabling it emits `before_accept`, `after_accept`, `after_recv`, `after_parse`, `after_enqueue`, `response_sent`, and `request_cleanup` capability snapshots.
- `network_worker` arms one `esp_timer` per dashboard-overview pressure window. The 250 ms network loop no longer calls the overview admission path while that timer is active; only timer expiry re-enqueues the low-priority probe. A cycle emits one `SET`, optional one `SKIP`, and one `EXPIRED` transition.
- `radar_service` reports `DISABLED`, `DRIVER_ALLOCATED`, `WAITING_FOR_DEVICE`, `ONLINE`, `DEVICE_TIMEOUT`, `DRIVER_FAULT`, and `MEMORY_DEFERRED` lifecycle transitions. Empty UART reads now retain the driver and wait for a device; delete/reinstall remains limited to driver-error/overflow recovery. Driver admission records Internal and DMA free/largest values while retaining the documented 12 KiB reserve.
- ESPC52 has an explicit `RADAR_BLE_RADAR_ENABLED` gate, defaulting off for this no-radar run. Disabled mode reaches `DISABLED` without NimBLE scanning or reconnect timers. When enabled, exact configured MAC matching remains mandatory, nonmatching device logs are debug-only, and retry caps at 60 seconds.

## C5 Client Audit

`ESPC51` and `ESPC52` ordinary HTTP requests create a stack-local `esp_http_client_handle_t` for each request, then close and clean it on the common exit path. Raw streaming uses one separately allocated stream object and its own handle, which the stream close path closes and cleans. No shared global HTTP client handle was found, so adding a broad scheduler mutex would not fix the observed reset and could regress the confirmed zero-drop scheduler.

## Required Hardware Evidence

1. Enable the diagnostic macro/path and compare `before_accept`, `after_accept`, `after_recv`, `after_parse`, `after_enqueue`, `response_sent`, and `request_cleanup` capability snapshots around the first sensor ingress.
2. On each accept/reset, capture `HTTPD_SESSION` and correlate open sessions with C5 connection close/retry behavior. Confirm no accumulating session count and no client descriptor retained after an error response.
3. Run past the historical failure interval while mixing ingress, command poll, radar snapshot, and overview. Confirm overview's one-shot deadline does not wake every 250 ms and critical ingress/command paths continue.
4. With local UART enabled, verify driver install occurs while Internal/DMA contiguous capacity is sufficient; with no device attached, verify `DEVICE_TIMEOUT` without delete/reinstall. Induce a genuine driver fault before accepting the reinstall branch.
5. With C52 radar disabled, confirm no NimBLE scan or reconnect log; when enabled, confirm only the configured target can connect.

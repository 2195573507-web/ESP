# ESPS3 Serial Debug Notes

## 2026-07-06 Initial Survey
- User goal: read S3 status through serial and solve the issue.
- Preserve stack boundaries: C5 remains lightweight; ESPS3 owns gateway/fusion/server-facing behavior.
- `find . -maxdepth 3 -type d -name .git -print` shows:
  - `./.git`
  - `./ESP-server/.git`
- `git status --short` shows many existing firmware/tool changes. Treat them as user or previous-run work unless this debug pass clearly owns a new edit.
- ESPS3 sdkconfig:
  - `CONFIG_IDF_TARGET="esp32s3"`
  - `CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200`
- Visible serial candidates:
  - `/dev/cu.usbmodem141101`
  - `/dev/cu.usbmodem14301`
  - `/dev/cu.usbmodem5B901601171`
- System Python lacks `pyserial`, so serial probing should use repo/tool-local dependencies or ESP-IDF tooling.

## 2026-07-06 Serial Findings
- `/dev/cu.usbmodem141101` is ESPS3, MAC `90:e5:b1:cc:ee:40`.
- Initial serial symptoms:
  - `gateway heartbeat softap=1 sta=1 server=0 ... last_error=server_rejected`
  - `offline_policy: server path degraded error_code=server_rejected ret=ESP_ERR_INVALID_RESPONSE http_status=404`
  - `gateway_event: system log upload failed status=404 ret=ESP_ERR_INVALID_RESPONSE`
- Remote server checks:
  - `GET /api/device/v1/status` returns 200.
  - `POST /api/logs/v1/system` returns 404 on the remote server.
  - `POST /api/device/v1/gateway-state` accepts `schema_version=2` and `payload_type=gateway.dashboard_snapshot`.
- Applied firmware-side changes:
  - Optional system/alarm log upload failure no longer updates global `offline_policy`.
  - `server_client` now logs rejected endpoint/status/body for 4xx/5xx.
  - `sensor_aggregator` logs dashboard snapshot schema/payload/bytes before upload.
- Verification after flash:
  - S3 reports `dashboard snapshot upload schema=2 payload_type=gateway.dashboard_snapshot`.
  - Later heartbeat reaches `gateway heartbeat softap=1 sta=1 server=1`.
  - Remaining issue: `Guru Meditation Error: Core 0 panic'ed (StoreProhibited)` still occurs around the 30-second heartbeat window.
- Next hypothesis:
  - S3 CSI trigger is enabled at `interval_ms=10` and sends UDP every 10 ms once a C5 peer is online. This is a likely stressor because the sender path opens/sends/closes sockets through a queue. Test by reducing the trigger interval to 100 ms.

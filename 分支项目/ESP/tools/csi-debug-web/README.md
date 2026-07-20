# CSI Motion State Debug Web

Local-only CSI motion-state workbench for the ESP-111 three-node CSI sensing area.

This tool is intentionally scoped to local debugging:

- It serves one local page at `http://127.0.0.1:8787`.
- It can read CSI logs from `/dev/cu.*` serial ports when `serialport` is installed.
- It can parse pasted CSI log lines and generate mock samples without any real device.
- It visualizes the S3 -> C51 / S3 -> C52 motion-state links.
- It derives motion state, link freshness, state timeline, and recent CSI_RESULT_V2 features locally.
- It stores history in `server.js` memory only.
- It does not connect to ESP-server.
- It does not connect to the formal Dashboard.
- It does not modify firmware.
- It does not use a database.

## Install

```sh
cd path/to/ESP-111/tools/csi-debug-web
npm install
```

`serialport` is optional at runtime. If it is not installed, `node server.js` still starts and the page still works for pasted logs and mock samples. Serial listing/connect APIs return a clear JSON error/status that says serial mode is unavailable.

## Start

Start without auto-connecting a serial port:

```sh
cd path/to/ESP-111/tools/csi-debug-web
node server.js
```

The default bind address is:

```text
http://127.0.0.1:8787
```

`http://localhost:8787` also works in Safari, Chrome, or Edge.

Use another web port:

```sh
PORT=8788 node server.js
```

Start and connect immediately to a serial port:

```sh
CSI_SERIAL_PORT=/dev/cu.usbmodemXXXX CSI_BAUD=115200 node server.js
```

`CSI_SERIAL_PORT` and `CSI_BAUD` are still supported.

Adjust in-memory sample capacity or default telemetry history:

```sh
CSI_MAX_HISTORY=5000 CSI_HISTORY_LIMIT=1000 node server.js
```

`CSI_MAX_HISTORY` defaults to `5000`; `CSI_HISTORY_LIMIT` defaults to `1000`.

## Verify Without Real Serial Hardware

1. Start the server:

   ```sh
   cd path/to/ESP-111/tools/csi-debug-web
   node server.js
   ```

2. Open:

   ```text
   http://127.0.0.1:8787
   ```

3. Click `模拟帧`.

4. Confirm `Current State`, `Motion Score`, `Confidence`, `Active Links`, `State Timeline`, and `Recent CSI_RESULT_V2 Features` update.

5. Paste this example into `导入` / `日志行` and click `解析日志`:

   ```text
   I (45678) csi_gateway: CSI_FUSION_TELEMETRY {"type":"csi_fusion","schema_version":2,"trace_id":"csi-v2-42","tick_id":42,"links":["C51","C52"],"fused_state":{"state":"HOLD","confidence":0.58,"motion_score":0.58},"confidence":0.58,"motion_score":0.58,"timestamp_ms":4200}
   ```

6. Use `导出 JSON`, `导出 CSV`, and `清空历史`.

## Serial Mode

Inspect serial ports.

macOS:

```sh
ls /dev/cu.*
```

Common ESP USB serial names look like:

```text
/dev/cu.usbmodemXXXX
/dev/cu.usbserial-XXXX
```

Windows:

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```

Common Windows ESP USB serial names look like:

```text
COM3
COM4
```

In the page:

1. Click `刷新端口`.
2. Click a listed port, or type it manually.
   - macOS accepts `/dev/cu.*`.
   - Windows accepts `COM` ports such as `COM3`.
3. Select `baudRate` such as `115200`.
4. Click `连接`.
5. Click `断开` to release the port.

If `serialport` is missing, the page shows:

```text
未安装 serialport，串口功能不可用，但可使用粘贴日志/模拟样本
```

Run `npm install` in this directory and restart the server to enable serial mode.

## Tool Platform Resolver

External tool operations must go through `src/toolResolver.js` instead of calling platform commands directly from business logic.

The resolver detects:

- Darwin / macOS as `macOS`
- win32 / Windows as `Windows`
- other platforms as an explicit safe fallback

It centralizes path joins, shell command construction, file/directory selection, opening folders, opening applications, local tool execution, tool existence checks, and serial port path rules. Startup logs include:

```text
tool platform detected: macOS
```

or:

```text
tool platform detected: Windows
```

Run resolver self-checks:

```sh
npm test
```

## Supported Log Lines

S3 fusion telemetry:

```text
I (45678) csi_gateway: CSI_FUSION_TELEMETRY {"type":"csi_fusion","schema_version":2,"trace_id":"csi-v2-42","tick_id":42,"links":["C51","C52"],"fused_state":{"state":"HOLD","confidence":0.58,"motion_score":0.58},"confidence":0.58,"motion_score":0.58,"timestamp_ms":4200}
```

CSI_RESULT_V2 feature frame:

```text
I (12000) csi_gateway: CSI_RESULT_V2 link_id=S3_TO_C51 device=C51 energy=18.200 variance=0.150 cv=0.090 rssi=-44 quality=0.930 state=MOTION
```

Canonical CSI fact from ESP-server-compatible surfaces:

```text
{"payload_type":"csi.motion","device_id":"gateway-1","timestamp_ms":123456,"payload":{"state":"MOTION","frame_energy":22.5,"variance":0.32,"rssi":-38,"motion_score":0.81,"confidence":0.77}}
```

Legacy summary parsing is still accepted for old logs. It is expanded into synthetic four-link telemetry for the UI:

```text
I (12345) csi_service: csi summary {"schema":1,"device_id":"c5","state":"occupied","motion_score":0.72,"variance":0.44,"rssi":-35,"updated_at_ms":123456}
```

Legacy line:

```text
CSI_SAMPLE {"device_id":"sensair_c51","room_id":"bedroom","rssi":-42,"packet_count":123,"amplitude_variance":0.123,"motion_score":0.76,"presence_score":0.76,"presence":true,"esp_time_ms":123456}
```

Parse failures return JSON with `ok:false`, `error`, and `raw_line`. They do not stop the server.

## Local Telemetry Engine

`src/csiTelemetryEngine.js` upgrades every accepted input into the same motion-state telemetry shape. It stays in memory and does not write to ESP-server or any database.

Normalized link event:

```json
{
  "timestamp": 4200,
  "link_id": "S3_TO_C51",
  "source": "S3",
  "target": "C51",
  "motion_score": 0.7,
  "energy": 20.2,
  "variance": 0.12,
  "quality": 0.8,
  "rssi": -41,
  "state": "MOTION"
}
```

Supported links:

- Active: `S3_TO_C51`
- Active: `S3_TO_C52`
- Legacy, shown as disabled when present in old logs: `C51_TO_C52`, `C52_TO_C51`

It produces:

- `motion_state_model`: current state, motion score, confidence, active links, legacy links, state timeline, recent CSI_RESULT_V2 features, latest logs, and raw parsed events.
- `fusion_status`: compact summary with current state, motion score, confidence, active link ids, and last update time.
- `state_timeline`: dwell-time/hysteresis-aware state timeline.
- `telemetry_events`: parsed link events used by the motion-state model.
- `fusion_events`: parsed S3 fused-state events.

The parser auto-detects S3 `csi_fusion` telemetry, canonical CSI facts, C5 feature frames, and older summary facts. Alignment uses a sliding window capped to 200-500 ms; use `window_ms` to tune it.

## Curl Checks

Generate a mock sample:

```sh
curl -X POST http://127.0.0.1:8787/api/csi/mock
```

Read latest:

```sh
curl http://127.0.0.1:8787/api/csi/latest
```

Read recent history:

```sh
curl 'http://127.0.0.1:8787/api/csi/history?limit=10'
```

Read local UI telemetry:

```sh
curl 'http://127.0.0.1:8787/api/csi/telemetry?limit=200&window_ms=400'
```

Parse one log line:

```sh
curl -X POST http://127.0.0.1:8787/api/csi/log-line \
  -H 'Content-Type: application/json' \
  -d '{"line":"I (45678) csi_gateway: CSI_RESULT_V2 link_id=S3_TO_C51 device=C51 energy=18.200 variance=0.150 cv=0.090 rssi=-44 quality=0.930 state=MOTION"}'
```

Export JSON:

```sh
curl http://127.0.0.1:8787/api/csi/export.json
```

Export CSV:

```sh
curl http://127.0.0.1:8787/api/csi/export.csv
```

Clear history:

```sh
curl -X DELETE http://127.0.0.1:8787/api/csi/history
```

## API

### GET /

Returns the local CSI debug page.

### GET /api/serial/ports

Returns available `/dev/cu.*` serial ports. Works even when `serialport` is missing, but metadata and connect mode require `serialport`.

### POST /api/serial/connect

Body:

```json
{
  "port": "/dev/cu.xxx",
  "baudRate": 115200
}
```

`baud` is accepted as a backwards-compatible alias.

### POST /api/serial/disconnect

Stops the active serial reader and releases the serial port.

### GET /api/serial/status

Returns `connected`, `port`, `baudRate`, `lastError`, plus compatibility fields such as `baud` and `last_error`.

### POST /api/csi/log-line

Body:

```json
{
  "line": "I (12000) csi_gateway: CSI_RESULT_V2 link_id=S3_TO_C51 device=C51 energy=18.200 variance=0.150 cv=0.090 rssi=-44 quality=0.930 state=MOTION"
}
```

Parses one line and writes the sample to memory history.

### POST /api/csi/mock

Generates one mock motion-state sample and writes it to memory history.

### GET /api/csi/latest

Returns the latest sample, all latest samples by `device_id`, and a compact status object.

### GET /api/csi/history

Returns recent memory history.

Optional query:

```text
device_id=c5
limit=200
```

`limit` defaults to `1000` and is capped at `5000` unless `CSI_MAX_HISTORY` is set.

### GET /api/csi/telemetry

Builds local-only CSI motion-state telemetry output from in-memory history.

Optional query:

```text
device_id=c5
limit=200
window_ms=400
min_dwell_ms=600
```

The response includes `motion_state_model`, `fusion_status`, `telemetry_events`, and `fusion_events`. This endpoint is for local tools/frontends only; it does not persist or send data to ESP-server.

### DELETE /api/csi/history

Clears memory history.

### GET /api/csi/export.json

Downloads all in-memory history as JSON.

### GET /api/csi/export.csv

Downloads all in-memory history as CSV.

### POST /api/csi/sample

Backwards-compatible manual sample insertion endpoint.

## Acceptance Checklist

- `node server.js` starts at `http://127.0.0.1:8787`.
- Missing `serialport` does not prevent startup.
- `POST /api/csi/mock` updates Latest, Timeline, Link Cards, and Raw JSON.
- Pasting a `CSI_RESULT_V2`, `CSI_RX`, `CSI_LATEST`, or `CSI_FUSION_TELEMETRY` line and clicking `解析日志` updates the page.
- History sample limit can switch between `200`, `500`, `1000`, `2000`, and `5000`.
- `DELETE /api/csi/history` clears Latest, Timeline, Link Cards, and Raw JSON.
- JSON and CSV export endpoints return current in-memory history.
- Serial mode remains local and uses platform-specific serial rules.
- No firmware, ESP-server, Dashboard, or database files are touched.

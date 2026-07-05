# CSI Debug Web

Local-only WiFi CSI debug workbench for the ESP-111 workspace.

This tool is intentionally scoped to local debugging:

- It serves one local page at `http://127.0.0.1:8787`.
- It can read CSI logs from `/dev/cu.*` serial ports when `serialport` is installed.
- It can parse pasted CSI log lines and generate mock samples without any real device.
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

Adjust in-memory sample capacity or default chart history:

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

3. Click `生成模拟样本`.

4. Confirm `最新状态区`, `曲线区`, and `Raw JSON 区` update.

5. Paste this example into `手动调试区` and click `解析这一行`:

   ```text
   I (12345) csi_service: csi summary {"schema":1,"device_id":"c5","room_id":"lab","state":"occupied","motion_score":0.72,"variance":0.44,"cv":0.08,"rssi":-35,"noise_floor":-90,"packet_count":64,"updated_at_ms":123456}
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

Current summary JSON:

```text
csi_service: csi summary {"schema":1,"state":"vacant","motion_score":0.02,"variance":0.14,"cv":0.013,"rssi":-31,"packet_count":32,"updated_at_ms":123456}
```

ESP-IDF prefixed summary:

```text
I (12345) csi_service: csi summary {"schema":1,"device_id":"c5","room_id":"lab","state":"occupied","motion_score":0.72,"variance":0.44,"cv":0.08,"rssi":-35,"noise_floor":-90,"packet_count":64,"updated_at_ms":123456}
```

Short `CSI:` style JSON:

```text
I (12345) CSI: {"device_id":"c5","motion_score":0.61,"rssi":-42,"amplitude":23.4,"packet_count":88}
```

Placeholder gateway style with key/value fields:

```text
W (12345) csi_placeholder_gateway: device_id=c5 room_id=lab motion_score=0.18 rssi=-51 packet_count=18 state=vacant
```

Legacy line:

```text
CSI_SAMPLE {"device_id":"sensair_c51","room_id":"bedroom","rssi":-42,"packet_count":123,"amplitude_variance":0.123,"motion_score":0.76,"presence_score":0.76,"presence":true,"esp_time_ms":123456}
```

Parse failures return JSON with `ok:false`, `error`, and `raw_line`. They do not stop the server.

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

Parse one log line:

```sh
curl -X POST http://127.0.0.1:8787/api/csi/log-line \
  -H 'Content-Type: application/json' \
  -d '{"line":"I (12345) csi_service: csi summary {\"schema\":1,\"device_id\":\"c5\",\"room_id\":\"lab\",\"state\":\"occupied\",\"motion_score\":0.72,\"variance\":0.44,\"cv\":0.08,\"rssi\":-35,\"noise_floor\":-90,\"packet_count\":64,\"updated_at_ms\":123456}"}'
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
  "line": "I (12345) csi_service: csi summary {\"motion_score\":0.72}"
}
```

Parses one line and writes the sample to memory history.

### POST /api/csi/mock

Generates one mock CSI summary sample and writes it to memory history.

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
- `POST /api/csi/mock` updates Latest, Curves, and Raw JSON.
- Pasting a `csi summary` line and clicking `解析这一行` updates the page.
- History sample limit can switch between `200`, `500`, `1000`, `2000`, and `5000`.
- `DELETE /api/csi/history` clears Latest, Curves, Raw JSON, and device list.
- JSON and CSV export endpoints return current in-memory history.
- Serial mode remains local and uses platform-specific serial rules.
- No firmware, ESP-server, Dashboard, or database files are touched.

# ESP-server 前端接口使用文档

生成日期：2026-07-07

本文面向 `public/` 前端开发人员，按当前后端源码扫描生成。接口真源来自 `server.js`、`src/routes/*.js`、`src/services/*.js`，不是按旧页面 mock 注释推断。

## 1. 前端接入原则

- 前端页面由同一个 Express 服务托管，开发时默认使用同源请求，例如 `fetch("/api/dashboard/v1/overview")`。
- 本地默认服务地址：`http://localhost:3000`，端口由后端 `PORT` 环境变量覆盖。
- 新页面优先使用 `/api/dashboard/v1/*`、`/api/device/v1/*`、`/api/smart-home/v1/*`、`/api/logs/v1/*`、`/api/events/v1/stream`。
- `/sensor`、`/asr`、`/llm` 是旧兼容接口。新页面只在兼容旧设备或排查历史数据时使用。
- 网关写入、设备轮询、ack 类接口需要网关身份，不应把网关 token 暴露在普通浏览器页面。
- 用户数据删除接口需要 admin token，不应在公开前端中直接调用，建议只给本地管理工具或受控后端代理使用。

## 2. 响应格式

### 新版统一 JSON

多数新接口返回：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {},
  "error": null
}
```

失败时：

```json
{
  "ok": false,
  "server_time_ms": 1780000000000,
  "data": null,
  "error": {
    "code": "REQUEST_FAILED",
    "message": "Request failed"
  }
}
```

前端推荐封装：

```js
async function apiJson(path, options = {}) {
  const response = await fetch(path, {
    cache: "no-store",
    headers: {
      "Content-Type": "application/json",
      ...(options.headers || {})
    },
    ...options
  });
  const payload = await response.json();
  if (!response.ok || payload.ok === false) {
    const message = payload.error?.message || payload.error || response.statusText;
    throw new Error(message);
  }
  return payload.data ?? payload;
}
```

### 旧接口格式

旧接口可能直接返回对象或数组，例如 `/sensor/latest` 返回一条传感器对象，`/sensor/history` 返回数组，`/asr/latest` 返回 `{ id, timestamp, text }`。前端不要假设这些接口有 `data` 字段。

### 通用 404

机器接口路径不存在时返回：

```json
{
  "ok": false,
  "error": "Not found"
}
```

## 3. 认证与敏感接口

### 网关认证

以下接口需要 `requireGatewayAuth`：

- 设备或网关上报接口。
- 设备轮询命令、ack 命令。
- 智能家居网关状态上报、pending 命令、ack。
- 网关写入系统日志、报警日志。

可用 header：

```http
X-Gateway-Id: sensair_s3_gateway_01
X-Gateway-Token: <server-configured-token>
X-Device-Id: esp32-c5-001
```

也支持：

```http
Authorization: Bearer <server-configured-token>
```

后端未配置 `GATEWAY_AUTH_TOKEN` 或 `GATEWAY_AUTH_TOKENS` 时，只要求 `gateway_id`，不校验 token。生产环境不要依赖这种模式。

### 用户数据 admin 认证

用户数据接口要求服务器配置 `USER_DATA_DELETE_TOKEN` 或 `ADMIN_TOKEN`。请求使用：

```http
X-Admin-Token: <admin-token>
```

或：

```http
Authorization: Bearer <admin-token>
```

普通前端不要保存或暴露此 token。

## 4. 当前页面已使用接口

`public/app.js` 当前实际调用：

| 场景 | 方法 | 路径 |
| --- | --- | --- |
| 最新传感器 | GET | `/api/dashboard/v1/sensors/latest` |
| 最新 ASR 文本 | GET | `/api/dashboard/v1/asr/latest` |
| 最新 LLM 回复 | GET | `/api/dashboard/v1/llm/latest` |
| CSI 历史 | GET | `/api/dashboard/v1/csi/history?limit=80` |

当前页面里历史曲线、报警日志、系统日志仍有 mock 注释，但后端已经提供可替换接口，见后续章节。

## 5. Dashboard v1 接口

基础前缀：`/api/dashboard/v1`

统一返回 `{ ok, server_time_ms, data, error }`。

### 5.1 总览

`GET /api/dashboard/v1/overview`

查询参数：

| 参数 | 必填 | 说明 |
| --- | --- | --- |
| `device_id` | 否 | 只看某个设备的数据 |

用途：主 Dashboard 首屏聚合。包含 gateway、devices、home_summary、history、modules、alarms、recent_commands、recent_voice_events、system_logs、csi。

示例：

```js
const overview = await apiJson("/api/dashboard/v1/overview");
```

关键字段：

```json
{
  "gateway": {
    "gateway_id": "sensair_s3_gateway_01",
    "online": true,
    "sta_connected": true,
    "server_connected": true,
    "voice_busy": false,
    "free_heap": 123456,
    "last_error": "",
    "lastSeen": 1780000000000
  },
  "devices": [
    {
      "device_id": "esp32-c5-001",
      "device_type": "C5",
      "room_name": "living",
      "online": true,
      "temperature_c": 25.6,
      "humidity_percent": 58.3,
      "air_quality_score": 88,
      "voice": {
        "online": true,
        "last_event_ms": 1780000000000
      },
      "csi": {
        "available": true,
        "state": "MOTION",
        "motion_score": 0.72
      }
    }
  ],
  "home_summary": {
    "online_device_count": 1,
    "offline_device_count": 0,
    "avg_temperature": 25.6,
    "avg_humidity": 58.3,
    "avg_air_quality": 88
  },
  "modules": {
    "wifi": { "online": true, "status": "online" },
    "voice": { "online": true, "busy": false, "last_event_ms": 1780000000000 },
    "bme": { "online": true },
    "csi": { "online": true, "last_result_ms": 1780000000000 },
    "server": { "online": true }
  }
}
```

### 5.2 最新网关快照

`GET /api/dashboard/v1/latest`

用途：读取最近一次 `gateway.dashboard_snapshot`。没有快照时 `data` 为 `null`。

### 5.3 快照历史

`GET /api/dashboard/v1/history?limit=50`

查询参数：

| 参数 | 默认 | 最大 | 说明 |
| --- | --- | --- | --- |
| `limit` | 50 | 500 | 最近快照数量 |

返回：

```json
{
  "snapshots": [
    {
      "snapshot_id": "snapshot_xxx",
      "gateway_id": "sensair_s3_gateway_01",
      "server_recv_ms": 1780000000000,
      "schema_version": 2,
      "payload": {},
      "created_at": "2026-07-07T00:00:00.000Z"
    }
  ]
}
```

### 5.4 最新传感器

`GET /api/dashboard/v1/sensors/latest`

查询参数：

| 参数 | 必填 | 说明 |
| --- | --- | --- |
| `device_id` | 否 | 指定设备 |

返回 `data` 为单个传感器对象或 `null`：

```json
{
  "id": 1,
  "timestamp": 1780000000000,
  "temperature": 25.6,
  "humidity": 58.3,
  "pressure": 1009.2,
  "gas_resistance": 123456,
  "device_id": "esp32-c5-001",
  "payload_type": "sensor.bme690",
  "server_recv_ms": 1780000000000,
  "upload_delay_ms": 32,
  "online": true,
  "device_online": true,
  "sensor_online": true,
  "latest_upload_delay_ms": 32,
  "avg_upload_delay_ms": 40,
  "air_quality": {
    "air_quality_score": 88,
    "air_quality_level": "good",
    "air_quality_confidence": "high",
    "air_quality_source": "s3_mapped"
  },
  "time_sync": {
    "ok": true,
    "server_time_ms": 1780000000000,
    "latest_ping": null
  }
}
```

### 5.5 传感器历史

`GET /api/dashboard/v1/sensors/history?limit=50&device_id=esp32-c5-001`

返回 `data` 为数组，顺序为时间升序。适合替换当前前端历史曲线 mock 数据。

### 5.6 单设备历史

`GET /api/dashboard/v1/devices/:device_id/history?limit=50`

等价于传感器历史带 `device_id`，适合设备详情页。

### 5.7 CSI 历史

`GET /api/dashboard/v1/csi/history?limit=80`

查询参数：

| 参数 | 默认 | 最大 | 说明 |
| --- | --- | --- | --- |
| `limit` | 50 | 500 | 最近 CSI 事件数量 |
| `device_id` | 空 | 空表示全部设备 |

返回：

```json
{
  "events": [
    {
      "id": 1,
      "device_id": "sensair_s3_gateway_01",
      "gateway_id": "sensair_s3_gateway_01",
      "link_id": "fused",
      "state": "MOTION",
      "frame_energy": null,
      "variance": null,
      "rssi": null,
      "motion_score": 0.72,
      "timestamp": 1780000000000,
      "server_recv_ms": 1780000000100,
      "server_time_iso": "2026-07-07T00:00:00.100Z"
    }
  ]
}
```

CSI 当前前端可展示的是 `state`、`motion_score`、`timestamp`、`server_recv_ms` 等 canonical fact，不是 raw CSI 或 subcarrier 曲线。

### 5.8 ASR 与 LLM 最新记录

`GET /api/dashboard/v1/asr/latest`

返回：

```json
{
  "id": 1,
  "timestamp": 1780000000000,
  "text": "用户语音识别文本"
}
```

`GET /api/dashboard/v1/llm/latest`

返回：

```json
{
  "id": 1,
  "timestamp": 1780000000000,
  "prompt": "用户输入",
  "response": "模型回复"
}
```

### 5.9 时间与设备状态

`GET /api/dashboard/v1/time/status`

返回服务器时间和最近一次 `/api/time/ping`。

`GET /api/dashboard/v1/device/status?device_id=esp32-c5-001`

返回单设备在线状态。没有 `device_id` 时返回当前默认设备状态。

`GET /api/dashboard/v1/modules/status?device_id=esp32-c5-001`

返回：

```json
{
  "modules": [
    {
      "device_id": "esp32-c5-001",
      "module_type": "sensor.bme690",
      "online": true,
      "module_online": true,
      "last_seen_ms": 1780000000000,
      "latest_upload_delay_ms": 32
    }
  ]
}
```

## 6. 设备只读接口

### 6.1 设备状态

`GET /api/device/v1/status`

`GET /api/device/v1/status/:device_id`

返回格式：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "devices": []
  },
  "error": null,
  "status": {}
}
```

### 6.2 模块状态

`GET /api/device/v1/modules/status?device_id=esp32-c5-001`

返回：

```json
{
  "ok": true,
  "modules": [],
  "server_time_ms": 1780000000000
}
```

### 6.3 LLM 设备上下文

`GET /api/device/v1/context?device_id=esp32-c5-001`

用途：调试 LLM prompt 注入的设备、环境、空气质量和新鲜度上下文。

### 6.4 最新传感器兼容读取

`GET /api/device/v1/sensors/latest?device_id=esp32-c5-001`

返回：

```json
{
  "ok": true,
  "sensor": {},
  "server_time_ms": 1780000000000
}
```

新 Dashboard 优先用 `/api/dashboard/v1/sensors/latest`，它的在线状态和 `time_sync` 字段更适合前端展示。

## 7. 智能家居接口

基础前缀：`/api/smart-home/v1`

### 7.1 状态

`GET /api/smart-home/v1/status`

适合替换当前智能家居 mock 状态。

返回：

```json
{
  "available": true,
  "configured": true,
  "provider": "s3_gateway",
  "last_update_ms": 1780000000000,
  "devices": [
    {
      "id": "air_conditioner_1",
      "type": "air_conditioner",
      "name": "客厅空调",
      "room_id": "living",
      "room_name": "客厅",
      "online": true,
      "state": {
        "power": true,
        "mode": "cool",
        "target_temperature": 26
      },
      "updated_at_ms": 1780000000000
    }
  ]
}
```

### 7.2 创建控制命令

`POST /api/smart-home/v1/control`

浏览器可调用，用于让后端创建待网关拉取的控制命令。

请求：

```json
{
  "target_id": "air_conditioner_1",
  "action": "set_power",
  "params": {
    "power": true
  },
  "room_id": "living",
  "room_name": "客厅",
  "requested_by": "dashboard"
}
```

响应 `202`：

```json
{
  "command": {
    "command_id": "shcmd_xxx",
    "target_id": "air_conditioner_1",
    "action": "set_power",
    "params": { "power": true },
    "source": "dashboard",
    "status": "queued",
    "created_at_ms": 1780000000000,
    "updated_at_ms": 1780000000000
  },
  "message": "queued; waiting for gateway pull"
}
```

### 7.3 命令历史

`GET /api/smart-home/v1/commands?limit=50`

返回最近智能家居命令。`limit` 最大 200。

## 8. 命令队列接口

### 8.1 命令白名单

`GET /api/commands/whitelist`

返回后端允许入队的设备命令定义：

| 命令 | payload |
| --- | --- |
| `device.noop` | 空对象 |
| `voice.set_volume` | `volume`: 0 到 100 整数 |
| `sensor.set_upload_interval` | `interval_ms`: 1000 到 3600000 整数 |
| `display.show_text` | `text`: 最多 120 字符，`ttl_ms`: 1000 到 60000 |
| `alert.play_tone` | `tone`: `confirm`、`warning`、`error`，可选 `duration_ms` |

### 8.2 设备能力

`GET /api/devices/:device_id/capabilities`

返回设备已经注册支持的命令。未注册返回 `404`。

### 8.3 创建设备命令

`POST /api/commands`

请求：

```json
{
  "target_device_id": "esp32-c5-001",
  "name": "voice.set_volume",
  "payload": {
    "volume": 35
  },
  "reason": "dashboard control"
}
```

注意：目标设备必须先注册 capabilities，且包含该命令，否则返回 `DEVICE_CAPABILITIES_REQUIRED` 或 `DEVICE_COMMAND_UNSUPPORTED`。

响应 `201`：

```json
{
  "ok": true,
  "command": {
    "command_id": "uuid",
    "device_id": "esp32-c5-001",
    "name": "voice.set_volume",
    "payload": { "volume": 35 },
    "status": "queued",
    "created_at": "2026-07-07T00:00:00.000Z"
  }
}
```

### 8.4 命令历史

`GET /api/commands/history?device_id=esp32-c5-001&limit=50`

返回：

```json
{
  "ok": true,
  "commands": [
    {
      "command_id": "uuid",
      "device_id": "esp32-c5-001",
      "name": "voice.set_volume",
      "payload": { "volume": 35 },
      "status": "queued",
      "source": "api",
      "requested_by": "server",
      "created_at": "2026-07-07T00:00:00.000Z",
      "updated_at": "2026-07-07T00:00:00.000Z"
    }
  ]
}
```

### 8.5 自然语言命令

`POST /api/commands/v1/natural-language`

用途：保存自然语言控制意图，不等同于 LLM 结构化命令。返回统一 `data.command`。

`GET /api/commands/v1/recent?limit=50`

返回最近自然语言命令，统一 `data.commands`。

## 9. 日志、报警和事件流

### 9.1 报警日志

`GET /api/logs/v1/alarms?limit=50`

返回统一 `data.alarms`，适合替换当前报警 mock。

报警字段：

```json
{
  "id": "alarm_xxx",
  "level": "warning",
  "source": "gateway",
  "gateway_id": "sensair_s3_gateway_01",
  "device_id": "esp32-c5-001",
  "room_id": "living",
  "room_name": "客厅",
  "title": "空气质量异常",
  "message": "AQI 超阈值",
  "payload": {},
  "created_at_ms": 1780000000000,
  "acknowledged": false
}
```

### 9.2 系统日志

`GET /api/logs/v1/system?limit=50`

返回统一 `data.logs`，包含 `system`、`device`、`csi`、`command`、`voice` 类型日志。

### 9.3 语音事件

`GET /api/voice/v1/events?limit=50`

返回统一 `data.events`，用于语音诊断页。

### 9.4 SSE 实时事件

`GET /api/events/v1/stream`

返回 `text/event-stream`。前端使用：

```js
const stream = new EventSource("/api/events/v1/stream");

stream.addEventListener("connected", event => {
  console.log(JSON.parse(event.data));
});

stream.addEventListener("ping", event => {
  console.log(JSON.parse(event.data).server_time_ms);
});

stream.addEventListener("command_created", event => {
  const payload = JSON.parse(event.data);
  console.log(payload.data);
});
```

常见事件名：

| 事件名 | 说明 |
| --- | --- |
| `connected` | SSE 已连接 |
| `ping` | 每 15 秒心跳 |
| `alarm_created` | 新报警 |
| `system_log_created` | 系统日志 |
| `command_created` | 命令创建 |
| `voice_event_created` | 语音事件 |
| `device_status_changed` | 设备状态事件 |
| `csi_motion` | canonical CSI 状态 |
| `logs_cleaned` | 日志清理完成 |

事件 data 统一外层：

```json
{
  "server_time_ms": 1780000000000,
  "event": "command_created",
  "data": {}
}
```

## 10. LLM 和语音接口

### 10.1 文本 LLM

`POST /api/llm/text`

请求：

```json
{
  "text": "现在空气质量怎么样？",
  "device_id": "esp32-c5-001",
  "session_id": "optional"
}
```

响应：

```json
{
  "ok": true,
  "text": "模型回复文本",
  "id": 123,
  "model": "Doubao-Seed-1.6-flash",
  "server_time_ms": 1780000000000
}
```

### 10.2 结构化 LLM

`POST /api/llm/structured`

请求：

```json
{
  "text": "把音量调到 35",
  "device_id": "mic-gateway-001",
  "target_device_id": "esp32-c5-001",
  "session_id": "optional"
}
```

响应包含自然语言回复、已入队命令和被拒命令：

```json
{
  "ok": true,
  "text": "好的，我会把音量调到 35。",
  "chat": {
    "text": "好的，我会把音量调到 35。"
  },
  "commands": [],
  "rejected_commands": [],
  "structured": {
    "parsed": true,
    "version": "agent-command-v1",
    "error": ""
  },
  "id": 123,
  "model": "Doubao-Seed-1.6-flash",
  "server_time_ms": 1780000000000
}
```

### 10.3 语音唤醒提示音配置

`GET /api/voice/prompt/config`

`PUT /api/voice/prompt/config`

PUT 请求体是配置 patch，例如：

```json
{
  "wake_prompt_text": "我在，你说",
  "voice_id": "server_prompt_v1",
  "speed": 1,
  "pitch": 1,
  "volume": 1
}
```

### 10.4 语音提示音 PCM

`GET /api/voice/prompt`

`GET /api/voice/prompt-cache`

查询参数：

| 参数 | 说明 |
| --- | --- |
| `prompt_key` | 缓存 key，默认 wake prompt |
| `refresh=1` | 强制刷新 |
| `force_refresh=1` | 同上 |
| `device_id` | 可选设备 ID |

成功返回 PCM 音频，不是 JSON。前端如果要播放，需要按 `audio/L16; rate=16000; channels=1` 处理，普通浏览器 `<audio>` 不能直接播放 raw PCM。

### 10.5 语音 turn

`POST /api/voice/turn`

请求体为 raw PCM，成功返回 raw PCM。该接口主要给 ESP 设备使用，浏览器调试需要自行构造二进制 body。

关键 header：

```http
Content-Type: audio/L16; rate=16000; channels=1
X-Audio-Format: pcm_s16le_mono_16k
X-Device-Id: esp32-c5-001
X-Voice-Turn-Id: optional-request-id
```

错误响应为 JSON：

```json
{
  "ok": false,
  "code": "VOICE_UNSUPPORTED_AUDIO_FORMAT",
  "error": "X-Audio-Format must be pcm_s16le_mono_16k"
}
```

## 11. 时间同步接口

基础前缀：`/api/time`

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/api/time/now` | 当前服务器毫秒时间和 ISO 时间 |
| GET | `/api/time/status` | 当前时间和最近一次 ping |
| POST | `/api/time/ping` | 设备上报 ESP 时间，后端估算单向延迟 |

`POST /api/time/ping` 请求：

```json
{
  "device_id": "esp32-c5-001",
  "esp_send_ms": 1780000000000,
  "esp_uptime_ms": 12345678
}
```

响应：

```json
{
  "ok": true,
  "device_id": "esp32-c5-001",
  "esp_send_ms": 1780000000000,
  "esp_uptime_ms": 12345678,
  "server_recv_ms": 1780000000032,
  "server_reply_ms": 1780000000033,
  "server_time_iso": "2026-07-07T00:00:00.033Z",
  "estimated_one_way_delay_ms": 32
}
```

## 12. 记忆、画像和 Agent 状态接口

这些接口适合管理页、调试页或未来 Agent UI。多数不是当前 Dashboard 主屏必需。

### 12.1 对话和长期记忆

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| POST | `/api/conversation/turns` | 新增对话 turn |
| GET | `/api/conversation/turns?device_id=&session_id=` | 查询对话 turn |
| POST | `/api/memory/daily` | 新增每日记忆 |
| GET | `/api/memory/daily?memory_date=YYYY-MM-DD` | 查询每日记忆 |
| POST | `/api/memory/profile` | 新增或更新用户画像 |
| GET | `/api/memory/profile?status=active&category=` | 查询用户画像 |
| POST | `/api/memory/corrections` | 应用记忆纠错 |
| POST | `/api/jobs/daily-summary/run` | 触发每日摘要任务 |
| POST | `/api/jobs/weekly-profile/run` | 触发周画像任务 |
| GET | `/api/jobs/memory?job_name=&target_date=YYYY-MM-DD` | 查询任务运行记录 |

日期字段必须使用真实 `YYYY-MM-DD`，例如 `2026-07-07`。

### 12.2 环境、关系、提醒、紧急事件

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| POST | `/api/environment/profile` | 新增或更新环境画像 |
| GET | `/api/environment/profile?device_id=&status=active` | 查询环境画像 |
| POST | `/api/memory/experience` | 新增经验记忆 |
| GET | `/api/memory/experience?status=active` | 查询经验记忆 |
| POST | `/api/memory/relation` | 新增关系记忆 |
| GET | `/api/memory/relation?status=active&relation_type=` | 查询关系记忆 |
| POST | `/api/reminders/rules` | 新增提醒规则 |
| GET | `/api/reminders/rules?status=active` | 查询提醒规则 |
| POST | `/api/reminders/events` | 新增提醒事件 |
| GET | `/api/reminders/events?status=pending` | 查询提醒事件 |
| POST | `/api/emergency/events` | 新增紧急事件，并写入报警日志 |
| GET | `/api/emergency/events?device_id=&status=received` | 查询紧急事件 |

### 12.3 CSI 行为和 LCD

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| POST | `/api/csi/behavior` | 记录 CSI 行为事件 |
| GET | `/api/csi/behavior?device_id=&behavior_type=` | 查询 CSI 行为事件 |
| POST | `/api/lcd/status` | 更新 LCD 状态 |
| GET | `/api/lcd/status?device_id=` | 查询 LCD 状态 |
| POST | `/api/lcd/display` | 入队 `display.show_text` 命令并更新 LCD 状态 |

`POST /api/lcd/display` 请求：

```json
{
  "device_id": "esp32-c5-001",
  "text": "你好",
  "ttl_ms": 5000
}
```

该接口要求设备已经注册并支持 `display.show_text` capability。

## 13. 用户数据管理接口

这些接口要求 admin token，普通公开前端不要直接调用。

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| GET | `/api/user-data/summary` | 查看可删除数据范围和数量 |
| POST | `/api/user-data/delete/preview` | 预览删除影响 |
| POST | `/api/user-data/delete` | 执行删除 |
| GET | `/api/user-data/deletion-runs?limit=50` | 查看删除审计 |
| GET | `/api/user-data/export` | 预留接口，当前返回 501 |

删除预览请求：

```json
{
  "scope": "conversation",
  "mode": "soft_delete",
  "reason": "user_request",
  "requested_by": "dashboard_admin",
  "include_audit_logs": false
}
```

执行删除必须额外传：

```json
{
  "confirm": "DELETE"
}
```

## 14. 设备和网关专用写入接口

这些接口由 ESPS3 网关或设备使用。普通浏览器前端只应在受控调试工具里调用。

| 方法 | 路径 | 认证 | 用途 |
| --- | --- | --- | --- |
| POST | `/api/device/v1/ingest` | 网关认证 | 上报 `sensor.bme690` |
| POST | `/kernel/csi_event` | 网关认证 | 上报 canonical CSI v2 |
| POST | `/api/device/v1/gateway-state` | 网关认证 | 上报网关 Dashboard 快照 |
| POST | `/api/dashboard/v1/snapshot` | 网关认证 | 上报 Dashboard 快照，等价写入面 |
| POST | `/api/devices/capabilities` | 网关认证 | 注册设备命令能力 |
| GET | `/api/commands/pending?device_id=` | 网关认证 | 设备拉取命令 |
| POST | `/api/commands/:command_id/ack` | 网关认证 | 设备确认命令结果 |
| POST | `/api/smart-home/v1/state` | 网关认证 | 网关上报智能家居状态 |
| GET | `/api/smart-home/v1/commands/pending` | 网关认证 | 网关拉取智能家居命令 |
| POST | `/api/smart-home/v1/commands/:command_id/ack` | 网关认证 | 网关确认智能家居命令 |
| POST | `/api/logs/v1/alarms` | 网关认证 | 网关写入报警 |
| POST | `/api/logs/v1/system` | 网关认证 | 网关写入系统日志 |

### 14.1 canonical CSI v2

`POST /kernel/csi_event`

请求必须只包含以下顶层字段：

```json
{
  "schema_version": "v2",
  "trace_id": "trace-001",
  "tick_id": 1,
  "fused_state": "MOTION",
  "confidence": 0.72,
  "links": ["link_0", "link_1"],
  "timestamp_ms": 1780000000000
}
```

限制：

- `fused_state` 只能是 `IDLE`、`MOTION`、`HOLD`。
- `confidence` 范围是 0 到 1。
- `links` 必须是 `link_0`、`link_1` 这种连续 canonical 名称，最多 8 个。
- 该接口拒绝 raw CSI、subcarrier 数据和旧 occupancy 模型。前端展示只消费 canonical fact。

### 14.2 Dashboard 快照写入

`POST /api/device/v1/gateway-state`

`POST /api/dashboard/v1/snapshot`

请求必须：

```json
{
  "payload_type": "gateway.dashboard_snapshot",
  "schema_version": 2,
  "gateway": {
    "gateway_id": "sensair_s3_gateway_01",
    "online": true
  },
  "devices": [],
  "history": [],
  "recent_voice_events": [],
  "recent_commands": [],
  "home_summary": {},
  "source": "s3_gateway"
}
```

限制：

- 快照不能携带 legacy `occupancy` 字段。
- 快照不能携带有效 CSI state。CSI 必须通过 `/kernel/csi_event` 上报。

## 15. 旧兼容接口

新前端不推荐优先使用，但它们仍存在。

| 方法 | 路径 | 返回 |
| --- | --- | --- |
| POST | `/sensor` | `{ ok, success, id, payload_type, payload, ...timing }` |
| GET | `/sensor/latest` | 单条传感器对象或 `{}` |
| GET | `/sensor/history?limit=50` | 传感器数组，默认 50，最大 500 |
| POST | `/asr` | `{ ok, success, id }` |
| GET | `/asr/latest` | 单条 ASR 对象或 `{}` |
| POST | `/llm` | `{ ok, success, id }` |
| GET | `/llm/latest` | 单条 LLM 对象或 `{}` |

## 16. 日志清理接口

这些接口会修改数据，前端管理页需二次确认。

`POST /api/logs/v1/cleanup`

请求：

```json
{
  "type": "system",
  "older_than_ms": 604800000,
  "dry_run": true,
  "force": false
}
```

`type` 可使用 `alarm`、`system`、`command`、`voice`、`device`、`csi`、`all`。`older_than_ms` 小于 3600000 时必须 `force=true`。

`DELETE /api/logs/v1/events?type=system&older_than_ms=604800000&force=false`

该接口直接执行删除，返回统一 `data.deleted`。

## 17. 前端页面建议

- 主面板：优先使用 `GET /api/dashboard/v1/overview`，减少多接口并发和前端拼装。
- 历史曲线：使用 `GET /api/dashboard/v1/sensors/history?limit=...` 替换当前 mock。
- 报警表格：使用 `GET /api/logs/v1/alarms?limit=...` 替换当前 mock。
- 系统日志：使用 `GET /api/logs/v1/system?limit=...`，需要实时刷新时叠加 SSE。
- 智能家居：用 `GET /api/smart-home/v1/status` 渲染设备卡片，用 `POST /api/smart-home/v1/control` 创建控制命令，再用 `GET /api/smart-home/v1/commands` 展示状态。
- CSI：用 `GET /api/dashboard/v1/csi/history` 和 SSE `csi_motion` 展示 `IDLE`、`MOTION`、`HOLD`，不要画 raw subcarrier。
- 命令控制：先读 `GET /api/commands/whitelist` 和 `GET /api/devices/:device_id/capabilities`，只展示目标设备支持的按钮。

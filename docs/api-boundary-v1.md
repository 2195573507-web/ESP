# ESP-111 API Boundary v1

生成日期: 2026-06-17

本文档是 ESP-111 当前代码扫描后的唯一 API 边界规范。本文只记录真实存在的接口、表结构、协议路径和风险；未在代码中注册或实现的接口不得作为已存在能力引用。

## 1. 系统架构边界

### 1.1 当前边界事实

ESP-111 当前由四层组成:

| 层级 | 代码范围 | 当前职责 | 真实入口 |
| --- | --- | --- | --- |
| Device layer | `ESPC51/`, `ESPC52/` | 本地终端、传感器、语音采集、命令展示/执行 | 只访问 ESPS3 暴露的 `/local/v1/*` 相对路径 |
| Gateway layer | `ESPS3/` | C5 本地网关、协议适配、Server-facing 上传、命令转发、语音代理 | 本地 `/local/v1/*`; 上游 `/api/*` |
| Server layer | `ESP-server/` | HTTP API、持久化、Dashboard 聚合、voice/command/event/smart-home 业务 | Express routes |
| Frontend layer | `ESP-server/public/` | Dashboard UI | 当前只 fetch legacy `/sensor/latest`, `/asr/latest`, `/llm/latest` |

路由挂载事实:

- `ESP-server/server.js:95` 在全局 JSON parser 前挂载 voice router, 以支持 `/api/voice/turn` 原始 PCM body。
- `ESP-server/server.js:120-131` 挂载 LLM、command、device、dashboard v1、smart-home、event、memory、agent-state、user-data、record、sensor routes。
- `ESP-server/server.js:124` 将 dashboard v1 挂载到 `/api/dashboard/v1`。
- `ESP-server/server.js:134` 将 time sync 挂载到 `/api/time`。

固件边界事实:

- `ESPS3/components/esp111_protocol_common/include/esp111_protocol_common.h:139-155` 定义 C5 到 S3 的 `/local/v1/*` 和 S3 到 Server 的 `/api/*` 两组路径。
- `ESPS3/components/Middlewares/local_http_server/local_http_server.c:448-460` 只注册 `/local/v1/*` 本地 HTTP routes, 没有注册 `/api/*`。
- `ESPC51/components/Middlewares/server_comm/server_comm_config.c:79-91` 拒绝 `http://` 或 `https://` 绝对 endpoint, C5 正式链路只能访问 S3 local 相对路径。

### 1.2 v1 目标边界

v1 的系统边界如下:

```text
ESPC51/ESPC52
  -> only /local/v1/*
ESPS3
  -> owns all /api/* Server-facing traffic
ESP-server
  -> owns persistence, business state, event stream, dashboard API
Frontend
  -> reads only /api/dashboard/v1/* and approved admin/read APIs
```

强规则: C5 禁止直连 ESP-server。任何 `/api/*` 调用必须由 ESPS3 发起或代理。

## 2. API 归属矩阵

### 2.1 Device / Gateway APIs

| Endpoint | Method | 归属 | 分类 | 代码定位 | v1 归属 |
| --- | --- | --- | --- | --- | --- |
| `/api/device/v1/ingest` | POST | Gateway -> Server | device | `ESP-server/src/routes/deviceRoutes.js:102` | Gateway layer |
| `/api/device/v1/gateway-state` | POST | Gateway -> Server | device/dashboard | `ESP-server/src/routes/deviceRoutes.js:163` | Gateway layer |
| `/api/device/v1/status` | GET | Server read | device/dashboard | `ESP-server/src/routes/deviceRoutes.js:227` | Server layer |
| `/api/device/v1/status/:device_id` | GET | Server read | device/dashboard | `ESP-server/src/routes/deviceRoutes.js:236` | Server layer |
| `/api/device/v1/modules/status` | GET | Server read | device/dashboard | `ESP-server/src/routes/deviceRoutes.js:245` | Server layer |
| `/api/device/v1/context` | GET | Server read | device | `ESP-server/src/routes/deviceRoutes.js:255` | Server layer |
| `/api/device/v1/sensors/latest` | GET | Server read | device/dashboard | `ESP-server/src/routes/deviceRoutes.js:265` | Server layer |

`/api/device/v1/ingest` 当前只接受 `sensor.bme690` 和 `csi.motion`; 见 `ESP-server/src/routes/deviceRoutes.js:102-105`。

### 2.2 Dashboard APIs

挂载前缀: `/api/dashboard/v1`, 见 `ESP-server/server.js:124`。

| Endpoint | Method | 分类 | 代码定位 | v1 归属 |
| --- | --- | --- | --- | --- |
| `/api/dashboard/v1/overview` | GET | dashboard | `ESP-server/src/routes/dashboardRoutes.js:60` | Frontend read |
| `/api/dashboard/v1/snapshot` | POST | dashboard/device | `ESP-server/src/routes/dashboardRoutes.js:65` | Gateway write only |
| `/api/dashboard/v1/latest` | GET | dashboard | `ESP-server/src/routes/dashboardRoutes.js:85` | Frontend read |
| `/api/dashboard/v1/history` | GET | dashboard | `ESP-server/src/routes/dashboardRoutes.js:90` | Frontend read |
| `/api/dashboard/v1/sensors/latest` | GET | dashboard | `ESP-server/src/routes/dashboardRoutes.js:107` | Frontend read |
| `/api/dashboard/v1/sensors/history` | GET | dashboard | `ESP-server/src/routes/dashboardRoutes.js:112` | Frontend read |
| `/api/dashboard/v1/devices/:device_id/history` | GET | dashboard | `ESP-server/src/routes/dashboardRoutes.js:127` | Frontend read |
| `/api/dashboard/v1/asr/latest` | GET | dashboard | `ESP-server/src/routes/dashboardRoutes.js:145` | Frontend read |
| `/api/dashboard/v1/llm/latest` | GET | dashboard | `ESP-server/src/routes/dashboardRoutes.js:150` | Frontend read |
| `/api/dashboard/v1/time/status` | GET | dashboard | `ESP-server/src/routes/dashboardRoutes.js:155` | Frontend read |
| `/api/dashboard/v1/device/status` | GET | dashboard | `ESP-server/src/routes/dashboardRoutes.js:160` | Frontend read |
| `/api/dashboard/v1/modules/status` | GET | dashboard | `ESP-server/src/routes/dashboardRoutes.js:165` | Frontend read |

### 2.3 Voice APIs

| Endpoint | Method | 分类 | 代码定位 | v1 归属 |
| --- | --- | --- | --- | --- |
| `/api/voice/turn` | POST | voice | `ESP-server/src/routes/voiceRoutes.js:596` | Gateway -> Server streaming |
| `/api/voice/prompt/config` | GET | voice/admin-like config | `ESP-server/src/routes/voiceRoutes.js:597` | Admin/Server |
| `/api/voice/prompt/config` | PUT | voice/admin-like config | `ESP-server/src/routes/voiceRoutes.js:598` | Admin/Server |
| `/api/voice/prompt` | GET | voice | `ESP-server/src/routes/voiceRoutes.js:599` | Gateway/Device read |
| `/api/voice/prompt-cache` | GET | voice | `ESP-server/src/routes/voiceRoutes.js:600` | Gateway/Device read |

Voice turn identity 当前从 header/query 读取 device id, fallback 到 `req.ip` 或 `unknown`; 见 `ESP-server/src/voice/http.js:44-60`。

### 2.4 Command APIs

| Endpoint | Method | 分类 | 代码定位 | v1 归属 |
| --- | --- | --- | --- | --- |
| `/api/commands/whitelist` | GET | command | `ESP-server/src/routes/commandRoutes.js:108` | Server read |
| `/api/devices/capabilities` | POST | command/device | `ESP-server/src/routes/commandRoutes.js:115` | Gateway/Device registration |
| `/api/devices/:device_id/capabilities` | GET | command/device | `ESP-server/src/routes/commandRoutes.js:125` | Server read |
| `/api/commands` | POST | command | `ESP-server/src/routes/commandRoutes.js:145` | Server command creation |
| `/api/commands/pending` | GET | command | `ESP-server/src/routes/commandRoutes.js:160` | Gateway poll only |
| `/api/commands/:command_id/ack` | POST | command | `ESP-server/src/routes/commandRoutes.js:175` | Gateway ack only |
| `/api/commands/history` | GET | command | `ESP-server/src/routes/commandRoutes.js:190` | Server/admin read |
| `/api/commands/v1/natural-language` | POST | command/voice | `ESP-server/src/routes/commandRoutes.js:207` | Server command creation |
| `/api/commands/v1/recent` | GET | command/dashboard | `ESP-server/src/routes/commandRoutes.js:223` | Frontend/server read |

### 2.5 Smart-home APIs

| Endpoint | Method | 分类 | 代码定位 | v1 归属 |
| --- | --- | --- | --- | --- |
| `/api/smart-home/v1/status` | GET | command/dashboard | `ESP-server/src/routes/smartHomeRoutes.js:21` | Server read |
| `/api/smart-home/v1/state` | POST | event/device | `ESP-server/src/routes/smartHomeRoutes.js:31` | Gateway write |
| `/api/smart-home/v1/control` | POST | command | `ESP-server/src/routes/smartHomeRoutes.js:50` | Server command creation |
| `/api/smart-home/v1/commands` | GET | command | `ESP-server/src/routes/smartHomeRoutes.js:67` | Server/admin read |
| `/api/smart-home/v1/commands/pending` | GET | command | `ESP-server/src/routes/smartHomeRoutes.js:81` | Gateway poll only |
| `/api/smart-home/v1/commands/:command_id/ack` | POST | command | `ESP-server/src/routes/smartHomeRoutes.js:96` | Gateway ack only |

### 2.6 Event / Log APIs

| Endpoint | Method | 分类 | 代码定位 | v1 归属 |
| --- | --- | --- | --- | --- |
| `/api/logs/v1/alarms` | GET | event/log | `ESP-server/src/routes/eventRoutes.js:37` | Frontend/admin read |
| `/api/logs/v1/alarms` | POST | event/log | `ESP-server/src/routes/eventRoutes.js:42` | Gateway/Server write |
| `/api/logs/v1/system` | GET | event/log | `ESP-server/src/routes/eventRoutes.js:81` | Frontend/admin read |
| `/api/logs/v1/system` | POST | event/log | `ESP-server/src/routes/eventRoutes.js:86` | Gateway/Server write |
| `/api/voice/v1/events` | GET | voice/event | `ESP-server/src/routes/eventRoutes.js:114` | Frontend/admin read |
| `/api/logs/v1/cleanup` | POST | event/admin | `ESP-server/src/routes/eventRoutes.js:135` | Admin only |
| `/api/logs/v1/events` | DELETE | event/admin | `ESP-server/src/routes/eventRoutes.js:152` | Admin only |
| `/api/events/v1/stream` | GET | event/SSE | `ESP-server/src/routes/eventRoutes.js:173` | Frontend read stream |

Event stream 当前是 SSE。没有发现 public 前端使用 `EventSource`; 扫描 `ESP-server/public` 只发现 fetch legacy reads。

### 2.7 Legacy APIs

| Endpoint | Method | 分类 | 代码定位 | v1 状态 |
| --- | --- | --- | --- | --- |
| `/sensor` | POST | legacy/device | `ESP-server/src/routes/sensorRoutes.js:116` | 保留兼容, 禁止新增依赖 |
| `/sensor/latest` | GET | legacy/dashboard | `ESP-server/src/routes/sensorRoutes.js:216` | 前端当前依赖, 待迁移 |
| `/sensor/history` | GET | legacy/dashboard | `ESP-server/src/routes/sensorRoutes.js:255` | 保留兼容 |
| `/asr` | POST | legacy/voice | `ESP-server/src/routes/recordRoutes.js:25` | 保留兼容 |
| `/llm` | POST | legacy/voice | `ESP-server/src/routes/recordRoutes.js:45` | 保留兼容 |
| `/asr/latest` | GET | legacy/dashboard | `ESP-server/src/routes/recordRoutes.js:66` | 前端当前依赖, 待迁移 |
| `/llm/latest` | GET | legacy/dashboard | `ESP-server/src/routes/recordRoutes.js:80` | 前端当前依赖, 待迁移 |

### 2.8 Admin / Memory / Agent APIs

| Endpoint family | 分类 | 代码定位 | v1 归属 |
| --- | --- | --- | --- |
| `/api/user-data/*` | admin | `ESP-server/src/routes/userDataRoutes.js:32-48` | Admin layer |
| `/api/conversation/*`, `/api/memory/*`, `/api/jobs/memory` | memory/admin-like | `ESP-server/src/routes/memoryRoutes.js:237-306` | Admin/Server layer |
| `/api/environment/profile`, `/api/reminders/*`, `/api/emergency/*`, `/api/csi/behavior`, `/api/lcd/*` | server/agent/admin-like | `ESP-server/src/routes/agentStateRoutes.js:239-403` | Server/Admin layer |
| `/api/llm/text`, `/api/llm/structured` | server/LLM | `ESP-server/src/routes/llmTextRoutes.js:22`, `ESP-server/src/routes/structuredLlmRoutes.js:29` | Server layer |
| `/api/time/now`, `/api/time/status`, `/api/time/ping` | server/time | `ESP-server/server-time-sync/timeSync.js:92-105` | Server/Gateway support |

只有 `/api/user-data/*` 明确挂载 `requireUserDataAdmin`; 见 `ESP-server/src/routes/userDataRoutes.js:32-48` 与 `ESP-server/src/services/userDataService.js:624-656`。

## 3. 数据归属模型

### 3.1 Sensor 数据

权威表: `sensor_records`, 见 `ESP-server/src/db/sensorRecords.js:5-38`。

关键字段:

- 时间: `timestamp`, `esp_time_ms`, `esp_uptime_ms`, `server_recv_ms`, `server_time_iso`, `upload_delay_ms`
- identity: `device_id`, `sensor_id`, `device_type`, `firmware_version`
- payload: `payload_type`, `payload_json`, `raw_payload`, `raw_json`, `metadata_json`
- BME690: `temperature`, `humidity`, `pressure`, `gas_resistance`, `air_quality_*`, `gas_*`, `humidity_score`

v1 归属:

- C5 只产生本地 sensor payload。
- S3 将 C5 payload 转成 Server envelope; BME 转换位于 `ESPS3/components/Middlewares/protocol_adapter/protocol_adapter.c:164-200`。
- Server `/api/device/v1/ingest` 写入 `sensor_records`; legacy `/sensor` 也写同表作为兼容入口。

不一致点:

- S3 BME adapter 写 `air_quality_source: "s3_mapped"`; Server 侧 `sensorBme690Service.js:74` 会将非 `server_fallback` 来源归一为 `"esp"`。v1 文档中 `air_quality_source` 的枚举应统一。
- CSI 只能以 `payload_type: "csi.motion"` 作为摘要事件进入 Server; 不得假设存在原始 CSI 上传合同。

### 3.2 Voice 数据

权威表:

- `voice_turns`, 见 `ESP-server/src/db/voiceTurns.js:5-25`。
- legacy `asr_records`, `llm_records`, 见 `ESP-server/src/db/records.js:5-15`。

v1 归属:

- C5 -> S3 使用 `/local/v1/voice/turn`。
- S3 -> Server 使用 `/api/voice/turn`; `ESPS3/components/Middlewares/server_client/server_client.c:421-447` 设置 PCM content-type 与 `X-Device-Id`。
- Server 记录 turn 级元数据, 不将 PCM 作为业务表 dual-write 对象。

mock 来源:

- `VOICE_TURN_MOCK` 开关定义在 `ESP-server/src/voice/turnConfig.js:15`。
- mock PCM 生成在 `ESP-server/src/voice/mockTurn.js:5-36`。
- voice route 在 mock 开启时走 `streamMockVoiceTurn`; 见 `ESP-server/src/routes/voiceRoutes.js:501-507`。

### 3.3 Command 数据

权威表:

- generic command: `command_queue`, 见 `ESP-server/src/db/commands.js:16-34`。
- smart-home command: `smart_home_commands`, 见 `ESP-server/src/db/smartHome.js:64-90`。
- natural language command: `natural_language_commands`, 见 `ESP-server/src/db/smartHome.js:74-85`。

v1 归属:

- Server 创建命令并持久化。
- S3 轮询 `/api/commands/pending` 与 `/api/smart-home/v1/commands/pending`。
- C5 只轮询 S3 的 `/local/v1/commands/pending` 并向 S3 ACK。
- S3 再向 Server ACK `/api/commands/:command_id/ack` 或 smart-home ACK。

不一致点:

- generic command 状态使用 `queued`, `dispatched`, `completed`, `failed`。
- smart-home ACK 将 `completed` 归一为 `success`, 且有效终态为 `success` 或 `failed`; 见 `ESP-server/src/services/smartHomeService.js:380-424`。
- v1 规范应统一对外语义: `queued -> dispatched -> succeeded|failed|expired`, 并在兼容层映射旧字段。

### 3.4 Event / Log 数据

权威表: `event_logs`, 见 `ESP-server/src/db/eventLogs.js:6-18`。

允许事件类型来自 `EVENT_TYPES = ["alarm","system","command","voice","device","csi"]`; 见 `ESP-server/src/services/eventLogService.js:7`。

v1 归属:

- Server `recordEvent()` 是统一写入函数, 写入 `event_logs` 并广播 SSE; 见 `ESP-server/src/services/eventLogService.js:150-199`。
- SSE 服务位于 `ESP-server/src/services/eventStreamService.js:38-84`。
- `/api/events/v1/stream` 是当前唯一注册的事件流 HTTP endpoint; 见 `ESP-server/src/routes/eventRoutes.js:173`。

不一致点:

- command、voice、smart-home、memory、agent-state 仍各有历史表或业务表, event_logs 不是唯一事件事实源。
- Event 输出 shape 在 alarm/system/voice 查询中有映射差异, payload_json 为宽松 JSON。

### 3.5 Dashboard 数据

权威表: `dashboard_snapshots`, 见 `ESP-server/src/db/dashboardSnapshots.js:6-14`。

v1 归属:

- Gateway 可以写 `/api/device/v1/gateway-state` 或 `/api/dashboard/v1/snapshot`, 两者都走 `ingestDashboardSnapshot`。
- Frontend 读应迁移到 `/api/dashboard/v1/*`。
- `mockAppliances()` 只能作为 degraded placeholder, 不得作为真实设备状态。

mock 来源:

- backend `mockAppliances()` 位于 `ESP-server/src/services/dashboardService.js:135`。
- fallback snapshot appliances 位于 `ESP-server/src/services/dashboardService.js:478` 和 `ESP-server/src/services/dashboardService.js:1177`。

## 4. 强边界规则

1. C5 禁止直连 Server。C5/ESPC51/ESPC52 只能访问 `/local/v1/*` 相对 endpoint; `server_comm_build_url()` 已拒绝绝对 URL, 见 `ESPC51/components/Middlewares/server_comm/server_comm_config.c:79-91`。
2. ESPS3 是唯一 Server-facing gateway。所有 `/api/device/v1/*`, `/api/voice/*`, `/api/commands/*`, `/api/smart-home/v1/*`, `/api/logs/v1/*` 的设备侧调用必须由 ESPS3 发起。
3. `/local/v1/*` 不得暴露 Server 语义。S3 local HTTP server 当前只注册 local routes, 见 `ESPS3/components/Middlewares/local_http_server/local_http_server.c:448-460`。
4. Frontend 不得直接调用 Device/Gateway write API。Frontend v1 读源只能是 `/api/dashboard/v1/*` 和显式允许的 read/admin endpoints。
5. Legacy `/sensor`, `/asr`, `/llm` 只保留兼容, 禁止新依赖。当前 public 仍依赖 legacy latest routes, 必须先迁移再关闭。
6. Server 不得信任 body/header/query 中的 `device_id` 作为已认证身份。当前 `readDeviceMetadata()` 从 body/header/query 读取 identity, 见 `ESP-server/src/services/deviceMetadata.js:145-174`; v1 必须引入 gateway-device binding。
7. ACK 必须校验 command ownership。ACK 不得只凭 `command_id` 更新状态; 必须验证 gateway_id/device_id/command_id 绑定。
8. Mock/fallback 数据必须显式标记并禁止写入真实执行成功语义。没有真实 smart-home device 时, ACK 必须失败而不是伪装成功。
9. CSI 只允许摘要模型进入 Server。不得新增 raw CSI upload, 除非后端合同和存储模型明确变更。
10. Voice turn 不得 dual-write PCM。PCM 是流式请求/响应链路, Server 只持久化 turn 元数据、耗时、字节数和错误状态。

## 5. Command 状态机

### 5.1 当前实现

generic command:

```text
queued
  -> dispatched
  -> completed | failed
```

证据:

- 创建时写入 `status: "queued"`; 见 `ESP-server/src/commands/queue.js:217-239`。
- pending 查询会领取 `queued` 或超时的 `dispatched`, 并更新为 `dispatched`; 见 `ESP-server/src/commands/queue.js:295-341`。
- ACK 只接受 `completed` 或 `failed`; 见 `ESP-server/src/commands/queue.js:344-419`。

smart-home command:

```text
queued
  -> dispatched
  -> success | failed
```

证据:

- ACK 中 `completed` 被映射为 `success`; 有效终态为 `success` 或 `failed`, 见 `ESP-server/src/services/smartHomeService.js:380-424`。

S3 local command:

```text
queued
  -> dispatched
  -> acked locally
  -> server ack best effort
```

证据:

- S3 pending build 会拉 Server pending 并将本地 queue 中 matching entry 标记为 dispatched; 见 `ESPS3/components/Middlewares/command_router/command_router.c:290-373`。
- S3 有本地 queue 状态, 但 Server ACK 失败不应被 C5 视为全局成功。

### 5.2 v1 规范状态机

v1 对外规范状态:

```text
created
  -> queued
  -> dispatched
  -> acknowledged
  -> succeeded | failed | expired | rejected
```

映射规则:

- 当前 `completed` 映射为 v1 `succeeded`。
- 当前 smart-home `success` 映射为 v1 `succeeded`。
- 当前 `COMMAND_ACK_NOT_ACCEPTED` 映射为 v1 `rejected`。
- 超过 dispatch timeout 但被重新领取的命令必须保留 redispatch count; 不能无限静默重发。

ACK 必须包含:

- `command_id`
- `gateway_id`
- `device_id`
- `status`
- `executed_at_ms`
- `result` 或 `error_code/error_message`

ACK 校验必须验证:

- command 存在且未终结
- command 属于该 device
- device 属于该 gateway
- ACK status 是允许终态
- 重复 ACK 不改变终态, 返回幂等结果

## 6. Event 模型

### 6.1 当前模型

当前统一事件表:

```text
event_logs(
  event_id,
  event_type,
  event_name,
  device_id,
  severity,
  message,
  payload_json,
  source,
  server_recv_ms,
  created_at,
  updated_at
)
```

来源: `ESP-server/src/db/eventLogs.js:6-18`。

`recordEvent()` 会:

- normalize event_type
- 生成 event_id
- 写入 `event_logs`
- broadcast SSE

来源: `ESP-server/src/services/eventLogService.js:150-199`。

SSE payload:

```json
{
  "server_time_ms": 0,
  "event": "event_name",
  "data": {}
}
```

来源: `ESP-server/src/services/eventStreamService.js:69-84`。

### 6.2 v1 事件合同

v1 event 必须统一字段:

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `event_id` | 是 | 全局唯一 |
| `event_type` | 是 | `alarm/system/command/voice/device/csi` |
| `event_name` | 是 | 具体事件名 |
| `source` | 是 | `gateway/server/voice/command/dashboard/admin` 等 |
| `gateway_id` | 建议 | Gateway 事件必须有 |
| `device_id` | 条件必填 | Device 事件必须有 |
| `severity` | 是 | `info/warning/error/critical` |
| `server_recv_ms` | 是 | Server 接收时间 |
| `payload` | 是 | JSON object, 不得为任意 string |

v1 stream 规则:

- `/api/events/v1/stream` 是唯一 SSE stream。
- 不得并行新增 WebSocket event stream, 除非明确建立同一 event schema 的 transport adapter。
- voice 上游 WebSocket 只属于 Server 到第三方 ASR/TTS 链路, 不等同于系统 event stream。

## 7. 版本策略

### 7.1 命名规则

- Device/Gateway 到 Server: `/api/<domain>/v1/*`
- Frontend dashboard: `/api/dashboard/v1/*`
- Local C5/S3: `/local/v1/*`
- Legacy 无版本 routes: `/sensor`, `/asr`, `/llm` 只保留兼容, 不新增能力。

### 7.2 迁移路径

Phase A: 保持现有 legacy 可用。

- 不能关闭 `/sensor/latest`, `/asr/latest`, `/llm/latest`, 因为 `ESP-server/public/app.js:497`, `506`, `515` 当前仍依赖它们。

Phase B: Frontend read 迁到 dashboard v1。

- 主 Dashboard 应读取 `/api/dashboard/v1/overview` 或细粒度 `/api/dashboard/v1/sensors/latest`, `/asr/latest`, `/llm/latest`。
- S3 页面当前使用 mock data; 见 `ESP-server/public/pages/s3.js:371-395`; 迁移前不得声称其显示真实后端状态。

Phase C: Server 侧 legacy deprecation。

- legacy 写入口 `/sensor`, `/asr`, `/llm` 增加弃用指标后进入只读或 410 计划。
- 删除前必须确认 public、脚本、旧设备均不依赖 legacy。

Phase D: v1-only。

- 设备数据只通过 S3 -> `/api/device/v1/ingest`。
- Dashboard 只读 `/api/dashboard/v1/*`。
- Command 只通过 Server queue 与 S3 gateway 转发。

### 7.3 Dual-write 策略

- 不允许 C5 dual-write。C5 继续只 POST S3 local endpoints。
- Sensor dual-write 是 Server 同表双入口兼容: legacy `/sensor` 与 v1 `/api/device/v1/ingest` 都写 `sensor_records`。
- Snapshot 写入 `dashboard_snapshots`; `ESP-server/src/db/dashboardSnapshots.js:6-14` 是当前表结构。
- Voice 不 dual-write PCM。
- Command 不在 C5 和 Server 双写; Server 是命令事实源, S3 是转发/缓存/ACK 桥。

### 7.4 Rollback 策略

- Frontend 可临时回滚到 legacy latest routes, 直到 Phase C 前。
- 设备回滚点在 ESPS3 `server_client` 或 Server route adapter, 不允许把 C5 改为直连 Server。
- 数据回滚不做破坏性清理, 优先读 `sensor_records`, `dashboard_snapshots`, `command_queue`, `event_logs` 已持久化数据。
- 发布前必须确认协议头副本一致: `shared_components/esp111_protocol_common` 与 `ESPS3/components/esp111_protocol_common` 同时存在, 不能只改一份。

## 8. 安全信任模型

### 8.1 当前风险事实

当前 Server machine APIs 未发现统一认证中间件。只有 user-data 路由显式使用 admin token:

- `ESP-server/src/routes/userDataRoutes.js:32-48`
- `ESP-server/src/services/userDataService.js:624-656`

Device identity 当前来自 body/header/query:

- `ESP-server/src/services/deviceMetadata.js:145-174`
- `ESP-server/src/voice/http.js:44-60`

ESPS3 上游只发送声明式 headers, 例如 voice 中设置 `X-Device-Id`; 见 `ESPS3/components/Middlewares/server_client/server_client.c:421-447`。

### 8.2 v1 trust boundary

v1 必须区分:

| 身份 | 来源 | 是否可信 | 规则 |
| --- | --- | --- | --- |
| `gateway_id` | Gateway credential/session | 可信前提 | 必须认证 |
| `device_id` | Gateway binding registry | 条件可信 | 必须属于该 gateway |
| body/header `device_id` | 请求自报 | 不可信 | 只能作为候选, 不可直接授权 |
| frontend user | browser session/admin token | 条件可信 | 不得调用 device write/ack |
| command_id | Server generated | 条件可信 | 必须校验 owner/device/gateway |

v1 最小安全要求:

1. 所有 Gateway write/poll/ack API 必须有 Gateway auth。
2. Server 必须维护 gateway-device binding。
3. `/api/commands/pending` 必须按 authenticated gateway 和 bound device 过滤。
4. `/api/commands/:command_id/ack` 必须验证 command belongs to gateway/device。
5. `/api/device/v1/ingest` 不得仅凭 body/header `device_id` 入库为可信设备。
6. Admin-only cleanup/config/delete APIs 必须统一 admin auth, 不只保护 user-data。

## 9. 前端接口边界

### 9.1 当前事实

主 Dashboard 当前 fetch:

- `/sensor/latest`, 见 `ESP-server/public/app.js:497`
- `/asr/latest`, 见 `ESP-server/public/app.js:506`
- `/llm/latest`, 见 `ESP-server/public/app.js:515`

未发现 public 直接调用 `/api/device/v1/*`, `/api/commands/*`, `/api/smart-home/v1/*`, `/local/v1/*`。

当前 public mock/fallback:

- 初始 mock state: `ESP-server/public/app.js:43-119`
- API 空数据/错误 fallback 到 mock: `ESP-server/public/app.js:476-492`
- history/logs mock: `ESP-server/public/app.js:522-533`
- update loop 混合 mock logs: `ESP-server/public/app.js:1739-1761`
- S3 页面 mock data/render: `ESP-server/public/pages/s3.js:2`, `371`, `395`

### 9.2 v1 前端规则

Frontend allowed:

- `GET /api/dashboard/v1/overview`
- `GET /api/dashboard/v1/latest`
- `GET /api/dashboard/v1/history`
- `GET /api/dashboard/v1/sensors/latest`
- `GET /api/dashboard/v1/sensors/history`
- `GET /api/dashboard/v1/asr/latest`
- `GET /api/dashboard/v1/llm/latest`
- `GET /api/dashboard/v1/time/status`
- `GET /api/dashboard/v1/device/status`
- `GET /api/dashboard/v1/modules/status`
- `GET /api/logs/v1/alarms`
- `GET /api/logs/v1/system`
- `GET /api/events/v1/stream`

Frontend forbidden:

- 直接调用 `/local/v1/*`
- 直接写 `/api/device/v1/ingest`, `/api/device/v1/gateway-state`, `/api/dashboard/v1/snapshot`
- 直接 poll/ack `/api/commands/pending`, `/api/commands/:command_id/ack`
- 依赖 mock/fallback 作为真实设备状态
- 新增对 legacy `/sensor`, `/asr`, `/llm` 的依赖

## 10. 风险总结 + Must Fix Top 10

### 10.1 风险总览

| 风险 | 等级 | 事实依据 | 影响 |
| --- | --- | --- | --- |
| Server machine APIs 缺少统一 Gateway auth | critical | 只有 user-data 有 admin token; `userDataRoutes.js:32-48` | 公网暴露时可伪造设备、命令、日志 |
| Server 信任 body/header/query `device_id` | critical | `deviceMetadata.js:145-174`, `voice/http.js:44-60` | identity spoofing |
| Command ACK 缺少 gateway/device ownership 校验 | high | `queue.js:344-419` 按 `command_id` 更新 | 可伪造 ACK 或污染状态 |
| Frontend 仍依赖 legacy latest APIs | high | `public/app.js:497`, `506`, `515` | v1 dashboard 不能关闭 legacy |
| backend/frontend mock 数据可能进入真实展示 | high | `dashboardService.js:135`, `public/app.js:43-119` | 真实状态与占位数据混淆 |
| Voice mock 可绕过真实 ASR/LLM/TTS 链路 | medium | `turnConfig.js:15`, `voiceRoutes.js:501-507` | 测试/生产模式边界不清 |
| Event schema 未完全统一 | medium | `eventLogs.js:6-18`, 多业务历史表 | 查询和流式消费不一致 |
| Command 状态机不完整 | medium | generic `completed`; smart-home `success` | 前端/设备解释不一致 |
| 协议头存在多份 | medium | `shared_components/...` 与 `ESPS3/components/...` | 迁移时容易只改一份 |
| `/local/v1/time/now` 仅 C5 头文件预留, S3 未注册 | low | S3 local route table `local_http_server.c:448-460` | 容易误文档化不存在接口 |

### 10.2 Must Fix Top 10

1. 为所有 Gateway write/poll/ack APIs 增加统一 Gateway auth。
2. 建立 server-side gateway-device binding, 禁止直接信任请求自报 `device_id`。
3. 修复 generic command ACK: 校验 `command_id + gateway_id + device_id` ownership, 并实现幂等 ACK。
4. 统一 command 终态: `succeeded/failed/expired/rejected`, 兼容映射 `completed/success`。
5. 将 public Dashboard 读源迁移到 `/api/dashboard/v1/*`, 然后再规划 legacy shutdown。
6. 对所有 mock/fallback 数据加显式 `source=mock` 与 UI/日志标识, 禁止 mock 进入真实成功语义。
7. 统一 event schema 与 SSE payload, 明确 event_logs 与业务历史表的主从关系。
8. 统一 BME `air_quality_source` 枚举, 解决 `s3_mapped` 与 Server `"esp"` 归一差异。
9. 收敛协议头副本, 或建立构建期校验, 防止 `shared_components` 与 ESPS3 协议常量漂移。
10. 为 legacy `/sensor`, `/asr`, `/llm` 增加 deprecation metrics, 在 frontend 迁移完成后进入只读/410 计划。

## 附录 A: 不得假设存在的接口

以下接口或能力在当前扫描中不能作为已实现事实使用:

- `/local/v1/time/now`: C5 端有预留引用, 但 S3 local HTTP route table 未注册。
- raw CSI upload: Server v1 ingest 当前只支持 `csi.motion` 摘要, 没有 raw CSI 合同。
- Frontend EventSource 实时事件消费: Server 有 SSE endpoint, 但 public 扫描未发现 `EventSource` 使用。
- WebSocket event stream: 当前 WebSocket 只属于 voice 上游链路相关实现, 不是系统事件 stream。

## 附录 B: Legacy 关闭条件

Legacy routes 只有同时满足以下条件才能关闭:

1. `ESP-server/public` 不再 fetch `/sensor/latest`, `/asr/latest`, `/llm/latest`。
2. 旧脚本、旧设备、手工流程不再 POST `/sensor`, `/asr`, `/llm`。
3. dashboard v1 smoke 覆盖 latest/history/asr/llm/device/module/time。
4. Gateway ingest 与 snapshot 在真实运行中有成功记录。
5. rollback 文档确认回退不需要改 C5 直连 Server。

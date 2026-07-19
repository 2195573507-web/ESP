# ESP Device Protocol v1 Roadmap

本文是 2026-06-09 基于当前 `Whole-project` 固件和 `ESP-server` 后端现状整理并落地的统一设备协议 v1 记录。本文保留原方案细节，同时记录本轮实际实施状态、接口、数据库和 legacy 边界。

## 实施状态

- P1 已完成：新增 `POST /api/device/v1/ingest`，BME690 固件主上传链路切到 envelope v1，ESP 本地计算并上传 BME690 相对空气状态。
- P2 已完成：新增 `device_status`、`device_module_status`，拆分整机在线和模块在线，实现 `latest_upload_delay_ms`、`avg_upload_delay_ms`、`delay_sample_count`。
- P3 已完成：新增服务器 voice prompt cache，cache hit 不请求 TTS/LLM/网关；ESP wake prompt 主路径请求 `/api/voice/prompt-cache`。
- P4 已完成：新增 `deviceContextService` 与 `llmPromptContextService`，所有 LLM 路由和 voice chain LLM 调用统一使用该服务。
- P5 已完成：`voice.turn`、`voice.prompt`、`command.capabilities`、`command.poll`、`command.ack`、`time.ping` 接入 v1 metadata 和 device/module status 刷新。
- P6 已完成：新增 `GET /api/device/v1/status`、`GET /api/device/v1/modules/status`、`GET /api/device/v1/context`、`GET /api/device/v1/sensors/latest`。
- P7 已完成：`/sensor`、`/sensor/latest`、`/sensor/history` 保留为 legacy/前端兼容入口，API 文档、项目记忆和 smoke regression 已同步。
- 未接入：CSI 底层采集、LCD 底层驱动仍只预留协议和状态，不作为本轮已实现功能。

## 0. 原始现状确认

以下内容是本轮实施前的只读快照，保留用于对照迁移前后的路径与字段；当前真实状态以本文 `实施状态`、`project-memory.md` 和 `api.md` 为准。

### 固件侧确认

- BME690 当前真实上传链路为 `app_orchestrator_start()` -> `bme_sensor_service_start()` -> `bme_sensor_task()` -> `bme_server_client_upload_reading()` -> `POST /sensor`。
- `bme_server_client.c` 当前仍拼接旧扁平 JSON：`device_id`、`temperature`、`humidity`、`pressure`、`gas_resistance`。
- `server_comm_config.h` 当前公共整机 ID 为 `SERVER_COMM_DEVICE_ID="esp32-c5-whole-001"`，服务器地址为 `SERVER_COMM_BASE_URL="http://124.221.162.188:3000"`。
- BME 上传仍使用 `BME_SENSOR_DEVICE_ID`，当前语义上和 `SERVER_COMM_DEVICE_ID` 不一致。后续必须统一：`device_id` 表示整机，`sensor_id` 或 `module_id` 表示模块。
- `app_time_sync` 已提供 `app_time_sync_once()`、`app_time_sync_get_uptime_ms()`、`app_time_sync_get_unix_ms()`、`app_time_sync_is_synced()`，但 `app_orchestrator_start()` 当前没有调用 `app_time_sync_once(APP_TIME_SYNC_SERVER_URL)`。
- `server_comm_http` 当前公共 header 只自动加 `X-Device-Id`；还没有统一加 `X-Schema-Version`、`X-Request-Seq`、`X-Esp-Uptime-Ms`、`X-Esp-Time-Ms`、`X-Time-Synced`、`X-Payload-Type`。
- `wake_prompt_cache.c` 当前启动后请求 `GET /api/voice/prompt?wake=1&device_id=...`，下载 `audio/L16; rate=16000; channels=1` / `pcm_s16le_mono_16k`，保存到 `/spiffs/wake_prompt.pcm` 和 `/spiffs/wake_prompt.meta`，唤醒时优先播放 ESP 本地 SPIFFS 缓存，失败时回退到内嵌 `wake_ack_wozai_nishuo_16k.pcm`。
- `server_voice_client.c` 当前 `POST /api/voice/turn` 使用 raw chunked PCM body，请求头包含 `Content-Type: audio/L16; rate=16000; channels=1`、`X-Audio-Format: pcm_s16le_mono_16k` 和公共 `X-Device-Id`，未携带统一时间字段。
- `system_server_client.c` 当前使用独立 `agent-command-v1`：`POST /api/devices/capabilities`、`GET /api/commands/pending?device_id=...&limit=1`、`POST /api/commands/:id/ack`，未携带统一 metadata。
- CSI 当前为 placeholder：`csi_server_client_upload_features()` 返回 `ESP_ERR_NOT_SUPPORTED`。
- LCD/screen 当前为 placeholder/上层命令桥：`screen_server_client_poll_commands()` 返回 `ESP_ERR_NOT_SUPPORTED`，`ai_screen_bridge_*()` 只记录或转发到占位服务，不代表硬件屏已接入。
- `server_upload_bridge` 仍作为独立组件参与构建，内部也拼旧扁平 `/sensor` JSON，但当前源码调用点未指向它。它应标记为 legacy 重复路径，不继续扩展。

### 后端侧确认

- `server.js` 挂载了 `createVoiceRouter()`、`express.json()`、静态 `public/`、`/api/llm/text`、`/api/llm/structured`、command、memory、agent state、legacy record、sensor、`/api/time/*`。
- `sensorRoutes.js` 当前只有 `POST /sensor`、`GET /sensor/latest`、`GET /sensor/history`，并写入旧 `sensor_records` 列。
- `sensor_records` 当前已有 `timestamp`、`temperature`、`humidity`、`pressure`、`gas_resistance`、`device_id`、`esp_time_ms`、`esp_uptime_ms`、`server_recv_ms`、`server_time_iso`、`upload_delay_ms`。
- `timeSync.js` 的 `/api/time/now` 返回 `server_time_ms/server_time_iso`；`/api/time/ping` 当前只在内存保留 `latestPingRecord`，尚未统一刷新 `device_status`。
- `voiceRoutes.js` 中 `GET /api/voice/prompt` 当前每次 miss/mock 以外会调用 TTS 生成固定提示音，没有服务器文件缓存层。
- `voiceRoutes.js` 中 `POST /api/voice/turn` 记录 `voice_turns` 诊断数据，但未统一刷新 device/module status。
- `src/voice/chain.js` 中 voice turn 的 LLM 文本由 ASR 文本直接传入 `requestLlmText()`，没有统一设备上下文 prompt 拼接。
- `llmTextRoutes.js` 直接把用户 text 发给 `requestLlmText()`；`structuredLlmRoutes.js` 只使用 `buildStructuredPrompt()` 做结构化命令提示，不统一注入设备上下文。
- `commandRoutes.js` 当前是独立命令队列 API，没有统一 device protocol envelope。
- `agentStateRoutes.js` 已有 CSI behavior、LCD status、emergency、memory 等 server-side 协议/存储基础，但不是统一设备 ingest 入口。
- `scripts/smoke-regression.js` 已覆盖临时数据库、旧 schema 迁移、LLM/command/voice 等回归路径，后续应扩展新 envelope、空气质量、device/module status 和 prompt cache 测试。
- `ESP-server/docs/` 当前已有 `api.md`、`deploy-branches.md`、`frontend-backend-boundary.md`。

### 与已知现状的差异

- 已知结论大体一致。
- 需要更精确的一点：`server_upload_bridge` 不在当前 BME 真实调用链内，但它确实仍作为 ESP-IDF component 参与构建；因此应描述为 "legacy duplicate component, built but unused by current BME service path"。
- 当前唤醒提示音已经有 ESP 本地 SPIFFS 缓存；本方案的新增重点是把 "固定提示音生成与文件缓存" 前移到服务器端，ESP 可以继续保留小型本地 fallback/cache，但不再依赖大体积内嵌固定语音作为主路径。

## 1. 当前问题总览

- ESP 与服务器接口风格不统一：BME、voice、command、time、CSI/LCD 各自定义字段和路径。
- BME690 使用旧扁平 `POST /sensor`，主字段是 `temperature`、`humidity`、`pressure`、`gas_resistance`。
- voice turn 是 raw PCM body，只通过 header/query 携带少量设备信息。
- command 使用独立 `agent-command-v1` 风格，不和传感器/语音请求共享 envelope。
- `time.ping` 当前只在内存或局部响应里存在，未统一进入 `device_status`。
- `device_id` 语义混乱：整机 ID 和 BME 模块 ID 混用。
- `esp_time_ms`、`esp_uptime_ms`、`time_synced`、`request_seq`、`schema_version` 没有全面上传。
- online 判断不能继续绑定 BME 最新记录；BME 停止上传不等于整机离线。
- `upload_delay_ms` 不能只看瞬时值，dashboard 和 LLM context 应优先使用有效样本平均值。
- 旧 `server_upload_bridge` 是 legacy 重复路径，不应继续扩展。
- 前端本轮暂不改，但后端要提供新接口并保持旧前端不崩。
- 空气质量后续由 ESP 本地计算，不再作为纯服务器派生指标。

## 2. 旧协议废弃策略

- 新 ESP 固件不再向 `/sensor` 发送旧扁平 body。
- 新主入口推荐为 `POST /api/device/v1/ingest`。
- `/sensor` 可以保留为 legacy 后端兼容入口和旧前端查询适配，但不再作为新 ESP 主上传协议。
- 不再让 ESP 新上传 body 携带 `temperature`、`humidity`、`pressure`、`gas_resistance` 作为主字段。
- 后端数据库可以继续保留旧列名，用于历史兼容和旧前端映射。
- 新服务层可以把 `payload.temperature_c` 映射到旧列 `temperature`，把 `payload.humidity_percent` 映射到旧列 `humidity`，把 `payload.pressure_hpa` 映射到旧列 `pressure`，把 `payload.gas_resistance_ohm` 映射到旧列 `gas_resistance`。
- `server_upload_bridge` 标记 legacy，不继续扩展；后续如要清理，应单独评估构建依赖和二进制体积。

## 3. 统一设备协议 v1

### JSON envelope

```json
{
  "schema_version": 1,
  "device_id": "esp32-c5-whole-001",
  "device_type": "esp32c5_env_voice_node",
  "firmware_version": "0.1.0",
  "request_seq": 123,
  "esp_uptime_ms": 12345678,
  "esp_time_ms": 1780732142207,
  "time_synced": true,
  "payload_type": "sensor.bme690",
  "payload": {}
}
```

### 字段语义

- `device_id` 表示整机 ID，不表示单个 BME690、Mic、LCD 或 CSI 模块。
- `sensor_id` 或 `module_id` 放在 `payload` 内。
- `server_recv_ms` 和 `server_time_iso` 只由服务器生成。
- `upload_delay_ms` 只由服务器计算。
- `time_synced=false` 时，`esp_time_ms` 可以省略或为 `null`，不允许用 `0` 伪装真实 Unix 时间。
- `esp_uptime_ms` 是 ESP 自启动以来的单调时间，即使未同步服务器时间也应上传。
- `request_seq` 是 ESP 本地单调递增请求序号，用于排查重试、丢包和乱序，不作为全局唯一 ID。
- `schema_version` 第一版固定为数字 `1`。

## 4. BME690 新 payload 示例

```json
{
  "schema_version": 1,
  "device_id": "esp32-c5-whole-001",
  "device_type": "esp32c5_env_voice_node",
  "firmware_version": "0.1.0",
  "request_seq": 123,
  "esp_uptime_ms": 12345678,
  "esp_time_ms": 1780732142207,
  "time_synced": true,
  "payload_type": "sensor.bme690",
  "payload": {
    "sensor_id": "bme690_01",
    "temperature_c": 29.57,
    "humidity_percent": 30.29,
    "pressure_hpa": 986.26,
    "gas_resistance_ohm": 35164.0,
    "air_quality_score": 72,
    "air_quality_level": "moderate",
    "air_quality_confidence": "low",
    "air_quality_algo_version": "esp-bme690-relative-v1",
    "air_quality_source": "esp",
    "gas_baseline_ohm": 82000.0,
    "gas_ratio": 0.43,
    "gas_score": 43,
    "humidity_score": 87
  }
}
```

- 原始 BME690 字段必须保留：`temperature_c`、`humidity_percent`、`pressure_hpa`、`gas_resistance_ohm`。
- 空气质量字段由 ESP 本地计算后上传。
- 服务器负责接收、校验、入库、展示和 prompt 拼接。
- 服务器可 fallback 补算，但必须标记 `air_quality_source="server_fallback"`。
- 不得把 `air_quality_score` 命名为 `AQI`，也不得宣传为国标 AQI、PM2.5、PM10 或 CO2。

## 5. 非 JSON、raw PCM、GET 请求 metadata

raw PCM 或 GET 请求不强行塞进 JSON body，统一使用 header metadata：

```http
X-Schema-Version: 1
X-Device-Id: esp32-c5-whole-001
X-Device-Type: esp32c5_env_voice_node
X-Firmware-Version: 0.1.0
X-Request-Seq: 123
X-Esp-Uptime-Ms: 12345678
X-Esp-Time-Ms: 1780732142207
X-Time-Synced: true
X-Payload-Type: voice.turn
```

- `voice.turn` raw PCM 不强行塞进 JSON body。
- `command.poll`、`voice.prompt` 这类 GET 请求可以使用 header metadata。
- 原有音频格式 header 必须保留：`Content-Type: audio/L16; rate=16000; channels=1`、`X-Audio-Format: pcm_s16le_mono_16k`。
- 后端解析 header metadata 后，仍由相应业务 handler 处理 raw PCM、文件流或 JSON body。

## 6. 服务器统一响应 envelope

### 成功响应

```json
{
  "ok": true,
  "server_recv_ms": 1780732144669,
  "server_time_iso": "2026-06-09T20:10:44.669Z",
  "request_id": "...",
  "error": null,
  "data": {}
}
```

### 失败响应

```json
{
  "ok": false,
  "server_recv_ms": 1780732144669,
  "server_time_iso": "2026-06-09T20:10:44.669Z",
  "request_id": "...",
  "error": {
    "code": "INVALID_PAYLOAD",
    "message": "..."
  }
}
```

- 第一阶段可以只让新 `/api/device/v1/*` 采用该 envelope；旧 `/sensor`、`/api/voice/turn`、`/api/commands/*` 保持兼容响应。
- raw PCM 成功响应不能改成 JSON；这类响应通过 headers 携带 `X-Server-Time-Ms`、`X-Request-Id` 等 metadata。

## 7. payload_type 规划

| payload_type | Body | Metadata | 刷新 device_status | 刷新 module_status | 历史表 | 进入 LLM context |
| --- | --- | --- | --- | --- | --- | --- |
| `sensor.bme690` | JSON envelope | body fields | 是 | `sensor.bme690` | `sensor_records` | 是 |
| `voice.turn` | raw PCM | headers | 是 | `voice.turn` | `voice_turns` | 是，使用诊断摘要 |
| `voice.prompt` | GET/empty body | headers | 是 | `voice.prompt` | 可选 `voice_prompt_cache_events` | 否，除非调试 |
| `command.capabilities` | JSON envelope 或兼容 JSON | body/header | 是 | `command.capabilities` | `device_capabilities` | 是，设备能力摘要 |
| `command.poll` | GET | headers/query | 是 | `command.poll` | `command_queue` 状态 | 否，除非调试 |
| `command.ack` | JSON envelope 或兼容 JSON | body/header | 是 | `command.ack` | `command_queue` | 是，命令执行结果摘要 |
| `time.ping` | JSON envelope 或兼容 JSON | body/header | 是 | `time.ping` | 可选 `device_latency_samples` | 是，延迟摘要 |
| `lcd.status` | JSON envelope | body | 是 | `lcd.status` | `lcd_status` | 是 |
| `lcd.event` | JSON envelope | body | 是 | `lcd.event` | 可选 `lcd_events` | 是，最近事件摘要 |
| `csi.motion` | JSON envelope | body | 是 | `csi.motion` | `csi_behavior_events` 或新表 | 是，轻量行为摘要 |
| `csi.features` | JSON envelope | body | 是 | `csi.features` | 可选特征表 | 可选，摘要化 |
| `csi.raw` | 二进制或分片 JSON | headers | 是 | `csi.raw` | 专用 raw 表/对象存储 | 否，默认不进 prompt |
| `agent.state` | JSON envelope | body | 是 | `agent.state` | `agent_state`/现有 agent 表 | 是，摘要化 |
| `emergency.event` | JSON envelope | body | 是 | `emergency.event` | `emergency_events` | 是，高优先级 |

## 8. 数据库规划

### A. `sensor_records` 扩展字段

建议新增字段：

- `schema_version`
- `device_type`
- `firmware_version`
- `request_seq`
- `time_synced`
- `payload_type`
- `sensor_id`
- `raw_json`
- `air_quality_score`
- `air_quality_level`
- `air_quality_confidence`
- `air_quality_algo_version`
- `air_quality_source`
- `gas_baseline_ohm`
- `gas_ratio`
- `gas_score`
- `humidity_score`

旧列继续保留，用于历史记录和旧前端兼容。

### B. `device_status`

建议字段：

- `device_id`
- `device_type`
- `firmware_version`
- `last_seen_ms`
- `last_seen_iso`
- `last_payload_type`
- `last_module_type`
- `last_esp_uptime_ms`
- `last_esp_time_ms`
- `time_synced`
- `reboot_count`
- `latest_upload_delay_ms`
- `avg_upload_delay_ms`
- `delay_sample_count`
- `updated_at`

### C. `device_module_status`

建议字段：

- `device_id`
- `module_type`
- `last_seen_ms`
- `last_seen_iso`
- `last_payload_type`
- `last_esp_uptime_ms`
- `latest_upload_delay_ms`
- `avg_upload_delay_ms`
- `delay_sample_count`
- `updated_at`

### D. 可选 `device_latency_samples`

- 当需要跨模块统计、p95、时间窗口聚合、异常延迟排查时引入。
- 第一阶段可先不引入，因为 `sensor_records` 已有 `upload_delay_ms`，`device_status` 和 `device_module_status` 可以保存滚动统计。
- 当 voice/command/time 都接入延迟统计后，再引入该表更合适。

### E. 索引

```sql
CREATE INDEX IF NOT EXISTS idx_sensor_records_device_recv_ms
ON sensor_records(device_id, server_recv_ms DESC);

CREATE INDEX IF NOT EXISTS idx_sensor_records_recv_ms
ON sensor_records(server_recv_ms DESC);

CREATE INDEX IF NOT EXISTS idx_sensor_records_payload_type_recv_ms
ON sensor_records(payload_type, server_recv_ms DESC);
```

另建议：

```sql
CREATE UNIQUE INDEX IF NOT EXISTS idx_device_status_device_id
ON device_status(device_id);

CREATE UNIQUE INDEX IF NOT EXISTS idx_device_module_status_device_module
ON device_module_status(device_id, module_type);
```

## 9. device_online / module_online 计划

- `device_online` 基于 `device_status.last_seen_ms`。
- `module_online` 基于 `device_module_status.last_seen_ms`。
- 任意有效 ESP 请求都刷新 `device_status`。
- BME 只刷新 `sensor.bme690` 模块状态。
- BME 停止上传不代表整机离线。
- 建议阈值：
  - `device_online_threshold_ms = 120000`
  - `sensor_online_threshold_ms = 30000`
- 状态接口返回字段：
  - `device_online`
  - `sensor_online`
  - `last_seen_age_ms`
  - `module_last_seen_age_ms`

## 10. 延迟统计计划

- 只有 server 计算 `upload_delay_ms`。
- 只有 `time_synced=true` 且 `esp_time_ms` 有效时才计算。
- 过滤 `null`、负数、大于 `60000ms`、`time_synced=false` 的样本。
- `latest_upload_delay_ms` 用于调试。
- `avg_upload_delay_ms` 用于 dashboard 和 LLM context。
- 第一阶段用最近 20 条有效 `sensor_records` 计算平均。
- 后续多模块统一后可引入 `device_latency_samples`，用于 p95、分模块趋势和异常诊断。

## 11. BME690 改造计划

- 新 ESP 固件改为 `POST /api/device/v1/ingest`。
- `payload_type=sensor.bme690`。
- `payload` 使用：
  - `sensor_id`
  - `temperature_c`
  - `humidity_percent`
  - `pressure_hpa`
  - `gas_resistance_ohm`
  - `air_quality_score`
  - `air_quality_level`
  - `air_quality_confidence`
  - `air_quality_algo_version`
  - `air_quality_source`
  - `gas_baseline_ohm`
  - `gas_ratio`
  - `gas_score`
  - `humidity_score`
- 不再上传旧扁平字段作为主字段。
- 后端新增 `sensorBme690Service` 或等价服务层，负责校验 payload、映射到数据库旧列和新增列。
- 空气质量由 ESP 本地计算，服务器接收入库。
- smoke test 要覆盖新 envelope、ESP 空气质量字段、legacy `/sensor` 兼容和旧前端查询不崩。

## 12. voice / command / time.ping 改造计划

- `voice.turn` raw PCM 保持 raw body，但增加统一 header metadata。
- `voice.prompt` 请求服务器缓存提示音，携带统一 header metadata。
- `command.capabilities` 和 `command.ack` 尽量使用 JSON envelope，同时保留旧 body 兼容解析。
- `command.poll` 作为 GET 使用 header metadata，query 中可继续保留 `device_id` 作为兼容。
- `time.ping` 作为 `payload_type=time.ping` 接入 `device_status`，用于证明整机仍在线。
- 所有有效请求刷新 `device_status`。
- 各类请求刷新对应 `device_module_status`：`voice.turn`、`voice.prompt`、`command.capabilities`、`command.poll`、`command.ack`、`time.ping`。

## 13. CSI/LCD 后续接入计划

- CSI/LCD 以后直接按 envelope v1 接入。
- 不再新增旧式扁平协议。
- `csi.motion` 只上传轻量结果，例如 motion/no_motion、confidence、zone、summary。
- `csi.raw` 后续单独做压缩、分片、批量上传设计，不和普通 JSON ingest 混在一起。
- `lcd.status` 和 `lcd.event` 直接作为 `payload_type`。
- CSI/LCD 不上传不代表整机离线。
- 当前 CSI/LCD 固件仍是 reserved / not supported，不能把后端协议基础误写成硬件已完成。

## 14. 前端迁移接口

本轮不改前端。后端 v1 API 已提供，前端后续可逐步迁移：

- 设备状态：`GET /api/device/v1/status?device_id=...`
- 模块状态：`GET /api/device/v1/modules/status?device_id=...`
- LLM/调试上下文：`GET /api/device/v1/context?device_id=...`
- 环境最新数据：`GET /api/device/v1/sensors/latest?device_id=...`
- dashboard 延迟显示优先 `avg_upload_delay_ms`，`latest_upload_delay_ms` 放详情。
- 空气质量显示必须标注：`ESP 本地 BME690 相对空气状态估算，不是国标 AQI`。
- 旧 `/sensor/latest`、`/sensor/history` 可继续服务旧前端，直到前端迁移完成。

## 15. 分阶段实施计划

### P0: 方案冻结与文档

- 目标：冻结 v1 envelope、空气质量、prompt cache、LLM context 和前端迁移建议。
- 修改范围：只改 docs。
- 不修改范围：固件业务代码、后端路由、数据库、测试脚本、`public/`、`managed_components`。
- 主要文件候选：本文件、`esp-air-quality-roadmap.md`、`voice-prompt-cache-roadmap.md`、`llm-prompt-context-roadmap.md`、`project-memory.md`。
- 接口变更：无运行时变更。
- 数据库变更：无。
- 测试项：只读审计、文档自检、`git diff --name-only` 范围确认。
- 验收标准：文档覆盖协议、废弃策略、数据库规划、阶段计划和风险；业务代码未变。
- 回滚策略：删除或回退新增 docs。
- 风险：方案过大，需要后续分批执行。

### P1: `/api/device/v1/ingest` + BME 新协议 + ESP 空气质量计算

2026-06-09 状态：已完成。

- 目标：让新 BME 上传走 envelope v1，ESP 上传本地空气质量。
- 修改范围：后端新增 ingest route/service/db migration/test；固件 BME client 改新 JSON；固件新增轻量空气质量计算模块。
- 不修改范围：前端、真实数据库手工改动、CSI/LCD 低层、`server_upload_bridge` 扩展。
- 主要文件候选：后端 `src/routes/deviceIngestRoutes.js`、`src/services/sensorBme690Service.js`、`src/db/sensorRecords.js`、`scripts/smoke-regression.js`；固件 `bme_server_client.c`、`bme_sensor_service.c`、`app_time_sync` 调用点、新 `bme_air_quality.*`。
- 接口变更：新增 `POST /api/device/v1/ingest`，保留 `/sensor`。
- 数据库变更：扩展 `sensor_records` 空气质量和 envelope 字段。
- 测试项：新 envelope 成功、字段非法 400、legacy `/sensor` 不坏、旧查询可读、ESP 构建。
- 验收标准：新协议记录入库且旧前端查询仍可得到映射字段。
- 回滚策略：固件切回 `/sensor`；后端保留新增列无害。
- 风险：ESP JSON buffer、flash/ram 增量、空气质量 baseline 初期置信度低。

### P2: `device_status` / `device_module_status` + 平均延迟

2026-06-09 状态：已完成。

- 目标：拆分整机在线和模块在线，并提供平均延迟。
- 修改范围：后端 status 表、status service、ingest/voice/command/time 更新点、状态查询 API。
- 不修改范围：前端展示、不改 raw PCM body。
- 主要文件候选：`src/db/deviceStatus.js`、`src/services/deviceStatusService.js`、`src/routes/deviceStatusRoutes.js`、`timeSync.js`、`voiceRoutes.js`、`commandRoutes.js`。
- 接口变更：新增 `GET /api/device/v1/status`、`GET /api/device/v1/modules/status`。
- 数据库变更：新增 `device_status`、`device_module_status`。
- 测试项：BME 停止不导致整机离线、任意有效请求刷新整机、延迟过滤、重启计数。
- 验收标准：dashboard 可用新 API 判断整机和模块状态，LLM context 可取状态。
- 回滚策略：保留旧 `/sensor/latest` 判断；status 表不影响旧接口。
- 风险：状态阈值需要根据实际上传周期调整。

### P3: voice prompt cache

2026-06-09 状态：已完成。

- 目标：服务器缓存固定唤醒提示音，命中时不请求 TTS/LLM/网关。
- 修改范围：后端缓存目录、metadata、prompt cache service、GET route；固件 prompt URL 可切换到 cache route。
- 不修改范围：`POST /api/voice/turn` 主链路、前端、真实数据库。
- 主要文件候选：`src/voice/promptCache.js`、`src/routes/voiceRoutes.js`、`cache/voice_prompts/`、固件 `wake_prompt_cache.c` endpoint。
- 接口变更：新增 `GET /api/voice/prompt-cache?prompt_key=wake_ack_zh`，保留 `GET /api/voice/prompt?wake=1&prompt_key=wake_ack_zh` 兼容。
- 数据库变更：第一阶段可无；metadata 可放 JSON 文件。
- 测试项：miss 生成、hit 不请求 TTS、stale 返回、格式匹配、voice turn 不受影响。
- 验收标准：缓存命中可直接返回 PCM/WAV，headers 完整。
- 回滚策略：回退到旧 `/api/voice/prompt` 每次 TTS 或 mock。
- 风险：音频格式不匹配、缓存文件损坏、上游 TTS 失败。

### P4: LLM prompt context

2026-06-09 状态：已完成。

- 目标：所有 LLM 路径统一通过设备上下文服务生成 prompt 片段。
- 修改范围：后端新增 `deviceContextService`、`llmPromptContextService`，接入 text/structured/voice turn LLM。
- 不修改范围：不把大量历史记录塞进 prompt，不改前端。
- 主要文件候选：`src/services/deviceContextService.js`、`src/llm/promptContext.js`、`llmTextRoutes.js`、`structuredLlmRoutes.js`、`voice/chain.js`。
- 接口变更：可新增 `GET /api/device/v1/context?device_id=...`。
- 数据库变更：通常依赖 P1/P2 表，无新增。
- 测试项：设备在线/离线、环境新鲜/过期、空气质量 low confidence、CSI/LCD unavailable、LLM 上游请求 body 检查。
- 验收标准：route 不再散查 `sensor_records`，prompt 明确数据新鲜度和非 AQI 属性。
- 回滚策略：保留旧 prompt 路径配置开关。
- 风险：prompt 过长、上下文缺失导致回答保守。

### P5: voice / command / time.ping metadata 统一

2026-06-09 状态：已完成。

- 目标：非 JSON 和 GET 请求都携带 v1 metadata，并刷新状态。
- 修改范围：固件 `server_comm_http` 公共 metadata；后端 header parser；voice/command/time status 更新。
- 不修改范围：raw PCM body 格式、旧 command 基本响应。
- 主要文件候选：固件 `server_comm_http.c`、`server_comm_types.h`、`system_server_client.c`、`server_voice_client.c`、`wake_prompt_cache.c`；后端 `voice/http.js`、`commandRoutes.js`、`timeSync.js`。
- 接口变更：header 增加 v1 metadata。
- 数据库变更：写入 `device_status`/`device_module_status`。
- 测试项：缺 metadata 兼容、带 metadata 刷新状态、time_synced=false 不算延迟。
- 验收标准：BME 以外请求也能证明整机在线。
- 回滚策略：后端 header parser 兼容旧请求；固件可关闭新 metadata。
- 风险：header buffer、ESP 字符串拼接、大小写兼容。

### P6: CSI/LCD 与前端 v1 API 迁移建议

2026-06-09 状态：部分完成。v1 status/modules/context/latest API 已完成；CSI/LCD 仍只保留 unavailable/reserved 状态，前端未迁移且未修改。

- 目标：给 CSI/LCD 后续接入统一 envelope，并让前端读取 v1 status/context。
- 修改范围：后端协议接口和文档；前端迁移在单独明确任务中做。
- 不修改范围：本阶段不实现 CSI raw 上传压缩，不声称硬件接入完成。
- 主要文件候选：`agentStateRoutes.js`、未来 `deviceIngestRoutes.js`、`docs/api.md`、后续 `public/*`。
- 接口变更：`lcd.status`、`lcd.event`、`csi.motion`、`csi.features`。
- 数据库变更：可能扩展 LCD/CSI 表或复用现有表。
- 测试项：CSI/LCD unavailable 进入 context、LCD status ingest、csi.motion ingest。
- 验收标准：CSI/LCD 不上传不影响整机在线判断。
- 回滚策略：保留现有 `/api/lcd/status`、`/api/csi/behavior`。
- 风险：前端迁移和后端兼容窗口需要协调。

### P7: 旧接口 legacy 清理与长期维护

2026-06-09 状态：已完成本轮要求。旧 `/sensor`、`/sensor/latest`、`/sensor/history` 已标记并保持 legacy 兼容，没有删除旧接口或历史数据。

- 目标：收敛旧协议，保留必要兼容，删除不再使用的重复路径。
- 修改范围：标记 `/sensor` legacy、文档和告警、可选清理 `server_upload_bridge`。
- 不修改范围：不突然删除仍在旧设备或旧前端使用的接口。
- 主要文件候选：`docs/api.md`、`sensorRoutes.js`、固件 CMake、`server_upload_bridge`。
- 接口变更：旧接口加 deprecation 文档和日志。
- 数据库变更：无强制变更。
- 测试项：旧前端、旧设备、迁移后设备、文档链接。
- 验收标准：新设备全部走 `/api/device/v1/ingest`，旧接口仅兼容。
- 回滚策略：继续保留 legacy route/component。
- 风险：还有未发现旧调用方，需要用日志观测一段时间。

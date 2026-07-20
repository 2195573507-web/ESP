# Dashboard v1 API 统一迁移计划

本文档规划 Dashboard 前端统一读取接口层。执行边界是新增面向前端的 `/api/dashboard/v1/*` 读取层，保留现有 legacy 接口，不删除、不破坏旧接口响应；后端与前端任务分开推进，但接口契约必须对齐。后续任何后端接口新增、删除、字段变更或错误格式变更，都必须同步更新 `docs/api.md`。

## 一、现状梳理

### 1. 当前 Dashboard 可能读取的 legacy 接口

当前旧 Dashboard 或历史版本可能读取以下接口：

| legacy 接口 | 当前定位 | 现状备注 |
| --- | --- | --- |
| `GET /sensor/latest` | 读取最新传感器数据 | `docs/api.md` 已记录为前端兼容读取最新数据接口；当前 `public/app.js` 直接 `fetch("/sensor/latest")`。 |
| `GET /sensor/history` | 读取传感器历史数据 | `docs/api.md` 已记录；当前前端代码里历史曲线仍可能保留 mock/占位逻辑，迁移前必须再次审计调用点。 |
| `GET /asr/latest` | 读取最新 ASR 文本 | `docs/api.md` 已记录；当前 `public/app.js` 直接 `fetch("/asr/latest")`。 |
| `GET /llm/latest` | 读取最新 LLM 文本 | `docs/api.md` 已记录；当前 `public/app.js` 直接 `fetch("/llm/latest")`。 |
| `GET /api/time/status` | 读取时间同步状态 | `docs/api.md` 标注为前端当前使用；当前前端调用点需在 F0 阶段核实，防止旧版本或分支残留被漏迁。 |

现有 `server.js` 负责挂载静态 `public/`、`/dashboard`、legacy record/sensor routes、`/api/time/*` 以及 `/api/device/v1/*`。后续新增 Dashboard v1 时，应保持这些旧挂载可访问，并避免把旧 Dashboard 的响应结构改成新 envelope。

### 2. 当前 `/api/device/v1/*` 的职责边界

`/api/device/v1/*` 当前主要服务 ESP 设备侧协议、设备状态和设备上下文，不应成为 Dashboard 前端唯一读取层：

| 接口 | 职责边界 |
| --- | --- |
| `POST /api/device/v1/ingest` | ESP 设备上报统一 envelope，当前接入 `payload_type=sensor.bme690`。 |
| `GET /api/device/v1/status` | 读取整机状态，偏设备协议/调试视角。 |
| `GET /api/device/v1/modules/status` | 读取模块状态，偏设备协议/调试视角。 |
| `GET /api/device/v1/context` | 读取 `deviceContextService` 输出，供 LLM prompt、调试和后续读取层复用。 |
| `GET /api/device/v1/sensors/latest` | 读取最新 BME690 记录，返回 v1 字段、legacy 映射字段和原始/metadata 信息。 |

Dashboard 可以在后端内部复用 device v1 service、status service 或 sensor mapper，但前端不应直接绑定 ESP ingest protocol 的 envelope、字段命名和设备上下文字段。Dashboard v1 应稳定输出“页面需要什么”，device v1 保持“设备协议是什么”。

### 3. 当前 `docs/api.md` 中与前端读取相关的位置

当前 `docs/api.md` 已经包含以下相关位置，后续 P4 必须在这些位置基础上新增 Dashboard v1 章节并标注 legacy 保留策略：

| 位置 | 内容 | 后续处理 |
| --- | --- | --- |
| 文档开头通用说明 | 已声明接口字段变更必须先更新 `docs/api.md`，再改后端或调用方代码。 | 保留，并追加 Dashboard v1 也适用。 |
| “统一设备协议 v1”章节 | 记录 `/api/device/v1/ingest`、`/api/device/v1/status`、`/api/device/v1/modules/status`、`/api/device/v1/context`、`/api/device/v1/sensors/latest`。 | 保持设备侧定位，不写成前端主入口。 |
| “Legacy 传感器兼容接口”章节 | 记录 `POST /sensor` 以及 legacy sensor schema。 | 标注保留兼容，不作为新前端主入口。 |
| “前端读取最新数据接口”章节 | 记录 `GET /sensor/latest`、`GET /asr/latest`、`GET /llm/latest`。 | 标注 legacy compatibility，新增迁移指向。 |
| “前端读取历史数据接口”章节 | 记录 `GET /sensor/history`。 | 标注 legacy compatibility，新增迁移指向。 |
| “状态/健康检查接口”章节 | 记录 `GET /api/time/status`。 | 标注 legacy compatibility，新增 Dashboard v1 time/status。 |

### 4. `gas_resistance` 与空气质量显示的现状风险

当前旧前端存在一个“空气/气体”合并显示槽位：历史点归一化中使用 `aqi ?? gas`，实时传感器归一化中优先尝试 `aqi` 或 `air_quality`，再回退到 `gas_resistance`，页面默认文案也倾向“空气质量 (AQI)”。这会带来两个迁移风险：

- 如果后端新增了数字型 `aqi` 或 `air_quality` 字段，前端可能用空气质量覆盖原本的气体阻值显示。
- 当前 legacy 响应已有 `air_quality_score`、`air_quality_level`、`air_quality_confidence`、`air_quality_source`，但旧前端不一定按这些字段独立展示。
- `air_quality` 可能是对象，不是数值；前端不能直接把整个对象当作曲线数值。
- 新 Dashboard v1 必须同时提供 `gas_resistance` 和空气质量字段，前端最终也必须同时展示“气体阻值”和“空气质量”，不能用一个字段替代另一个字段。

迁移注意点：`gas_resistance` 是 BME690 气体阻值，单位为 `Ω`；空气质量是基于 BME690 的相对估算，不是国标 AQI，不代表 PM2.5、PM10 或 CO2。

## 二、目标架构

### 1. 路由职责分层

| 路由族 | 目标职责 | 消费方 | 保留策略 |
| --- | --- | --- | --- |
| `/api/device/v1/*` | 设备侧上报、设备状态、设备上下文、设备协议调试。 | ESP 设备、后端服务、调试脚本。 | 保留，继续按设备协议演进。 |
| `/api/dashboard/v1/*` | Dashboard 前端统一读取层，稳定输出页面需要的数据形状。 | Dashboard 前端。 | 新增，迁移后作为前端唯一主读取入口。 |
| `/sensor/*`、`/asr/*`、`/llm/*` | legacy 兼容层，保持旧设备、旧脚本、旧 Dashboard 可用。 | 旧 Dashboard、旧脚本、旧设备。 | 保留，不立即删除，不破坏旧响应结构。 |

### 2. 前端不直接依赖 ESP ingest 协议的原因

- ESP ingest 协议反映设备上报事实，字段会随固件 payload、模块类型和设备状态扩展而变化；Dashboard 页面需要的是稳定展示模型。
- 设备侧接口可能包含 raw/metadata/debug 字段，前端直接依赖会放大协议变更影响范围。
- Dashboard 需要聚合传感器、ASR、LLM、时间同步、整机状态和模块状态；这些数据来自多个表和 service，不应由前端拼接设备协议细节。
- 前端统一读取层可以集中处理空数据、默认值、错误码、字段命名、单位和兼容映射。

### 3. 旧接口保留但不作为新前端主入口的原因

- 保留 legacy 接口是回滚路径，也是旧脚本、旧 Dashboard 或旧设备继续可用的兼容层。
- legacy 成功响应已经被现有前端/脚本依赖，不能为了新 envelope 强行改成 `{ ok, server_time_ms, data, error }`。
- 新功能、聚合字段和展示字段应进入 `/api/dashboard/v1/*`，避免继续把旧接口做成越来越宽的混合接口。

### 4. Dashboard v1 统一响应结构

Dashboard v1 新接口统一成功格式：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {},
  "error": null
}
```

Dashboard v1 新接口统一失败格式：

```json
{
  "ok": false,
  "server_time_ms": 1780000000000,
  "data": null,
  "error": {
    "code": "ERROR_CODE",
    "message": "readable error"
  }
}
```

空数据规则建议：

- `latest` 类接口：`ok=true`，`data=null` 或 `data={}` 二选一，必须在 `docs/api.md` 固定；推荐 `data=null` 表示暂无记录。
- `history` 类接口：`ok=true`，`data=[]`。
- `status` 类接口：`ok=true`，`data` 返回状态对象；未知状态字段使用 `null` 或 `false`，不得省略核心字段。
- 失败响应：HTTP 状态码和 `error.code` 同步表达错误类型，`message` 面向人类可读，但前端判断只依赖 `code`。

## 三、后端任务计划

### P0：只读审计

- 审计 `server.js` 的 route 挂载顺序，确认新增 Dashboard v1 不影响静态 `public/`、`/dashboard`、legacy routes 和 `/api/device/v1/*`。
- 审计 `src/routes` 中 `sensorRoutes.js`、`recordRoutes.js`、`deviceRoutes.js`、未来可能相关的 time sync router。
- 审计 `src/services` 中 device status、device context、BME690 sensor mapper 与空气质量 mapper。
- 审计 `public/` 当前所有 `fetch`、`axios`、`XMLHttpRequest`、轮询和动态 URL 生成点，只读列出调用接口和页面显示字段。
- 确认 legacy 接口字段、数据库字段、v1 字段映射，特别是 `sensor_records.temperature/humidity/pressure/gas_resistance` 与 v1 `payload.temperature_c/humidity_percent/pressure_hpa/gas_resistance_ohm` 的对应关系。
- 确认 `gas_resistance` 是否仍从 `sensor_records` 返回；当前 `sensorRoutes.js` 与 `deviceRoutes.js` 都保留 `gas_resistance` 映射，P0 需要以实时代码和测试数据再次验证。
- 确认 `air_quality_score`、`air_quality_level`、`air_quality_confidence`、`air_quality_source` 的来源优先级：拆分列、`air_quality_json`、server fallback。
- 确认 `online`、`device_online`、`sensor_online`、`latest_upload_delay_ms`、`avg_upload_delay_ms`、`delay_sample_count`、`time_sync` 的来源和缺省值。

### P1：新增 Dashboard v1 后端接口

建议新增独立路由模块，例如 `src/routes/dashboardRoutes.js`，由 `server.js` 挂载；必要时新增 dashboard service/mapper，复用现有 db/service，不直接复制 device v1 的响应形状。

计划新增接口：

- `GET /api/dashboard/v1/sensors/latest`
  - 返回最新传感器展示数据。
  - 必须包含 `temperature`、`humidity`、`pressure`、`gas_resistance`。
  - 必须同时包含空气质量字段：`air_quality_score`、`air_quality_level`、`air_quality_confidence`、`air_quality_source`，可附带 `air_quality` 对象。
  - 必须包含在线和延迟字段：`online`、`device_online`、`sensor_online`、`latest_upload_delay_ms`、`avg_upload_delay_ms`、`delay_sample_count`、`time_sync`。

- `GET /api/dashboard/v1/sensors/history`
  - 返回历史传感器数组，保留 `gas_resistance`，同时输出空气质量字段。
  - 支持 `limit`，建议沿用 legacy 上限 `500`，默认 `50`。
  - 响应按时间从旧到新，便于图表直接消费。

- `GET /api/dashboard/v1/asr/latest`
  - 返回最新 ASR 记录，至少包含 `id`、`timestamp`、`text`。

- `GET /api/dashboard/v1/llm/latest`
  - 返回最新 LLM 记录，至少包含 `id`、`timestamp`、`prompt`、`response`。

- `GET /api/dashboard/v1/time/status`
  - 返回 Dashboard 需要的时间同步状态，兼容 legacy `latest_ping` 信息。
  - 必须纳入统一 envelope 的 `data`。

- `GET /api/dashboard/v1/device/status`
  - 返回整机在线状态、最后上报时间、上传延迟、时间同步和必要设备标识。
  - 可复用 `deviceStatusService.readDeviceStatus()`。

- `GET /api/dashboard/v1/modules/status`
  - 返回模块在线状态列表。
  - BME 模块离线不等于整机离线，前端应分别显示。

- `GET /api/dashboard/v1/overview`
  - 聚合 Dashboard 首屏所需数据，建议包含 `sensor_latest`、`asr_latest`、`llm_latest`、`time_status`、`device_status`、`modules_status`。
  - 聚合接口用于首屏减少请求；单项接口仍保留，便于局部刷新和故障隔离。

P1 约束：

- 不删除、不改坏旧接口。
- 不修改真实数据库数据。
- 优先不修改数据库 schema；若发现字段缺失，先记录迁移需求和 `docs/api.md` 变更点。
- 新接口只承诺 Dashboard v1 契约，不把 ESP raw envelope 暴露为前端唯一契约。

### P2：统一响应结构

- Dashboard v1 所有新接口统一使用 `{ ok, server_time_ms, data, error }`。
- legacy 接口保持原响应结构，例如 `/sensor/latest` 无数据仍按旧约定返回 `{}`，`/sensor/history` 仍返回数组。
- 统一错误码命名，建议前缀：
  - `DASHBOARD_SENSOR_READ_FAILED`
  - `DASHBOARD_HISTORY_READ_FAILED`
  - `DASHBOARD_ASR_READ_FAILED`
  - `DASHBOARD_LLM_READ_FAILED`
  - `DASHBOARD_TIME_STATUS_FAILED`
  - `DASHBOARD_DEVICE_STATUS_FAILED`
  - `DASHBOARD_MODULE_STATUS_FAILED`
  - `DASHBOARD_OVERVIEW_FAILED`
- 统一空数据返回规则，并在 `docs/api.md` 固化。
- 统一字段缺省值，避免前端为同一字段写多套 fallback。

### P3：测试

- 更新或新增 smoke 测试，覆盖新旧接口并存：
  - legacy `/sensor/latest`、`/sensor/history`、`/asr/latest`、`/llm/latest`、`/api/time/status` 仍可访问。
  - 新 `/api/dashboard/v1/*` 全部可访问。
- 验证新接口字段完整性：
  - `temperature`、`humidity`、`pressure`、`gas_resistance`。
  - `air_quality_score`、`air_quality_level`、`air_quality_confidence`、`air_quality_source`。
  - `online`、`device_online`、`sensor_online`。
  - `latest_upload_delay_ms`、`avg_upload_delay_ms`、`delay_sample_count`。
  - `time_sync`。
- 验证 `gas_resistance` 与 `air_quality` 同时存在，且前端 API client 不会互相覆盖。
- 验证旧 Dashboard legacy 接口不被破坏，包括旧响应 shape、空数据规则和错误响应。
- 使用临时数据库或 smoke regression 自带隔离数据库，不能修改真实 `db/database.db`。
- 建议执行：
  - `npm test`
  - 或 `npm run test:smoke`
  - `git diff --check`
  - `git diff -- public db/database.db`，确认不应变更的文件无差异。

### P4：文档更新

- 后端新增或修改接口时必须同步更新 `docs/api.md`。
- 在 `docs/api.md` 中新增 “Dashboard v1 前端统一读取接口” 章节，记录：
  - `/api/dashboard/v1/overview`
  - `/api/dashboard/v1/sensors/latest`
  - `/api/dashboard/v1/sensors/history`
  - `/api/dashboard/v1/asr/latest`
  - `/api/dashboard/v1/llm/latest`
  - `/api/dashboard/v1/time/status`
  - `/api/dashboard/v1/device/status`
  - `/api/dashboard/v1/modules/status`
- 在 legacy 章节标注：
  - `/sensor/*`、`/asr/*`、`/llm/*`、`/api/time/status` 保留兼容。
  - 前端迁移后不再作为 Dashboard 主读取入口。
- 在 device v1 章节标注：
  - `/api/device/v1/*` 面向 ESP 设备和设备状态，不作为 Dashboard 前端唯一读取层。
- 每次后端接口字段变更、错误码变更、空数据规则变更，都必须先更新 `docs/api.md`，再更新实现和测试。

## 四、前端任务计划

### F0：只读审计

- 找出 `public/` 中所有 `fetch`、`axios`、`XMLHttpRequest`、`setInterval` 轮询和动态 URL 生成点。
- 列出现有调用接口与页面显示字段：
  - `GET /sensor/latest`
  - `GET /sensor/history`
  - `GET /asr/latest`
  - `GET /llm/latest`
  - `GET /api/time/status`
- 特别确认 `gas_resistance` 显示逻辑是否被 `air_quality`、`aqi`、`air` 或单一图表 slot 替换。
- 记录当前 mock fallback 逻辑，迁移后决定是否保留、如何在统一 API client 中处理。

### F1：新增前端 API client 层

- 建议在 `public/` 中新增统一 Dashboard API client 模块，例如 `dashboardApi`。
- 所有前端页面通过 `dashboardApi` 读取 `/api/dashboard/v1/*`。
- 页面组件、渲染函数和事件处理函数不再散落硬编码 URL。
- API client 负责：
  - 拼接 URL 和 query。
  - 解析 `{ ok, server_time_ms, data, error }`。
  - 统一处理 HTTP 错误、后端错误码、空数据。
  - 统一把 null/undefined/空字符串交给展示层显示 `--`。

### F2：迁移数据读取

| 旧读取 | 新读取 | 迁移说明 |
| --- | --- | --- |
| `/sensor/latest` | `/api/dashboard/v1/sensors/latest` 或 `/api/dashboard/v1/overview` | 单项刷新用 latest，首屏聚合用 overview。 |
| `/sensor/history` | `/api/dashboard/v1/sensors/history` | 保持 limit 语义和时间排序。 |
| `/asr/latest` | `/api/dashboard/v1/asr/latest` | 从新 envelope 的 `data` 读取。 |
| `/llm/latest` | `/api/dashboard/v1/llm/latest` | 从新 envelope 的 `data` 读取。 |
| `/api/time/status` | `/api/dashboard/v1/time/status` | 从新 envelope 的 `data` 读取。 |

迁移完成后，Dashboard 前端不得再直接调用 `/sensor/latest`、`/sensor/history`、`/asr/latest`、`/llm/latest`、`/api/time/status`。

### F3：显示字段修复

- 同时显示 `gas_resistance` 和空气质量，不允许一个字段覆盖另一个字段。
- `gas_resistance` 显示为“气体阻值”，单位 `Ω`。
- 空气质量显示：
  - `score`: 来自 `air_quality_score`。
  - `level`: 来自 `air_quality_level`。
  - `confidence`: 来自 `air_quality_confidence`。
  - `source`: 来自 `air_quality_source`。
- `null`、`undefined`、空字符串、不可解析数值统一显示 `--`。
- 图表如需同时展示气体阻值和空气质量，应使用两条独立 series 或明确的切换控制，不能继续用 `aqi ?? gas` 这种覆盖关系。
- 页面文案避免把 BME690 相对估算称为国标 AQI；可写“空气质量分数/等级”。

### F4：前端验证

- 页面首屏加载成功，overview 或并发单项请求无阻塞。
- 传感器实时刷新正常。
- 历史曲线正常，数据按时间递增。
- ASR 最新文本显示正常。
- LLM 最新文本显示正常。
- 时间同步状态显示正常。
- 设备在线和模块在线状态分别显示正常。
- 空气质量与气体阻值同时显示，单位和空值显示正确。
- 浏览器控制台无未处理异常、无反复降级到 mock 的异常日志。

## 五、接口契约对齐表

| legacy 接口 | 新 dashboard v1 接口 | 前端使用场景 | 返回字段 | 是否保留 legacy | 是否需要修改前端 | 是否需要更新 `docs/api.md` |
| --- | --- | --- | --- | --- | --- | --- |
| `GET /sensor/latest` | `GET /api/dashboard/v1/sensors/latest` | 传感器实时卡片、状态摘要、局部刷新 | `temperature`、`humidity`、`pressure`、`gas_resistance`、`air_quality_score`、`air_quality_level`、`air_quality_confidence`、`air_quality_source`、`online`、`device_online`、`sensor_online`、`latest_upload_delay_ms`、`avg_upload_delay_ms`、`delay_sample_count`、`time_sync` | 是 | 是 | 是 |
| `GET /sensor/history` | `GET /api/dashboard/v1/sensors/history` | 历史曲线、趋势分析 | `id`、`timestamp`、`temperature`、`humidity`、`pressure`、`gas_resistance`、空气质量字段、上传延迟字段 | 是 | 是 | 是 |
| `GET /asr/latest` | `GET /api/dashboard/v1/asr/latest` | ASR 最新识别文本 | `id`、`timestamp`、`text` | 是 | 是 | 是 |
| `GET /llm/latest` | `GET /api/dashboard/v1/llm/latest` | LLM 最新回复 | `id`、`timestamp`、`prompt`、`response` | 是 | 是 | 是 |
| `GET /api/time/status` | `GET /api/dashboard/v1/time/status` | 时间同步状态 | `server_time_ms`、`server_time_iso`、`latest_ping`、必要时补充 `time_sync` | 是 | 是 | 是 |
| 无直接 legacy 单项 | `GET /api/dashboard/v1/device/status` | 整机在线状态、最后上报、上传延迟 | `online`、`device_online`、`device_id`、`last_seen_ms`、`last_seen_iso`、`last_seen_age_ms`、`time_synced`、`latest_upload_delay_ms`、`avg_upload_delay_ms`、`delay_sample_count` | 不适用 | 是 | 是 |
| 无直接 legacy 单项 | `GET /api/dashboard/v1/modules/status` | 模块在线状态 | `modules[]`，每项含 `module_type`、`online`、`module_online`、`last_seen_age_ms`、上传延迟字段 | 不适用 | 是 | 是 |
| 多个 legacy 并发读取 | `GET /api/dashboard/v1/overview` | 首屏聚合加载 | `sensor_latest`、`sensor_history` 可选、`asr_latest`、`llm_latest`、`time_status`、`device_status`、`modules_status` | legacy 分别保留 | 是 | 是 |

## 六、字段映射表

| 字段 | 中文显示 | 来源/映射 | 前端显示规则 |
| --- | --- | --- | --- |
| `temperature` | 温度 | legacy `sensor_records.temperature`；device v1 payload `temperature_c` 映射入库。 | 数字显示，空值 `--`。 |
| `humidity` | 湿度 | legacy `sensor_records.humidity`；device v1 payload `humidity_percent` 映射入库。 | 数字显示，空值 `--`。 |
| `pressure` | 气压 | legacy `sensor_records.pressure`；device v1 payload `pressure_hpa` 映射入库。 | 数字显示，空值 `--`。 |
| `gas_resistance` | 气体阻值 | legacy `sensor_records.gas_resistance`；device v1 payload `gas_resistance_ohm` 映射入库。 | 单位 `Ω`，必须独立显示，不能被空气质量覆盖。 |
| `air_quality_score` | 空气质量分数 | `sensor_records.air_quality_score` 或 `air_quality_json.air_quality_score`。 | 数字或 `--`，不称为国标 AQI。 |
| `air_quality_level` | 空气质量等级 | `sensor_records.air_quality_level` 或 `air_quality_json.air_quality_level`。 | 文本或 `--`。 |
| `air_quality_confidence` | 置信度 | `sensor_records.air_quality_confidence` 或 `air_quality_json.air_quality_confidence`。 | 文本或 `--`。 |
| `air_quality_source` | 来源 | `sensor_records.air_quality_source` 或 `air_quality_json.air_quality_source`。 | 文本或 `--`，例如 `esp`、`server_fallback`。 |
| `online` | 在线状态 | Dashboard 聚合状态，可来自 device status。 | 布尔值，未知时显示 `--` 或“未知”。 |
| `device_online` | 整机在线状态 | `device_status` 映射。 | 与模块在线分开显示。 |
| `sensor_online` | 传感器模块在线状态 | `device_module_status` 中 `sensor.bme690` 映射。 | 不等同于整机在线。 |
| `latest_upload_delay_ms` | 最近上传延迟 | `device_status.latest_upload_delay_ms` 或最新 sensor upload delay。 | 毫秒，空值 `--`。 |
| `avg_upload_delay_ms` | 平均上传延迟 | `device_status.avg_upload_delay_ms`。 | 毫秒，空值 `--`。 |
| `delay_sample_count` | 延迟样本数 | `device_status.delay_sample_count`。 | 整数，空值按 `0` 或 `--` 需在契约固定。 |
| `time_sync` | 时间同步 | legacy `/sensor/latest` 中嵌入的 time status，或 Dashboard v1 `time/status` 聚合。 | 显示同步状态、服务器时间和 latest ping，空值 `--`。 |

## 七、验收标准

- 旧接口全部仍可访问：
  - `GET /sensor/latest`
  - `GET /sensor/history`
  - `GET /asr/latest`
  - `GET /llm/latest`
  - `GET /api/time/status`
- 新 `/api/dashboard/v1/*` 接口全部可访问：
  - `GET /api/dashboard/v1/overview`
  - `GET /api/dashboard/v1/sensors/latest`
  - `GET /api/dashboard/v1/sensors/history`
  - `GET /api/dashboard/v1/asr/latest`
  - `GET /api/dashboard/v1/llm/latest`
  - `GET /api/dashboard/v1/time/status`
  - `GET /api/dashboard/v1/device/status`
  - `GET /api/dashboard/v1/modules/status`
- 前端修改后一律读取新接口。
- 前端不再直接调用 `/sensor/latest`、`/sensor/history`、`/asr/latest`、`/llm/latest`、`/api/time/status`。
- `gas_resistance` 和空气质量同时显示。
- `gas_resistance` 使用 `Ω`，空气质量显示 score/level/confidence/source。
- `docs/api.md` 已同步记录所有新增后端接口、字段、错误格式、空数据规则和 legacy 保留策略。
- `npm test` 或现有 `npm run test:smoke` / `scripts/smoke-regression.js` 通过。
- 验证不修改真实数据库：`git diff -- db/database.db` 无业务数据差异。
- 验证前端边界：后端阶段不修改 `public/`；前端阶段修改 `public/` 时必须只迁移读取层和显示逻辑，不顺手改后端契约。
- 完成后输出 `git diff --stat`。

## 八、风险与回滚

| 风险 | 影响 | 缓解与回滚 |
| --- | --- | --- |
| 新 Dashboard v1 字段遗漏 | 前端显示缺项或回退 mock | P0 字段审计和 P3 字段完整性测试；legacy 接口保留可快速回滚前端读取。 |
| 新 envelope 误套到 legacy 接口 | 旧 Dashboard/脚本解析失败 | P2 明确 legacy 保持原响应结构；smoke 同时验证新旧接口。 |
| 前端仍散落硬编码 legacy URL | 迁移不完整，后续接口变更继续影响页面 | F1 新增统一 `dashboardApi`，F2 后用全文搜索验证旧 URL 不再被前端直接调用。 |
| 空气质量覆盖气体阻值 | BME690 原始气体阻值不可见 | F3 拆分显示字段，测试同时断言 `gas_resistance` 和空气质量字段存在并渲染。 |
| 直接依赖 `/api/device/v1/*` | 前端被设备协议变更牵连 | 后端新增 Dashboard v1 mapper，前端只依赖 Dashboard v1 契约。 |
| 真实数据库被测试污染 | 本地数据丢失或 diff 噪声 | smoke 使用临时数据库；执行前后检查 `git diff -- db/database.db`。 |
| 必须新增字段或 schema | 迁移风险扩大 | 优先不改 schema；若必须新增字段，先更新 `docs/api.md` 和迁移说明，再做可重复迁移和回滚说明。 |
| 新接口上线异常 | Dashboard 无法读取新层 | 旧接口保留作为回滚路径；前端 `dashboardApi` 可临时切回 legacy mapping。 |

回滚优先级：

1. 保持后端新接口代码不影响 legacy，前端可切回 legacy URL。
2. 若新 mapper 有问题，只回滚 `/api/dashboard/v1/*` 路由挂载或 mapper，不动 legacy routes。
3. 若涉及 schema，先停止前端迁移，恢复使用 legacy 接口，并按 `docs/api.md` 迁移说明处理数据库兼容。

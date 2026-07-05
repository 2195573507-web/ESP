# C5-S3-Server-Dashboard 数据链路重构计划

本文档基于 2026-06-10 对 `/Users/zhiqin/ESP-111` 的审计结果编写。2026-06-10 更新：协议公共组件改为每个固件工程内置，不再依赖顶层 `shared_components` 作为外部公共组件。

## 目标边界

目标链路：

1. C51/C52 -> S3：使用轻量短字段协议，C5 只负责采集、基础状态、语音 PCM 上传、命令回执和预留 CSI，不构造 Dashboard 或 Server 侧复杂 JSON。
2. S3 -> ESP-server：S3 维护节点状态表，把 C5 短字段转换为标准设备模型，并上传标准化网关/设备/历史/语音/命令数据。
3. ESP-server -> Dashboard v1：后端聚合并返回前端可直接读取的 `gateway`、`devices`、`home_summary`、`history`、`recent_voice_events`、`recent_commands`。
4. 前端暂时不改。房间电器状态 `air_conditioner`、`fan`、`light`、`tv`、`curtain` 暂时使用固定假数据，并显式标注 `source="mock"` 或 `mock=true`。

硬性约束：

- 不修改 `ESP-server/public` 前端。
- 不删除旧接口，不改变 legacy 成功响应形状。
- 不破坏 `ESP-server` smoke regression。
- 不修改任何 `managed_components`。
- 不删除、不移动、不清理 `archive`。
- 不把 Dashboard 嵌套 JSON 或 Server 完整 envelope 下放到 C5。
- 协议公共头以后同时维护在 `ESPC51/components/esp111_protocol_common/include/esp111_protocol_common.h`、`ESPC52/components/esp111_protocol_common/include/esp111_protocol_common.h`、`ESPS3/components/esp111_protocol_common/include/esp111_protocol_common.h`，三份内容必须保持一致；`shared_components/esp111_protocol_common` 仅保留归档/参考。

## 当前真实代码现状审计

### firmware-local esp111_protocol_common

已审计文件：

- `ESPC51/components/esp111_protocol_common/include/esp111_protocol_common.h`
- `ESPC52/components/esp111_protocol_common/include/esp111_protocol_common.h`
- `ESPS3/components/esp111_protocol_common/include/esp111_protocol_common.h`

当前现状：

- 共享头已经定义 C5 <-> S3 本地短字段：`id`、`t`、`u`、`v`、`cid`、`c`、`a`、`ok`、`e`、`cmds`。
- 当前本地类型仍以字符串为主：`reg`、`hb`、`st`、`bme`、`csi`。
- 已定义本地路由：`/local/v1/health`、`/local/v1/register`、`/local/v1/heartbeat`、`/local/v1/status`、`/local/v1/sensor`、`/local/v1/csi/result`、`/local/v1/voice/turn`、`/local/v1/commands/pending`、`/local/v1/commands/*/ack`。
- 已定义 S3 -> Server 路由：`/api/device/v1/ingest`、`/api/voice/turn`、`/api/commands/pending`、`/api/commands/*/ack`。
- 头文件注释已经明确：C5 <-> S3 使用轻量 JSON；S3 <-> Server 使用完整 v1 JSON；C5 不直接构造 Server 完整 envelope。
- 缺口：还没有面向 Dashboard 标准模型的短字段枚举、S3 状态快照上传 payload 类型、标准 dashboard snapshot 字段常量。

### ESPC51

已审计文件：

- `ESPC51/components/Middlewares/CMakeLists.txt`
- `ESPC51/components/Middlewares/terminal_config/terminal_config.h`
- `ESPC51/components/Middlewares/sensor_domain/bme690/server_client/bme_server_client.c`
- `ESPC51/components/Middlewares/command_domain/system_command/system_server_client.c`
- `ESPC51/components/Middlewares/server_voice/server_voice_client.c`
- `ESPC51/components/Middlewares/sensor_domain/csi_placeholder/*`

当前现状：

- 当前编译集包含 Wi-Fi、app orchestrator、Mic、server voice、wake、display placeholder、speaker、voice domain、runtime、device protocol、BME690、CSI placeholder、system command。
- C51 默认身份来自 `terminal_config.h`：`device_id="sensair_shuttle_01"`，`local_id=1`，默认房间 `unassigned`。
- BME690 当前已经向 S3 `POST /local/v1/sensor` 发送短 JSON：

```json
{
  "id": 1,
  "t": "bme",
  "u": 123456,
  "v": [29.57, 48.2, 1008.6, 35164, 72, 41000, 0.8571, 70, 80, 1, 1, 128]
}
```

- `v[]` 当前位置含义依次为：temperature、humidity、pressure、gas resistance、air quality score、gas baseline、gas ratio、gas score、humidity score、baseline_ready、warmup_done、sample_count。
- `system_server_client.c` 当前向 S3 发送 `reg`、`hb`、`st`，并轮询 `/local/v1/commands/pending?id=<local_id>&limit=1`。
- 命令 ack 当前 body 是短字段：`id`、`cid`、`ok`、`e`，POST 到 `/local/v1/commands/<command_id>/ack`。
- `server_voice_client.c` 当前把 Mic PCM 上传到 S3 `/local/v1/voice/turn`，读取 S3 返回 PCM 播放；C5 不做 ASR/LLM/TTS。
- CSI 仍是 placeholder，不代表真实 CSI 算法已接入。
- 缺口：`t` 仍是字符串；health/register/status 分散；没有统一的 numeric enum 文档与代码常量；C5 未显式发送标准化所需的 Wi-Fi RSSI 等最小状态字段。

### ESPC52

已审计文件：

- `ESPC52/components/Middlewares/CMakeLists.txt`
- `ESPC52/components/Middlewares/server_comm/CMakeLists.txt`

当前现状：

- C52 与 C51 使用同一套 Middlewares 编译集。
- C52 的 `server_comm/CMakeLists.txt` 通过编译定义覆盖默认身份：`device_id="sensair_shuttle_02"`，`local_id=2`，`alias="SensaiShuttle02"`。
- 缺口与 C51 一致：需要跟随 shared protocol 和共用 Middlewares 更新，不应复制出另一套协议逻辑。

### ESPS3

已审计文件：

- `ESPS3/sdkconfig`
- `ESPS3/main/idf_component.yml`
- `ESPS3/components/Middlewares/CMakeLists.txt`
- `ESPS3/components/Middlewares/local_http_server/local_http_server.c`
- `ESPS3/components/Middlewares/protocol_adapter/protocol_adapter.c`
- `ESPS3/components/Middlewares/sensor_aggregator/sensor_aggregator.c`
- `ESPS3/components/Middlewares/server_client/server_client.c`
- `ESPS3/components/Middlewares/child_registry/child_registry.c`
- `ESPS3/components/Middlewares/voice_proxy/voice_proxy.c`
- `ESPS3/components/Middlewares/command_router/command_router.c`
- `ESPS3/components/Middlewares/csi_placeholder_gateway/*`

当前现状：

- `sdkconfig` 目标为 `esp32s3`，flash 32MB，PSRAM 已启用。
- `main/idf_component.yml` 只声明 IDF `>=5.5.0`，未引入 esp-sr。
- Middlewares 当前包含 `gateway_config`、`gateway_wifi`、`child_registry`、`protocol_adapter`、`server_client`、`sensor_aggregator`、`voice_proxy`、`command_router`、`offline_policy`、`csi_placeholder_gateway`、`local_http_server`、`gateway_orchestrator`。
- `local_http_server.c` 暴露 `/local/v1` 路由，`/api/*` 不在本地 server 暴露。
- `protocol_adapter.c` 当前能把 `id/t/u/v` 短包映射为完整 server ingest envelope：
  - local id 1 -> `sensair_shuttle_01`
  - local id 2 -> `sensair_shuttle_02`
  - `bme` -> `payload_type="sensor.bme690"`
  - `room_id` 当前固定为 `unassigned`
  - `air_quality_level` 当前写为 `"unknown"`，`air_quality_confidence` 当前写为 `"s3_mapped"`
- `sensor_aggregator.c` 当前只把 envelope 立即转发到 Server `/api/device/v1/ingest`，没有维护完整节点状态表，没有生成 Dashboard-ready snapshot。
- `child_registry.c` 当前只记录注册、last_seen、last_seq、online、capabilities 等基础状态。
- `voice_proxy.c` 当前按 `X-Device-Id` 代理 raw PCM 到 Server `/api/voice/turn`，S3 不解析语义。
- `command_router.c` 当前把 Server pending commands 转成 C5 短命令 `cmds[]`，并把 C5 `id/cid/ok/e` ack 转成 Server ack。
- 缺口：没有标准 device model、没有 home summary 计算、没有 appliances mock、没有 gateway 状态快照上传、没有 recent voice/commands dashboard 汇总上传。

### ESP-server

已审计文件：

- `ESP-server/server.js`
- `ESP-server/src/routes/deviceRoutes.js`
- `ESP-server/src/services/sensorBme690Service.js`
- `ESP-server/src/routes/dashboardRoutes.js`
- `ESP-server/src/services/dashboardService.js`
- `ESP-server/src/routes/sensorRoutes.js`
- `ESP-server/package.json`
- `ESP-server/scripts/smoke-regression.js`

当前现状：

- `server.js` 已挂载 legacy routes、`createDeviceRouter`，以及 `/api/dashboard/v1`。
- `POST /api/device/v1/ingest` 当前只接受 `payload_type="sensor.bme690"`；其他 payload_type 返回 `UNSUPPORTED_PAYLOAD_TYPE`。
- `sensorBme690Service.js` 会校验 BME690 envelope，写入 `sensor_records`，并刷新 `device_status` / `device_module_status`。
- air quality server fallback 规则当前是：`>=90 excellent`、`>=75 good`、`>=55 moderate`、`>=30 poor`、其他 `bad`，无效为 `unknown`。
- 当前 `/api/dashboard/v1/overview` 返回旧聚合形状：
  - `sensor_latest`
  - `asr_latest`
  - `llm_latest`
  - `time_status`
  - `device_status`
  - `modules_status`
- 当前 Dashboard v1 history 是 `GET /api/dashboard/v1/sensors/history?device_id=&limit=`，返回传感器历史数组。
- `package.json` 中 `npm test` 和 `npm run test:smoke` 都执行 `node scripts/smoke-regression.js`。
- 缺口：ESP-server 还不能接收 S3 标准化 dashboard snapshot；`overview` 还不是目标 `gateway/devices/home_summary/history/recent_voice_events/recent_commands` 形状；还没有按 `device_id` 命名的设备 history 主接口。

### ESP-server/docs/api.md

已审计文件：

- `ESP-server/docs/api.md`

当前现状：

- 已记录 `/api/device/v1/ingest`、`/api/device/v1/status`、`/api/device/v1/modules/status`、`/api/device/v1/context`、`/api/device/v1/sensors/latest`。
- 已记录 Dashboard v1 旧读取层，并明确 legacy 保留策略。
- 当前文档没有记录 S3 标准化 dashboard snapshot 上传模型，也没有记录目标 `gateway/devices/home_summary/history/recent_voice_events/recent_commands` overview 响应。
- 后续实现时必须同步更新 `ESP-server/docs/api.md`，并保留旧接口文档。

## C5 -> S3 轻量协议设计

### 设计原则

- C5 使用短字段、短数组、数字枚举，降低 JSON 生成、字符串常量和解析成本。
- C5 不构造 Server 完整 envelope，不生成 Dashboard 嵌套 JSON，不计算 home summary。
- S3 负责把短字段还原为可读字段、标准 device model 和 Server 上传模型。
- 迁移期 S3 应同时接受当前字符串 `t` 值和新数字枚举；C5 切换完成后再考虑清理旧字符串分支。

### 通用字段

| 字段 | 类型 | 含义 | 说明 |
| --- | --- | --- | --- |
| `p` | number | local protocol version | 建议新版本为 `2`；迁移期缺省按当前 v1 处理。 |
| `id` | number | local node id | `1=C51`，`2=C52`。 |
| `t` | number | packet type | 新协议使用数字枚举。 |
| `u` | number | uptime_ms | C5 启动后毫秒数。 |
| `q` | number | local seq | C5 本地递增序号，可选但建议加入。 |
| `v` | array | compact values | 各 packet type 自定义位置语义。 |

### packet type 枚举

| `t` | 名称 | 路由 | 说明 |
| --- | --- | --- | --- |
| `1` | `sensor` | `POST /local/v1/sensor` | 传感器数据。 |
| `2` | `health` | `POST /local/v1/health` 或兼容现有 register/heartbeat/status | 注册、心跳、状态合并后的健康包。 |
| `3` | `voice` | `POST /local/v1/voice/turn` | raw PCM 语音 turn，JSON 只做短 metadata 或事件。 |
| `4` | `cmd_ack` | `POST /local/v1/commands/<cid>/ack` | 命令执行回执。 |
| `5` | `csi` | `POST /local/v1/csi/result` | CSI 预留，当前只保留占位协议。 |

### sensor 包

扩展字段：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `s` | number | sensor kind，`1=BME690`。 |
| `v[0]` | number | temperature_c |
| `v[1]` | number | humidity_percent |
| `v[2]` | number | pressure_hpa |
| `v[3]` | number | gas_resistance_ohm |
| `v[4]` | number | air_quality_score，0-100 |
| `v[5]` | number | gas_baseline_ohm |
| `v[6]` | number | gas_ratio |
| `v[7]` | number | gas_score |
| `v[8]` | number | humidity_score |
| `v[9]` | number | flags bitset，bit0=`baseline_ready`，bit1=`warmup_done` |
| `v[10]` | number | sample_count |

S3 转换责任：

- `s=1` 转成 `sensor_type="bme690"` / `payload_type="sensor.bme690"`。
- `v[]` 展开为标准字段。
- 根据 `air_quality_score` 计算 `air_quality_level`。
- 补齐 `device_id`、`room_name`、`online`、`wifi_rssi`、`timestamp`。

### health 包

扩展字段：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `h` | number | health subtype，`1=register`、`2=heartbeat`、`3=status`。 |
| `r` | number/null | Wi-Fi RSSI，单位 dBm。 |
| `v[0]` | number | wifi_connected，`0/1`。 |
| `v[1]` | number | voice_ready，`0/1`。 |
| `v[2]` | number | command_ready，`0/1`。 |
| `v[3]` | number | free_heap_bytes，可选。 |
| `v[4]` | number | min_free_heap_bytes，可选。 |

S3 转换责任：

- `h=register` 时创建或更新节点状态。
- `h=heartbeat/status` 时刷新 `last_seen`、`online`、`wifi_rssi`、`health`。
- 对缺失字段使用 S3 侧默认值，不要求 C5 携带 room/dashboard 字段。

### voice 包

语音主数据继续保持 raw PCM，不改成复杂 JSON。

建议：

- `POST /local/v1/voice/turn?id=<local_id>&u=<uptime_ms>&q=<seq>&f=1`
- body 为 `pcm_s16le_mono_16k` raw PCM。
- `f` 音频格式枚举：`1=pcm_s16le_mono_16k`。

可选短事件 JSON：

```json
{"p":2,"id":1,"t":3,"u":123456,"q":44,"e":1,"v":[64000,2000]}
```

其中 `e=1` 表示 voice turn finished，`v[0]=pcm_bytes`，`v[1]=duration_ms`。S3 可据此写入 `recent_voice_events`，但 C5 不处理 ASR/LLM/TTS 语义。

### cmd_ack 包

字段：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `id` | number | local node id |
| `t` | number | 固定 `4` |
| `cid` | string | command_id |
| `ok` | number | `1=completed`，`0=failed` |
| `e` | number | local error enum，成功为 `0` |
| `u` | number | uptime_ms |
| `q` | number | local seq，可选 |

错误枚举沿用并文档化：

- `0=none`
- `1=command_failed`
- `2=unsupported_command`
- `3=invalid_payload`
- `4=timeout`
- `255=unknown`

S3 转换为 Server ack：

- `ok=1` -> `status="completed"`，`result.applied=true`。
- `ok=0` -> `status="failed"`，按 `e` 转成稳定 `error_code`。
- S3 记录到 `recent_commands`。

### csi 预留包

字段：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `id` | number | local node id |
| `t` | number | 固定 `5` |
| `k` | number | CSI record kind，预留 |
| `u` | number | uptime_ms |
| `q` | number | local seq |
| `v` | array/string | 预留 payload |

当前只保留协议与路由，不承诺真实 CSI 行为识别。Dashboard 不依赖 CSI 真实数据。

## S3 职责设计

S3 是本次重构的协议转换和聚合中心。

必须实现：

1. 接收 C51/C52 短字段包，并兼容当前 `reg/hb/st/bme/csi` 字符串类型。
2. 维护节点状态表，建议以内存结构为主：
   - `local_id`
   - `device_id`
   - `room_name`
   - `alias`
   - `online`
   - `wifi_rssi`
   - `last_seen_ms`
   - `last_uptime_ms`
   - `last_sensor`
   - `last_health`
   - `last_voice_event`
   - `last_command_ack`
3. 将短字段转换为标准 device model。
4. 补全：
   - `device_id`
   - `room_name`
   - `online`
   - `wifi_rssi`
   - `timestamp`
5. 转换 `air_quality_level`，建议与当前 Server fallback 阈值保持一致：
   - `score >= 90`: `excellent`
   - `score >= 75`: `good`
   - `score >= 55`: `moderate`
   - `score >= 30`: `poor`
   - `score < 30`: `bad`
   - 缺失或非法：`unknown`
6. 生成 gateway 状态：
   - `gateway_id`
   - `online`
   - `softap_ready`
   - `sta_connected`
   - `server_available`
   - `voice_busy`
   - `last_error`
   - `timestamp`
7. 计算 `home_summary`：
   - `online_device_count`
   - `offline_device_count`
   - `avg_temperature`
   - `avg_humidity`
   - `avg_air_quality`
8. 为每个 room/device 填充 mock appliances：
   - `air_conditioner`
   - `fan`
   - `light`
   - `tv`
   - `curtain`
9. 上传标准化数据到 ESP-server。

标准 device model 建议：

```json
{
  "device_id": "sensair_shuttle_01",
  "local_id": 1,
  "name": "SensaiShuttle",
  "room_name": "living_room",
  "online": true,
  "wifi_rssi": -58,
  "timestamp": 1781100000000,
  "sensors": {
    "temperature": 29.57,
    "humidity": 48.2,
    "pressure": 1008.6,
    "gas_resistance": 35164,
    "air_quality_score": 72,
    "air_quality_level": "moderate",
    "air_quality_source": "s3_mapped"
  },
  "appliances": {
    "air_conditioner": {"power": false, "mode": "cool", "target_temperature": 26, "source": "mock", "mock": true},
    "fan": {"power": false, "speed": 0, "source": "mock", "mock": true},
    "light": {"power": true, "brightness": 60, "source": "mock", "mock": true},
    "tv": {"power": false, "source": "mock", "mock": true},
    "curtain": {"open_percent": 70, "source": "mock", "mock": true}
  }
}
```

## S3 -> Server 标准上传模型

推荐新增 Server 接收接口：

- `POST /api/device/v1/gateway-state`

兼容策略：

- 保留当前 `POST /api/device/v1/ingest` 的 `sensor.bme690` 行为。
- 迁移期 S3 可以继续向 `/api/device/v1/ingest` 转发单条 BME690，同时新增向 `/api/device/v1/gateway-state` 上传 dashboard snapshot。
- 后续稳定后，Server 可从 snapshot 写入最新状态，也可以继续把 `history[]` 同步落到 `sensor_records`。

上传模型必须包含：

| 字段 | 含义 |
| --- | --- |
| `schema_version` | Server 标准上传 schema，建议 `2`。 |
| `payload_type` | 固定 `gateway.dashboard_snapshot`。 |
| `source` | 固定 `s3_gateway`。 |
| `gateway` | S3 网关状态。 |
| `devices` | 当前设备状态数组。 |
| `home_summary` | S3 计算的家庭聚合指标。 |
| `recent_voice_events` | 最近语音事件。 |
| `recent_commands` | 最近命令与 ack 状态。 |
| `history` | history 所需传感器数据点。 |

## ESP-server 后端职责设计

必须实现：

1. 保留旧接口兼容：
   - `/sensor/*`
   - `/asr/*`
   - `/llm/*`
   - `/api/time/*`
   - `/api/device/v1/ingest`
   - 现有 `/api/dashboard/v1/sensors/*` 等读取接口
2. 新增 `POST /api/device/v1/gateway-state` 接收 S3 标准化上传数据，或等价地扩展 device v1 ingest，但不得破坏当前 `sensor.bme690`。
3. 新增或调整 `/api/dashboard/v1/overview`，在 v1 envelope 中返回：
   - `gateway`
   - `devices`
   - `home_summary`
   - `history`
   - `recent_voice_events`
   - `recent_commands`
4. 新增或调整按 `device_id` 查询 history 的接口：
   - 推荐新增：`GET /api/dashboard/v1/devices/:device_id/history?limit=50`
   - 保留兼容：`GET /api/dashboard/v1/sensors/history?device_id=<device_id>&limit=50`
5. 后端返回前端可直接读取的数据，不要求前端理解 S3 短字段或 device ingest envelope。
6. 如果 S3 未上传 appliances，后端可以兜底补 mock appliances，但必须标注 `source="mock"` / `mock=true`。
7. `ESP-server/docs/api.md` 必须同步新增：
   - S3 标准上传接口
   - Dashboard overview 新响应
   - device_id history 接口
   - mock appliances 字段说明
   - legacy 保留说明

建议存储策略：

- 简单优先：新增 `gateway_snapshots` 或复用 JSON 存储最新 snapshot，同时把 `history[]` 中 BME690 数据落入现有 `sensor_records`。
- Dashboard overview 读取最新 snapshot；如果无 snapshot，再回退当前 `sensor_records/device_status` 旧聚合。
- recent voice/commands 可先从 snapshot JSON 中读，后续再拆表。

## 假数据策略

以下字段暂时使用 fake/mock 数据，不代表真实控制模块已接入：

- `appliances.air_conditioner`
- `appliances.fan`
- `appliances.light`
- `appliances.tv`
- `appliances.curtain`

统一要求：

- 每个 appliance 对象必须包含 `source: "mock"`。
- 每个 appliance 对象必须包含 `mock: true`。
- 可选包含 `updated_at` 和 `note`，例如 `note: "placeholder until appliance control module is integrated"`。
- S3 优先生成 mock appliances；Server 可作为兜底补齐。
- 后续真实控制模块接入后，把 `source` 改为真实来源，例如 `source: "control_module"`，并移除或置空 `mock`。

建议固定 mock 值：

```json
{
  "air_conditioner": {
    "power": false,
    "mode": "cool",
    "target_temperature": 26,
    "source": "mock",
    "mock": true
  },
  "fan": {
    "power": false,
    "speed": 0,
    "source": "mock",
    "mock": true
  },
  "light": {
    "power": true,
    "brightness": 60,
    "source": "mock",
    "mock": true
  },
  "tv": {
    "power": false,
    "source": "mock",
    "mock": true
  },
  "curtain": {
    "open_percent": 70,
    "source": "mock",
    "mock": true
  }
}
```

## 后续每个工程的修改范围

### firmware-local esp111_protocol_common

允许修改：

- `ESPC51/components/esp111_protocol_common/include/esp111_protocol_common.h`
- `ESPC52/components/esp111_protocol_common/include/esp111_protocol_common.h`
- `ESPS3/components/esp111_protocol_common/include/esp111_protocol_common.h`

修改内容：

- 增加 C5 -> S3 numeric enum 常量。
- 增加 sensor/health/voice/cmd_ack/csi 短字段说明常量。
- 增加 S3 -> Server dashboard snapshot payload type / route 常量。
- 保留当前字符串类型常量供迁移兼容。
- 三份头文件同步修改并保持 byte-for-byte 一致。顶层 `shared_components/esp111_protocol_common` 可以在确认后作为参考更新，但不能恢复为三套工程的构建依赖。

### ESPC51

允许修改：

- `ESPC51/components/Middlewares/sensor_domain/bme690/server_client/bme_server_client.c`
- `ESPC51/components/Middlewares/command_domain/system_command/system_server_client.c`
- `ESPC51/components/Middlewares/server_voice/server_voice_client.c`
- `ESPC51/components/Middlewares/server_voice/server_voice_protocol.h`
- `ESPC51/components/Middlewares/device_protocol/device_protocol_metadata.*`
- `ESPC51/components/Middlewares/terminal_config/*`
- 必要时更新 `ESPC51/components/Middlewares/CMakeLists.txt`

修改内容：

- BME sensor 包改为 numeric enum。
- register/heartbeat/status 收敛为 health 短包，或在迁移期由 S3 同时兼容旧三路。
- voice turn 增加短 metadata，不增加复杂 JSON。
- cmd_ack 使用 numeric `t=4`，保留 `cid/ok/e`。

禁止修改：

- `ESPC51/managed_components`

### ESPC52

允许修改：

- 与 C51 共用 Middlewares 的相同文件。
- `ESPC52/components/Middlewares/server_comm/CMakeLists.txt` 仅在身份或编译定义需要调整时修改。

修改内容：

- 跟随 C51 的共用协议实现。
- 保持 `local_id=2`、`device_id="sensair_shuttle_02"`。

禁止修改：

- `ESPC52/managed_components`

### ESPS3

允许修改：

- `ESPS3/components/Middlewares/local_http_server/local_http_server.c`
- `ESPS3/components/Middlewares/protocol_adapter/*`
- `ESPS3/components/Middlewares/sensor_aggregator/*`
- `ESPS3/components/Middlewares/server_client/*`
- `ESPS3/components/Middlewares/child_registry/*`
- `ESPS3/components/Middlewares/gateway_config/*`
- `ESPS3/components/Middlewares/voice_proxy/*`
- `ESPS3/components/Middlewares/command_router/*`
- 新增 `dashboard_state` / `gateway_snapshot` 之类 S3 聚合模块
- 必要时更新 `ESPS3/components/Middlewares/CMakeLists.txt`

修改内容：

- 本地短包兼容解析。
- 节点状态表扩展。
- 标准 device model 生成。
- gateway 状态生成。
- home summary 计算。
- mock appliances 填充。
- 标准 snapshot 上传 Server。

禁止修改：

- `ESPS3/managed_components`

### ESP-server/src

允许修改：

- `ESP-server/src/routes/deviceRoutes.js`
- `ESP-server/src/routes/dashboardRoutes.js`
- `ESP-server/src/services/dashboardService.js`
- `ESP-server/src/services/sensorBme690Service.js`
- `ESP-server/src/services/deviceStatusService.js`
- `ESP-server/src/services/deviceMetadata.js`
- `ESP-server/src/db/*`
- 可新增 `gatewayStateService.js` / `dashboardSnapshotService.js`

修改内容：

- 新增 S3 snapshot ingest。
- 持久化或缓存标准 dashboard snapshot。
- 调整 overview 聚合输出。
- 增加按 `device_id` 的 history 接口。
- 保留旧接口和旧响应。

禁止修改：

- `ESP-server/public`
- `ESP-server/db/database.db` 真实运行库

### ESP-server/docs/api.md

允许修改：

- `ESP-server/docs/api.md`

修改内容：

- 新增 S3 标准上传接口说明。
- 更新 Dashboard overview 目标响应说明。
- 新增 device_id history 接口说明。
- 标注 appliances mock/fake/source=mock。
- 保留 legacy 文档，不删除旧接口。

## 最终 JSON 示例

### C5 -> S3 sensor 短字段包

```json
{
  "p": 2,
  "id": 1,
  "t": 1,
  "s": 1,
  "u": 123456,
  "q": 42,
  "v": [29.57, 48.2, 1008.6, 35164, 72, 41000, 0.8571, 70, 80, 3, 128]
}
```

解释：

- `t=1`: sensor
- `s=1`: BME690
- `v[9]=3`: bit0 baseline_ready + bit1 warmup_done

### C5 -> S3 health 包

```json
{
  "p": 2,
  "id": 1,
  "t": 2,
  "h": 2,
  "u": 124000,
  "q": 43,
  "r": -58,
  "v": [1, 1, 1, 182000, 148000]
}
```

解释：

- `t=2`: health
- `h=2`: heartbeat
- `r=-58`: Wi-Fi RSSI
- `v[0]=1`: Wi-Fi connected
- `v[1]=1`: voice ready
- `v[2]=1`: command ready

### S3 -> Server 标准上传包

```json
{
  "schema_version": 2,
  "payload_type": "gateway.dashboard_snapshot",
  "source": "s3_gateway",
  "gateway": {
    "gateway_id": "sensair_s3_gateway_01",
    "online": true,
    "softap_ready": true,
    "sta_connected": true,
    "server_available": true,
    "voice_busy": false,
    "last_error": "",
    "timestamp": 1781100000000
  },
  "devices": [
    {
      "device_id": "sensair_shuttle_01",
      "local_id": 1,
      "name": "SensaiShuttle",
      "room_name": "living_room",
      "online": true,
      "wifi_rssi": -58,
      "timestamp": 1781100000000,
      "sensors": {
        "temperature": 29.57,
        "humidity": 48.2,
        "pressure": 1008.6,
        "gas_resistance": 35164,
        "air_quality_score": 72,
        "air_quality_level": "moderate",
        "air_quality_source": "s3_mapped"
      },
      "appliances": {
        "air_conditioner": {"power": false, "mode": "cool", "target_temperature": 26, "source": "mock", "mock": true},
        "fan": {"power": false, "speed": 0, "source": "mock", "mock": true},
        "light": {"power": true, "brightness": 60, "source": "mock", "mock": true},
        "tv": {"power": false, "source": "mock", "mock": true},
        "curtain": {"open_percent": 70, "source": "mock", "mock": true}
      }
    },
    {
      "device_id": "sensair_shuttle_02",
      "local_id": 2,
      "name": "SensaiShuttle02",
      "room_name": "bedroom",
      "online": false,
      "wifi_rssi": null,
      "timestamp": 1781100000000,
      "sensors": null,
      "appliances": {
        "air_conditioner": {"power": false, "mode": "cool", "target_temperature": 26, "source": "mock", "mock": true},
        "fan": {"power": false, "speed": 0, "source": "mock", "mock": true},
        "light": {"power": true, "brightness": 60, "source": "mock", "mock": true},
        "tv": {"power": false, "source": "mock", "mock": true},
        "curtain": {"open_percent": 70, "source": "mock", "mock": true}
      }
    }
  ],
  "home_summary": {
    "online_device_count": 1,
    "offline_device_count": 1,
    "avg_temperature": 29.57,
    "avg_humidity": 48.2,
    "avg_air_quality": 72
  },
  "recent_voice_events": [
    {
      "device_id": "sensair_shuttle_01",
      "event": "voice_turn_completed",
      "timestamp": 1781100000000,
      "duration_ms": 2000,
      "source": "s3_gateway"
    }
  ],
  "recent_commands": [
    {
      "command_id": "cmd-001",
      "device_id": "sensair_shuttle_01",
      "command_code": 2,
      "status": "completed",
      "timestamp": 1781100000000,
      "source": "s3_gateway"
    }
  ],
  "history": [
    {
      "device_id": "sensair_shuttle_01",
      "sensor_type": "bme690",
      "timestamp": 1781100000000,
      "temperature": 29.57,
      "humidity": 48.2,
      "pressure": 1008.6,
      "gas_resistance": 35164,
      "air_quality_score": 72,
      "air_quality_level": "moderate"
    }
  ]
}
```

### Server -> Frontend dashboard overview 响应

```json
{
  "ok": true,
  "server_time_ms": 1781100000100,
  "data": {
    "gateway": {
      "gateway_id": "sensair_s3_gateway_01",
      "online": true,
      "softap_ready": true,
      "sta_connected": true,
      "server_available": true,
      "voice_busy": false,
      "last_error": "",
      "timestamp": 1781100000000
    },
    "devices": [
      {
        "device_id": "sensair_shuttle_01",
        "local_id": 1,
        "name": "SensaiShuttle",
        "room_name": "living_room",
        "online": true,
        "wifi_rssi": -58,
        "timestamp": 1781100000000,
        "sensors": {
          "temperature": 29.57,
          "humidity": 48.2,
          "pressure": 1008.6,
          "gas_resistance": 35164,
          "air_quality_score": 72,
          "air_quality_level": "moderate"
        },
        "appliances": {
          "air_conditioner": {"power": false, "mode": "cool", "target_temperature": 26, "source": "mock", "mock": true},
          "fan": {"power": false, "speed": 0, "source": "mock", "mock": true},
          "light": {"power": true, "brightness": 60, "source": "mock", "mock": true},
          "tv": {"power": false, "source": "mock", "mock": true},
          "curtain": {"open_percent": 70, "source": "mock", "mock": true}
        }
      }
    ],
    "home_summary": {
      "online_device_count": 1,
      "offline_device_count": 1,
      "avg_temperature": 29.57,
      "avg_humidity": 48.2,
      "avg_air_quality": 72
    },
    "history": [
      {
        "device_id": "sensair_shuttle_01",
        "timestamp": 1781100000000,
        "temperature": 29.57,
        "humidity": 48.2,
        "air_quality_score": 72,
        "air_quality_level": "moderate"
      }
    ],
    "recent_voice_events": [
      {
        "device_id": "sensair_shuttle_01",
        "event": "voice_turn_completed",
        "timestamp": 1781100000000,
        "duration_ms": 2000,
        "source": "s3_gateway"
      }
    ],
    "recent_commands": [
      {
        "command_id": "cmd-001",
        "device_id": "sensair_shuttle_01",
        "command_code": 2,
        "status": "completed",
        "timestamp": 1781100000000,
        "source": "s3_gateway"
      }
    ]
  },
  "error": null
}
```

## 分阶段落地步骤

### 阶段 1：firmware-local shared protocol

修改：

- `ESPC51/components/esp111_protocol_common/include/esp111_protocol_common.h`
- `ESPC52/components/esp111_protocol_common/include/esp111_protocol_common.h`
- `ESPS3/components/esp111_protocol_common/include/esp111_protocol_common.h`

任务：

- 增加 numeric packet type enum。
- 增加 sensor kind、health subtype、voice format、cmd ack error enum 文档常量。
- 增加 S3 dashboard snapshot route / payload type 常量。
- 保留当前字符串类型和旧路由兼容。
- 确认 C51/C52/S3 的 `CMakeLists.txt` 不再通过 `../shared_components/esp111_protocol_common` 引入外部组件。

验收：

```bash
cd /Users/zhiqin/ESP-111
git diff --check
```

### 阶段 2：C51/C52 上报

修改：

- C51/C52 共用 Middlewares 中的 BME、health/system、voice metadata、cmd ack。

任务：

- C5 sensor 使用 `t=1`、`s=1`。
- C5 health 使用 `t=2`、`h=1/2/3`。
- C5 voice turn 保持 raw PCM，只增加短 metadata。
- C5 cmd ack 使用 `t=4`，保留 `cid/ok/e`。
- 不引入 Dashboard 或 Server 完整 JSON。

验收：

```bash
cd /Users/zhiqin/ESP-111/ESPC51
idf.py build
cd /Users/zhiqin/ESP-111/ESPC52
idf.py build
```

### 阶段 3：S3 转换、状态表和上传

修改：

- `ESPS3/components/Middlewares/protocol_adapter/*`
- `ESPS3/components/Middlewares/sensor_aggregator/*`
- `ESPS3/components/Middlewares/child_registry/*`
- `ESPS3/components/Middlewares/server_client/*`
- 新增 S3 dashboard snapshot 聚合模块

任务：

- S3 同时兼容旧字符串包和新 numeric 包。
- 扩展节点状态表。
- 生成标准 device model。
- 计算 gateway 状态和 home summary。
- 填充 mock appliances。
- 上传 `gateway.dashboard_snapshot` 到 Server。
- 保持 voice proxy 与 command router 兼容。

验收：

```bash
cd /Users/zhiqin/ESP-111/ESPS3
idf.py build
```

### 阶段 4：ESP-server 后端接收与聚合

修改：

- `ESP-server/src/routes/deviceRoutes.js`
- `ESP-server/src/routes/dashboardRoutes.js`
- `ESP-server/src/services/dashboardService.js`
- `ESP-server/src/db/*`
- 新增所需 service

任务：

- 新增 `POST /api/device/v1/gateway-state`。
- 保留 `/api/device/v1/ingest` 的 `sensor.bme690`。
- 持久化或缓存最新 S3 snapshot。
- `/api/dashboard/v1/overview` 返回目标模型。
- 新增 `GET /api/dashboard/v1/devices/:device_id/history`。
- 保留 `/api/dashboard/v1/sensors/history` 兼容。
- appliances 缺失时后端兜底 mock 并标注 `source="mock"`。

验收：

```bash
cd /Users/zhiqin/ESP-111/ESP-server
node --check server.js
find src -name '*.js' -print0 | xargs -0 -n1 node --check
npm test
git diff --check
```

### 阶段 5：更新 docs/api.md 与 smoke regression

修改：

- `ESP-server/docs/api.md`
- 必要时更新 `ESP-server/scripts/smoke-regression.js`

任务：

- 记录 S3 snapshot 上传接口。
- 记录 Dashboard overview 目标响应。
- 记录 device_id history 接口。
- 标注 appliances 为 mock/fake/source=mock。
- 增加 smoke 覆盖，确保 legacy routes 和新 routes 同时通过。

验收：

```bash
cd /Users/zhiqin/ESP-111/ESP-server
npm test
git diff -- public db/database.db
```

最终总验收建议：

```bash
cd /Users/zhiqin/ESP-111/ESPC51 && idf.py build
cd /Users/zhiqin/ESP-111/ESPC52 && idf.py build
cd /Users/zhiqin/ESP-111/ESPS3 && idf.py build
cd /Users/zhiqin/ESP-111/ESP-server && npm test
cd /Users/zhiqin/ESP-111 && git diff --check
```

## 实施顺序总表

1. 先改 shared protocol，建立短字段 numeric enum 和 S3 snapshot 常量。
2. 再改 C51/C52 上报，确保 C5 只发送轻量 sensor/health/voice/cmd_ack/csi。
3. 再改 S3 转换和上传，完成状态表、标准 device model、gateway/home summary/mock appliances。
4. 再改 ESP-server 后端接收与 Dashboard v1 聚合。
5. 最后更新 `ESP-server/docs/api.md`，补 smoke regression，并运行语法检查、`idf.py build`、`npm test`。

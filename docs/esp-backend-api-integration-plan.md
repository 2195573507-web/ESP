# ESP Backend API Integration Plan

本文基于 `/Users/zhiqin/ESP-111/ESP-server/docs/api.md` 的当前接口契约，规划 ESPC51、ESPC52、ESPS3 对接后端 v1 API 的固件侧改造。本文只描述计划，不包含本轮源码、Kconfig、分区、构建或烧录改动。

## 1. 当前结论

- 后端 `api.md` 已经定义 Dashboard、设备状态、事件日志、智能家居、SSE、voice prompt cache、voice turn 和统一设备协议 v1 等接口。
- ESP 端需要改造，但主要改 ESPS3：S3 是唯一 Server-facing 网关，负责家庭 WiFi、Server HTTP、Dashboard snapshot 构造、日志/报警、智能家居命令轮询与 ACK、voice turn 转发、wake prompt cache 代理、CSI summary 转换。
- ESPC51/ESPC52 只做轻量协议字段补齐和断联重连优先级调整：继续保持 C5 -> S3 的本地轻量 frame，不把后端完整 JSON envelope 下放到 C5。
- Server-facing HTTP 统一放在 ESPS3，不放在 C5。C5 不直连家庭 WiFi，不直接访问 Server，只连接 S3 SoftAP。
- S3 负责把 C5/S3 状态转换为后端 schema，包括 `schema_version=2` 的 `gateway.dashboard_snapshot`、`sensor.bme690`、`csi.motion`、logs、alarms、smart-home state/command ack 和 voice 相关请求。

## 2. 设备职责边界

### ESPS3 职责

- 连接家庭 WiFi / Server，维护 `server_available`、超时、重试和降级状态。
- 开 SoftAP 给 C5，保持 C5 本地链路独立于 Server 可用性。
- 维护 `child_registry`，记录 C51/C52 `last_seen_ms`、online/offline、link_lost、voice_busy 等状态。
- 汇总 C51/C52 在线状态、BME690 传感器、语音事件、CSI 摘要、模块状态和 S3 自身状态。
- 上传 Dashboard 标准快照到 `POST /api/device/v1/gateway-state`。
- 上传 heartbeat / logs / alarms 到已存在的 device/log API。
- 拉取智能家居 pending commands，执行后 ACK；没有真实硬件时返回 `failed`，不假装成功。
- 必要时上报 smart-home state；没有真实设备时不伪造设备状态。
- 代理 wake prompt cache：S3 调 `GET /api/voice/prompt/config` 和 `GET /api/voice/prompt-cache`，缓存后提供给 C5。
- 代理 voice turn：C5 音频经 S3 转发到 `POST /api/voice/turn`，S3 再把响应音频返回给 C5。

### ESPC51/ESPC52 职责

- 只连接 S3 SoftAP。
- 采集 BME690。
- 本地计算 `air_quality_score` / `air_quality_level`，并提供置信度、算法版本和来源字段。
- 本地 wake / VAD / voice capture，语音音频只发给 S3。
- CSI 本地摘要，默认可关闭；只产生 occupancy/motion_score 等轻量结果。
- 给 S3 发轻量 frame，不直接访问 Server。
- 与 S3 断联时，重连 S3 是最高优先级；重连前暂停 Server-dependent 任务。
- 不处理 Server token，不处理 Server URL，不处理 HTTPS/TLS 到 Server。

## 3. 后端接口映射

| 功能 | 后端接口 | 调用方 | 优先级 | 说明 |
| --- | --- | --- | --- | --- |
| S3 Dashboard 快照 | `POST /api/device/v1/gateway-state` | ESPS3 | P0 | `schema_version=2`, `payload_type=gateway.dashboard_snapshot` |
| 手动/调试快照 | `POST /api/dashboard/v1/snapshot` | ESPS3/调试 | P1 | 与 `gateway-state` 同结构 |
| C5/S3 heartbeat | `POST /api/device/v1/ingest` | ESPS3 | P0 | 计划目标可抽象为 `payload_type=gateway.heartbeat` 或复用 `sensor.bme690` / `csi.motion` 状态刷新；但当前 `api.md` 已接入的 ingest payload 只有 `sensor.bme690` 和 `csi.motion`，所以 `gateway.heartbeat` 不能直接调用，需先扩展后端文档和实现 |
| 设备状态读取 | `GET /api/device/v1/status` | 调试/前端 | P0 | ESP 不一定调用 |
| 系统日志 | `POST /api/logs/v1/system` | ESPS3 | P1 | C5 offline/online、server unavailable 等 |
| 报警日志 | `POST /api/logs/v1/alarms` | ESPS3 | P1 | 设备离线、命令失败、异常状态 |
| 智能家居状态 | `POST /api/smart-home/v1/state` | ESPS3 | P1 | 没有真实设备时不伪造 |
| 智能家居 pending | `GET /api/smart-home/v1/commands/pending` | ESPS3 | P1 | S3 轮询，可带 `gateway_id` 和 `limit` |
| 智能家居 ACK | `POST /api/smart-home/v1/commands/:command_id/ack` | ESPS3 | P1 | `success` / `failed` |
| wake prompt config/cache | `GET /api/voice/prompt/config`, `GET /api/voice/prompt-cache` | ESPS3 | P0 | S3 缓存后给 C5 |
| voice turn | `POST /api/voice/turn` | ESPS3 | P0 | C5 音频经 S3 转发 |
| CSI motion | `POST /api/device/v1/ingest` with `payload_type=csi.motion` | ESPS3 | P1 | 只上传摘要，不上传 raw CSI |
| SSE 实时事件 | `GET /api/events/v1/stream` | 调试/前端 | P2 | ESP 不需要常驻订阅，调试可用 |

禁止事项：

- 不要让 C5 调用 `/api/device/v1/ingest`。
- 不要让 C5 调用 `/api/device/v1/gateway-state`。
- 不要让 C5 调用 `/api/logs/v1/*`。
- 不要让 C5 调用 `/api/smart-home/v1/*`。
- 不要让 C5 调用 `/api/voice/prompt/config`、`/api/voice/prompt-cache` 或 `/api/voice/turn` 的 Server URL；C5 只访问 S3 的本地接口。

## 4. ESPS3 需要新增/调整的模块计划

### gateway_server_client

- 统一封装 Server HTTP `POST` / `GET`。现有落地点可沿用 `server_client`，或在后续实现中拆出 `gateway_server_client` 作为更清晰的 Server-facing 层。
- 支持 JSON body，用于 `gateway-state`、`device/v1/ingest`、logs、alarms、smart-home state 和 ACK。
- 支持 raw PCM voice turn，用于 `POST /api/voice/turn`。
- 支持超时、重试、`server_available` 标记和错误码归一化。
- 避免 voice turn 长连接阻塞 heartbeat/snapshot：voice turn 需要独立超时、独立任务或互斥边界，普通 snapshot/log 不应等待语音链路结束。
- 统一添加必要 header metadata，例如 `X-Device-Id`、`X-Schema-Version`、`X-Payload-Type`、`X-Request-Seq`、`X-Esp-Uptime-Ms`、`X-Time-Synced`。

### gateway_snapshot_builder

- 从 `child_registry`、`sensor_aggregator`、`voice_proxy`、`csi_placeholder_gateway`、`offline_policy` 读取状态。
- 生成 `api.md` 要求的 `schema_version=2`、`payload_type=gateway.dashboard_snapshot`。
- 生成 `gateway`、`devices`、`home_summary`、`history`、`recent_voice_events`、`recent_commands`。
- C5 设备字段必须包括 `device_id`、`room_name`、`online`、`timestamp`、`sensors`、`occupancy`。
- `devices[].sensors` 使用 S3 从 C5 BME frame 映射后的字段：temperature/humidity/pressure/gas_resistance/air_quality_*。
- mock appliances 如果保留，必须标注 `source:"mock"`、`mock:true`；如果要避免假数据，可不上传 `appliances`，或上传明确的 unavailable 状态，不冒充真实智能家居。

### gateway_heartbeat

- 固定周期上报 S3 自身状态，推荐周期 5s 或 10s。
- P0 阶段可通过 `gateway-state` 的 `gateway` 字段和必要 system log 表达 S3 状态；如果要新增 `payload_type=gateway.heartbeat` 到 `POST /api/device/v1/ingest`，必须先在后端 `api.md` 与实现中扩展，因为当前 `api.md` 已接入的 ingest payload 是 `sensor.bme690` 和 `csi.motion`。
- `server_recv_ms` 由后端生成，ESP 不上传伪 server 时间。
- `time_synced=false` 时不要上传 `esp_time_ms`，或置 `null`。
- 心跳失败只改变 `server_available` 和降级策略，不影响 S3 SoftAP 继续服务 C5。

### child_registry

- 维护 C51/C52 `last_seen_ms`。
- 维护 online/offline/link_lost/voice_busy。
- 支持 offline timeout。
- C5 任何有效 frame 到达都刷新 `last_seen_ms`。
- child offline/online 状态变化产生 system log 或 alarm。
- voice_busy 期间不要因普通 heartbeat 暂停而误判 offline。

### gateway_event_reporter

- 上报 system logs 和 alarms。
- 去重/限频，避免断网或 C5 抖动时刷爆日志。
- `server unavailable` 时缓存少量关键事件或丢弃低优先级事件。
- 设备上线/离线、S3 Server 断开/恢复、智能家居命令失败、voice turn 失败、CSI 异常可进入该模块。

### smart_home_gateway

- 轮询 `GET /api/smart-home/v1/commands/pending`。
- 执行控制命令或返回 `failed`。
- 没有真实智能家居硬件时，不假装 `success`。
- ACK 后更新 `recent_commands`，并同步到下一次 Dashboard snapshot。
- 必要时用 `POST /api/smart-home/v1/state` 上报真实设备状态；没有真实设备时不上报假设备。

### csi_summary_uploader

- 接收 C5 CSI summary。
- 统一转成 `POST /api/device/v1/ingest` with `payload_type=csi.motion`。
- 只上传 `occupancy.state`、`motion_score`、`variance`、`rssi`、`sample_count`、`updated_at`。
- 不上传 raw CSI/IQ/相位/子载波数组。
- CSI 上传失败不能影响 BME/voice 主链路。

## 5. ESPC51/ESPC52 需要改的内容

C5 发给 S3 的 frame 必须稳定包含：

- `device_id`
- `payload_type`
- `request_seq`
- `esp_uptime_ms`
- `time_synced`
- `room_id` / `room_name`，如已有

BME690 frame 包含：

- `temperature_c`
- `humidity_percent`
- `pressure_hpa`
- `gas_resistance_ohm`
- `air_quality_score`
- `air_quality_level`
- `air_quality_confidence`
- `air_quality_algo_version`
- `air_quality_source`

voice event frame 包含：

- `wake_count`
- `voice_turn_id`
- `state`
- `error_code`，如失败

CSI summary frame 包含：

- `occupancy.state`
- `motion_score`
- `variance`
- `rssi`
- `sample_count`
- `updated_at` 或 `esp_uptime_ms`

C5 与 S3 断联时：

- 暂停 server-dependent 任务。
- 优先重连 S3。
- 重连成功后恢复 BME/voice/CSI 上报。
- 不把失败转为直接访问家庭 WiFi 或 Server。

C5 不处理：

- Server token。
- Server URL。
- HTTPS/TLS 到 Server。
- `/api/device/v1/*`、`/api/logs/v1/*`、`/api/smart-home/v1/*` 等后端路径。

## 6. 建议 JSON 示例

### S3 gateway-state 示例

使用 `POST /api/device/v1/gateway-state`，包含一个 C51。

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
      "device_id": "sensair_c51_01",
      "local_id": 1,
      "name": "Sensair C51",
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
        "air_quality_source": "esp"
      },
      "occupancy": {
        "state": "unknown",
        "available": false,
        "motion_score": null,
        "variance": null,
        "rssi": -58,
        "sample_count": 0,
        "updated_at": null
      }
    }
  ],
  "home_summary": {
    "online_device_count": 1,
    "offline_device_count": 0,
    "avg_temperature": 29.57,
    "avg_humidity": 48.2,
    "avg_air_quality": 72
  },
  "history": [],
  "recent_voice_events": [],
  "recent_commands": []
}
```

### C5 -> S3 sensor frame 示例

使用轻量 JSON 或项目现有 frame 结构；重点是字段稳定，不要求 C5 构造 Server 完整 envelope。

```json
{
  "device_id": "sensair_c51_01",
  "payload_type": "sensor.bme690",
  "request_seq": 123,
  "esp_uptime_ms": 12345678,
  "time_synced": false,
  "room_id": "living_room",
  "room_name": "客厅",
  "payload": {
    "temperature_c": 29.57,
    "humidity_percent": 48.2,
    "pressure_hpa": 1008.6,
    "gas_resistance_ohm": 35164,
    "air_quality_score": 72,
    "air_quality_level": "moderate",
    "air_quality_confidence": "low",
    "air_quality_algo_version": "esp-bme690-relative-v1",
    "air_quality_source": "esp"
  }
}
```

### S3 -> Server csi.motion 示例

使用 `POST /api/device/v1/ingest`，`payload_type=csi.motion`。

```json
{
  "schema_version": 1,
  "device_id": "sensair_c51_01",
  "device_type": "esp32c5_env_voice_node",
  "firmware_version": "0.1.0",
  "request_seq": 456,
  "esp_uptime_ms": 12345678,
  "time_synced": false,
  "payload_type": "csi.motion",
  "room_id": "living_room",
  "payload": {
    "occupancy": {
      "state": "occupied"
    },
    "motion_score": 0.73,
    "variance": 0.0182,
    "rssi": -58,
    "sample_count": 96,
    "updated_at": 1781100000000
  }
}
```

### Smart-home command ack 示例

success:

```json
{
  "status": "success",
  "executed_at_ms": 1780000000200,
  "result": {
    "applied": true
  },
  "error_message": ""
}
```

failed:

```json
{
  "status": "failed",
  "executed_at_ms": 1780000000200,
  "result": {
    "applied": false
  },
  "error_message": "no real smart-home device attached"
}
```

## 7. 分阶段实施计划

### P0：Dashboard 和在线状态打通

- S3 build gateway snapshot。
- S3 `POST /api/device/v1/gateway-state`。
- `child_registry` 支持 `last_seen` / online / offline。
- C5 frame 补 `device_id` / `payload_type`。
- S3 将 C5 BME frame 映射为 snapshot `devices[].sensors`。
- S3 不要求 C5 直接理解后端 Dashboard schema。

验收：

- 后端 `GET /api/dashboard/v1/overview` 能看到 `gateway` / `devices`。
- 后端 `GET /api/device/v1/status` 能看到 S3/C5 online/offline。
- 重启 Server 后 latest snapshot 能恢复。
- C5 断 S3 后显示 offline，恢复连接后回到 online。

### P1：日志、报警、智能家居命令队列

- S3 上报 logs/alarms。
- S3 轮询 smart-home pending。
- S3 ACK command。
- 没有真实智能家居设备时返回 `failed`，不假成功。
- S3 将 ACK 结果写入 `recent_commands`，供 Dashboard snapshot 展示。

验收：

- `GET /api/logs/v1/system` 有真实日志。
- `GET /api/logs/v1/alarms` 有真实报警。
- `GET /api/smart-home/v1/commands` 可看到 `queued` / `dispatched` / `success` / `failed`。
- `GET /api/events/v1/stream` 能看到对应实时事件。

### P2：CSI、voice、时间偏移完善

- CSI summary 上传。
- voice events 统一记录。
- `time_synced` / `esp_time_ms` 策略完善。
- offline/reconnect 压测。
- voice turn 与 heartbeat/snapshot 并发隔离。

验收：

- `csi.motion` 不影响 BME/voice 主链路。
- future timestamp 不造成假在线。
- C5 断 S3 后能优先重连。
- voice turn 超时或 Server 不可用时，S3 SoftAP 和 C5 本地状态不被拖垮。

## 8. 验收命令

后端 curl 验收命令基于 `api.md` 当前接口，假设：

```sh
BASE_URL=http://127.0.0.1:3000
```

`POST /api/device/v1/gateway-state`:

```sh
curl -sS -X POST "$BASE_URL/api/device/v1/gateway-state" \
  -H 'Content-Type: application/json' \
  -d '{
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
        "device_id": "sensair_c51_01",
        "local_id": 1,
        "name": "Sensair C51",
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
          "air_quality_source": "esp"
        },
        "occupancy": {
          "state": "unknown",
          "available": false,
          "motion_score": null,
          "variance": null,
          "rssi": -58,
          "sample_count": 0,
          "updated_at": null
        }
      }
    ],
    "home_summary": {
      "online_device_count": 1,
      "offline_device_count": 0,
      "avg_temperature": 29.57,
      "avg_humidity": 48.2,
      "avg_air_quality": 72
    },
    "history": [],
    "recent_voice_events": [],
    "recent_commands": []
  }'
```

`GET /api/dashboard/v1/overview`:

```sh
curl -sS "$BASE_URL/api/dashboard/v1/overview"
```

`GET /api/device/v1/status`:

```sh
curl -sS "$BASE_URL/api/device/v1/status"
```

`POST /api/logs/v1/system`:

```sh
curl -sS -X POST "$BASE_URL/api/logs/v1/system" \
  -H 'Content-Type: application/json' \
  -d '{
    "device_id": "sensair_c51_01",
    "level": "info",
    "message": "child online",
    "payload": {"reason":"local_frame_seen"},
    "source": "s3_gateway"
  }'
```

`POST /api/logs/v1/alarms`:

```sh
curl -sS -X POST "$BASE_URL/api/logs/v1/alarms" \
  -H 'Content-Type: application/json' \
  -d '{
    "device_id": "sensair_c51_01",
    "level": "warning",
    "title": "C5 offline",
    "message": "child did not report before offline timeout",
    "room_id": "living_room",
    "room_name": "客厅",
    "acknowledged": false,
    "payload": {"offline_timeout_ms":30000},
    "source": "s3_gateway"
  }'
```

`GET /api/logs/v1/system`:

```sh
curl -sS "$BASE_URL/api/logs/v1/system?limit=20"
```

`GET /api/logs/v1/alarms`:

```sh
curl -sS "$BASE_URL/api/logs/v1/alarms?limit=20"
```

`GET /api/smart-home/v1/commands/pending`:

```sh
curl -sS "$BASE_URL/api/smart-home/v1/commands/pending?gateway_id=sensair_s3_gateway_01&limit=20"
```

`POST /api/smart-home/v1/commands/:command_id/ack`:

```sh
COMMAND_ID=shcmd_uuid
curl -sS -X POST "$BASE_URL/api/smart-home/v1/commands/$COMMAND_ID/ack" \
  -H 'Content-Type: application/json' \
  -d '{
    "status": "failed",
    "executed_at_ms": 1780000000200,
    "result": {"applied": false},
    "error_message": "no real smart-home device attached"
  }'
```

`curl -N /api/events/v1/stream`:

```sh
curl -N "$BASE_URL/api/events/v1/stream"
```

ESP 本地只读检查命令：

```sh
# 查找当前 S3 server client 相关文件
rg -n "server_client|gateway_server_client|esp_http_client|/api/" ESPS3/components
grep -RInE "server_client|gateway_server_client|esp_http_client|/api/" ESPS3/components

# 查找 gateway_state/dashboard_snapshot
rg -n "gateway-state|gateway.dashboard_snapshot|dashboard_snapshot|gateway_state" ESPS3/components ESPC51/components ESPC52/components
grep -RInE "gateway-state|gateway.dashboard_snapshot|dashboard_snapshot|gateway_state" ESPS3/components ESPC51/components ESPC52/components

# 查找 child_registry last_seen
rg -n "child_registry|last_seen|offline|link_lost|voice_busy" ESPS3/components
grep -RInE "child_registry|last_seen|offline|link_lost|voice_busy" ESPS3/components

# 查找 C5 上报 frame 字段
rg -n "device_id|payload_type|request_seq|esp_uptime_ms|time_synced|room_id|air_quality_score|motion_score|voice_turn_id" ESPC51/components ESPC52/components
grep -RInE "device_id|payload_type|request_seq|esp_uptime_ms|time_synced|room_id|air_quality_score|motion_score|voice_turn_id" ESPC51/components ESPC52/components

# 顶层工作区不是 git 仓库时该命令会失败；若在某个子仓库内执行则用于确认改动范围
git diff --stat
```

本轮文档检查命令：

```sh
ls -l docs/esp-backend-api-integration-plan.md
sed -n '1,220p' docs/esp-backend-api-integration-plan.md
git diff -- docs/esp-backend-api-integration-plan.md
git status --short
```

## 9. 风险与约束

- 不要让 C5 直连 Server。
- 不要让 C5 直连家庭 WiFi；C5 只连接 S3 SoftAP。
- 不要上传 raw CSI/IQ/相位/子载波数组。
- 不要用 ESP 未来时间判断 online；`server_recv_ms` 由后端生成，`esp_time_ms` 仅在 `time_synced=true` 且合理时参与延迟统计。
- 不要用 mock appliances 冒充真实智能家居；mock 必须显式标注，或不上传。
- voice turn 不能阻塞 heartbeat/snapshot/log 上报。
- 日志/报警要限频，断网时要降级，避免重连后刷爆后端。
- S3 断 Server 时要本地降级，不影响 C5 SoftAP。
- `POST /api/device/v1/ingest` 当前已接入 `sensor.bme690` 和 `csi.motion`；若未来需要 `gateway.heartbeat` 这类新 payload，必须先改后端契约和实现，再让 S3 调用。
- 后端 `public/`、ESP-server 代码、C/C++ 固件源码、Kconfig、分区表、build 产物和烧录流程不属于本文档本轮改动范围。

## 10. 本轮实现状态（2026-06-14）

- ESPS3 已承担 Server-facing HTTP、dashboard snapshot、system/alarm log、smart-home pending/failed ack、voice turn 代理、wake prompt cache 和 CSI summary 上报。
- ESPC51/ESPC52 仅保留 C5->S3 轻量 frame 字段和断联重连优先级；不直连家庭 WiFi，不直连 Server。
- C5 侧不上传 raw CSI；CSI 只以 `csi.motion` 摘要进入 S3 -> Server `POST /api/device/v1/ingest`。
- 无真实智能家居设备时，command ack 返回 `failed`，不伪造 success。
- voice turn 与 snapshot/log 分离调度，避免被语音请求阻塞。
- `gateway.heartbeat` 未加入 `/api/device/v1/ingest`，仍只允许 `sensor.bme690` 和 `csi.motion`。
- 本轮未修改 `ESP-server/public`、Kconfig 或分区表。

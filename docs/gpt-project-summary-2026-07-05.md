# ESP-111 项目结构与功能总结

生成日期：2026-07-05

用途：给新的 GPT/Codex 会话快速理解 `/Users/zhiqin/ESP-111` 当前项目。本文基于当前源码与关键文档梳理；旧计划文档只能作为历史参考，修改前仍必须重新读 live source。

## 1. 一句话概览

ESP-111 是一个三层 IoT / 智能感知系统：

```text
ESPC51 / ESPC52 两个 ESP32-C5 终端
  -> 连接 ESPS3 的 SoftAP
  -> 通过 /local/v1/* 本地轻量协议上报传感、语音和命令状态

ESPS3 ESP32-S3 本地网关
  -> 接收 C5 本地请求
  -> 做协议适配、设备注册、聚合、语音代理、命令转发
  -> 唯一负责访问 ESP-server 的 /api/*

ESP-server Node/Express 后端
  -> 提供 HTTP API、SQLite 持久化、Dashboard 聚合、语音/LLM/命令/事件/记忆等业务
  -> 同时托管 public Dashboard 前端
```

最重要边界：`ESPC51/ESPC52` 不能直连 `ESP-server`，也不能构造 `/api/...` Server 请求；所有 Server-facing HTTP 都必须在 `ESPS3` 层完成。

## 2. 顶层目录地图

```text
/Users/zhiqin/ESP-111/
  ESPC51/                         # ESP32-C5 终端 1 固件，device_id=sensair_shuttle_01
  ESPC52/                         # ESP32-C5 终端 2 固件，device_id=sensair_shuttle_02
  ESPS3/                          # ESP32-S3 本地网关固件，gateway_id=sensair_s3_gateway_01
  ESP-server/                     # Node.js + Express + SQLite 后端，唯一嵌套 git repo
  docs/                           # 项目架构、API 边界、迁移计划、审计报告
  shared_components/              # 协议组件参考副本，不是当前固件构建依赖
  archive/                        # 历史归档模块，不应作为 active runtime 使用
  tools/csi-debug-web/            # 本地 CSI 日志/串口调试网页工具
  .codex-skills/project-memory/   # 项目记忆、规则、历史 handoff
```

注意：顶层 `/Users/zhiqin/ESP-111` 不是 git 仓库。只有 `ESP-server/` 是嵌套 git 仓库。做 git 检查时必须进入具体子仓库或明确说明顶层无 git。

## 3. 三层运行拓扑

### 3.1 Device layer：ESPC51 / ESPC52

两个目录都是 ESP32-C5 终端固件，逻辑基本相同，默认身份不同：

- `ESPC51`: `sensair_shuttle_01`，本地短 ID 为 `1`。
- `ESPC52`: `sensair_shuttle_02`，本地短 ID 为 `2`。

主要职责：

- 连接 `ESPS3` SoftAP：默认 SSID `SensaiHub_S3_01`，默认网关 IP `192.168.4.1`。
- 采集 BME690 环境数据。
- 运行 Mic / VAD / WakeNet 语音采集链路。
- 播放 S3 或 Server 回传的 PCM 音频。
- 通过本地命令轮询接收简单系统/显示/音频命令，并回 ACK。
- 保留 LCD/display placeholder。
- 保留 CSI placeholder / CSI phase A 摘要算法代码，但不能上传 raw CSI。

启动链路：

```text
app_main
  -> app_startup_task
    -> app_orchestrator_start
      -> wifi_manager_init
      -> gateway_link_start
      -> wifi_connect_to_ap              # 只连 S3 SoftAP
      -> gateway_link_wait_ready          # health + register 成功后才进入业务
      -> system_service_init              # register / heartbeat / status / command polling
      -> optional CSI gate
      -> bme_sensor_service_start
      -> voice_chain_start
```

关键模块：

- `main/main.c`: 固件入口，只创建启动任务。
- `components/app_config/`: 运行开关、日志/栈配置。
- `components/app_time_sync/`: 时间同步辅助。
- `components/BSP/IIC`, `components/BSP/IIS`: BME/I2C 与 speaker/I2S 硬件支撑。
- `components/Middlewares/app_orchestrator/`: C5 启动编排。
- `components/Middlewares/terminal_config/`: 终端身份、网关地址、上传周期等配置。
- `components/Middlewares/wifi/`: STA WiFi，连接 S3 SoftAP。
- `components/Middlewares/server_comm/`: 名字仍叫 server_comm，但当前是 C5 -> S3 本地 HTTP 客户端；会拒绝 `http://` 或 `https://` 绝对 endpoint。
- `components/Middlewares/sensor_domain/bme690/`: BME690 驱动、空气质量计算、上传客户端。
- `components/Middlewares/sensor_domain/csi_placeholder/`: CSI 占位上报接口。
- `components/Middlewares/sensor_domain/csi_phase_a/`: CSI Phase A 本地摘要算法、离线测试、结果编码；不应上传 raw CSI。
- `components/Middlewares/mic/`: Mic ADC、PCM 转换、VAD、语音流状态。
- `components/Middlewares/wake/`: 本地 wake word 和 wake prompt cache。
- `components/Middlewares/server_voice/`: C5 -> S3 `/local/v1/voice/turn` raw PCM 客户端。
- `components/Middlewares/speaker/`: PCM 播放与重采样。
- `components/Middlewares/voice_domain/`: 半双工语音链路编排。
- `components/Middlewares/command_domain/system_command/`: 注册、心跳、状态、命令轮询和 ACK。
- `components/Middlewares/runtime/`: 语音独占期间暂停/恢复非语音任务。
- `components/Middlewares/display_placeholder/`: 屏幕/AI 显示占位层。
- `components/esp111_protocol_common/`: C5 本地协议常量副本。

当前 CSI 开关需要特别核对：

- `ESPC51/components/app_config/app_main_config.h` 默认 `MAIN_ENABLE_CSI_SERVICE=1`，`ESPC51/sdkconfig.defaults` 启用 `CONFIG_ESP_WIFI_CSI_ENABLED=y`。
- `ESPC52/components/app_config/app_main_config.h` 默认 `MAIN_ENABLE_CSI_SERVICE=0`，`ESPC52/sdkconfig.defaults` 未启用 WiFi CSI。
- 因此不要只根据旧 docs 断言“两块 C5 都默认关闭 CSI”；修改前要读当前源码。

### 3.2 Gateway layer：ESPS3

`ESPS3/` 是 ESP32-S3 本地网关固件，是整个固件侧唯一可以访问 `ESP-server` 的层。

主要职责：

- 开 SoftAP 给 C5 连接。
- 可配置 STA 连接外部 WiFi，用于访问云端 Server。
- 暴露 `/local/v1/*` 本地 HTTP 服务。
- 管理 C5 child registry / allowlist / online 状态。
- 将 C5 短字段协议映射成 Server 完整 v1 JSON。
- 聚合传感器、设备状态、语音事件，定期上传 dashboard snapshot / gateway-state。
- 代理语音：C5 上传 PCM 到 S3，S3 再转发到 Server `/api/voice/turn`，并把 PCM 响应流回 C5。
- 路由命令：S3 从 Server 拉 pending command，再让 C5 轮询本地 command pending，C5 ACK 后 S3 再 ACK Server。
- 管理 smart-home 命令，但没有真实智能家居设备时不能伪造成功。
- 上报 system / alarm event log。
- 处理 CSI trigger/result placeholder 或 summary ingest，禁止 raw CSI 上云。

启动链路：

```text
app_main
  -> gateway_startup_task
    -> gateway_orchestrator_start
      -> offline_policy_init
      -> gateway_event_reporter_init
      -> child_registry_init
      -> command_router_init
      -> sensor_aggregator_init
      -> smart_home_gateway_init
      -> voice_proxy_init
      -> wake_prompt_cache_gateway_init
      -> optional csi_placeholder_gateway_init
      -> gateway_wifi_start                 # SoftAP + optional STA
      -> local_http_server_start            # 暴露 /local/v1/*
      -> gateway_periodic_task              # snapshot + smart-home polling
```

关键模块：

- `components/Middlewares/gateway_config/`: 网关身份、SoftAP、Server base URL、STA 凭据、CSI 开关、allowlist。
- `components/Middlewares/gateway_wifi/`: SoftAP + STA 网络。
- `components/Middlewares/local_http_server/`: 注册 `/local/v1/*` handlers，只面向本地 C5。
- `components/Middlewares/child_registry/`: C5 注册和在线状态。
- `components/Middlewares/protocol_adapter/`: C5 短 JSON 到 Server v1 envelope 的转换。
- `components/Middlewares/server_client/`: S3 -> ESP-server HTTP 客户端，负责 `/api/*`。
- `components/Middlewares/gateway_event_reporter/`: system/alarm event 上报。
- `components/Middlewares/sensor_aggregator/`: BME、device status、voice event、occupancy 等聚合，上传 gateway snapshot。
- `components/Middlewares/smart_home_gateway/`: smart-home pending poll 和 ACK。
- `components/Middlewares/voice_proxy/`: voice turn 独占、PCM 转发、非语音任务 skip gate。
- `components/Middlewares/wake_prompt_cache_gateway/`: wake prompt cache 代理。
- `components/Middlewares/command_router/`: 通用 command pending / local queue / ACK。
- `components/Middlewares/offline_policy/`: Server 可用性和错误状态记录。
- `components/Middlewares/csi_placeholder_gateway/`: CSI trigger/result 边界。
- `components/Middlewares/gateway_orchestrator/`: S3 启动编排。
- `components/esp111_protocol_common/`: S3 协议常量副本，额外包含 Server `/api/*` 路径常量。

当前 S3 配置要点：

- 默认 gateway id：`sensair_s3_gateway_01`。
- SoftAP：`SensaiHub_S3_01` / `sensaihub123` / `192.168.4.1`。
- 默认 Server base URL：`http://124.221.162.188:3000`。
- `GATEWAY_CONFIG_ENABLE_CSI_TRIGGER=1`。
- `GATEWAY_CONFIG_ENABLE_CSI_RESULT_INGEST=1`。
- CSI trigger 默认目标为 C51，间隔 `10 ms`，修改前必须确认硬件和网络压力。

### 3.3 Server layer：ESP-server

`ESP-server/` 是 Node.js + Express + SQLite 后端，同时托管 dashboard 静态前端。

启动入口：

- `ESP-server/server.js`
- `npm start` -> `node server.js`
- 默认端口：`PORT` 环境变量或 `3000`
- `npm test` / `npm run test:smoke` -> `node scripts/smoke-regression.js`

启动时做的事：

- 读取 `.env`。
- 创建 SQLite 连接和 db helpers。
- 在 JSON parser 前挂载 voice router，以支持 `/api/voice/turn` raw PCM body。
- 挂载 `express.json()`。
- 托管 `public/` dashboard。
- 挂载所有 API routes。
- 初始化/迁移 SQLite 表。
- 写入 `system_log_created` 启动事件。

后端目录：

```text
ESP-server/
  server.js                         # Express app 入口
  package.json
  db/                               # SQLite 数据库目录，真实 database.db 不要乱动
  docs/api.md                       # 当前后端 API 参考
  public/                           # Dashboard 前端，默认不要改
  scripts/smoke-regression.js       # 后端烟测
  server-time-sync/timeSync.js      # /api/time/*
  src/
    routes/                         # Express route 层
    services/                       # 业务服务/聚合/校验
    db/                             # SQLite 表结构与 DAO
    voice/                          # ASR/LLM/TTS/mock voice turn 相关
    llm/                            # LLM 客户端
    commands/                       # command schema/queue
    memory/                         # memory store
    agent/                          # agent state store
    jobs/                           # memory jobs
    utils/                          # envelope/date/env/logging
```

主要 route 家族：

- `/api/device/v1/*`: device ingest、gateway-state、设备状态、模块状态、设备上下文、最新传感器。
- `/api/dashboard/v1/*`: Dashboard 前端统一读取层和 snapshot 写入。
- `/api/voice/*`: raw PCM voice turn、prompt config、prompt/prompt-cache。
- `/api/commands/*`: 命令白名单、能力注册、创建、pending、ACK、history、自然语言命令、recent。
- `/api/smart-home/v1/*`: smart-home status/state/control/commands/pending/ack。
- `/api/logs/v1/*`, `/api/events/v1/stream`, `/api/voice/v1/events`: event/log/SSE。
- `/api/llm/text`, `/api/llm/structured`: 文本 LLM 与结构化 LLM。
- `/api/conversation/*`, `/api/memory/*`, `/api/jobs/*`: 对话记忆与 memory job。
- `/api/environment/*`, `/api/reminders/*`, `/api/emergency/*`, `/api/csi/behavior`, `/api/lcd/*`: agent state / 环境 / 提醒 / LCD 等服务端状态。
- `/api/user-data/*`: 用户数据摘要、删除预览、删除、删除记录、导出；该组有 admin guard。
- `/api/time/*`: 时间同步。
- legacy `/sensor`, `/sensor/latest`, `/sensor/history`, `/asr`, `/asr/latest`, `/llm`, `/llm/latest`: 兼容旧设备/旧 dashboard/旧脚本，不能随便删除。

主要数据表：

- `sensor_records`: BME690 与 `csi.motion` 等 device ingest 记录。
- `device_status`, `device_module_status`: 设备和模块在线/状态。
- `gateway_auth`, `gateway_device_bindings`: 网关认证和设备绑定。
- `dashboard_snapshots`: S3 gateway dashboard snapshot 持久化。
- `event_logs`: alarm/system/command/voice/device/csi 等事件日志和 SSE 来源。
- `voice_turns`: `/api/voice/turn` 诊断记录。
- `asr_records`, `llm_records`: legacy ASR/LLM 最新记录。
- `device_capabilities`, `command_queue`: 通用命令能力与队列。
- `smart_home_devices`, `smart_home_commands`, `natural_language_commands`: smart-home 和自然语言命令。
- `conversation_turns`, `daily_memory`, `long_term_profile`, `memory_corrections`, `memory_job_runs`: memory 功能。
- `environment_profile`, `experience_memory`, `relation_memory`, `reminder_rules`, `reminder_records`, `emergency_events`, `csi_behavior_events`, `lcd_status`: agent state。
- `data_deletion_runs`: 用户数据删除任务记录。

Dashboard 前端：

- 位于 `ESP-server/public/`。
- 当前主要读取 `/api/dashboard/v1/sensors/latest`、`/api/dashboard/v1/asr/latest`、`/api/dashboard/v1/llm/latest` 等新 dashboard v1 接口。
- 除非用户明确要求前端工作，否则不要修改 `ESP-server/public/`。

## 4. 协议和数据流

### 4.1 C5 -> S3 本地协议

C5 只访问 S3 本地 HTTP：

```text
/local/v1/health
/local/v1/register
/local/v1/heartbeat
/local/v1/status
/local/v1/sensor
/local/v1/csi/result
/local/v1/voice/turn
/local/v1/audio/wake-prompt
/local/v1/commands/pending
/local/v1/commands/{command_id}/ack
```

说明：协议头中保留了 `/local/v1/voice/prompt-cache` 常量，但当前 `local_http_server` 实际注册并由 C5 使用的是 `GET /local/v1/audio/wake-prompt`。

本地 JSON 字段是短字段，例如：

- `p`: local schema version
- `id`: C5 短 ID
- `t`: message type
- `u`: uptime ms
- `q`: request seq
- `pt`: payload type
- `v`: values
- `cid`: command id
- `c`: command code
- `a`: command args
- `ok`: success flag
- `e`: error/event
- `cmds`: command list

常见本地类型：

- `reg`: register
- `hb`: heartbeat
- `st`: status
- `bme`: BME690 sensor
- `csi`: CSI result summary

### 4.2 S3 -> Server 协议

S3 使用完整 `/api/*` 路径和完整 JSON / raw PCM：

```text
POST /api/device/v1/ingest
POST /api/device/v1/gateway-state
POST /api/logs/v1/system
POST /api/logs/v1/alarms
POST /api/voice/turn
GET  /api/smart-home/v1/commands/pending
POST /api/smart-home/v1/commands/{command_id}/ack
GET  /api/commands/pending
POST /api/commands/{command_id}/ack
```

这组 Server 路径常量只应存在于 S3 侧协议头/`server_client` 使用路径中。C5 侧协议头不应获得这些 Server route 常量。

### 4.3 传感器数据流

```text
C5 BME690
  -> bme_sensor_service
  -> C5 /local/v1/sensor 短 JSON
  -> S3 local_http_server
  -> protocol_adapter 展开为完整 payload
  -> server_client POST /api/device/v1/ingest payload_type=sensor.bme690
  -> ESP-server sensorBme690Service + sensor_records
  -> dashboardService / api/dashboard/v1/*
  -> Dashboard UI
```

### 4.4 语音数据流

```text
C5 Mic/VAD/Wake
  -> voice_chain 进入语音独占
  -> server_voice_client 上传 PCM 到 S3 /local/v1/voice/turn
  -> S3 voice_proxy 独占并转发到 Server /api/voice/turn
  -> ESP-server voice chain 或 VOICE_TURN_MOCK
  -> Server 返回 PCM
  -> S3 转回 C5
  -> C5 speaker 播放
  -> voice_chain 释放语音独占，恢复 BME/heartbeat/command 等非语音任务
```

语音期间的原则：

- 不阻塞 WiFi / Mic / wake / speaker 主链路。
- 非语音上报、snapshot、smart-home poll 等应按 voice busy gate 跳过或延迟。
- Server 侧 raw PCM route 必须挂在 JSON parser 前。

### 4.5 命令数据流

```text
Server 创建 command_queue / smart_home_commands
  -> S3 poll /api/commands/pending 或 /api/smart-home/v1/commands/pending
  -> S3 command_router / smart_home_gateway 本地排队
  -> C5 poll /local/v1/commands/pending
  -> C5 执行 noop/show_text/play_audio/set_volume/config_set 等有限命令
  -> C5 ACK /local/v1/commands/{id}/ack
  -> S3 ACK Server /api/commands/{id}/ack 或 smart-home ack
```

没有真实 smart-home 设备时，S3 不应伪造执行成功，应返回失败 ACK，并说明没有真实设备连接。

### 4.6 CSI 数据流

当前要非常谨慎：

- CSI Phase A 代码目标是本地摘要：occupancy / motion_score 等。
- 禁止上传 raw CSI、I/Q 数组、相位序列、完整子载波数据。
- Server 接受的 CSI 主体应是 `payload_type="csi.motion"` 这样的摘要事件，而不是原始 CSI。
- `ESPC51` 与 `ESPC52` 当前 CSI 默认开关不同。
- `ESPS3` 当前 CSI trigger/result ingest 默认开启。做任何 CSI 相关改动前先重新核对当前宏。

## 5. 协议头和同步规则

active 固件协议头有三份：

```text
ESPC51/components/esp111_protocol_common/include/esp111_protocol_common.h
ESPC52/components/esp111_protocol_common/include/esp111_protocol_common.h
ESPS3/components/esp111_protocol_common/include/esp111_protocol_common.h
```

当前事实：

- `ESPC51` 和 `ESPC52` 协议头内容一致。
- `ESPS3` 协议头比 C5 多 Server route 常量，这是边界设计的一部分。
- `shared_components/esp111_protocol_common/` 是参考/归档，不是当前三套固件的 active 构建依赖。

修改建议：

- C5 local 协议字段变更：同步 C51/C52，并确认 S3 `protocol_adapter` 和 `local_http_server`。
- S3 Server route 变更：通常只应改 S3 协议头和 S3 `server_client`，不应把 `/api/*` 常量同步到 C5。
- 若用户要求“协议完全同步”，先确认是否允许破坏“C5 不知道 Server route”的边界。

## 6. 重要文档

高价值文档：

- `docs/api-boundary-v1.md`: 当前 API 边界规范，解释 Device/Gateway/Server/Frontend 归属。
- `docs/esp-111-firmware-architecture.md`: 固件结构清理记录和模块说明，但部分 CSI 默认状态可能已漂移。
- `docs/esp-backend-api-integration-plan.md`: 固件与后端集成计划。
- `docs/project-audit-2026-06-16-report.md`: 早期审计报告。
- `ESP-server/docs/api.md`: 后端 API 参考，接口字段变化应同步这里。
- `.codex-skills/project-memory/gpt-project-handoff-2026-06-16.md`: 旧 handoff，结构有参考价值，但必须用当前源码刷新事实。
- `.codex-skills/project-memory/project-rules.md`: 项目硬规则和保护路径。
- `.codex-skills/project-memory/architecture-current.md`: 架构快照，含历史风险说明。

阅读顺序建议：

```text
1. 本文
2. docs/api-boundary-v1.md
3. ESP-server/server.js
4. ESP-server/src/routes/*
5. ESPS3/components/Middlewares/gateway_orchestrator/gateway_orchestrator.c
6. ESPS3/components/Middlewares/local_http_server/local_http_server.c
7. ESPS3/components/Middlewares/server_client/server_client.c
8. ESPC51/components/Middlewares/app_orchestrator/app_orchestrator.c
9. ESPC52/components/Middlewares/app_orchestrator/app_orchestrator.c
10. 三份 esp111_protocol_common.h
```

## 7. 保护边界和协作习惯

默认不要改：

- `ESP-server/public/`，除非用户明确要求前端。
- `ESP-server/db/database.db` 和真实数据库文件。
- `node_modules/`。
- `managed_components/`。
- `archive/`。
- 各工程 `build/` 产物。

默认不要执行：

- `idf.py fullclean`
- `idf.py erase-flash`
- `idf.py flash`
- `idf.py monitor`
- 会改真实 DB 或线上服务状态的命令。

做后端修改时：

- 先读 `ESP-server/server.js` 和相关 route/service/db。
- 保持 `ESP-server/public/` 不动，除非范围明确放开。
- 优先用临时 SQLite DB 做 smoke。
- 至少跑 `node --check` 或相关 smoke test。

做固件修改时：

- C5 改动通常要同时检查 `ESPC51` 和 `ESPC52`。
- S3 是唯一 Server-facing 固件层。
- C5 不应新增 `/api/*` endpoint 或 Server base URL。
- 语音链路不要阻塞 heartbeat/snapshot/log 等核心路径，应该通过已有 voice busy gate 协调。
- CSI 只能 summary，不上传 raw CSI。

## 8. 常用验证命令

后端静态检查：

```bash
cd /Users/zhiqin/ESP-111/ESP-server
node --check server.js
find src scripts server-time-sync -name '*.js' -print0 | xargs -0 -n1 node --check
npm test
```

固件构建环境：

```bash
source /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh >/dev/null
```

固件构建：

```bash
cd /Users/zhiqin/ESP-111/ESPC51 && idf.py build
cd /Users/zhiqin/ESP-111/ESPC52 && idf.py build
cd /Users/zhiqin/ESP-111/ESPS3 && idf.py build
```

协议边界扫描示例：

```bash
rg -n '"/api/|/api/' ESPC51/components ESPC52/components
rg -n '"/local/v1|/local/v1' ESPS3/components ESPC51/components ESPC52/components
diff -u ESPC51/components/esp111_protocol_common/include/esp111_protocol_common.h ESPC52/components/esp111_protocol_common/include/esp111_protocol_common.h
diff -u ESPC51/components/esp111_protocol_common/include/esp111_protocol_common.h ESPS3/components/esp111_protocol_common/include/esp111_protocol_common.h
```

注意：`diff` 中 S3 多出的 Server route 常量是预期差异，不一定是错误。

## 9. 给 GPT 的接手提示

可以把下面这段直接贴给新的 GPT：

```text
你正在接手 /Users/zhiqin/ESP-111。

这是一个 ESPC51/ESPC52 -> ESPS3 -> ESP-server 的三层项目。C5 终端只连 S3 SoftAP，只访问 /local/v1/*，不能直连 Server，不能新增 /api/* Server-facing 调用。ESPS3 是唯一固件侧 Server gateway，负责 protocol_adapter、server_client、sensor_aggregator、voice_proxy、command_router、smart_home_gateway。ESP-server 是 Node/Express/SQLite 后端，负责 /api/device/v1、/api/dashboard/v1、/api/voice、/api/commands、/api/smart-home/v1、/api/logs/v1、/api/events/v1/stream、memory/agent/user-data 和 legacy /sensor /asr /llm。

修改前先读 docs/api-boundary-v1.md、ESP-server/server.js、相关 ESP-server/src/routes + services + db、ESPS3 gateway_orchestrator/local_http_server/server_client/protocol_adapter，以及 ESPC51/ESPC52 app_orchestrator/server_comm/terminal_config。顶层不是 git repo，ESP-server 是唯一 nested git repo。

默认不要改 ESP-server/public、ESP-server/db/database.db、node_modules、managed_components、archive、build。不要运行 idf.py fullclean/erase-flash/flash/monitor，除非用户明确要求。

CSI 相关必须重新读当前源码：ESPC51 当前默认 MAIN_ENABLE_CSI_SERVICE=1 且 sdkconfig 启用 WiFi CSI；ESPC52 默认 MAIN_ENABLE_CSI_SERVICE=0；ESPS3 当前默认开启 CSI trigger/result ingest。CSI 只能上传 summary/csi.motion，禁止 raw CSI/IQ/phase/subcarrier 上云。

后端修改要有 node --check / smoke 证据；固件修改要说明 build proof 与真实硬件 proof 的区别。
```

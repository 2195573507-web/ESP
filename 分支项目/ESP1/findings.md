# 发现与决策

## 本轮需求

- 熟悉 `/Users/zhiqin/ESP-111` 当前项目。
- 本轮按只读接管扫描执行，不修改业务源码。

## 待验证的历史线索

- 项目可能是 `ESPC51/ESPC52 -> ESPS3 -> ESP-server` 三层结构。
- 顶层目录与 `ESP-server` 可能是两个独立 Git 仓库。
- 近期变更集中在 C5/S3 runtime event bus、CSI 三层边界和 CanonicalEvent v2，需要以当前源码复核。
- C5 不应直接访问 Server API，原始 CSI 不应离开边缘采集层。

## 研究发现

- Git 边界：顶层 `/Users/zhiqin/ESP-111/.git` 当前为 `main...origin/main`；嵌套 `/Users/zhiqin/ESP-111/ESP-server/.git` 当前为 `api...origin/api`。两个状态必须分开解读。
- 顶层核心目录包括 `ESPC51`、`ESPC52`、`ESPS3`、`ESP-server`、`shared_components`、`tools/csi-debug-web`、`docs` 与只作历史参考的 `archive/legacy_modules_20260610`。
- 当前工作区本来就很脏：C51/C52/S3 的 runtime、CSI、BME、协议、网关代码均有修改或新增；后端有 device-status/dashboard 相关改动；`ESP-server/public` 也有顶层可见改动。本轮不归因、不回退。
- 新增源码名显示当前演进方向包括：C5 `c5_event_bus`/`c5_runtime_workers`/`csi_edge_detector`，S3 `s3_event_bus`/`bme_cache_manager`/`network_replay_worker`。
- 三个固件目录均为独立 ESP-IDF application，具备各自 `CMakeLists.txt`、`main/main.c`、`sdkconfig.defaults`、分区表与 components。
- 公共协议头同时存在于顶层 `shared_components/esp111_protocol_common` 与三个固件工程内的组件副本，需进一步核对真实构建引用和一致性。
- C51/C52 的 `main/main.c` 当前逐行同构：`app_main` 创建 `app_startup` 任务，任务调用 `app_orchestrator_start()`；C5 入口本身不承载 WiFi、BME、CSI、语音或协议逻辑。
- S3 的 `main/main.c` 同样保持薄入口：创建 `gateway_startup` 任务后调用 `gateway_orchestrator_start()`，本地协议与 Server 协议均在 middleware 内终止。
- 三个工程的根 CMake 均通过 ESP-IDF `project.cmake` 构建；C51/C52 project 名仍为泛化的 `00_Learn`，S3 为 `sensair_s3_gateway`。
- 2026-07-05 总览文档中的“顶层不是 Git 仓库”已过时；当前 live source 明确存在顶层 `.git`。其他宏、默认开关和模块职责也必须继续由源码验证。
- 现有文档仍提供稳定导航：`docs/api-boundary-v1.md` 是 API 归属规范，`docs/gpt-project-summary-2026-07-05.md` 是三层总览，`docs/c5-s3-code-smell-audit-2026-07-09-report.md` 是近期可维护性风险清单。
- 顶层 Git 实际追踪大量 `ESP-server/*` 文件，同时 `ESP-server` 自身也有独立 `.git`；这是双重版本归属，未来提交/上传必须分别检查两套 diff，不能只看其中一个仓库。
- 顶层 `src/` 当前只有未被 Git 追踪的 `.DS_Store`，不是 active backend；有效后端源码在 `ESP-server/src/`。
- C51 与 C52 的 active `esp111_protocol_common.h` SHA-256 完全一致；S3 版本有意不同，包含 Server-facing `/api/*` 和 `/kernel/csi_event` 路由；顶层 `shared_components` 版本也不同且只是参考副本。
- `tools/csi-debug-web` 是 Node.js + `serialport` 的本地调试工具，默认监听 `127.0.0.1:8787`，只读串口/粘贴日志/模拟帧并保存在内存，不连接 `ESP-server`、正式 Dashboard 或固件。
- 2026-07-09 固件审计给出的主要维护风险与当前文件体量一致：S3 `network_worker.c`、`s3_scheduler.c`、`protocol_adapter.c` 职责偏重；C51/C52 的风险主要是镜像复制后单边漂移，而不是当前 parity 已失控。
- 现有显式保护包括：C5 URL builder 拒绝绝对 HTTP(S) endpoint 并限定 `/local/v1`；S3 protocol adapter 拒绝 `raw_csi`、子载波、I/Q、phase 等原始 CSI 字段。
- 当前 C51/C52 live config 都是 `MAIN_ENABLE_CSI_SERVICE=1`、`CSI_REPORT_INTERVAL_MS=100U`、HTTP/log 输出开启，算法版本 `edge_feature_v2`；这已经覆盖了旧文档中 C52 关闭或 250 ms 周期的描述。
- 当前 S3 初始化链已扩展为：offline/event reporter -> BME cache -> registry/router/aggregator/smart-home/voice/prompt/device-stream/CSI -> scheduler/network/replay workers -> WiFi -> scheduler/local HTTP/device stream/CSI start。
- 当前 S3 `GATEWAY_CONFIG_CSI_TRIGGER_INTERVAL_MS=50U`、result ingest 开启；STA SSID/password 默认空，Server base URL 仍硬编码公网 HTTP 地址，auth token 默认空。
- S3 本地 HTTP 当前注册 health、register、heartbeat、status、sensor、CSI result、device stream、voice turn、wake prompt、command pending/ACK；device stream 另有 UDP 33434 接收面。
- 正式 CSI 上云已从通用 ingest 拆出：S3 调用 `server_client_post_csi_event_json()` 到 `POST /kernel/csi_event`；后端 live route 中 `/api/device/v1/ingest` 明确只接受 `sensor.bme690` 并提示 CSI 改用 `/kernel/csi_event`。
- `ESP-server/docs/api.md` 仍在若干位置描述通用 ingest 接受 `csi.motion`，与当前 `deviceRoutes.js` 和 `csiMotionService.js` 不一致，是需要后续同步的文档漂移。
- 后端是 Express 5 + sqlite3；voice raw PCM router 必须先于 `express.json()`，其余 routes 再挂载，启动时逐表执行 ensure/migration。`npm test` 与 `npm run test:smoke` 都运行 `scripts/smoke-regression.js`。
- 三套固件当前均存在 `build/compile_commands.json` 和 `.bin`，可用于静态定位或增量构建参考；但这些产物可能早于当前 8k+ 行未提交改动，不能当作当前工作区通过构建的证据。
- C51/C52 `sdkconfig.defaults` 都启用 WiFi CSI 与 quad PSRAM；S3 使用 octal PSRAM。三者均使用自定义 partition table，另有一致的 16 MiB factory/VFS/SPIFFS 分区样板。
- C5 CSI 特征主链包含校准、稳定子载波筛选、baseline/noise floor、平滑 delta、frame energy、variance、CV、RSSI、quality/confidence 和 local state hint；raw I/Q 只在 C5 callback/采集层转为特征，不进入外发合同。
- C5 runtime 已形成 event bus + dispatcher/workers；S3 event bus 按 `CRITICAL > REALTIME > STATE > BACKGROUND` 处理，STATE 可合并最新值，后台事件可在压力下丢弃，并记录 queue/drop/coalesce/worker latency。
- S3 scheduler 内仍集中实现 protocol worker、stream worker、CSI fusion worker；网络 worker 另负责 Server gate、上传、命令、snapshot 和 CSI latest 等，这解释了近期审计所说的结构性堆叠。
- 固件中的测试主要是 CSI Phase A 离线测试和 Mic 自测代码，不是统一主机测试套件；后端使用 smoke regression，`tools/csi-debug-web` 使用 Node `--test` 的三个测试文件。
- 当前 C5->S3 transport 是混合模式：register、commands、voice、BME、CSI feature 使用 `/local/v1/*` HTTP；heartbeat/status 使用扁平 telemetry frame，优先 UDP/33434，失败回退 `POST /local/v1/stream`。
- BME 仍由 `bme_server_client` 构造包含环境读数和空气质量完整派生字段的本地 JSON，并 `POST /local/v1/sensor`；S3 负责后续展开/缓存/上云。
- CSI feature 有意不走 UDP stream：`csi_server_client` 把 C5 算出的 `frame_energy/variance/cv/rssi/quality/state_hint/motion_score/confidence` 格式化为 v2 envelope，固定 `POST /local/v1/csi/result`。
- 普通 device stream 只允许 `sensor/status/event` 三种扁平类型，UDP 是低延迟主路径且 C5 不做离线缓存重试；HTTP fallback 仍只到 S3 本地 route。
- C5 command client 当前实际执行 `device.noop` 和 `lcd.show_text/display.show_text`，其他命令返回 unsupported；pending/ACK 都通过 S3 本地 HTTP，由 S3 再映射到 Server。
- 仓库没有 `AGENTS.md`、`CLAUDE.md` 或 `GEMINI.md`；本地保护规则位于被 Git 忽略的 `.codex-skills/project-memory/project-rules.md`，最后更新 2026-07-08。
- 项目规则把 `ESPC51/ESPC52/ESPS3/ESP-server/archive/managed_components/node_modules/build` 默认设为只读，并禁止 `idf.py fullclean/erase-flash/flash`、修改真实 DB、修改前端或运行中服务；后续实现必须由用户明确放开相应范围。
- 规则要求 C51/C52 源码 parity；协议三份需要语义同步，但 live source 明确保留 S3-only Server route，所以不能机械做成三份字节一致。
- 固件环境约定为 ESP-IDF v5.5.4；C5 target `esp32c5`，S3 target `esp32s3`。active C5 分区为 5 MiB factory + 2 MiB model + 约 9 MiB storage；S3 为双 7 MiB OTA + 4 MiB storage + coredump。
- 现有 bin 时间为 C51/C52 2026-07-09、S3 2026-07-10，但仍需在真正修改任务中重新构建，不能仅凭文件时间认定当前全部改动已验证。
- S3 新增 `bme_cache_manager`：容量 300 的 RAM ring 同时保存 C5 BME 派生字段和精确 Server ingest JSON；实时上云 2xx 后删除，失败保留并由独立 `network_replay_worker` 请求重放。S3 不重算空气质量。
- S3 network state 采用 `DOWN -> UP -> IP_READY -> LINK_STABLE`，WiFi/IP callback 只投事件；Server ready 还有连续成功/失败去抖，只有稳定后才执行上云工作。
- CSI 网络策略是 latest/coalesce：离线时标 dirty，恢复后上传最新 canonical fact；旧低优先级 backlog 可丢弃/合并，不把高频历史全部重放。
- `child_registry` 是 C5 在线状态唯一真源：registered/online/link_lost/voice_busy/offline_reason 全由 register、heartbeat、link grace 和 voice 状态机生成，snapshot/dashboard 不应再用 Server 或聚合时间戳覆盖。
- S3 fusion 固定 100 ms tick、两条 active link、`IDLE/MOTION/HOLD`；输入包含 C5 motion score/confidence/quality/RSSI/低维 metrics，但 C5 local state 只用于日志提示，不直接决定 S3 最终状态。
- 后端没有独立 controller 层：`routes` 同时承担 HTTP/controller，调用 `services`/`commands`/store，再经 `db` helpers 访问 SQLite；统一事件最终写 `event_logs` 并广播 SSE。
- `POST /kernel/csi_event` 严格接收 CanonicalEvent v2，links 必须连续命名 `link_0...`；写 `csi_motion_events`、`event_logs`、Dashboard 内存态并广播 `csi_motion`。当前 v2 的核心事实是 fused state/confidence，energy/variance/RSSI 入库为 null。
- Dashboard snapshot schema 2 明确禁止携带 CSI state，CSI 只能走 `/kernel/csi_event`；BME ingest、gateway-state、snapshot、CSI 是彼此清晰的写入面。
- 后端路由面还包括通用 command、smart-home 状态机、raw PCM voice turn（ASR -> LLM -> TTS）、memory/jobs/agent state、用户数据删除、time sync 和 legacy sensor/ASR/LLM 兼容接口。
- SQLite migration 不是版本迁移框架，而是启动期 `CREATE TABLE IF NOT EXISTS` + `PRAGMA table_info` + `ALTER TABLE`；默认真实 DB 为 `ESP-server/db/database.db`，可用 `ESP_SERVER_DB_PATH` 覆盖。
- Gateway auth 是逐路由挂载：ID 来自 `X-Gateway-Id`，token 来自 header/Bearer 与环境变量比较；未配置 token 时声明 gateway ID 即可通过，SQLite `gateway_auth.token_hash` 当前未参与校验。普通读面、legacy `/sensor` 和若干控制/LLM/voice/cleanup route 仍公开。
- 当前嵌套后端本地分支 `api@00149d1`，最后已知远端跟踪引用为 `origin/api@42ab462`，本地 0 ahead / 8 behind；本轮不 pull/rebase。
- 后端既有未提交源码差异把 S3 `child_registry` 字段（status_source/child_status/last_seen/link_lost/voice_busy）作为 C5 在线真源，避免 Server timeout 覆盖；真实 `db/database.db` 也有二进制差异，不能由本轮归因。
- 后端 smoke 会创建临时 SQLite 并 spawn `server.js` 和 mock LLM 服务；因本轮明确只读且不启动服务，未运行 `npm test`。
- C5 启动精确顺序：WiFi/NVS -> gateway_link reconnect -> S3 SoftAP -> 稳定等待 -> health/register 达到 LINK_READY -> system service -> CSI -> BME 注册 -> dispatcher/workers -> voice chain。
- `gateway_link` 状态为 `DOWN -> WIFI_CONNECTED -> REGISTERING -> READY`；重连优先于普通 CSI/BME/command/voice 业务。
- C5 CSI callback 只覆盖 latest I/Q snapshot 并投事件；worker 计算幅值、7 秒/至少 50 帧校准、baseline/time-delta/EWMA 特征，再进入 32 帧 edge detector。edge activity 权重为 variance 45%、CV 35%、energy change 20%，进入/退出阈值 0.55/0.35，持续 300/500 ms。
- BME 链是 forced-mode read -> Bosch compensation -> 空气质量模型 -> S3 upload；空气质量含 gas EMA、自适应 baseline、湿度/波动惩罚和 score EMA，30 个样本后 warmup ready。
- C5 runtime 精确拓扑：timer/callback -> FIFO event queue(24) -> dispatcher priority 4 -> CSI/BME/system queue(各 6) -> worker priority 3。C5 bus 队满直接 drop，没有 S3 的 priority/state coalescing；latest-only 只在 CSI snapshot/feature slot。
- 当前 C51/C52 自有源码除 `terminal_config.h` 和 `server_comm_config.h` 的身份 fallback 外保持一致；C51 是 device 01/local id 1，C52 是 device 02/local id 2。
- C5 范围已有 51 个 tracked modified 和 14 个 untracked runtime/edge 文件；CMake 已引用这些未跟踪文件，未来若提交必须整体纳入，否则新 checkout 会缺源码而无法构建。
- C5 live code 仍有过时注释：orchestrator 注释称 CSI 默认不编译，但宏已开启；debug metrics 宏注释称关闭时不带 energy/variance/CV，但 serializer 非 debug 分支仍输出这些字段。
- S3 本地接入与上云彻底解耦：SoftAP/local HTTP/UDP 在 STA 或 Server 不可用时仍工作；上云另需 STA+IP、3 秒稳定窗口和 Server probe。
- S3 device stream 只接受 7 个扁平字段、最大 128 bytes，并检查 allowlist/单调时间/重连基线；CSI stream 已废弃并返回 NOT_SUPPORTED，正式 CSI 只走 `/local/v1/csi/result`。
- S3 protocol adapter 兼容 v2 字符串/数字和旧 compact result，但递归拒绝 raw/IQ/phase/subcarrier/amplitude 字段，最后统一转成 runtime ingress。
- CSI fusion 使用 S3 接收时间生成 100 ms tick，C5 时间只作诊断；样本 3 秒过期，链路权重为 quality × freshness × RSSI，融合分数约为 C5 motion score 70% + 低维 metrics 30%。
- S3 CSI 状态机高阈值 0.62，连续 5 tick 进入 MOTION；低阈值 0.30，连续 20 tick 经 HOLD 回 IDLE。Server 输出把内部 `S3_TO_C51/S3_TO_C52` 匿名化为 `link_0/link_1`。
- Scheduler 100 ms tick 驱动 fusion；trigger 默认 50 ms 并可按负载放慢，voice busy 时暂停。CSI worker 每轮最多处理 12 条后 yield，并按 link latest-only 合并。
- Server ready 采用 3 次失败降级、2 次成功恢复；CSI 状态变化可立即上报，MOTION/HOLD 最快 1 秒、IDLE 2 秒。BME replay 限速约 10 条/秒。
- Snapshot 当前不携带 active links 或 CSI motion state；CSI latest fact 只通过 `/kernel/csi_event` 上报，这与后端对 snapshot 的禁止规则一致。
- Smart-home 当前 `real_device_attached=0`，pending 命令统一 ACK `failed/no_real_smart_home_device_attached`；S3 voice 仅单会话代理 PCM 到 `/api/voice/turn` 并流式回传。
- S3 当前配置文件含明文 STA WiFi 凭据（未提交差异还增加了一组），同时有硬编码公网 HTTP Server URL、空 auth token；属于需要保护且不应在输出中复述的敏感/部署风险。
- ESPS3 范围有 23 个 tracked modified（约 +4249/-656）及 6 个未跟踪新源码文件；本轮 `git diff --check -- ESPS3` 通过，但未构建、未真机联调。
- S3 live code 也有注释漂移：main 启动顺序、device stream route 名称与当前实现不一致；旧架构/集成文档仍写 CSI 默认关闭或走通用 ingest。

## 端到端主链

- BME：C5 forced read/Bosch compensation/AQ -> `POST /local/v1/sensor` -> S3 adapter + 300 条 RAM cache -> network worker -> `POST /api/device/v1/ingest` (`sensor.bme690`) -> `sensor_records`/device status/event -> Dashboard。
- CSI：C5 WiFi CSI callback latest snapshot -> Phase A feature + edge detector -> `POST /local/v1/csi/result` -> S3 CSI worker/fusion -> anonymized CanonicalEvent v2 -> latest/coalesced network worker -> `POST /kernel/csi_event` -> `csi_motion_events`/event/SSE/Dashboard。
- 状态：C5 free heap/uptime/RSSI -> UDP/33434（HTTP `/local/v1/stream` fallback）-> S3 device stream + child registry truth -> snapshot/gateway-state -> Server device/module status + Dashboard。
- 命令：Server command queue -> S3 poll `/api/commands/pending` -> C5 poll `/local/v1/commands/pending` -> noop/display execution -> C5 local ACK -> S3 Server ACK。
- 语音：C5 wake/Mic/VAD/raw PCM -> S3 `/local/v1/voice/turn` 单会话 proxy -> Server `/api/voice/turn` ASR/LLM/TTS -> PCM 经 S3 流回 C5 speaker；voice busy gate 暂停/延迟非语音低优先级任务。
- Smart-home：Server pending -> S3 poll；当前无真实执行器，S3 明确 failed ACK，不伪造 applied/succeeded。

## 证据等级

- 源码已确认：三层 ownership、route 对接、算法与 queue 参数、raw CSI 拒绝、C51/C52 parity、BME cache、CSI latest、child registry truth、Server 持久化与 auth 逻辑。
- 静态检查已确认：C5 与 S3 当前 `git diff --check` 通过；协议头 C51/C52 哈希一致；所有检查均为只读。
- 本轮未确认：当前整个脏工作区三套固件能否重新构建、真实 C5/S3 WiFi 时序、CSI 在真实场景的阈值表现、BME 硬件读数、断网缓存重放闭环、语音/命令/Server 在线闭环。
- 原因：项目规则默认业务代码/build/服务/硬件操作只读，本轮未运行 `idf.py build`、后端 smoke、服务启动、flash 或 monitor。

## 技术决策

| 决策 | 理由 |
|------|------|
| 并行检查 C5、S3、Server | 三部分边界清楚，可独立收集证据后再串联 |
| 不执行完整构建 | 本轮目标是熟悉项目，先识别验证入口，不制造构建产物 |

## 遇到的问题

| 问题 | 解决方案 |
|------|---------|
| 工作区存在大量先前改动 | 保持只读，将状态作为背景，不把任何差异归因于本轮 |
| 总览文档中的 Git 根描述已漂移 | 以当前 `find . -name .git` 与 `git status` 输出为准，继续逐项核源码 |
| 顶层与嵌套仓库同时追踪后端文件 | 后续任何 Git 操作都要分别查看 `/Users/zhiqin/ESP-111` 与 `/Users/zhiqin/ESP-111/ESP-server` |
| CSI API 文档与 live route 不一致 | 以 `deviceRoutes.js` + `csiMotionService.js` + S3 `server_client.c` 为当前合同；文档漂移单独标注 |
| 项目硬规则默认业务源码只读 | 后续修改前要求用户明确给出目标与放开的目录/操作范围 |

## 资源

- `task_plan.md`
- 当前 Git 状态、源码、CMake/Package 配置及项目文档

---
*每完成一轮关键浏览后更新本文件。*

## 2026-07-10 稳定性与问题处理流程审计

### 审计目标

- 范围：ESPC51/ESPC52（C5）、ESPS3、ESP-server，以及三者之间的 BME、CSI、状态、命令、语音链路。
- 维度：故障预防、故障隔离、降级与恢复、数据一致性、资源边界、可观测性、问题发现/定位/处置/复盘闭环。
- 交付：`docs/c5-s3-espserver-stability-audit-2026-07-10.md`。

### 初始基线

- 上一轮 live-source 接管扫描已确认三层职责、主要数据流和大量既有未提交改动；本轮将在该基线上重新核对高风险代码，不直接把旧结论当作当前证据。
- 本轮报告优先寻找会造成“故障不可见、恢复不可控、状态不一致、排障无法关联、验证无法复现”的问题，而不是泛化代码风格建议。
- 规划文件和最终报告是本轮唯一预期改动；业务源码、前端、数据库和运行配置保持不动。

### 项目规则与既有审计定位

- `.codex-skills/project-memory/project-rules.md` 明确把 `ESPC51/52`、`ESPS3`、`ESP-server`、`build`、`managed_components` 设为只读，并禁止 flash/fullclean/erase、真实 DB 修改、前端修改和运行中服务重启；本轮严格在此范围内。
- C51/C52 公共业务源码要求保持一致，协议副本要求语义同步；稳定性报告需要把 parity 自动检查列为过程门禁，而不是依靠人工记忆。
- 2026-07-09 的 code-smell 报告已指出 S3 `network_worker.c`、`s3_scheduler.c`、`protocol_adapter.c` 的职责堆叠，以及 C51/C52 镜像漂移风险；本轮只将其作为导航，重点新增故障触发、传播、恢复、观测和验收证据。
- 旧报告未覆盖 ESP-server，也未完整建立跨 C5/S3/Server 的问题处理闭环；这是本轮的主要增量。

### 文档与排障基线风险

- `docs/api-boundary-v1.md`（2026-06-17）仍声明 `/api/device/v1/ingest` 接受 `csi.motion`，而上一轮 live source 已确认正式 CSI 改为 `/kernel/csi_event` 且通用 ingest 拒绝 CSI；排障手册若引用该文档会走错入口。
- `docs/gpt-project-summary-2026-07-05.md` 仍声明顶层不是 Git 仓库，并含已漂移的 C52 CSI 开关、S3 trigger 周期等信息；当前顶层 `.git`、实时配置和运行链必须重新核对。
- 这类文档漂移不是普通文案问题：它会直接影响事故中的仓库归因、API 探测、日志搜索与回滚判断，应纳入“单一事实源 + 自动合同检查”的改进项。

### 跨层可观测性初扫

- CSI 链已在 C5/S3/Server 使用 `trace_id` 与 `tick_id`，并有 canonical schema 校验；这是当前最接近端到端可关联的链路。
- S3 已有 `error_code`、`offline_reason`、queue/drop/coalesce、高水位和 server degraded 相关日志，C5 也有 stack high-water 与 reconnect backoff 观测点。
- 当前搜索没有显示 BME、命令、语音、状态都采用统一 correlation ID；各域使用 `request_id`、`command_id`、`trace_id`、本地 seq 等不同标识，事故中需要人工跨日志拼接。
- 需继续确认 Server 是否有一致的 readiness/liveness/metrics、进程级异常处理和结构化日志；若仅有分散 route/log，则应将统一运行状态面列为高优先级治理项。

### 运行状态与生命周期候选问题

- `ESP-server/server.js:213-223` 已实现 SIGTERM/SIGINT 顺序关 HTTP 与 DB；这是可保留的保护。
- `ESP-server/server.js:196-211` 的 HTTP 关停没有 deadline/强制关闭策略；SSE 或 keep-alive 连接可让 `httpServer.close()` 长时间不返回，进而阻塞 DB close 与进程退出，需要在报告中作为候选风险复核。
- `ESP-server/server.js:270-286` 未注册 `unhandledRejection`/`uncaughtException` 的受控记录与退出路径，`app.listen` 也未看到独立 error 事件处理；启动/运行故障主要落到普通 console。
- `ESP-server/server.js:139-140` 的“Health/debug API”实际只挂载 time sync，当前文件没有专用 liveness/readiness；Server 进程存活、DB 可写、migration 完成、依赖可用无法被同一探针区分。
- `ESPS3/.../s3_scheduler.h:91-111` 已汇总 queue depth、drop/coalesce、CSI worker yield、network/voice/cadence 等负载字段，具备诊断快照基础；但还需核对这些指标是否通过 health/snapshot 持久化和告警，而非只在串口日志中存在。
- `ESPS3/.../offline_policy.c:18-87` 维护全局 server available/last error/failure count；需要核对多 worker 并发访问是否有同步，以及“最后一次请求结果”是否会覆盖整个 Server 健康判定。

### C5-S3 链路恢复与诊断候选问题

- C5 `gateway_link.c` 已实现 `DOWN/WIFI_CONNECTED/REGISTERING/READY/LOST`、health probe、register、连续失败阈值、业务 gate 与重连任务；`wifi_manager.c` 另负责物理 WiFi 重连，故障分层基础合理。
- C5 WiFi 与 gateway link 都采用确定性阶梯/线性退避，当前搜索未见随机 jitter；C51/C52 同时被 S3 重启或断网时可能形成同步 probe/register 风暴，需查看精确实现后决定优先级。
- S3 `/local/v1/health` 返回 `server_available` 与 last error，但当前 handler 搜索未显示 scheduler queue depth、drop/coalesce、heap/PSRAM 或 worker 健康；health 能证明 HTTP 活着，却不能证明网关未进入资源压力或业务队列阻塞。
- S3 已每周期输出 gateway heartbeat，包含 net/SoftAP/STA/server/voice/queue/drop/coalesce/CSI drop/free heap/PSRAM/last error；这些信息若只存在串口，不足以支撑远程事故定位和趋势告警。

### 已核实的链路健康语义问题

- C5 `gateway_link.c:108-212,383-447` 对 state、failure counter、callback 使用 `portMUX`，并区分 transient voice `EAGAIN` 与真实链路失败；该部分并发保护可保留。
- C5 `gateway_link.c:420-437` 和 `wifi_manager.c:84-93` 的退避均为确定性阶梯/线性值，无 jitter；两块设备同时恢复时会同相发起 WiFi/health/register。建议作为 P2 稳定性加固，以 device-id/随机源增加 10%-20% 抖动并做并发恢复测试。
- S3 `offline_policy.c:31-73` 将任何非 2xx（包括 409/429 业务忙、普通 4xx 校验失败）都视为 `s_server_available=false`；这会把“Server 可达但业务拒绝”误报成“Server 不可用”。
- S3 `local_http_server.c:690-708` 直接把上述最后一次请求结果作为 health 的 `server_available`；因此 health 语义不是可达性/readiness，而是跨所有业务请求共享的 last-result 状态。
- `offline_policy.c:18-20,54-87` 的 bool、字符串和计数没有锁；若 voice、upload、command 等不同 worker 并发记录/读取，会有竞态和 last-writer-wins 健康翻转。需用单一 owner/event 更新或锁保护，并拆分 `transport_reachable`、`server_ready`、`last_business_error`。
- S3 heartbeat 的 queue/drop/heap 指标位于 `s3_scheduler.c:1374-1392`，而 health body `local_http_server.c:690-708` 不包含这些字段；当前远程探针无法分辨“HTTP 存活但队列/内存已危险”。

### 命令可靠性候选问题

- Server command 表已有唯一 `command_id`、`dispatch_count`、dispatched timeout 重领与终态 ACK 幂等保护；这能避免 Server 自身重复终结，但不自动保证 C5 执行 exactly-once。
- S3 `command_router_ack()` 在接收 C5 ACK 后构造 Server ACK，再异步调用 `network_worker_enqueue_command_ack()`；需精读确认本地 command 何时删除、ACK work item 在队满/离线/重启时是否持久化或重试。
- 若 C5 已执行命令而 S3->Server ACK 在断电、队满或长期离线时丢失，Server 的 dispatch timeout 会重发同一 command；没有 C5 端已执行 command_id journal 时，显示/音量之外的未来非幂等动作可能重复执行。
- 问题处理流程应明确区分 `device_executed`、`gateway_ack_queued`、`server_acknowledged` 三个阶段，不能把 S3 本地接受 ACK 等同全局闭环成功。

### 已核实的命令 ACK 风险

- `command_router.c:525-533` 在任何上云动作前就把本地 entry 标记为 `COMMAND_STATE_ACKED`；`command_router.c:597-612` 即使 `network_worker_enqueue_command_ack()` 失败仍固定返回 `ESP_OK`，C5 会认为 ACK 已被可靠接受。
- Server `queue.js:320-375` 会对超时的 `dispatched` 命令重新领取并递增 `dispatch_count`；如果 S3 ACK 丢失，重发机制会按设计触发。
- Server `queue.js:378-499` 对终态 ACK 有 ownership 检查和幂等返回，这是 Server 端正确保护，但无法覆盖“C5 已执行、Server 从未收到 ACK”的窗口。
- 因此当前链路是 at-least-once dispatch + best-effort ACK，不是 exactly-once。短期应让 S3 持久/可恢复保存 pending ACK；C5 也应按 `command_id` 维护有界执行 journal，并对重复命令返回原结果而不再次执行。
- `network_worker_enqueue_command_ack()` 搜索结果显示使用 `portMAX_DELAY` 入 command queue；若队列被长期占满，本地 HTTP ACK handler 可能无限阻塞。需改为有界等待、返回可重试状态，同时确保 ACK 先落可靠 journal 再响应 C5。
- `command_router.c:91-108,126-149` 的去重只覆盖当前 RAM queue；`ACKED`/`TIMEOUT` 槽可被下一条命令立即复用。旧 ACK 未到 Server且槽已覆盖时，同一 `command_id` 重发会再次入队执行。
- 结论：当前不是简单“可能丢一条状态”，而是存在 C5 已执行后重复执行的窗口。建议优先建立 S3 durable ACK outbox + C5 bounded dedupe journal；Server 保留 dispatch_count 并对超过阈值自动标 `delivery_uncertain`/告警，而不是无限静默重发。

## 2026-07-10 代码质量与逻辑优化审计

### 范围与判定口径

- 目标范围是 active 的 `ESPC51`、`ESPC52`、`ESPS3`、`ESP-server` 以及正式链路相关工具；`build/`、`node_modules/`、`managed_components/`、归档代码和生成物不参与复杂度排名。
- “屎山代码”不按文件长度直接下结论：只有在职责混杂、隐式状态耦合、重复分支、不可验证副作用、错误语义丢失或修改放大效应有源码证据时，才列为结构性债务。
- 确认缺陷：存在明确触发条件和错误结果；结构性债务：当前未必立刻出错，但显著扩大修改/验证成本；逻辑优化：保持合同不变即可降低复杂度、资源消耗或故障窗口。
- 高优先级条目必须由根代理在当前源码重读确认；子审计和旧报告只提供候选位置。

### 当前基线

- 当前是顶层 `main` 与嵌套后端 `api` 两个 Git 根，均有大量既有改动；本轮不归因、不回退。
- 根规划文件显示此前稳定性审计已确认 S3 Server 健康语义混淆、无锁共享状态，以及 command ACK 丢失导致重复执行窗口；本轮会重新核对文件和行号后决定是否进入综合报告。
- 2026-06-16 审计的 repo 布局描述已经漂移：当时认为顶层不是 Git repo，而 live source 现在有顶层 `.git`。这再次说明历史报告只能作导航。
- 2026-07-09 code-smell 报告指向 S3 `network_worker.c`、`s3_scheduler.c`、`protocol_adapter.c` 的职责堆叠和 C51/C52 复制漂移风险；待用当前度量和控制流确认。
- 规划恢复脚本本轮无输出，未发现需要补录的会话上下文。

### 第一轮静态度量

- 排除 `build/`、`managed_components/`、`node_modules/`、归档和后端前端后，C/H/JS/TS 文件合计约 83,361 行；该数字只用于覆盖率和热点定位。
- 最大 active 文件包括：`ESP-server/scripts/smoke-regression.js` 3,749 行、S3 `network_worker.c` 2,453 行、`s3_scheduler.c` 2,151 行、`protocol_adapter.c` 1,574 行、后端 `dashboardService.js` 1,367 行、C5 两份 `server_comm_http.c` 各 1,341 行。
- 其他值得深读的单体包括 S3 `sensor_aggregator.c` 1,195 行、`csi_placeholder_gateway.c` 1,135 行、`server_client.c` 1,046 行、`local_http_server.c` 978 行，以及后端 `agent/stateStore.js` 1,021 行。
- 大文件不自动构成发现；只有确认其同时承担多个变化原因、存在隐式共享状态或难以隔离验证时才报告为结构性债务。
- 顶层当前 diff 约 91 个文件、9,993 行新增/3,463 行删除，其中 S3 `network_worker.c` 单文件约新增 1,671 行；说明本轮审计对象是快速演进中的未提交快照，报告必须把“代码事实”与“提交历史质量”分开。
- 嵌套 `ESP-server` 自身 diff 只显示真实 `db/database.db` 变化，而顶层 Git 同时追踪后端源码/前端改动；这进一步证明双重版本归属会使审计和发布归因复杂化。
- 项目硬规则明确禁止修改 C5/S3/Server 业务代码、真实数据库和前端；本轮静态取证与只新增报告符合该边界。

### 主线程已复核候选

- S3 `offline_policy.c:18-20,54-87` 用无锁静态全局保存 `server_available`、错误字符串和失败计数；写入和多个读取方之间没有互斥、原子或单 owner 约束，属于源码确认的数据竞争/快照撕裂风险。
- 同文件 `31-51,54-66` 只把 2xx 视为可用，409/429/普通 4xx 都会把全局 `server_available` 写成 false；这把“Server 可达但业务拒绝/限流”错误折叠成“Server 不可用”。
- `local_http_server.c:690-708` 的 `/local/v1/health` 直接读取上述 last-result，并不包含 scheduler queue/drop/coalesce/heap/worker 状态；因此该接口既不是 transport liveness，也不是业务 readiness，可能误导自动恢复和人工排障。
- S3 `command_router.c:515-533` 在构造/排队 Server ACK 之前就把 RAM entry 置为 `COMMAND_STATE_ACKED`；`allocate_locked():102-111` 允许下一条命令立即复用 ACKED/TIMEOUT 槽，当前去重只覆盖尚未被复用的 RAM entry。
- Server `commands/queue.js:320-375` 会把超时的 `dispatched` 命令重新领取，`:378-500` 虽然保护了 Server 终态 ACK 幂等和 ownership，但不能覆盖“C5 已执行、S3 ACK 未送达 Server”的窗口；完整严重级别还需核对 S3 enqueue 返回与持久化语义。
- Live config 确认 S3 默认 Server 是 `gateway_config.h:73-79` 的公网明文 HTTP 地址且 auth token 为空；SoftAP 密码是协议头 `esp111_protocol_common.h:39-40` 的固定明文。是否构成可直接利用漏洞取决于后端生产鉴权强制策略，待继续复核。

### 主线程确认问题

- **命令执行链存在 ACK 丢失与重复执行窗口（候选 P1）**：`network_worker.c:2418-2443` 复制 ACK 后用 `portMAX_DELAY` 写 command queue；队列持续满时，本地 HTTP ACK 处理可无限阻塞。work item 执行时，`:1875-1900,1902-1943` 只在 `ESP_ERR_INVALID_STATE && !server_link_stable()` 时重排；HTTP timeout/普通传输错误会释放，HTTP 4xx/5xx 甚至可能 `ret==ESP_OK` 后直接释放。`command_router.c:597-612` 不论 enqueue 是否成功都返回 `ESP_OK`。再结合本地 entry 提前 ACKED/可复用和 Server `queue.js:320-375` 超时重领，形成“C5 已执行 -> S3 ACK 丢失 -> Server 重发 -> C5 再执行”的闭环。当前合同是 at-least-once dispatch + best-effort ACK，而不是 exactly-once。
- 最小治理不是简单增加重试：S3 应先把 ACK 写入可恢复 outbox，再向 C5 成功响应；command worker 只在 Server 2xx/幂等终态确认后删除；C5 对 `command_id` 建有界持久或可恢复 dedupe journal。验收需注入 queue full、断网、timeout、500、重启五类故障并证明非幂等命令只执行一次。
- **网关鉴权默认 fail-open（候选 P0/P1，取决于部署暴露）**：`gatewayAuthService.js:33-44,82-120` 在 token 集为空时接受任意非空 `gateway_id`，并标记 `auth_required:false`；S3 默认 `gateway_config.h:73-79` 使用公网明文 HTTP 且空 token。若生产环境未额外设置 `GATEWAY_AUTH_TOKEN(S)`，攻击者可声明任意 gateway 身份访问 `gatewayOnly` 写入/ACK 面。代码文档自身也在 `frontend-api-guide.md:106` 警告生产环境不要依赖该模式。
- 最小治理是生产 fail-closed：非测试环境启动时缺 token/密钥直接失败，设备密钥按 gateway 独立轮换，传输改 TLS；开发无鉴权模式必须显式 `ALLOW_INSECURE_GATEWAY_AUTH=1` 且只绑定 loopback/隔离网络。验收应覆盖无 secret 启动失败、伪造 ID 401、错 token 401、正确 token + binding 成功。
- **后端没有真实 liveness/readiness**：`server.js:139-140` 注释为 Health/debug API，实际只挂载 time sync；启动虽然在 `:225-263` 先 ensure tables 再 listen，但运行期无法用探针区分进程存活、SQLite 可写、迁移完成和 draining。属于 P2 运维逻辑缺口，需增加无副作用 `/health/live` 与包含 DB 轻查询/启动状态的 `/health/ready`，并给 shutdown 设置 deadline。
- **双重 Git 归属是仓库级结构债务**：顶层 Git 当前追踪 `ESP-server` 下 95 个文件，同时目录内还有独立 `.git`。同一后端改动在顶层和嵌套仓库呈现不同 diff/status，已经造成真实 DB 在嵌套 repo dirty、源码只在顶层 dirty 的归因分裂。应明确采用 submodule/subtree/单 repo 之一，不能长期双重追踪普通文件。
- 协议头现状不是简单“四份都应字节一致”：C51/C52 SHA-256 相同，S3 和 `shared_components` 各不相同；S3 有 Server-facing 常量属于有意角色差异。真正优化方向是生成共享 local contract + S3-only 扩展或加语义 contract test，而非机械复制覆盖。

### 跨层幂等与测试结构候选

- **Canonical CSI ingest 缺少幂等落点（候选 P1/P2）**：`csiMotionService.js:147-170` 已验证并读取 `trace_id/tick_id`，但构造的 persisted fact 不包含它们；`:173-194` 每次请求都刷新状态、INSERT、写 event log 并广播。`db/csiMotion.js:5-20,31-45` 的表没有 trace/tick/event id 列或唯一键，`:47-69` 总是 INSERT。相同 CanonicalEvent 因响应丢失/重试再次到达时会形成重复 DB 行、日志和 SSE 事件。
- 优化建议：定义稳定 `event_id`，或以 `(gateway_id,tick_id)`/明确 epoch 组合做幂等键；在单事务中 insert-on-conflict、状态刷新和 event log，重复请求返回原 id 且不重复广播。需先确认 tick 重启语义，不能直接把裸 tick_id 设成全局唯一。
- **后端 smoke harness 已形成测试单体（P2 结构债务）**：`package.json:6-10` 的 `test` 与 `test:smoke` 都只运行 `scripts/smoke-regression.js`；该文件 3,749 行但只有约 22 个顶层 helper/test 函数，核心 `run()` 从约 751 行开始串联大量端到端断言。覆盖面较广是优点，但任一失败会中断后续场景，无法按域并行、筛选或单独复现。
- 最小治理：保留一条端到端 smoke，按 auth/device/CSI/command/voice/memory/user-data 拆为 Node test runner 测试文件；共享 temp DB/server fixture，每个用例显式名称与隔离环境。验收不是减少断言，而是 `node --test` 可按文件/名称运行且原 smoke 合同全部保留。
- **API 文档与 live route 明确漂移（P2）**：`deviceRoutes.js:112-123` 当前只允许 `sensor.bme690` 并把 CSI 指向 `/kernel/csi_event`；`docs/api.md:2145,2227,2244-2270,2729,3210` 仍多处宣称通用 ingest 接收 `csi.motion`。这会诱导客户端走必然返回 400 的路径，也会让 smoke 覆盖声明失真。
- **BME ingest 同样缺少端到端幂等（候选 P1）**：S3 `network_replay_worker.c:116-154` 在非 2xx/传输错误时保留同一 cache record 并重放；后端 `sensorBme690Service.js:218-254` 每次请求都直接 INSERT，虽然 `request_seq` 被存入列 `:237`，但 `db/sensorRecords.js` 只建普通 latest/payload 查询索引，没有 event identity 唯一约束。若 Server 已提交但响应丢失，同一样本会重复入历史。
- 重复 BME 不只影响曲线：`sensorBme690Service.js:256-267` 随后还刷新设备活跃状态，并在含 alarm 时再次写告警 event。应把 BME/CSI 统一为带 `event_id`/`boot_id + request_seq` 的至少一次传输、幂等消费合同，并把主记录、状态更新、事件日志纳入事务。
- S3 BME cache 的优点是明确标记 in-flight、2xx 后删除、失败后释放并限速 10/s；但它是 `bme_cache_manager.c` 的 300 条 RAM ring，重启不恢复且满时覆盖最老记录。报告应把它称为“有界离线缓冲”，不能误称 durable queue；是否升级 NVS/outbox 取决于允许丢失窗口的产品 SLO。
- **Gateway ingest 存在固定 SQLite 写放大（P2，规模扩大后可能 P1）**：`gatewayAuthService.js:132-173` 对每个 gatewayOnly 请求都同步 UPSERT `gateway_auth.last_seen`；`:216-251` 对已存在 binding 仍先 SELECT 再 UPSERT last_seen。随后 `deviceStatusService.js:601-607` 又更新 device 与 module 两个状态表，最后主记录/event log 各写一次。单次 machine ingest 会产生多次串行读写。
- 优化方式：鉴权验证与 presence bookkeeping 解耦，last_seen 按 gateway/device 节流（例如仅跨时间桶更新）；binding 只在首次绑定/变更时写；状态使用有唯一键的单条 UPSERT。以“每事件 SQL 数、p95 latency、busy/error rate”作为验收指标，而非只看功能通过。
- **多表 ingest 非事务导致部分提交（候选 P1）**：`db/sqlite.js:15-45` 只提供独立 auto-commit `db.run/db.all`；BME/CSI 依次写主事实、状态表、event log，任何后续步骤失败都会让 route 返回 500，但前面写入已提交，重试再制造重复。应将幂等主事实 + 状态更新 + event log 纳入事务；SSE 只在 COMMIT 后广播。
- **公开 SSE 缺连接上限和背压（候选 P1）**：`eventRoutes.js:188-192` 无鉴权直接订阅；`eventStreamService.js:3,38-66` 每连接创建一个 interval 并放入无上限 Set，`:33-36,69-83` 忽略 `res.write()` 的 false/backpressure，只在同步异常时删除。慢读或大量连接可持续增长 socket buffer/定时器并拖垮进程。
- SSE 最小治理：总量/IP 配额、认证或内网边界、反向代理超时/连接限制；写返回 false 时暂停/丢弃低优先级事件或断开慢客户端，统一 heartbeat timer/定时扫描减少 per-client timer。用慢消费者和连接洪泛压测验证常驻内存上限。
- **业务/管理 API 缺统一用户授权（候选 P0，公网暴露条件下）**：`server.js:95-140` 直接挂载所有 routers，没有全局 session/API key/RBAC。`commandRoutes.js:162-175` 匿名创建会下发到 C5 的命令；`smartHomeRoutes.js:66-81` 匿名创建控制命令；`eventRoutes.js:150-186` 匿名清理/删除日志；`voiceRoutes.js:596-600` 的 turn 和 prompt config PUT 未加 auth；`memoryRoutes.js:237-304` 的记忆写入和 job trigger 同样未授权。
- 这不是“所有读接口必须登录”的泛化建议，而是控制面、破坏性操作、隐私数据和可能产生上游模型费用的端点共享同一无授权边界。若默认 `app.listen(PORT)` 暴露在公网且没有外部反向代理强制认证，可被远程控制、污染记忆/日志、删除审计证据或消耗 ASR/LLM/TTS 资源。
- 治理应区分主体：gateway mTLS/独立 token、用户 session、admin role、内部 job secret；命令创建/日志删除/prompt 修改必须审计 actor。若依赖反向代理，仓库应提供可验证的部署配置与启动 fail-closed guard，不能把安全性留在不可见假设中。
- **唯一键迁移遇重复数据时静默降级（候选 P1/P2）**：`db/migrations.js:70-88` 发现重复 key 只 warning 并返回 false；`db/deviceStatus.js:65-87`、agent/memory/command 等 caller 均忽略结果继续启动。此后 `runUpdateThenInsert()` 的 UPDATE 会命中所有重复行，而 `deviceStatusService.js:129-137` 等读取使用无确定排序的 `LIMIT 1`，数据真源变得不确定。
- `smoke-regression.js:603-656` 明确断言 legacy duplicate 情况下唯一索引仍为 false，同时只断言 upsert 返回成功，没有要求重复行被收敛。这是“兼容启动成功”压过“不变量恢复”的测试固化。
- 改法应是版本化、可回滚的数据修复：先生成重复报告，按业务规则选 winner/合并引用，在事务中归档或软删 loser，再创建唯一索引；无法安全自动合并时 readiness 失败并给出运维命令，而不是永久运行在无约束模式。验收必须检查重复数归零和唯一索引真实存在。
- **Dashboard 前端是可维护性单体（P2/P3）**：`ESP-server/public/app.js` 2,833 行、`public/pages/s3.js` 1,139 行，两文件合计约 151 个函数，单脚本同时负责 API 调用、响应兼容/归一化、状态、图表、日志、smart-home、命令、modal、路由和 timer；现有 `npm test` 又只覆盖后端 smoke，没有独立前端单元/DOM 合同测试。修改一个 API shape 需要跨长文件回归。
- 前端抽离顺序应从纯逻辑开始：`api client -> normalizers/selectors -> feature controllers -> renderers`，先给现有兼容映射和命令 payload 加纯函数测试，再拆 DOM；不要在无测试时大改 UI。抽样核对的动态日志/设备 HTML 已使用 `escapeHtml`，timer 也有 beforeunload cleanup，这些不应误报为当前 XSS/泄漏。
- **本地 CSI 工具有两个未封顶维度（P3）**：`tools/csi-debug-web/server.js:382-395` 对没有换行的串口 chunk 持续拼接 `serialLineBuffer`，无最大行长；`:41-43,240-260` 虽限制每个 history 数量，但 `deviceHistories/latestByDevice` 的不同 device id 数量无上限。异常串口或大量伪造 ID 可让本地进程内存增长。
- CSI 工具的风险受限于它固定监听 `127.0.0.1`（`server.js:27-31`），请求体有 1 MiB 上限（`:598-627`），history 有 5,000 默认上限并有测试；建议只增加最大 serial line bytes、device key LRU/上限和丢弃计数，不需要把它按公网服务复杂化。
- **S3 存在三个结构性上帝模块（P2，高修改放大）**：当前 symbol scan 显示 `network_worker.c` 约 2,450 行，跨越 link state、Server health、queue/backpressure、CSI schema validate/latest/rate limit、BME upload、command ACK、三个 task 和 peer resource release；`s3_scheduler.c` 约 2,160 行，跨 ingress parse/runtime state、event-bus policy、stream/protocol/CSI workers 与周期调度；`protocol_adapter.c` 约 1,570 行，混合 BME、两代 CSI local parsing、validation 和 Server JSON 构造。
- 这三者的问题不是命名或文件长，而是一个需求（例如 CSI 字段、链路恢复、peer 下线）会同时穿越多个共享静态状态和 task/queue ownership。重构顺序应是先为 JSON golden、queue ownership、state transition 建合同测试，再按 `network state / upload outbox / CSI uploader / command transport`、`dispatcher / workers`、`BME / CSI adapters` 拆分；禁止边拆边改协议。
- **后端也有职责聚合热点（P2）**：`dashboardService.js` 约 1,367 行，覆盖 snapshot normalization、mock stripping、内存 cache/restore、CSI merge、BME/status/query adapters 和 overview 聚合；`agent/stateStore.js` 约 1,021 行，串联 environment、experience、relation、reminder、emergency、CSI behavior、LCD 多个领域。建议按业务域拆 service/store，并保留 route envelope 与 DB schema 合同测试。
- **部署示例放大了 fail-open 风险**：`ESP-server/.env.example:1-42` 没有列出 `GATEWAY_AUTH_TOKEN(S)`、用户/admin auth 或安全的 bind/代理要求；按该文件直接部署时 gateway auth 会进入 declared-only 模式。`.env.example` 应显式给出 required/unsafe-dev 区分，并由生产启动校验兜底。
- **C52 CSI 测试是复制粘贴假覆盖（P2）**：`ESPC52/.../csi_phase_a_tests.c:97-180` 与 C51 同段一致，硬编码 `S3_TO_C51`、device `C51`、local id `1`；当前搜索也未见正常 app/test harness 调用，只保留 `CSI_PHASE_A_TEST_MAIN` 条件入口。真实 C52 sender 走 `csi_server_client.c:36-41` 的 `S3_TO_C52`/`2` 分支，这份测试无法发现 C52 身份或 link 映射回归。
- 优化应将同一套 CSI contract test 参数化为 `{device_id,local_id,link_id}`，两个 target 分别注入身份；CI 同时编译/执行两组，并做 active C5 文件 parity allowlist。这样共享逻辑只维护一份，身份差异由数据而非复制文件表达。
- **S3 本地 C5 身份是“先声明后学习”，可跨设备读/ACK 命令（候选 P1）**：`local_http_server.c:506-565` 读取 peer IP，但 device identity 来自 JSON id；`s3_scheduler.c:682-707` 随后把该请求 IP/MAC写入声明的 device，`child_registry.c:279-318,342-390` 允许覆盖映射，没有预置 MAC/密钥反向验证。
- 命令面证据更直接：`local_http_server.c:821-852` 的 pending GET 只根据 query `id=1/2` 返回对应设备命令，不验证请求 peer；`:855-915` 的 ACK 只验证 body id 属于允许集合。`command_router.c:523-541` 按 command_id 找 entry 后直接置 ACKED，没有比较 ACK local id、peer 与 `target_device_id`。
- 固定共享 SoftAP PSK（协议头 `:39-40`）只能控制“能否进网”，不能区分 C51/C52；知道共享凭据的附近客户端可冒充设备、读取命令、伪造执行结果或抢占 peer 映射。应为每台 C5 provision 独立 secret/certificate，local envelope 带 nonce/seq + MAC/HMAC，命令 pending/ACK 同时绑定 authenticated device；预置 MAC 可作附加信号但不能单独当认证。

### ESP-server 子审计末次复核

- 子审计已在结束前重读全部 P1 live source；后端非 `public/` JS 的 `node --check` 通过，未启动服务、未运行 smoke、未访问或修改真实数据库。
- gateway auth 不仅 fail-open：当前配置 token 是全局共享集合，不绑定 `gateway_id`；`recordGatewaySeen()` 每次请求还把 `enabled=1`，使数据库中的 disabled 状态无法作为禁用控制。应按 gateway 查 token hash/enabled，token A 不能声明 B，disabled 不得被请求自动恢复。
- legacy `/sensor`、`/asr`、`/llm`、`/api/time/ping` 写入口未经过 gateway middleware；其中 sensor/time 还能刷新任意 device 在线/模块状态，绕过了 v1 路由建立的 Server-truth 边界。
- 用户数据删除存在两类源码确认缺口：`dashboard_snapshots/event_logs` soft delete 后读查询未过滤 `deleted_at`，Dashboard 内存 cache 也不失效；`all_user_data` 策略又漏掉 `csi_motion_events`、smart-home 三表和 natural-language commands。删除接口可报告成功但隐私数据仍可读/仍存在。
- `dashboardService.js` ingest 在 DB 写入前先替换 `latestDashboardSnapshot`；后续持久化/状态/event 任一步失败会返回 500，但 latest/overview 已展示未完整提交快照。重传还可能重复展开 recent voice/command events。
- `userDataService.js` 用共享 SQLite connection 执行 `BEGIN IMMEDIATE ... COMMIT/ROLLBACK`；同一 connection 上其他请求可能排入该事务。需用独占 transaction queue/connection 验证并发回滚不会卷入无关写入。
- smart-home command 领取只从 queued 改 dispatched，无 `lease_until`/超时重领；ACK 是先读后无条件 update，两个冲突 ACK 可后写覆盖先写。应增加 lease/dispatch_count 和带当前状态+gateway 条件的原子 UPDATE。
- S3-authored device status 不按时间超时，且 snapshot 只更新出现的 child；若全量快照漏掉旧 child，其历史 online=1 可永久保留。必须明确 snapshot 全量/增量语义，全量按 gateway reconciliation，增量带 tombstone。
- `latestCsiMotionByDevice` 只在进程 Map；重启仅恢复 dashboard snapshot，不从 `csi_motion_events` 恢复。DB 有最新 CSI 时 overview 仍 unavailable，直到下一帧。
- 多处数值 helper 用 `Number(null)` 得 0；daily average 把 NULL 样本当 0，home summary 还对温度/湿度/空气质量共用同一 count，缺字段时均值被拉低。每个指标必须独立有效样本计数，null 保持 null。
- daily memory job 从仅保存“当前行”的 device_status/module_status 重算历史日期，且查询缺 window 下界；次日更新会改变前一日重算结果。应改用不可变 event/snapshot/history。
- memory jobs 存在 check-then-insert 竞争，并在写最终产物前标 completed；并发可重复产物，后段失败可留下 completed 假记录和半套 candidates。需唯一键 + running/failed/completed 事务状态机。
- SSE shutdown 未先关闭 clients；活跃长连接可能让 `httpServer.close()` 无期限等待。应提供 close-all、清 timer/end response，并为整体 shutdown 设置 deadline。
- 合理设计：CSI v2 严格拒绝 raw/legacy 字段；command Server 侧 claim 使用条件 UPDATE、终态 ACK 幂等； `runUpdateThenInsert` 能处理 UNIQUE 插入竞争；mock appliances 明确标记并在持久化前剥离；smoke 使用临时 DB/端口/mock LLM。

### C51/C52 子审计末次复核

- C5 子审计已重读 live source；以下除身份配置外同时存在于 C51/C52，未运行 build/flash/monitor。
- **语音 exclusive gate 不能等待所有在途 HTTP（P1）**：`app_runtime.c:60-62` 关布尔 gate 后只等 BME；公共 HTTP 只在请求开始前检查一次。已经进入 `esp_http_client_perform()` 的 CSI/command 可继续最多 5 秒，与 voice socket/heap/S3 HTTP 并发。需公共 non-voice in-flight lease/counter，pause 后等待/中止全部在途请求。
- **CSI latest-only 与 FIFO event 语义冲突（P1）**：orchestrator 先开 CSI callback，后建 dispatcher/handler；CSI 数据只有单槽覆盖，callback 却每帧入主 FIFO，scheduler 又周期产生同类 event。高频时出现大量“有事件、无新样本”空转并挤压 BME/system。应先建消费者再开 producer，以 length-1 overwrite/notification 合并。
- **CSI 计算和 5 秒同步 HTTP 共用 worker（P1）**：worker 先 process 再 report；S3 慢响应会让 calibration/feature/window 最长 5 秒不推进。应拆 sensing/compute 与 egress worker，egress 只发送 latest。
- **暂停 sensing 后恢复不重置 temporal state（P1）**：CSI_PROCESS 受 voice、gateway READY、idle gate 整体停止；恢复后仍拿新帧与旧 `previous_clean` 差分且无 gap detection，可能产生恢复首帧假运动。应让 sensing 独立于 egress，或在 gap 后清 temporal state/重新校准。
- **BME AQ 忽略 gas validity（P1）**：driver 已给 `gas_valid/heat_stable`，`bme_air_quality.c` 只看电阻为正就更新 EMA/sample_count/baseline，并可能在 30 个无效样本后给 high confidence。只有 `new_data && gas_valid && heat_stable` 才能推进 gas baseline/warmup。
- **display placeholder 虚假成功（P1）**：command client 调 `screen_service_show_text()` 后 ACK success，而 `ai_screen_bridge.c` 只可选日志、无显示效果且固定 `ESP_OK`。未接真实 LCD 时应撤下 capability 或返回 unsupported，不能把“接受调用”当“已执行”。
- **WiFi down 后 LINK_READY/voice abort 延迟（P1）**：down notify 只改 WiFi 字段并请求重连，真正 link down 等 debounce path；voice abort 只在离开 READY 时触发。断线应立即清 READY/abort，debounce 只控制重连节流。
- **回压指标与真实负载错位（P1）**：controller 把“到期 timer 比例”当 load，启动 5 个 timer 同时 due 可直接 100% 并全局降频；worker queue depth/oldest age 不参与，HTTP 阻塞反而可能显示低 load。应按 queue depth/age/execution time/missed deadline 分维度度量。
- command client 用 `strstr/strchr` 手写 JSON，嵌套重复 key、escape 和超长对象可能静默截断/误匹配（P2）；应改结构化 parser 和严格长度/完整消费。
- CSI `calibration_samples` 是无上限递增的 `uint16_t`；长期不收敛时约 54.6 分钟回绕，累计和与分母失配（P2）。需宽计数 + 最大样本/墙钟失败策略。
- C51/C52 扫描 99 个成对文件，97 个逐字相同，单份约 19,865 行；差异只应是身份头，但 C52 CSI tests 已硬编码 C51。建议抽共享 C5 component，板级项目只注入 identity/config，并加 parity allowlist。
- startup task 的 12 KB 栈在 orchestrator 启动完成后仍永久 idle loop（P2）；应返回并删除 task，或改成小栈且承担实际健康职责的 supervisor。
- 合理设计：C5 URL guard 强制 `/local/v1`、拒绝公网 URL；raw CSI 只在 callback 内转有界低维 feature；event handler 锁外执行且业务已拆 worker；BME forced measurement 有界等待并检查 new_data。

### 审计期间的并发变更

- 审计读取期间 `ESPS3/components/Middlewares/network_worker/network_worker.c` 行号和内容连续变化；S3 子审计确认新增/接入 `resource_manager`，command router/server client 接口及 AP station 映射也在演进。
- 三个子审计均确认严格只读，变化来自本轮之外；本轮不覆盖、不归因。
- 所有关键结论必须在报告落盘前按最终 live source 重新 `rg/nl`。已变化或已修复的候选问题只记录为“审计期间变化”，不得继续当作当前缺陷。

### ESP-server SQLite 与 SSE 已核实问题

- `ESP-server/src/db/sqlite.js:5-12` 仅打开 SQLite 文件，未显式设置 WAL、`busy_timeout`、`foreign_keys=ON`、同步等级或启动 integrity check；多写链路与外部维护并发时缺少清晰锁等待/一致性策略。
- `eventLogService.js:190-216` 采用“事件先落库，再 SSE 广播”，该顺序正确，可作为可靠事件流基础保留。
- `eventStreamService.js:33-35,47-53,69-83` 忽略 `res.write()` 的 false/backpressure，只靠同步 try/catch 清理；慢客户端可累积发送缓冲，异步 socket 错误也未必被捕获。
- SSE clients 和 heartbeat intervals 是模块级集合，没有导出的 `closeAll()`；`server.js` 关停先等待 `httpServer.close()`，但 SSE 长连接不会主动结束，已确认存在 shutdown 卡住路径。
- 建议：DB 初始化集中执行受测 PRAGMA 并导出 health；SSE 为每客户端设置 backpressure/最大积压/错误监听，关停先 stop accepting + closeAll SSE，再限时 close HTTP/DB，超时强退并留下结构化事件。

### ESP-server 鉴权与事故证据完整性问题

- `gatewayAuthService.js:82-100` 在没有配置 token 时 fail-open：任何只声明 `gateway_id` 的请求都被标为 `ok`；S3 固件默认 auth token 为空，因此部署若未额外注入环境密钥，设备写面实际只依赖可伪造身份。
- `gateway_auth.token_hash` 表字段未参与 `authenticateGateway()`；当前认证只对环境变量中的共享 token 集合做比较，数据库记录不能禁用/轮换单个 gateway 凭据。
- `eventRoutes.js` 的日志 cleanup/delete、`voiceRoutes.js` 的 prompt config PUT、`commandRoutes.js` 的 command create、memory/agent-state 多个写路由没有统一 admin guard。攻击或误操作可以删除事故日志、注入命令/状态，直接破坏排障真源。
- 受保护的 gateway routes 至少覆盖 device ingest、CSI、snapshot、pending/ACK、event write；但 fail-open 使这些保护在未配置密钥时形同声明式身份。
- 建议列 P0：生产模式缺少 token 时拒绝启动；每 gateway 使用可轮换凭据或 mTLS，绑定 gateway-device；admin/write/read 分角色保护；日志删除改成审计化 tombstone/保留策略，禁止无身份物理删除事故证据。

### 传感/CSI 重放幂等候选问题

- S3 BME cache 使用本地 `cache_sequence` 管理 ring 和 in-flight，但该 sequence 搜索未显示进入 Server 合同；Server 将 `request_seq` 存入 `sensor_records`，没有看到 `(device_id, boot/session, request_seq)` 唯一约束或冲突更新。
- S3 BME cache 是 RAM ring；重启会丢失尚未重放的数据。容量满时的淘汰/告警和 replay 失败策略需由 S3 子审计确认。
- CSI CanonicalEvent 已有 `trace_id/tick_id`，但 `csi_motion_events` 搜索未显示对 trace 的唯一索引；请求在 Server 成功落库后响应丢失时，S3 latest retry 可能产生重复事实。
- 可靠性目标应明确采用 at-least-once transport + Server idempotent ingest：每条 BME/CSI 带 `event_id`/`boot_id`/monotonic seq，Server 建唯一键并在重复时返回原接受结果；S3 outbox 只在明确 2xx/idempotent ack 后删除。

### 已核实的 ingest 幂等与原子性问题

- `sensor_records` 表只有普通查询索引，`request_seq` 无唯一约束；`sensorBme690Service.js:218-254` 每次无条件 INSERT，同一 C5 envelope 重放会新增重复 row。
- BME ingest 的 sensor INSERT、device activity refresh、可选 alarm event 写入不是事务；后一步失败时前一步已提交，route 返回 5xx 后 S3 重试会重复 sensor 数据。
- `csi_motion_events` 表不存独立 `trace_id/tick_id` 列，也没有唯一键；`csiMotionService.js:173-194` 依次 refresh device、INSERT motion、更新内存 dashboard、INSERT event、broadcast。任一步失败/响应丢失都会产生部分状态或重复事件/SSE。
- BME ring 在 `bme_cache_manager.c:241-247` 初始化时清空；满容量时 `:281-315` 无条件覆盖 oldest 并只增加/log `overwritten_count`，没有持久 outbox、in-flight 保护门禁或上报告警。
- 建议列 P1：先引入 stable event key 和 Server unique/upsert，随后用事务覆盖权威 DB 写入；内存态/SSE 作为 commit 后派生。S3 cache overwrite、age、replay lag 达阈值时产生可持久告警，并评估 NVS/flash outbox 的写放大与寿命。

## 2026-07-10 稳定性审计最终收口

- 已恢复与当前 `/goal` 完全一致的活动目标；现有报告初稿为 485 行，结构已覆盖优先级、端到端可靠性合同、事故闭环、分诊、指标、路线图和验收矩阵。
- 最终复核前的明确缺口：S3 resource lifecycle / peer identity / unknown station disconnect、local HTTP binding、voice 同步占用控制面等结论尚未形成独立、可追踪的报告条目。
- 报告静态验证中的“未读取 `public`”与此前只读扫描记录不一致；最终稿应只声明未修改 `public`，不作错误的未读取声明。
- C5、S3、ESP-server 三条末次 live-source 复核均只返回证据，不编辑文件；根代理负责逐条去重和最终行号校准。
- S3 已确认：`network_worker.c:1347-1383` 在 SoftAP station 断开无法映射 device 时调用 `resource_manager_release_all()`，未知/陈旧 station 事件可让所有合法 C5 进入释放路径；应保留 fail-safe 观测，但不能用单个 mapping miss 扩大为全局释放。
- S3 已确认：`command_router.c:523-620` 在 Server ACK 持久成功前先把 entry 标为 ACKED，`network_worker.c:2438-2463` 又以 `portMAX_DELAY` 入队；ACK 上传失败只记录后释放 work，形成 local handler 阻塞、去重窗口丢失和重复执行风险。
- S3 已确认：`voice_proxy.c:263-401` 在 `local_http_server.c:918-930` 创建的同一 HTTP server 上同步读完整 PCM、等待 Server 并流回响应；长 voice turn 会占用 handler task，影响 register/heartbeat/commands/ACK 等同端口控制面。
- S3 已确认的正确保护：`resource_manager.c:226-399` 已有 stale disconnect timestamp、generation、锁外 release/restore、恢复中断复核；报告必须保留这些事实，只针对 mapping miss 和 stale-ingress 映射时序补缺口。
- C5 已确认：`csi_service.c:178` 每个 CSI callback 都向共享 bus 投事件；`c5_event_bus.c:127-157` 把 worker handler 的失败直接作为 dispatch 结果返回，`c5_runtime_workers.c:70-80` 在 CSI worker queue 满时返回 timeout，而 `c5_backpressure_controller.c:397-406` 的排空循环只在 `ESP_OK` 时继续。因此 CSI 拥塞不仅填满共享队列，还会形成 dispatcher 头阻塞，推迟后续 control event。
- C5 已确认：`system_server_client.c:623-669` 计算了 error code/message 的 JSON 转义，但最终 ACK payload 不包含这些字段，`ack_seq` 也被显式丢弃；当前官方链路不会直接把未转义文本带到 S3，却会失去详细失败上下文和 ACK attempt 关联。
- C51/C52 当前 runtime/sensor/voice/command 目录保持 byte parity，P1-2/3/4/5/6 均可在两侧 live source 复现；身份配置仍是预期差异。
- S3 voice 状态分叉已确认：AP disconnect 会先释放 resource/标记 link_lost，但 `voice_proxy.c:218-231` 清 busy 时无条件调用 `child_registry_set_voice_busy(false)`；`child_registry.c:496-510` 会清 `link_lost_since_ms`、刷新 `last_seen_ms` 并写 ONLINE，导致 registry online 与 resource RELEASED 分叉。
- S3 command 槽耗尽已确认：`command_router.c:460-520` 将 8 槽 entry 从 QUEUED 改为 DISPATCHED，但全文件没有基于 `dispatched_ms/ttl_ms` 的 sweep；未 ACK 的 8 条命令可永久耗尽本地槽。
- S3 bounded-progress 缺口已确认：`s3_scheduler.c:1323-1344` 对 CRITICAL/REALTIME push 无限重试，`:1513-1545` 又让 CRITICAL 下游 enqueue 使用 `portMAX_DELAY`；`ESPS3/sdkconfig:1344-1348` 启用 5 秒 Task WDT 但未启用 panic。`drop=0` 因此不能单独证明健康，必须同时监控 producer/dispatcher/worker progress age 和 bounded wait。
- S3 本地身份绑定缺口已确认：`s3_scheduler.c:682-707` 根据请求自报 `device_id` 直接重映射 peer IP/MAC，且在 resource stale-generation 检查之前执行；pending/ACK/voice handler 仅检查 query/body/header 的 allowlisted identity，没有验证请求 peer 是否绑定到命令 target/device。
- S3 register 门禁缺口已确认：`resource_manager.c:182-223` 会话初始 RELEASED，但 `resource_manager_confirm_peer():307-399` 接受 heartbeat/sensor/CSI 直接恢复 ACTIVE；`resource_manager_tick():436-444` 仅在 registry status view 返回 true 时检查 offline，而 `child_registry_get_status_view():657-687` 对未 registered entry 返回 false。合法格式的非 register ingress 因而可形成 `resource=ACTIVE + registry=unregistered` 并绕过 heartbeat timeout。
- ESP-server 最终复核确认 `gatewayAuthService.js:82-100` 在 token 未配置时 fail-open；`:132-149` 的 seen bookkeeping 还会把 `gateway_auth.enabled` 写回 1，数据库 token hash/enabled 不是认证真源。
- ESP-server 文档漂移 live 复核：`docs/api.md:2145-2293` 仍把 `csi.motion/occupancy` 写成通用 ingest 合同，而 `docs/frontend-api-guide.md:1004-1033` 与 live route 已改为 canonical CSI；`docs/deploy-branches.md:115` 仍用 `/api/time/status` 作为部署检查，Server 没有真正 `/live`/`/ready`。
- C5 短抖动假 READY 再确认：`gateway_link_notify_wifi_down():243-251` 不立即清 link state；只有 down-stable 才在 `:544-548` 写 LINK_DOWN，短抖动重新稳定后 `:554-556` 因旧 LINK_READY 直接跳过 health/register。
- ESP-server smart-home 命令语义已确认：`smartHomeService.js:341-367` 只把 queued 条件 claim 为 dispatched，没有 lease/attempt/expiry/reclaim；`:370-451` 的 ACK 先读后用仅含 command_id 的 UPDATE，未约束旧状态，两个并发终态可互相覆盖。
- `docs/deploy-branches.md:14-40` 的 api 部署文件清单漏掉实际路由/服务所在的整个 `src/`；按示例部署会更新 `server.js` 却保留旧业务实现。`:110-119` 又把 time status 当健康检查。
- C5 voice 复核：`app_runtime.c:45-79` 只等待 BME paused；`server_voice_client.c:666-718` 在 fixed upload begin 返回后才持有 stream，`:721-752` 的 abort 在这一阶段无法及时中止底层长调用。
- ESP-server voice legacy identity 已确认：`src/voice/http.js:44-60` 在 header/query 缺失时以 `req.ip` 作为 device ID，voice route 随后刷新 device activity；P0-3 报告证据已改为明确文件行号。

### 问题处理流程文档现状

- docs 扫描未发现独立、当前有效的 incident/runbook 文档；现有内容以架构计划、迁移记录、API 参考和审计报告为主。
- 旧迁移计划曾提出分段健康检查、trace_id、S3 watchdog、离线提示和重启恢复测试，但没有固化为当前阈值、查询步骤、责任层、止损动作、恢复判据和复盘模板。
- 因此最终报告需要给出一份可直接落地的故障处理闭环，并把常见症状映射到 C5/S3/Server 的第一检查点与证据字段。

### 已核实的运维文档风险

- `ESP-server/docs/deploy-branches.md:110-126` 将 time status、legacy latest read 与进程日志作为健康检查；这些不能证明 migration 完成、DB 可写、事件循环健康、SSE/队列无压力或外部依赖 readiness。
- `docs/esp-111-minimal-integration-checklist.md:13,55-90,175-230` 仍按 CSI 默认关闭的旧拓扑描述日志和验收，与当前 active chain 不一致。
- 同一旧清单含破坏性 flash 清理建议，和当前项目硬规则禁止 erase/flash 冲突；还把部署凭据示例写进正文。事故中照单执行可能扩大故障或泄露配置。
- 建议给所有运行文档增加 `适用版本/commit/schema_version/最后验证日期/owner`，过期清单移入 archive 并在标题显式标记；runbook 只引用无副作用的诊断命令，破坏性动作必须单独审批和备份门禁。

### 固件 watchdog / crash 证据现状

- S3 多个关键 task 已通过 `app_task_wdt_add_current()` 注册 WDT，并在阻塞/重试循环 reset；这部分保护应保留并纳入真机 stall 测试。
- C5 active runtime/system/BME/voice/CSI worker 搜索未见同类 Task WDT 注册，较依赖外围超时和默认系统 watchdog；单个 worker 死循环/长期阻塞的发现与自愈能力弱于 S3。
- S3 分区表预留了 coredump，但 `sdkconfig` 实际选择 `CONFIG_ESP_COREDUMP_ENABLE_TO_NONE`；当前不能产生 flash/UART coredump。源码也未见启动时读取 `esp_reset_reason`；C5 同样没有统一 reset reason 记录。
- 建议：三块固件启动统一记录 boot_id、firmware build、reset reason、uptime/min heap/last fatal marker；关键 task WDT 名单和预算显式化。coredump 保留本地受控提取，至少把 hash/size/reset reason 上报 Server，避免上传敏感原始内存。

### 固件发布、回滚与版本识别问题

- C51/C52 分区只有单 factory app，没有 OTA slot；S3 有 ota_0/ota_1，但 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 未开启，三者都缺少经验证的自动回退能力。
- 三套固件协议版本字符串固定为 `0.1.0`，搜索未见 git commit/build timestamp/build ID 注入；现场日志和 Server 状态无法唯一映射到实际二进制。
- C51/C52 的 CMake project 都叫 `00_Learn`，发布产物容易混淆；身份差异又来自配置/NVS，刷错设备时更难仅凭产物名发现。
- secure boot、flash encryption、NVS encryption 当前都关闭。启用涉及 provisioning 和不可逆风险，应在签名 OTA/回滚流程验证后分阶段执行，不能在事故中临时切换。
- 建议 P1/P2：先统一 artifact manifest（role/device family/semantic version/git SHA/config hash），S3 建立 signed OTA + health-confirm rollback；评估 C5 分区迁移与恢复策略。任何 OTA 必须先通过断电、坏镜像、健康超时和回滚演练。

### 测试与故障注入现状

- `ESP-server/package.json` 的 `test` 与 `test:smoke` 都只运行同一个 `scripts/smoke-regression.js`；没有分层 unit/integration/chaos 入口。
- smoke 使用临时 DB 和 mock 依赖，已覆盖大量 route 校验、唯一索引/upsert、command claim 等业务回归，是现有重要保护，应保留并拆分。
- 当前关键词扫描未看到针对 SQLite lock/损坏、重复 BME/CSI ingest、SSE slow consumer、shutdown deadline、gateway auth fail-closed 的专门稳定性测试。
- 建议建立三层故障注入矩阵：断 WiFi/Server、S3/C5 重启、ACK 丢包/重复、DB busy/disk full、SSE 慢端、voice timeout、队列压满；每项都定义“不丢关键命令、不重复副作用、可观测、在预算内恢复”的验收值。

### CI / 发布门禁现状

- live tree 未发现项目级 `.github/workflows`、C51/C52 parity check 脚本或统一 build/release check；关键命令只散落在历史 docs。
- 当前存在顶层 + `ESP-server` 两个 Git 根、C51/C52 镜像、三份 active protocol header、大量未跟踪且已被 CMake 引用的新文件；纯人工检查容易漏单边修改、漏文件或在错误仓库提交。
- 建议 P1 建立只读快速 gate：repo ownership、dirty/untracked source、C51/C52 allowlist parity、protocol semantic contract、C5 禁止 `/api`、raw CSI negative scan、JS syntax、Server temp-DB tests、文档 route 生成校验。固件完整 build 和硬件 soak 作为后续层级 gate。

### ESP-server 并行深审补充

- legacy `/sensor`、`/asr`、`/llm`、time ping 及部分 voice identity 路径可绕过 gateway auth/binding，甚至以请求 IP 兜底设备身份；会污染旧表和 device activity，需限制 loopback 或统一认证。
- gateway-device binding 只唯一约束 `(gateway_id, device_id)`，不保证 device 只有一个 active gateway owner；共享 token 下两个 gateway 可同时绑定同一 device，状态 last-writer-wins。
- 所有 route 共享一个 SQLite connection；user-data 删除事务跨多个 `await`，其他并发请求可能进入 connection-scoped transaction。需事务互斥/专用连接并做并发故障注入。
- migration 遇到历史重复键会跳过 unique index，但启动仍记录 migrations ensured；readiness 会假成功，数据约束长期缺失。
- Server 永久信任 S3 child 状态，timeout scan 排除 S3 来源且只在 GET 状态时触发；S3 失联后 child 可长期显示在线。需要后台 reconciler 把权威来源失联映射成 `unknown/stale`，并用带时间条件的 UPDATE 避免新心跳被旧扫描覆盖。
- generic/smart-home/natural-language 三套 command queue 的 lease、重试、终态语义不一致：无限重发、永久 dispatched、并发 ACK 覆盖或永远 queued 均可能发生。应统一 lease/deadline/max_attempt/dead-letter 与幂等执行 token。
- 启动事件在 listen 成功前写入，端口绑定错误不受当前 start catch 保护；DB/HTTP close 错误被吞后仍 exit 0。Dashboard snapshot 还存在内存先于 DB 更新、CSI current map 重启不恢复等分叉。
- CSI/event/snapshot 长期增长没有统一 retention、磁盘水位、backup/restore/RPO/RTO；LLM/TTS 缺 bulkhead/rate-limit/circuit breaker，prompt force refresh 缺 singleflight。
- 后端深审只读验证：69 个 JS 文件及 `server.js`/scripts 语法检查通过，嵌套 repo `git diff --check` 通过；未启动服务、未运行 smoke、未触碰真实 DB。

### C5 并行深审补充

- C5 无源码可确认的 P0；C51/C52 runtime、sensor、voice、command 等业务目录最终镜像一致，只有身份配置与 `.DS_Store` 差异；`git diff --check` 与 raw CSI 外发负面扫描通过。
- P1：CSI callback 虽只保留 raw latest slot，但每帧仍向共享 24 深度 FIFO 投事件；CSI HTTP 最长阻塞时会先灌满共享总线，再击穿三个 worker 的隔离，挤压 system/BME/control。应改为 pending bit/overwrite queue coalesce，并给 control plane 独立可靠容量。
- P1：WiFi disconnect 不立即清 `LINK_READY`；不足稳定窗口的短抖动后 reconnect task 可能看到旧 READY 而跳过 health/register。用 connection generation 强制每次新 IP 世代重新注册。
- P1：voice pause 只等待 BME，不等待/取消在途 system/CSI HTTP，CSI callback 也继续投事件；应引入 non-voice active lease + quiescence budget，语音前清空普通连接并停止 CSI 事件增长。
- P1：fixed PCM upload 在完成 write 后才发布可取消 stream handle，open/write loop不检查共享 cancel；S3 断联可卡到长 timeout。应由 owner 持有 cancellation token，并在 open/write 每轮检查 deadline/abort。
- P1：`gas_valid=false` 或 heater 未稳定时仍更新 AQ baseline/sample count 并上传，payload 不带 validity；连续无效样本会污染 high-confidence 结果。只有有效稳定气体样本可更新 baseline，温湿压独立标有效性。
- P1：BME 运行期连续 I2C/NACK 后只重复报错，没有 soft reset/reconfigure/bus recovery；需 `DEGRADED -> RECOVERING` 状态机、退避与 recovery count。
- P1：S3/WiFi 断联、换信道或长 gap 后 CSI 直接沿用旧 baseline；需 link generation/gap/RSSI/subcarrier attrition 触发 recalibration，在完成有效样本门槛前输出 `calibrating/stale` 而非 motion fact。
- P2：C5 backpressure 只看主 bus/timer due，未采集各 worker depth/inflight/latency；dispatcher 自算 CPU idle 不代表系统负载。应输出每 worker high-water、last-success age、按事件类型 drop/coalesce。
- P2：链路健康使用一个全局连续失败计数，任一 endpoint 成功会清掉另一个 endpoint 的持续故障；应拆 transport/registration/subsystem health。
- P2：命令没有本地执行 journal，ACK 诊断字段被丢弃；启动 init/task create 失败只记录不 supervised retry；CSI 100ms 稳态成功日志噪声大且 compact payload 缺跨层 trace。
- C5 真机重点：500Hz CSI + 5s endpoint delay、<1s WiFi bounce + 清 S3 registry、voice 与在途 HTTP 竞争、PCM 上传中拔 S3、BME invalid/NACK 恢复、换信道 CSI 重校准、ACK 丢包，以及 24/72h 双机 soak。

### S3 最终本地复核基线

- S3 当前有 26 个 tracked modified middleware 文件（约 +5853/-794）和 `bme_cache_manager`、`network_replay_worker`、`resource_manager`、`s3_event_bus` 等未跟踪但已进入 active tree 的新模块。
- 体量最大的文件仍是 `network_worker.c` 2455 行、`s3_scheduler.c` 2167 行、`protocol_adapter.c` 1574 行；结构拆分风险存在，但本轮先审行为和时序，不以行数直接定缺陷。
- S3 子代理连续三次在汇总阶段遭遇同一远端 502；根代理已停止重复调用，改为直接用最终 live source 复核关键路径。

### S3 resource manager 复核

- 新 `resource_manager` 以 device 为单位管理 `ACTIVE/GRACE/RELEASED/RESTORING`，release/restore 在 manager 锁外调用，generation 防止 restore 与新 disconnect 交叉；partial restore 失败后统一 release，整体方向正确。
- `resource_manager.c:316-328` 在读取 `observed_at_ms` 并检查 `restore_not_before_ms`（`:330-360`）之前，先调用 `child_registry_update_peer_ip()`。迟到的断联前 ingress 即使随后因 stale 被拒绝，也已经可能改写 peer IP/MAC 映射。
- 建议先在 manager lock 下验证 generation/timestamp，再提交 peer identity 更新；或把 identity update 作为带 expected-generation 的原子操作。验收需注入“disconnect 后才被 worker 处理的旧 ingress”，证明 registry/mapping/resource state 都不变化。
- `network_worker.c:1347-1372` 在单个 AP station disconnect 无法通过 MAC/IP 映射 child 时执行 `resource_manager_release_all()`。任意未注册 station 或映射暂时缺失的断开都可能释放两个合法 C5 的 command/sensor/CSI 资源。
- 建议：未知单 station 断联只计 mapping miss 并等待对应 child heartbeat timeout；只有 SoftAP stop/global reset 才 release-all。若必须保守隔离，应先按活跃 MAC/last_seen 排除仍在线 peers。验收：未知 station connect/disconnect 不改变两个合法 session generation/state。

### S3 多 worker 健康状态竞态

- `network_worker.c:202-210,509-553` 的 `s_server_ready`、success/failure counter、last error 和 snapshot/command/smart-home pending flags 没有锁/atomic/单 owner；`update_server_health()` 分别由 network probe、upload worker、command worker 调用。
- `offline_policy.c` 的 available/last error/failure count 也无锁，并由 voice、smart-home、sensor、replay、command、event 等多个 task 直接更新。
- 影响：计数 lost update、健康 transition 抖动、4xx/voice busy 覆盖 transport readiness、pending flag 竞态造成重复或漏调度。ESP32-S3 为双核，不能把普通 C 全局变量视为任务同步。
- 建议：由 network supervisor 单 owner 消费 typed result event；分别维护 transport/readiness/business rejection/per-endpoint health。其他 worker 只投结果，不直接写共享健康状态；pending flag 由队列 token 或锁保护。

### S3 command ACK 最终复核

- `command_router.c:533-620` 先把 entry 标 ACKED，ACK enqueue 失败仍返回 `ESP_OK`；ACKED slot 在 `:107-116` 可立即复用，去重窗口短暂。
- `network_worker.c:2438-2463` 使用 `portMAX_DELAY` 入 ACK queue；队列满时实际阻塞的是处理 ingress 的 protocol worker。local HTTP 已提前返回 200，但后续 register/heartbeat/sensor/ACK 无法推进，且 200 不代表 ACK 已进入可靠 outbox。
- `network_worker.c:1889-1913` 对 HTTP 非 2xx 只记录但仍可能返回 `ESP_OK`，普通网络失败也只在一种 `INVALID_STATE + gate closed` 情况重排；多数失败后 work 被释放，ACK 丢失。
- `command_router.c:574-597` 的手工 `snprintf` 边界没有 JSON escaping；当前官方 C5 payload 已不携带字符串 error/message，因此正常链路不触发该缺陷，但异常/非官方 local ACK 仍可触发。更直接的问题是 C5 已计算的错误文本与 `ack_seq` 没有进入 payload，诊断上下文被丢弃。
- 建议：ACK 先写 durable outbox 后才响应 C5；有界 queue wait；按 retryable/status 重试并保留 attempt/deadline；使用 cJSON 构造 payload；Server 幂等终态确认后再清 C5/S3 journal。

### S3 voice/control-plane bulkhead 与本地身份

- `local_http_server.c:924-949` 的同一个 ESP-IDF HTTP server task 承载 health/register/sensor/CSI/command/voice；`voice_proxy.c:263-401` 在同步 handler 内完成整段 PCM 接收、上游 voice turn 和流式回传。
- 一个慢/超时 voice turn 会长期占用 handler，另一块 C5 的 health/register/sensor/command HTTP 无法及时处理；现有 scheduler voice gate 只暂停后台任务，不能让 HTTP control plane 并发服务。
- voice 只检查 `X-Device-Id` 是否在 allowlist（`:269-303`），未验证 socket peer IP/MAC 是否绑定该 device。共享 SoftAP 上任意客户端可冒充另一 C5、设置其 voice_busy 并占用全局 voice session。
- 建议：用 `httpd_req_async_handler_begin` 或独立 voice worker/连接把长请求移出 control HTTP task；每个 local request 强制 `peer MAC/IP -> device_id -> active generation` 绑定校验，不能只信 body/header 短 ID。
- 验收：C51 90s voice 时 C52 health/register/command latency仍满足预算；伪造另一 device ID 返回 403且不改变 registry/resource/voice state。

### 代码质量审计恢复检查

- 2026-07-10 本次恢复确认：活动目标仍是“审计屎山代码与逻辑优化并写报告”，代码质量计划停在阶段 2；此前发现有效，但最终报告尚未生成。
- `docs/c5-s3-espserver-stability-audit-2026-07-10.md` 已存在且覆盖部分可靠性问题；它只能作为重叠证据索引，代码质量报告仍需独立回答职责混杂、复制维护、可测试性、修改放大和逻辑优化优先级。
- 顶层与嵌套后端仓库状态再次确认均为 dirty，嵌套仓库还包含真实数据库变化；本轮任何结论只描述当前 live snapshot，不把既有差异归因于审计。
- 当前 live source 再次确认 S3 ACK 链：`command_router.c:533-540` 先标 ACKED，`:589-597` 把解析出的外部字符串重新用 `snprintf` 拼 JSON，`:606-620` 入队失败仍向上返回 `ESP_OK`；`network_worker.c:2438-2463` 使用 `portMAX_DELAY`，`:1889-1913` 对非 2xx 只记录且多数传输错误不会重排。
- 当前 live source 再次确认 S3 lifecycle 时序：`resource_manager.c:316-360` 在 stale timestamp 门禁前先更新 peer IP；`network_worker.c:1347-1382` 对无法映射的单 station disconnect 调用 `release_all`。两者均为明确触发条件，不只是“大文件可能有风险”。
- 当前 live source 再次确认 S3 健康状态共享：`network_worker.c:180-225,509-553` 的 ready/counter/pending 普通全局由 probe/upload/command 路径更新，`offline_policy.c:18-87` 另有一套无锁 last-result 状态，并把业务拒绝折叠到 unavailable。
- 结构热点 live 行数更新为：`network_worker.c` 2473、`s3_scheduler.c` 2167、`protocol_adapter.c` 1574、`dashboardService.js` 1367、`stateStore.js` 1021、`smoke-regression.js` 3749；这些数字只定位修改放大，不单独作为缺陷。
- 当前 C5 live source 再确认：`c5_event_bus.h:21-32` 的 24 深度共享 FIFO 同时承载 CSI/BME/control，`csi_service.c:170-179` 每帧投 CSI event，`c5_runtime_workers.c:102-123` 在同一 worker 顺序执行 compute 和最长 5 秒 report，导致高频数据面可挤压控制面。
- `app_runtime.c:45-79` 的 voice pause 只等待 BME；`server_comm_http.c:275-315` 仅在请求开始前检查 gate；`server_voice_client.c:666-678,721-752` 说明 fixed upload 完成前 stream handle 尚不可用，断联取消无法覆盖 open/write 阶段。
- `bme_sensor_service.c:182-219` 无条件把读数送入 AQ 与上传，`bme_air_quality.c:81-128` 只检查 gas 电阻正值并推进 sample/EMA/baseline，不检查 driver 已提供的 `new_data/gas_valid/heat_stable`。
- C52 `csi_phase_a_tests.c:97-190` 仍硬编码 `S3_TO_C51`、`C51`、local id `1`，而真实 C52 client 在 `csi_server_client.c:34-42` 动态选择 C52/2；该测试无法覆盖 C52 身份合同。
- Live parity 复核：C51/C52 `Middlewares` 各 79 个 C/H 文件，`diff -qr` 仅显示 `server_comm_config.h` 与 `terminal_config.h` 两个身份配置差异；复制当前一致，但缺少自动门禁，任何单边修复仍会产生静默漂移。
- 顶层 Git 追踪 95 个 `ESP-server` 文件，嵌套后端 Git 追踪 94 个文件且当前只显示真实 `db/database.db` 变化；同一目录的源码与数据库在两个仓库呈现不同变更视图，双重归属已造成可观察的发布归因分裂。
- `deviceRoutes.js:114-123,184` 当前明确拒绝通用 ingest 的 CSI 并使用 `/kernel/csi_event`；`ESP-server/docs/api.md:2145,2227,2244-2270,2729,3210` 仍宣称 `csi.motion` 走通用 ingest，属于可复现的合同文档漂移。
- `migrations.js:70-88` 遇重复 key 仅 warning 并返回 false，调用方继续启动；唯一约束可能永久缺失。`userDataService.js:412-509` 在共享 SQLite connection 上跨多个 `await` 执行 `BEGIN IMMEDIATE/COMMIT`，无事务 owner/互斥。
- `deviceStatusService.js:154-169,719-745` 明确永不按时间降级 S3-authored child，并在 timeout scan 排除 S3 来源；S3 失联或全量快照漏项时 child 状态可能永久陈旧。
- smart-home command 当前 claim 仅 `queued -> dispatched`，无 lease/attempt/deadline；ACK 在 `smartHomeService.js:390-451` 先 SELECT 再无条件 UPDATE，冲突终态可后写覆盖。三类 command 队列语义不统一。
- `eventStreamService.js:33-83` 既忽略 write backpressure，也不导出 close-all；`server.js:196-223` 先等待 `httpServer.close()` 且无 deadline，活跃 SSE 可以让关停不收敛。
- 与 2026-06-16 报告相比，generic command 的 Server claim/终态 ACK 已增加条件 UPDATE、owner 校验和幂等返回，CSI 已迁到 canonical 专用入口；但 gateway auth 默认 fail-open、控制面 RBAC、本地 C5 peer 认证、ACK outbox 和 ingest 幂等仍未闭合。
- 与 2026-07-09 code-smell 报告相比，S3 三大结构热点继续增长到 2473/2167/1574 行，C51/C52 parity 仍高；本轮新增的高信号并非单纯行数，而是 resource lifecycle 时序、C5 shared FIFO、BME validity、C52 假测试和 Server 事务/幂等具体缺陷。

### 代码质量审计最终结论

- 最终报告已写入 `docs/project-code-quality-and-logic-audit-2026-07-10.md`，共 441 行。
- 结论为“有局部结构性屎山，但不是全项目”：S3 三大模块是最高修改放大热点；C5 主要是镜像复制和队列/状态语义；Server 主要是安全、事务、幂等和若干单体 service/test。
- 报告包含 2 个条件性 P0、12 个 P1、7 个 P2、2 个 P3，并给出 0-3 天、1-2 周、1-2 月三阶段路线及明确的不建议事项。
- 全部完整源码引用已验证文件存在且显式行号未越界；报告无缩写路径、TODO/TBD、待补或候选项。
- 无副作用静态验证通过，未执行 build/runtime/真机测试；这些限制已在报告中单独列明。

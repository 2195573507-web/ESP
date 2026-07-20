# Home AI Agent v2.0 实施报告

日期：2026-07-20

唯一实施依据：`docs/Home_AI_Agent_Repositioned_Development_Plan_v2.0.md`

## 1. 结论与边界

Phase 0 至 Phase 10 的软件实现、完成审计修复（Phase 11）、最终完成审计（Phase 14）和 Phase 15 缺口修复已完成，静态/host/build/Web 门禁已通过。实现保持以下职责边界：

- C51/C52 仅新增 S3 语音租约客户端、动态提示音播放、停止播放和命令 ACK 字段，仍只承担采集、基础雷达解析、唤醒、录音和播放。
- S3 承担本地网关、三房间状态、规则、覆盖、虚拟设备、离线固定命令、语音仲裁、紧急抢占、规则同步、本地历史和补传。
- ESP-server 承担 SQLite 数据、保留/聚合任务、规则控制面、Agent、联网工具、Feedback/Memory/Habit/probation 自主收口和 Web 增量能力。
- 复用现有 scheduler、network worker、replay worker、命令队列、语音资源管理、雷达/BME 数据源、SQLite、SSE 和 Web 首页，没有新增 Home AI task 或第二套网络链路。
- 未执行 flash、串口 monitor、真实生产服务或生产数据库迁移。

## 2. 阶段完成情况

| 阶段 | 完成内容 |
|---|---|
| Phase 0 | 只读确认 C51/C52/S3/Server/Web 的真实接口、资源、分区和已有能力 |
| Phase 1 | Schema v1、Server 数据底座、C5 语音租约、S3 全局语音锁 |
| Phase 2 | 固定三房间状态、presence 防抖、unknown 和 radar/environment 独立新鲜度 |
| Phase 3 | 固定容量规则引擎、优先级、cooldown、用户覆盖、partial activation |
| Phase 4 | 虚拟灯/空调/风扇、最小保持时间、状态回传和命令 ACK |
| Phase 5 | 条件播报、静音/有人/限频门禁、generation、防串话、emergency 抢占和路由 |
| Phase 6 | 启动恢复、通知和 15 分钟同步、两代回滚、24h/72h 历史和断网补传 |
| Phase 7 | Server 规则控制面、部署状态、配置快照、S3 页面中的 Web 增量面板 |
| Phase 8 | intent/tool/plan/action/speech 分层、白名单、超时、ACK 竞态持久化、decision API 和 S3 离线固定命令 |
| Phase 9 | 天气/新闻工具、严格新鲜度、weather fail-closed 和场景简报 |
| Phase 10 | 明确反馈、Memory 默认禁止自动化、Habit 门槛、Server 自主门禁、probation 定时收口和版本保护回滚 |
| Phase 11 | 语音会话生产状态接线、主动唤醒临时退出静音、紧急事件完整生命周期、用户确认同步和恢复播放 ACK |
| Phase 15 | `LINK_STABLE` 即时规则检查、可信 owner 掉线释放、动态 TTS 有界短期缓存、原始 PCM 生命周期清零和最终全端门禁 |

## 3. 修改清单

### 3.1 C51 与 C52 镜像修改

两端相同路径和核心实现保持字节一致：

- `components/Middlewares/home_ai/c5_voice_session_client.c/.h`：acquire/renew/release/state S3 语音租约，固定 320/384-byte 请求/响应缓冲和 2.5 秒超时。
- `components/Middlewares/voice_domain/voice_chain.c`：录音前拿租约，并同步 `RECORDING -> WAITING_SERVER -> PLAYING -> ENDING`；结束/异常释放，保留原本地资源管理器。
- `components/Middlewares/command_domain/system_command/system_server_client.c`：解析 `decision_id`、`prompt_text`、`playback_generation`、`emergency_event_id`，执行普通/紧急播放或 `speaker.stop_audio`，均回传 ACK。
- `components/Middlewares/wake/wake_prompt_cache.c/.h`：保留原 wake ACK 接口，新增最多 95-byte 文本的 URL 编码、拉取、临时文件和同一播放器资源路径。
- `components/esp111_protocol_common/include/esp111_protocol_common.h`：同步 Home AI Schema、字段、local route 和错误码。
- `components/Middlewares/CMakeLists.txt`：只加入上述客户端模块。

### 3.2 S3 修改

- 新增 `components/Middlewares/home_ai/`：
  - `home_ai_voice_session`：静态 mutex、全局租约状态机、generation 保护的生产状态转换、超时、emergency preempt 和可信 owner 掉线释放。
  - `home_ai_voice_router`：presence/quiet/busy/限频门禁、客厅 emergency 路由、generation 和 ACK。
  - `home_ai_emergency_coordinator`：8 个固定活动槽、完整紧急生命周期、1/3/5 分钟提醒、确认降频、恶化重升级和恢复播放 ACK。
  - `home_ai_room_state`：三房间固定槽、防抖、unknown、quiet/temporary-awake。
  - `home_ai_rule_engine`：16 条规则、每条 8 条件/4 动作、三代 store、partial activation 和 rollback。
  - `home_ai_user_override`：12 个固定覆盖槽、Server/local 合并和 TTL。
  - `home_ai_virtual_device`：9 个固定灯/空调/风扇槽、最小保持和状态一致性。
  - `home_ai_history_store`：2,048 个 Flash 槽、32 个 RAM pending 槽、24h/72h/80%/受控淘汰。
  - `home_ai_event_reporter`：decision、suppressed、playback、deployment、offline-buffer 事件。
  - `home_ai_config_store`：已校验 rooms/terminal 配置的 magic/version/CRC 快照与离线启动恢复。
  - `home_ai_weather_context`：Server 时间转本地 TTL，旧快照缺字段兼容 unavailable，字段存在但非法时 fail-closed。
  - `home_ai_local_voice_command`：严格匹配内置短词，执行停止/取消/静音/自动化暂停/灯光/保持/撤销，拒绝否定误触发和多房间歧义。
  - `home_ai_runtime`：规则/config 校验、恢复、评估、执行、同步和回滚，无自有 task。
- 新增 `local_http_server/home_ai_local_handler.c/.h`，并在原 local server 注册语音租约与离线命令路由。
- 增量接入 `gateway_orchestrator`、`s3_scheduler`、`network_worker`、`network_replay_worker`、`server_client`、`command_router`、`smart_home_gateway`、`sensor_aggregator`、`environment_alarm_reporter`、`wake_prompt_cache_gateway` 和现有 local HTTP server；`LINK_STABLE` 边沿复用既有 command worker 立即排入一次 Home AI full sync。
- `components/esp111_protocol_common/include/esp111_protocol_common.h` 同步 local 字段、Server route 和错误码。
- `partitions.csv` 新增独立 2 MiB `home_ai` SPIFFS 分区；两个 OTA app 分区仍各 7 MiB。
- `components/Middlewares/CMakeLists.txt` 只注册新增模块和既有依赖，不新增 task。

### 3.3 ESP-server 与 Web 修改

- 新增 `src/db/homeAi.js`：Home AI rooms/rules/deployments/events/virtual devices/overrides/feedback/memory/habits/candidates/probation/settings/decisions/ACK/紧急确认等表和迁移。
- 新增 `src/homeAi/schema.js`、`promptProfiles.js`、`toolRegistry.js`、`agentOrchestrator.js`、`voiceAgentService.js`、`learningService.js`。
- 新增 `src/services/homeAiService.js` 和 `src/routes/homeAiRoutes.js`；紧急事件按 `occurred_at_ms` 单调更新，同一事件确认幂等；`server.js` 只做增量挂载和初始化。
- 新增 `src/jobs/homeAiDataJobs.js`：15 分钟单飞聚合/保留调度、容量告警和维护记录；始终先聚合后清理。
- 新增 `src/jobs/homeAiProbationJobs.js`：启动即扫描、每 5 分钟扫描、单飞、每批最多 100 条，并在回滚前校验活动规则包版本。
- `src/db/smartHome.js`、`src/services/smartHomeService.js` 增加 `decision_id` 和 ACK 回写/早到 ACK 回放。
- `src/routes/voiceRoutes.js`、`src/voice/promptCache.js`、`src/voice/promptConfig.js` 增加按文本哈希的有界进程内动态提示音缓存，默认 15 分钟/16 项/8 MiB/单项 2 MiB，响应 `no-store`；旧静态 wake prompt API 保持兼容。voice turn 所有退出路径显式清零原始 PCM。
- `public/pages/home-ai.js` 和 `public/home-ai.css` 提供房间、规则、部署、事件、虚拟设备、覆盖、反馈、Memory、Habit、probation、工具、简报、decision 和紧急确认面板；确认后立即显示 Server 派生的 `user_acknowledged`，不伪造 S3 环境恢复状态。
- `public/pages/s3.js` 只增加 Home AI mount 点；`public/index.html` 只增加资源引用和空 favicon，不重写首页。
- `package.json`、`scripts/smoke-regression.js` 和新测试将 Home AI schema/control-plane/data jobs/probation/voice agent 纳入默认回归。

### 3.4 新增测试

- Server：`test/home-ai-schema.test.js`、`home-ai-control-plane.test.js`、`home-ai-data-jobs.test.js`、`home-ai-probation-jobs.test.js`、`home-ai-voice-agent.test.js`、`home-ai-prompt-cache.test.js`、`test/voice/raw-audio-lifecycle.test.js`，加上现有 smoke regression。
- S3：`components/Middlewares/home_ai/tests/` 覆盖 voice session/router、emergency coordinator、local voice command、ACK policy、weather context、config store、event reporter、room state、rule engine、virtual device、history 和 `LINK_STABLE`/owner-disconnect 接线契约。
- 最终资源迁移增加 rule engine 重复初始化清空且可重新加载的回归。

## 4. 跨端协议清单

### 4.1 C5 与 S3 本地协议

| Method/Route | 关键契约 |
|---|---|
| `POST /local/v1/voice/session` | `acquire/renew/release/state/emergency_preempt`；`voice_session_id`、`owner_device_id`、`generation`、`voice_state`、lease deadline；仅允许 `LOCKED -> RECORDING -> WAITING_SERVER -> PLAYING -> ENDING` |
| `POST /local/v1/voice/offline-command` | 受信任短文本 token，最多 12 类内置命令；严格匹配、房间/终端绑定、歧义拒绝和明确 result/reason，不承载 PCM 或开放语义 |
| `GET /local/v1/audio/wake-prompt` | 旧无参数 wake ACK 保持；可选 `prompt_text` 最多 95 UTF-8 bytes，返回 `audio/L16` PCM |
| `GET /local/v1/commands/pending` | 现有命令队列；新增 `decision_id`、播放文本、generation、emergency 字段 |
| `POST /local/v1/commands/{id}/ack` | 返回执行状态、错误码、`playback_generation`、`emergency_event_id`，S3 防旧 ACK 串话 |
| 既有 register/heartbeat/status/sensor/radar/stream/voice-turn | 路径和现有轻量扁平边界不变 |

C51 与 C52 协议头完全相同；C5 与 S3 的 Schema、local route、local JSON 字段和错误码宏值一致。新增 local command 数值 `speaker.stop_audio=6`，三端宏值一致；S3 头只额外包含 Server-only route 和 Server timeout。

### 4.2 S3 与 Server 网关协议

| Method/Route | 用途 |
|---|---|
| `GET /api/home-ai/v1/config` | canonical config、3 rooms、12 overrides、最多 8 条紧急确认、Server epoch/timezone、weather context、SHA-256 checksum |
| `GET /api/home-ai/v1/rules/package` | Schema v1 canonical payload、版本、SHA-256 transport checksum，最大 12,288 bytes |
| `GET /api/home-ai/v1/rules/notification` | 轻量版本通知；原 network worker 消费，15 分钟 full sync 兜底 |
| `POST /api/home-ai/v1/rules/deployments` | S3 实际 ACTIVE/ACTIVE_PARTIAL/REJECTED/rollback 状态和逐条结果 |
| `POST /api/home-ai/v1/events` | decision/suppressed/playback/feedback/offline-buffer；普通事件 insert-only 幂等，emergency 同 event ID 按时间单调更新 |
| `POST /api/home-ai/v1/virtual-devices/state` | 虚拟设备事实状态、source、decision 和 verification |
| `POST /api/home-ai/v1/history/replay` | 有界补传批次、accepted/existing 语义，失败留在本地重试 |

上述 7 个 S3 route 宏均在 `homeAiRoutes.js` 有同路径 handler，gateway-only route 使用现有 S3 鉴权边界。

### 4.3 Server 控制面和 Web API

- Rooms/config：`GET/PUT /rooms`、gateway `GET /config`。
- Rules：`POST /rules/validate`、`POST /rules/publish`、`GET /rules`、`GET /rules/current`、gateway package/notification、`POST /rules/rollback`、`GET/POST /rules/deployments`。
- Facts/control：`GET/POST /events`、`POST /history/replay`、`POST /emergencies/:event_id/acknowledge`、`GET /virtual-devices`、`POST /virtual-devices/state`、`GET/POST/DELETE /overrides`。
- Learning：`GET/POST /feedback`、memory candidates GET/POST/PATCH/DELETE、habits GET、rule candidates GET/POST/evaluate、probation GET/evaluate。candidate/probation evaluate 请求中的 `metrics`、`force`、`gates` 不是信任输入，最终判定由 Server 已落库证据生成。
- Tools/Agent：tools list/execute/settings，home-location/news settings，prompt profiles，decision list/detail，agent orchestrate。

共同 Schema 版本为 1。跨端错误码覆盖 invalid envelope/device/payload/ACK、voice busy/session busy/invalid/expired、rule package/resource、override、emergency preempt、gateway offline、timeout、server rejected/unavailable、command failed/unsupported。

## 5. 资源变化

### 5.1 Phase 10 fresh build 镜像和分区

| 目标 | 镜像 | 最小 app 分区 | 剩余 |
|---|---:|---:|---:|
| C51 | `0x1acf90` | `0x500000` | `0x353070`，66% |
| C52 | `0x1acf90` | `0x500000` | `0x353070`，66% |
| S3 | `0x12f460` | `0x700000` | `0x5d0ba0`，83% |

### 5.2 Phase 7 到 Phase 10 的 S3 map 对比

| 指标 | Phase 7 | Phase 10 | 变化 |
|---|---:|---:|---:|
| DIRAM | 305,631 / 341,760，89.43% | 305,631 / 341,760，89.43% | 0；剩余 36,129 |
| `.bss` | 198,832 | 198,832 | 0 |
| IRAM | 16,384 / 16,384，100% | 16,384 / 16,384，100% | 0 |
| Flash Code | 923,622 | 927,574 | +3,952 bytes |
| Flash Data | 189,980 | 191,308 | +1,328 bytes |
| map total image | 1,236,817 | 1,242,097 | +5,280 bytes |

Phase 10 不增加 S3 静态 DIRAM/IRAM。早先的资源门禁已将规则三代 store 23,856 bytes、rule runtime 1,280、override 2,976、virtual device 3,096，共 31,208 bytes 迁入一次性固定 PSRAM backing；当时 `.bss` 净回收 31,200 bytes。

### 5.3 C51/C52 map

| 指标 | C51 | C52 |
|---|---:|---:|
| HP SRAM | 188,754 / 320,928，余 132,174 | 188,754 / 320,928，余 132,174 |
| Flash | 1,604,682 | 1,604,688 |
| map total image | 1,756,964 | 1,756,970 |

相对 Phase 7，C51/C52 app 镜像各增加 272 bytes，HP SRAM 均无增加；两端 `system_server_client.c` 仍逐字一致。

### 5.4 Home AI 显式常驻 PSRAM 预算

| 模块 | Bytes |
|---|---:|
| history index + pending | 70,912 |
| rule stores + runtime + string pools | 62,000 |
| runtime HTTP/file/room/environment/decision/config buffers | 38,785 |
| user override slots | 2,976 |
| virtual device slots | 3,096 |
| emergency active slots（8 x 168-byte target ABI） | 1,344 |
| 合计 | 179,113 |

规则/覆盖/虚拟设备迁移使常驻 PSRAM 增加 31,208 bytes；Phase 11 紧急活动槽再增加 1,344 bytes。所有容量均为编译期常量并只在启动初始化一次；分配失败时 S3 初始化 fail-fast，不回退占用内部 RAM。cJSON、HTTP body 和事件序列化的临时堆峰值不包含在 179,113 bytes 中，必须在实机并发验收中测量。

### 5.5 Phase 11 最终 fresh build 与增量

| 目标 | 镜像 | 最小 app 分区剩余 | 关键 RAM |
|---|---:|---:|---:|
| C51 | `0x1ad2b0` | `0x352d50`，66% | HP SRAM 188,762 / 320,928，余 132,166 |
| C52 | `0x1ad2b0` | `0x352d50`，66% | HP SRAM 188,762 / 320,928，余 132,166 |
| S3 | `0x1306b0` | `0x5cf950`，83% | DIRAM 307,399 / 341,760，余 34,361；IRAM 16,384 / 16,384 |

相对 Phase 10，S3 Flash Code 增加 4,204 bytes、Flash Data 增加 480 bytes、map total image 增加 4,684 bytes，DIRAM 增加 1,768 bytes，IRAM 不变。C51/C52 map total image 各增加 792 bytes，HP SRAM 各增加 8 bytes。S3 新增协调器没有 task，每次 tick 最多派发 2 条紧急播报。

### 5.6 Phase 14 最终 fresh build 与 size 证据

本轮使用 ESP-IDF 5.5.4 和 `idf5.5_py3.14_env` 在全新隔离目录重建，随后对同一产物执行 `idf.py size`：

| 目标 | 构建目录 | app 镜像 | app 分区余量 | size 关键占用 |
|---|---|---:|---:|---|
| S3 | `/tmp/espcomplete-home-ai-final-s3` | `0x1306b0` | `0x5cf950`，83% | DIRAM `307399/341760`，余 `34361`；IRAM `16384/16384` |
| C51 | `/tmp/espcomplete-home-ai-final-audit-c51` | `0x1ad2b0` | `0x352d50`，66% | HP SRAM `188762/320928`，余 `132166` |
| C52 | `/tmp/espcomplete-home-ai-final-audit-c52` | `0x1ad2b0` | `0x352d50`，66% | HP SRAM `188762/320928`，余 `132166` |

S3 `idf.py size` 的 Flash Code 为 `931778`、Flash Data 为 `191788`、Total image size 为 `1246781`；C51/C52 Flash 分别为 `1605474`/`1605480`，Total image size 为 `1757756`/`1757762`。构建、IDF 环境和 size 日志精确扫描均没有 `warning:`、`error:`、`fatal error` 或 `FAILED`。

### 5.7 Phase 15 最终 build、size 与新增缓存上限

Phase 15 在隔离目录重建 C51/C52，并在 S3 隔离目录完成增量重链和同产物 `idf.py size`：

| 目标 | 构建目录 | app 镜像 | app 分区余量 | size 关键占用 |
|---|---|---:|---:|---|
| S3 | `/tmp/espcomplete-home-ai-phase15-s3` | `0x130790` | `0x5cf870`，83% | DIRAM `307399/341760`，余 `34361`；IRAM `16384/16384` |
| C51 | `/tmp/espcomplete-home-ai-phase15-c51` | `0x1ad2b0` | `0x352d50`，66% | HP SRAM `188762/320928`，余 `132166` |
| C52 | `/tmp/espcomplete-home-ai-phase15-c52` | `0x1ad2b0` | `0x352d50`，66% | HP SRAM `188762/320928`，余 `132166` |

S3 Flash Code 为 `931930`、Flash Data 为 `191596`、Total image size 为 `1246965`；相对 Phase 14，app 镜像增加 `224` bytes、Total image size 增加 `184` bytes，DIRAM/IRAM 不变。C51/C52 镜像和 RAM 占用不变。Server 动态 Home AI TTS 仅驻留进程内，固定为 15 分钟 TTL、16 项、8 MiB 总量和 2 MiB 单项上限；这不是 S3 常驻预算。三端构建与 size 日志扫描没有 `warning:`、`error:`、`fatal error` 或 `FAILED`。

## 6. 测试与构建结果

- Server `npm test`：smoke regression 加 Home AI schema、control-plane、data jobs、probation jobs、voice agent、prompt cache、raw-audio lifecycle 七个测试文件全部 PASS，使用临时 SQLite 和临时端口。
- 全部非 `node_modules` JavaScript 执行 `node --check`：PASS，共 101 个文件，零语法错误。
- C51/C52/S3 LD2450 parser/core：PASS。
- S3 radar continuity、spatial、protocol/registry/ingest/source isolation：PASS。
- S3 environment alarm engine/adapter/delivery/reporter：PASS。
- Home AI network recovery contract、voice session、voice router、emergency coordinator、local voice command、ACK policy、weather context、config store、event reporter、room state、rule engine、virtual device、history 十三类 host targets：PASS；本轮共执行 17 次（history 覆盖 basic/restart/prune/protected/priority），C 目标使用 `-Wall -Wextra -Werror`。紧急路由测试覆盖 C51/C52 occupied、vacant、unknown 的全部 9 种组合；新增覆盖 `LINK_STABLE` 即时同步、可信 owner 掉线释放和规则资源超限保留旧包。
- 既有 C5 BLE stream、smart-home gateway 和 ESPS3 Radar Debug parser 回归：PASS。
- C51/C52 隔离目录 fresh build、S3 隔离目录增量重链及三端同产物 `idf.py size`：PASS；精确扫描构建、IDF 环境和 size 日志，没有 `warning:`、`error:`、`FAILED` 或 `fatal error`。
- C51/C52 `system_server_client.c` 逐字一致；C51/C52/S3 新增协议宏（包括 offline route 和 `stop_audio=6`）一致；7 个 S3 Server route 与 Server handler 匹配。
- Phase 10 桌面与 390x844 Edge 隔离浏览器验收：反馈提交、Memory 候选/确认/默认禁止自动化、工具/简报配置、probation 与 decision 展示均通过。
- Phase 11 桌面与 390x844 Edge 隔离浏览器验收：活动紧急事件显示、确认请求、SQLite 幂等记录、刷新后即时“已确认”状态均通过；无 console/page/request error 和移动横向溢出。临时 Server 已关闭。
- Phase 15 使用临时 SQLite、临时端口和 Edge 对 1440x1000 桌面及 390x844 移动视口复验：首页既有导航与 Home AI 七个标签页均可用，无 console/page/request error、无横向溢出；临时 Server 已关闭。
- `git diff --check`：PASS。
- 本轮 untracked 实现/测试/报告文件执行等价 whitespace 检查：PASS；只读 v2.0 原始计划中的 Markdown 强制换行尾空格未改写。

### 6.1 Phase 14 完成审计结论

- C5 仍只负责采集、基础雷达解析、唤醒、录音和播放；规则、跨房间状态、联网工具、长期历史和控制面均留在 S3/Server。
- C51/C52 `system_server_client.c` 逐字一致，SHA-256 为 `498434c41bca98bae8920ff6d492048d65cc3f8a568721e2b811d9acb23ee117`；两端协议头逐字一致，SHA-256 为 `6f8d0568d21f6ee3fcbc7cd071c2920a189dacf0520f502c60ed06c2429a523e`。S3 共享宏保持兼容，仅增加网关侧 Server-only route 定义。
- S3 七条网关 route（config、rule package、notification、deployments、events、virtual-device state、history replay）均在 Server 有对应 handler；本地 voice session/offline-command、command ACK、动态 wake prompt 和 `speaker.stop_audio=6` 的字段/错误码在 C5/S3 共享协议中一致。
- Home AI 新模块均使用编译期固定上限和一次性 PSRAM backing，复用既有 orchestrator、scheduler、network worker、replay worker、command queue、SQLite、SSE 和 Web 首页链路；没有新增并行网络/语音/雷达/BME task。
- 9 种 C51/C52 presence 组合已由 host 矩阵证明：occupied/unknown 目标正常播报，vacant 目标不误播报，双 vacant 返回无目标并保留当前会话；这与 v2.0 的客厅紧急兜底语义一致。
- Phase 14 结束条件全部满足：源码实现、跨端协议、资源上限、阶段回归、fresh build/size、告警扫描、`git diff --check` 均通过。硬件启动、真实网络/音频/传感器、Flash 耐久和生产数据仍只列为第 8 节验收项。

### 6.2 Phase 15 最终收口结论

- `LINK_STABLE` 边沿已复用既有 command worker 的 pending flag 和资源门控立即排入一次 Home AI full sync；定时通知检查和 15 分钟兜底保持不变。
- SoftAP 确认掉线只按可信设备映射释放对应语音 owner；普通 release 仍校验 session id/generation，不放宽跨端协议。
- 动态 `home_ai_text_*` TTS 不再写 PCM/JSON 文件，改为有界进程内 LRU/TTL；静态可配置 wake prompt 继续走原兼容缓存。voice turn 原始 PCM 在 parser 拒绝、所有业务拒绝以及成功、失败、超时、断开路径均显式清零。
- 最新 Server、host、既有域回归、Web、三端 build/size、warning 和 whitespace 门禁全部通过。未执行 flash、串口 monitor、生产 Server 或生产数据库操作。

## 7. 保留现有功能的证据边界

- 代码结构：只把 Home AI 接到现有 orchestrator/scheduler/network/replay/command/resource 路径；没有替换网络、语音、雷达、BME690、告警、SQLite、SSE 或首页。
- Host/static：既有 radar 与 BME alarm host 回归和 Server smoke regression 已通过。
- Build：三端 fresh build 已通过且没有项目编译 warning。
- 不可据此宣称实机、RF、音频 DMA、Flash 耐久、真实网络或生产数据库已经通过。

## 8. 待硬件验收

- C51/C52/S3 实机启动、PSRAM 分配、长时间稳定性和 watchdog。
- SoftAP + STA、两个 C5 注册/心跳/掉线恢复。
- C51/C52 语音锁互斥、租约超时/掉线释放、真实录音与播放 ACK。
- C51/C52 实际播放中止、emergency 打断与 `speaker.stop_audio` ACK。
- emergency 抢占普通语音、客厅兜底路由、1/3/5 分钟持续提醒、确认后 10 分钟降频、恶化重升级和恢复播报 ACK。
- 真实本地离线识别器将 PCM 转成受信任固定 token，并在噪声、否定句、多房间和连续指令下验证不误触发。
- 雷达、BME690、语音、网络和告警并发时的内部 RAM、DMA、PSRAM 峰值与实时性。
- 真实断网 24h 历史、重启索引恢复、72h 尽力保留、Flash 满载和补传幂等。
- 规则通知、15 分钟同步、partial activation、两代 rollback 的实机链路。
- 真实天气/新闻服务、超时、过期和无网行为。
- 现有 Web 在真实设备数据、SSE 和长期运行下的回归。
- Server probation 调度器在长期运行、时钟跳变、进程重启和多次规则发布下的现场收口与版本保护。

## 9. 剩余风险

- S3 IRAM 仍为 100%，与资源迁移前相同；本轮未增加 IRAM，但后续任何 `IRAM_ATTR` 或底层库变化都必须重新 map gate。
- 179,113-byte 常驻 PSRAM 是源码和目标 ABI 的固定预算，不是实机峰值；cJSON/HTTP/TLS 临时分配需要设备遥测验证。
- Flash 历史策略由 host 文件模拟验证，不能替代真实 SPIFFS 掉电、磨损、满盘和坏块验证。
- Server 测试只迁移临时 SQLite；真实数据库的历史数据、索引冲突和备份恢复未操作。
- 天气/新闻只完成协议、校验和隔离测试，未调用真实生产服务。
- S3 离线命令入口仅接受受信任固定文本 token；当前源码不包含本地 PCM 到 token 的 ASR，因此 host parser/build 不能证明真实离线语音可用。
- probation 收口依赖 Server 进程周期扫描和系统时间；单进程内已防重入，但多实例部署仍需依赖 SQLite 状态与现场部署约束验证。
- 用户确认由 Server 配置 checksum 链同步到 S3；软件已验证幂等与固定 8 槽上限，但真实网络延迟、掉线期间确认和恢复播报时序仍需硬件验收。
- 动态 Home AI TTS 的 8 MiB 上限只约束单个 Server 进程；多实例总占用、进程重启后的缓存冷启动和上游 TTS 行为需在部署环境验收。静态 wake prompt 磁盘缓存为既有兼容路径，未改为短期缓存。
- 原始 PCM 已在本进程可控 Buffer 生命周期内清零且源码没有 raw PCM 文件/数据库写入路径，但 JavaScript 运行时、HTTP 栈和外部 ASR/TTS 提供方的内部副本不能由 host 测试证明已清除。
- 工作区原本存在大量用户修改/删除；本轮未清理、恢复、stage 或 commit，最终状态仍是 dirty worktree。

## 10. 本轮文件级修改清单

### 10.1 C51/C52 镜像文件

- `ESPC51/components/Middlewares/home_ai/c5_voice_session_client.c/.h`、`ESPC52/components/Middlewares/home_ai/c5_voice_session_client.c/.h`。
- 两端同步修改 `components/Middlewares/CMakeLists.txt`、`command_domain/system_command/system_server_client.c`、`voice_domain/voice_chain.c`、`wake/wake_prompt_cache.c/.h`、`components/esp111_protocol_common/include/esp111_protocol_common.h`。

### 10.2 S3 产品文件

- `ESPS3/components/Middlewares/home_ai/` 下的 `home_ai_{voice_session,voice_router,emergency_coordinator,room_state,rule_engine,user_override,virtual_device,history_store,event_reporter,config_store,weather_context,local_voice_command,runtime}.{c,h}`。
- `ESPS3/components/Middlewares/home_ai/tests/` 下的固定容量 host 测试、stub 和 `run_host_tests.sh`。
- 现有链接增量修改：`components/Middlewares/CMakeLists.txt`、`command_router/{command_router.c,command_ack_policy.c,command_ack_policy.h}`、`gateway_orchestrator/gateway_orchestrator.c`、`local_http_server/{local_http_server.c,radar_local_handler.c,home_ai_local_handler.c,home_ai_local_handler.h}`、`network_worker/{network_worker.c,network_worker.h}`、`network_replay_worker/{network_replay_worker.c,network_replay_worker.h}`、`runtime/s3_scheduler.c`、`sensor_aggregator/{sensor_aggregator.c,sensor_aggregator.h}`、`server_client/{server_client.c,server_client.h}`、`smart_home_gateway/{smart_home_gateway.c,smart_home_gateway.h}`、`environment_alarm_reporter/{environment_alarm_reporter.c,environment_alarm_reporter.h}`、`wake_prompt_cache_gateway/wake_prompt_cache_gateway.c`。
- 共享协议与分区：`ESPS3/components/esp111_protocol_common/include/esp111_protocol_common.h`、`ESPS3/partitions.csv`。

### 10.3 ESP-server 与 Web 文件

- 数据/业务：`ESP-server/src/db/homeAi.js`、`src/homeAi/{schema,promptProfiles,toolRegistry,agentOrchestrator,voiceAgentService,learningService}.js`、`src/services/homeAiService.js`、`src/routes/homeAiRoutes.js`、`src/jobs/{homeAiDataJobs,homeAiProbationJobs}.js`。
- 既有链接：`ESP-server/server.js`、`src/db/smartHome.js`、`src/services/{smartHomeService,sensorBme690Service}.js`、`src/routes/voiceRoutes.js`、`src/voice/{chain,promptCache,promptConfig}.js`、`src/llm/textClient.js`、`package.json`、`scripts/smoke-regression.js`。
- Web：`ESP-server/public/home-ai.css`、`public/pages/home-ai.js`、`public/pages/s3.js`、`public/index.html`；既有首页、SSE 和其他路由未替换。
- Server 测试：`ESP-server/test/home-ai-{schema,control-plane,data-jobs,probation-jobs,voice-agent,prompt-cache}.test.js`、`ESP-server/test/voice/raw-audio-lifecycle.test.js`。

### 10.4 跟踪与证据文件

- `task_plan.md`、`progress.md`、`findings.md`、本报告。
- 构建证据目录：`/tmp/espcomplete-home-ai-phase11-s3`、`/tmp/espcomplete-home-ai-phase11-c51`、`/tmp/espcomplete-home-ai-phase11-c52`、`/tmp/espcomplete-home-ai-final-s3`、`/tmp/espcomplete-home-ai-final-audit-c51`、`/tmp/espcomplete-home-ai-final-audit-c52`，以及最终 `/tmp/espcomplete-home-ai-phase15-s3`、`/tmp/espcomplete-home-ai-phase15-c51`、`/tmp/espcomplete-home-ai-phase15-c52`；目录内保留 ELF/map/bin 和 IDF 日志。
- 本轮新增/扩展定向测试：`ESPS3/components/Middlewares/home_ai/tests/test_network_recovery_contract.sh`、`test_home_ai_voice_session.c`、`test_home_ai_rule_engine.c`，以及 Server prompt-cache/raw-audio lifecycle tests；紧急矩阵继续由 `test_home_ai_voice_router.c` 覆盖。
- Web 证据：Phase 11 紧急确认和 Phase 15 七标签页桌面/移动自动验收均使用临时 SQLite/临时端口；临时 Server 已关闭，未保留生产数据。

上述清单仅包含本轮 Home AI 实施及其回归证据。工作区中原先存在的 Radar、Debug、BME/alarm 和其他用户 dirty 文件保留原状，未在本清单中重新归属。

# ESP-111 智能家居智能体研究发现

## 1. 需求解释

- 主目标是在现有服务器上实现类似 Hermes Agent 的持续型智能体，但领域限定为智能家居、环境感知、语音、提醒、异常与设备控制。
- 本轮只写学习文档和计划文档，不实现功能。
- Hermes 默认指 `NousResearch/hermes-agent`。同名 Rhasspy/Snips Hermes protocol 后续只用于本地语音消息协议对照。

## 2. 证据等级

| 等级 | 定义 |
| --- | --- |
| A | 当前本地源码，或官方仓库固定提交的源码/测试直接证明。 |
| B | 官方开发文档与官方仓库结构相互印证。 |
| C | 官方 README、release 或设计说明，尚未闭环到实现。 |
| D | 搜索摘要、博客或二手材料，只用于发现，不进入强结论。 |

## 3. ESP-111 实时基线

### 3.1 既有能力

- 三层边界仍是 C5 设备 -> ESPS3 网关 -> ESP-server。C5 只访问 `/local/v1/*`；S3 是唯一 Server-facing 网关；Server 负责业务事实、持久化和事件流。
- Server 已有文本 LLM 和结构化 LLM。结构化路径会把模型输出送入已有 command schema/queue，并返回 queued/rejected 两组结果。
- Server 已有 device context 和 prompt context，能显式表达设备在线状态、传感数据新鲜度、模块可用性和 BME690 相对空气状态不是国标 AQI。
- Server 已有 conversation turns、daily/weekly memory、profile candidate、correction、experience、relation、environment profile、reminder、emergency、CSI behavior、LCD state 等表和 CRUD/job 入口。
- Server 已有 generic command、smart-home command、pending poll、ACK、event log 和 SSE。
- Voice 当前是独立 ASR -> context text LLM -> TTS 流程，尚未与 structured command、smart-home queue 或 memory 闭环。

### 3.2 当前硬缺口

- 没有统一 agent run/runtime：缺 run 状态、step/trace、durable checkpoint、resume、budget、cancel、tool registry、policy engine 和 scheduler worker。
- Memory/agent-state 主要是 CRUD 与手工触发的日/周统计，不等于自动检索、反思、候选审核和长期学习闭环。
- 多数 LLM/voice/control/memory/agent/SSE/cleanup 路由没有统一用户认证和 RBAC；gateway token 未配置时存在 fail-open 风险。
- 当前命令链是 at-least-once dispatch + best-effort ACK，存在重复执行和 ACK 丢失窗口；自治前必须先有 idempotency key、lease、outbox/journal、TTL/dead-letter。
- Generic、smart-home、natural-language 三套命令状态与合同尚未统一。
- S3 当前明确没有真实家居执行器；smart-home 命令应 fail-closed。C5 真正执行能力也非常窄，LCD 仍是 placeholder。
- S3 capability 未完整同步到 Server，结构化 LLM 即使产生命令，也可能因 capability 缺失无法入队。
- S3 上游仍有明文 HTTP、空 token 默认值与硬编码 Wi-Fi 凭据等部署安全问题。

### 3.3 设计含义

- 新智能体首先应是“领域控制平面”，不是直接绕过队列调用设备。
- 所有设备动作必须走 typed tool -> policy -> command transaction -> S3 pull/ACK -> state reconciliation。
- 感知工具只读取 Server canonical facts；mock、stale、unknown、candidate 必须保留 provenance 和 confidence。
- 紧急本地快速动作仍由 C5/S3 负责；Server agent 只能解释、记录、升级和编排后续动作。

## 4. Hermes Agent 深读

### 4.1 来源

- 仓库：https://github.com/NousResearch/hermes-agent
- 架构：https://hermes-agent.nousresearch.com/docs/developer-guide/architecture
- Agent loop：https://hermes-agent.nousresearch.com/docs/developer-guide/agent-loop
- Memory：https://hermes-agent.nousresearch.com/docs/user-guide/features/memory
- Skills：https://hermes-agent.nousresearch.com/docs/user-guide/features/skills
- Cron：https://hermes-agent.nousresearch.com/docs/user-guide/features/cron
- Security：https://hermes-agent.nousresearch.com/docs/user-guide/security
- Messaging：https://hermes-agent.nousresearch.com/docs/user-guide/messaging
- 许可证：MIT（仓库 README/License 入口，待最终固定提交复核）。

### 4.2 可借鉴机制

- 单一 agent loop 负责 prompt、provider、tool dispatch、重试/回退、压缩和持久化；CLI、消息网关、cron 共享同一内核。
- Tool registry 把 schema collection、可用性和 dispatch 集中管理；多个工具调用可并发，但交互式工具强制串行，结果按原始调用顺序回填。
- Session 持久化与长期 memory 分离；session 用 SQLite/FTS5，memory 是有上限、精选、可审核的长期事实。
- Memory 在 session 开始时以冻结快照注入，避免每次写入破坏 prompt prefix；实时写入下一 session 生效。
- Memory 与 skill 分层：短小稳定事实常驻，长流程按需 progressive disclosure；skill 是程序性记忆。
- Memory/skill 自我改进可由后台 review 提议，但可开启 write approval，先 staged 再 approve/reject。
- Cron 是“新建隔离 agent session 执行任务”，可挂 skills、交付目标和 workdir；另有 no-agent 模式处理无需 LLM 的确定性 watchdog。
- Agent loop 有 iteration budget、interrupt/cancel、provider fallback、上下文压缩和 session lineage。
- 安全模型是 defense in depth：用户授权、危险操作审批、always-on hard block、用户 deny、fail-closed timeout、容器隔离、凭据过滤/脱敏、站点策略。
- 消息网关分离 adapter、session routing、授权、agent dispatch 和 delivery，并包含 circuit breaker、恢复通知与 busy input 模式。

### 4.3 不应照搬

- Hermes 是通用本机/云端工具智能体，默认工具含终端、文件、浏览器；智能家居生产服务器不应把这些能力暴露给家居 agent。
- Hermes 的同步大型 `AIAgent` 适合通用交互，但家居控制需要更严格的 typed state machine、审批、幂等和设备回执。
- JSON 文件 cron、文件式 memory 适合单用户；ESP-server 已有 SQLite，应复用事务、审计和多主体授权。
- Hermes 的“危险 shell 命令检测”不能替代家居领域 policy。开门、关燃气、解防、关闭告警等风险与 shell 模式完全不同。
- 自动创建/修改 skill 在物理控制域必须默认关闭或强制审核，不能让自我学习直接改变执行权限。

## 5. Home Assistant 深读

### 5.1 来源

- LLM API：https://developers.home-assistant.io/docs/core/llm/
- Conversation API：https://developers.home-assistant.io/docs/intent_conversation_api/
- Architecture：https://developers.home-assistant.io/docs/architecture_index/
- 仓库：https://github.com/home-assistant/core
- 许可证：Apache-2.0（待最终固定提交复核）。

### 5.2 可借鉴机制

- LLM 只能通过 Assist intents 和明确 exposed entities/capabilities 访问家居；built-in API 不允许 administrative tasks。
- 工具集合可按每次请求的 `LLMContext` 动态生成，因此不同用户、设备、房间、会话可得到不同的最小权限工具集。
- `Tool` 合同包含 name、description、参数 schema 和 async call；API instance 把工具、上下文和专用 prompt 绑定。
- Conversation API 明确区分 `action_done`、`query_answer`、`error`，并列出 success/failed targets；这比“LLM 文本说已执行”可靠。
- Conversation id 独立于设备实体状态，便于继续对话但不把对话当事实源。

### 5.3 对 ESP-111 的映射

- 建立 `AgentToolContext`：actor、role、device/gateway、room、session、channel、risk budget、current facts。
- 每次 run 只生成当前上下文允许的工具，不把整个 REST API 暴露给模型。
- Tool result 必须结构化返回 `observed/queued/dispatched/succeeded/failed/unknown` 和目标列表。
- 读取工具与控制工具分离；admin 配置永不进入普通家居对话 agent。

## 6. LangGraph 深读

### 6.1 来源

- Persistence：https://docs.langchain.com/oss/python/langgraph/persistence
- Interrupts：https://docs.langchain.com/oss/python/langgraph/interrupts
- 仓库：https://github.com/langchain-ai/langgraph
- 许可证：MIT（待最终固定提交复核）。

### 6.2 可借鉴机制

- Checkpointer 保存 thread-scoped graph state，Store 保存 cross-thread 长期事实；不要把两者混成一张“memory”表。
- Interrupt 在关键动作前保存完整状态，持久化后等待人类 approve/reject/edit，再以同一 thread id 恢复。
- 恢复时节点可能从头重跑，因此 interrupt 之前的副作用必须幂等，最好把副作用拆到单独节点。
- 审批 payload 必须 JSON 可序列化；运行时 interrupt、stream events 与状态快照可用于 UI/SSE。

### 6.3 不应照搬

- 当前 ESP-server 是 Node/SQLite，小型部署不必立即引入 Python LangGraph sidecar。
- 第一阶段可以先实现有限状态 run engine 和 checkpoint 表，保留未来接入图框架的接口。

## 7. Temporal 深读

### 7.1 来源

- Activity definition：https://docs.temporal.io/activity-definition
- 仓库：https://github.com/temporalio/temporal
- 许可证：MIT（待最终固定提交复核）。

### 7.2 可借鉴机制

- 外部 I/O 封装成可失败、可重试的 Activity；写操作必须尽量幂等。
- “观察到完成一次”不等于“只执行一次”：Activity 可能执行和部分完成多次。
- timeout、retry policy、heartbeat 和 event history 是耐久执行的核心合同。

### 7.3 不应照搬

- 引入完整 Temporal 服务对当前单机 Node/SQLite 项目过重。
- 应借用 activity/idempotency/heartbeat/event-history 语义，先在现有数据库上实现轻量版本。

## 8. Eclipse Ditto 深读

### 8.1 来源

- Overview：https://eclipse.dev/ditto/intro-overview.html
- 仓库：https://github.com/eclipse-ditto/ditto
- 许可证：EPL-2.0（待最终固定提交复核）。

### 8.2 可借鉴机制

- 物理设备映射为 JSON Thing，保存 last-known state、metadata 和访问 policy。
- 应用通过稳定 Web API 访问 twin，不必知道设备协议或连接方式。
- Twin 层负责设备抽象、授权、离线缓存和状态变更通知，但不负责边缘软件与设备协议。

### 8.3 对 ESP-111 的映射

- 现有 `smart_home_devices` 可逐步演化为 reported state；agent 的 desired action 单独进入 command transaction。
- 必须区分 `desired`、`dispatched`、`reported`，避免把排队状态覆盖为设备事实。
- ESPS3 仍负责协议适配；Server agent 不直接理解 C5 wire format。

## 9. 初步路线比较

### 路线 A：原生 ESP-server 领域智能体内核（暂推荐）

- 在 Node/SQLite 内新增 agent run、tool registry、policy、scheduler、memory retrieval 和 audit。
- 优点：复用现有数据和命令链，部署简单，边界最清楚。
- 缺点：需要自己实现耐久状态、审批和工具生态。

### 路线 B：Hermes Agent Python sidecar + ESP-server tools

- Hermes 负责 loop/memory/skills/cron，ESP-server 通过 HTTP/MCP 提供家居工具。
- 优点：最快得到通用智能体体验。
- 缺点：双运行时、双存储、权限与事实源分裂；通用终端/文件工具扩大攻击面。

### 路线 C：Home Assistant 为设备控制面，ESP-server 作为领域感知/网关集成

- 设备实体、场景、自动化交给 Home Assistant，ESP-server 提供 C5/S3 集成与专用 CSI/BME facts。
- 优点：成熟设备生态和实体/intent 权限模型。
- 缺点：改变当前 Server-truth 边界，迁移与运维显著增加；ESP-111 专有数据仍需自定义 integration。

## 10. 待验证假设

- 用户期望的自治等级：仅建议/问答、可审批执行，还是可主动闭环。
- 目标部署是单用户家庭，还是多家庭/多租户服务器。
- 是否计划接入真实 Matter/Home Assistant/第三方家电，还是先只控制 C5/S3 已有能力。
- Hermes 是否确指 Nous Research 项目。
- 当前后端最终使用顶层 `main` 还是嵌套 `ESP-server/api` 分支作为实施基线。

## 11. openHAB 深读

### 11.1 来源

- 仓库：https://github.com/openhab/openhab-core
- Concepts：https://www.openhab.org/docs/concepts/
- Semantic Model：https://www.openhab.org/docs/tutorial/model.html
- Rules DSL：https://www.openhab.org/docs/configuration/rules-dsl.html
- 许可证：EPL-2.0。

### 11.2 可借鉴机制

- 明确区分 physical view 和 functional view；智能体应操作稳定的功能能力，不直接理解底层设备协议。
- `Thing -> Channel -> Item` 把设备、设备能力和应用状态/命令分层，Binding 是协议适配器，Link 只启用选定能力。
- Semantic Model 以 `Location -> Equipment -> Point + Property` 组织房间、设备、测量和控制点，适合自然语言 target resolution。
- `postUpdate` 与 `sendCommand` 分离：更新软件状态不等于向物理设备发命令。这一语义应直接映射为 ESP-111 的 `reported_state` 与 `desired_action`。
- 规则触发来源包括 item command/update/change、group member、time、system、thing 和 discrete channel event，适合构建确定性自动化而不是让 LLM 持续轮询。

### 11.3 不应照搬

- Java/OSGi 运行时对当前 Express/SQLite 项目过重，EPL 源码复用也需额外许可证评估。
- 应借鉴领域模型和事件包络，不迁移整个 openHAB 平台。

## 12. Node-RED 与 json-rules-engine 深读

### 12.1 来源

- Node-RED：https://github.com/node-red/node-red
- Node-RED context：https://nodered.org/docs/user-guide/context
- Node-RED errors：https://nodered.org/docs/user-guide/handling-errors
- json-rules-engine：https://github.com/CacheControl/json-rules-engine
- 许可证：Node-RED Apache-2.0；json-rules-engine ISC。

### 12.2 可借鉴机制

- Node-RED 的 Message/Node/Flow/Subflow 适合表达事件驱动家居自动化，Context 明确分 node/flow/global，并可配置持久 store。
- Catch 与 Status 分开处理可捕获错误和连接状态变化，提醒我们“错误事件”和“模块状态”不能混成一个布尔量。
- json-rules-engine 用可持久化 JSON 表达递归 `all/any` 条件、优先级和异步 facts，不使用 `eval()`，与当前 Node 后端更容易做小范围 PoC。

### 12.3 实施边界

- 规则引擎只能产出 `proposed_action`，不能直接调用 S3/C5 或修改 reported state。
- Node-RED 若未来使用，只能是受控规则编辑器/sidecar；其输出仍走 ESP-server policy 与 command transaction。
- 当前不建议把 Node-RED 编辑器嵌入 `server.js` 或暴露公网，避免引入新的管理面和部署权限。

## 13. ESPHome 与 ESP-Matter 深读

### 13.1 ESPHome

- 来源：https://developers.esphome.io/architecture/components/ 、https://esphome.io/automations/ 、https://esphome.io/components/safe_mode/
- 仓库：https://github.com/esphome/esphome
- 许可证：仓库标注 View license，最终文档需按具体复用对象核对；本轮只借设计。
- `CONFIG_SCHEMA + final validation + to_code` 展示了“先强校验配置，再生成执行代码”的边界；组件还有 dependency、conflict、auto-load、deprecation alias。
- `Component/PollingComponent` 生命周期、非阻塞 loop、setup priority、safe shutdown/teardown 适合设备能力 adapter 设计参考。
- Safe Mode 在连续启动失败后只保留日志、网络和 OTA，体现控制系统需要独立于智能体的恢复面。

### 13.2 ESP-Matter

- 来源：https://docs.espressif.com/projects/esp-matter/en/latest/esp32/introduction.html 、https://docs.espressif.com/projects/esp-matter/en/latest/esp32/controller.html
- 仓库：https://github.com/espressif/esp-matter
- 许可证：Apache-2.0。
- C5 可用于 Matter Thread 设备，S3 可用于 Matter Wi-Fi/commissioning 方案，但这不表示当前 ESP-111 已接入 Matter。
- Controller 通过 fabric/NOC/ACL 获得 Administrator 或 Operator 权限，设备交互分 invoke command、read attribute/event、write attribute 和 subscribe。
- success/error/timeout callback 与 attribute/event report 是可靠控制面的基本合同；Server agent 的 tool result 也应保留这些状态，而不是只返回文本。
- Matter 可作为未来真实家居 adapter，不应在第一阶段重构 C5/S3 现有协议。

## 14. 语音项目深读

### 14.1 Home Assistant Assist Pipeline

- 来源：https://developers.home-assistant.io/docs/voice/pipelines/
- Pipeline 明确分 wake word、STT、intent、TTS，并为每阶段发 start/end/progress/error 事件和 run timeout。
- 音频上传前可由本地 VAD 限制无效流量；这与 C5/S3 本地实时能力边界相符。

### 14.2 Wyoming

- 仓库：https://github.com/OHF-Voice/wyoming
- 许可证：MIT。
- 是 voice assistant 的 peer-to-peer TCP 协议，核心格式为 JSONL header + 可选 JSON data + PCM payload。
- Event type 覆盖 audio、describe/info、ASR、TTS、wake、VAD、intent、handler 和 satellite；`info` 能声明模型、语言、streaming、external VAD 和 `supports_home_control`。
- 适合未来拆分服务器内部 ASR/TTS/wake sidecar，但裸 TCP 不应暴露公网，也不替代 agent policy。

### 14.3 Rhasspy Hermes

- 仓库：https://github.com/rhasspy/rhasspy-hermes
- 许可证：MIT。
- 这是 Rhasspy 的 MQTT Hermes protocol Python classes，与 Nous Research Hermes Agent 不是同一项目。
- CLI 可通过 MQTT topic + JSON 消息驱动 transcription、intent、TTS、wake word；可作为历史消息协议参考，不作为本轮主智能体模板。

### 14.4 边缘模型选择

- microWakeWord 面向 TFLite Micro/低功耗 MCU，可作为 S3 唤醒词 PoC；仍需真实家庭噪声 FAR/FRR 验证。
- openWakeWord 更适合服务器或较强设备，官方资料表明不适合直接当作 S3 中文量产默认，且内置模型许可证需单独核对。
- whisper.cpp 适合服务器侧本地 ASR，不适合 ESP32 内存预算。
- Piper 可作本地 TTS 进程，但当前项目/voice 模型许可证需逐项审查，不应默认打包分发。

## 15. Letta 与 Mem0 深读

### 15.1 Letta

- 来源：https://docs.letta.com/guides/core-concepts/stateful-agents 、https://github.com/letta-ai/letta
- 许可证：Apache-2.0。
- Stateful agent 由 system prompt、memory blocks、messages 和 tools 组成；run 可包含多个 step。
- 重要 memory block 固定在上下文，其余历史即使被压缩/驱逐仍保存在数据库并可检索。
- Tool 分 server-side、MCP 和 client-side，schema 与执行位置分离。
- 当前 `letta-ai/letta` README 已说明该仓库是 legacy server，活跃开发迁移；因此只借鉴状态/run/memory 概念，不选作 ESP-111 基座。

### 15.2 Mem0

- 来源：https://docs.mem0.ai/open-source/overview 、https://github.com/mem0ai/mem0
- 许可证：Apache-2.0。
- 强调 User/Session/Agent 多级记忆、语义/关键词/实体检索和自托管服务。
- 当前 README 明确说明部分最新 benchmark 包含托管平台专有优化，不能把指标直接当作 OSS 部署保证。
- ADD-only 累积事实适合回忆，但不适合作为实时设备状态真相；ESP-111 首先应增强现有 SQLite memory 的 provenance、TTL、conflict 和 activation review。

## 16. 策略与工具协议深读

### 16.1 OPA 与 Casbin

- OPA：https://www.openpolicyagent.org/docs ，Apache-2.0。
- Casbin：https://casbin.apache.org/docs/overview/ ，Node 实现：https://github.com/apache/casbin-node-casbin 。
- OPA 把 policy decision 与 enforcement 解耦，接受 JSON input，以 Rego 评估复杂层级数据；适合未来多服务统一策略，但首期 sidecar/语言成本较高。
- Casbin 原生支持 Node 的 ACL/RBAC/ABAC，适合 HTTP/tool 层 `subject-object-action` 授权；它不负责 authentication，也不能单独表达家居动作的物理风险闭环。
- 推荐首期采用统一认证 + Casbin/等价 RBAC 做入口权限，另设领域 policy evaluator 处理 freshness、target、risk、occupancy、approval 和 device capability。

### 16.2 MCP

- Tool spec：https://modelcontextprotocol.io/specification/2025-11-25/server/tools
- Security：https://modelcontextprotocol.io/docs/tutorials/security/security_best_practices
- TypeScript SDK：https://github.com/modelcontextprotocol/typescript-sdk
- Tool 需要唯一 name、description、JSON Schema input 和符合 output schema 的 structured result；服务器必须验证输入、鉴权、限流、清洗输出，客户端应对敏感调用确认、超时、审计和结果校验。
- 安全规范特别警告 confused deputy、token passthrough、SSRF、session hijacking、local server compromise 和 scope inflation。
- 2026-07-10 的 TS SDK `main` 是面向 2026-07-28 spec 的 v2 beta；仓库明确要求生产仍用 v1.x，不能把 main 直接列为稳定依赖。
- ESP-server 可在后期提供内部 MCP adapter，但 MCP 只是 tool transport，不是授权或执行真源。

## 17. 可观测性方向

- OpenTelemetry GenAI 语义约定已从主文档迁到：https://github.com/open-telemetry/semantic-conventions-genai
- 新仓库覆盖 GenAI client、MCP、provider spans/metrics/events，Apache-2.0；当前 README 的 schema URL 仍有 TODO，说明规范仍在演进。
- 实施计划应先定义稳定内部字段：`trace_id/run_id/step_id/tool_call_id/approval_id/command_id/device_id`，再做 OTel adapter，不把实验语义约定写死进业务表。

## 18. 扩展候选池

| 项目 | 角色 | 当前结论 |
| --- | --- | --- |
| Hermes Agent | 通用 agent 生命周期 | 深读，借 loop/tool/memory/cron/security，不直接嵌入通用工具。 |
| Home Assistant Core | 家居 intent/LLM/entity | 深读，借动态最小工具集与 action/query/error 结果。 |
| openHAB Core | 设备语义/Binding/规则 | 深读，借 Thing/Channel/Item 与 semantic model。 |
| Node-RED | 可视化事件流 | 深读，可选 sidecar，不是 truth 或执行入口。 |
| json-rules-engine | Node JSON 规则 | 深读，适合 deterministic rules PoC。 |
| ESPHome | 设备组件/恢复 | 深读，借 schema/final validation/lifecycle/safe mode。 |
| ESP-Matter | 标准家居设备模型 | 深读，后期真实 adapter。 |
| Wyoming | 本地语音服务协议 | 深读，后期内部 voice sidecar。 |
| Rhasspy Hermes | 历史 MQTT 语音协议 | 对照，消除同名歧义。 |
| microWakeWord | MCU wake word | PoC 候选。 |
| whisper.cpp | 服务器本地 ASR | 可选 sidecar。 |
| Piper | 服务器本地 TTS | 可选，先审许可证。 |
| LangGraph | state graph/HITL | 深读，借 checkpoint/interrupt，不首期引入 Python。 |
| Temporal | durable workflow | 深读，借 activity/idempotency；多实例后再评估。 |
| Letta | stateful agent/memory | 深读，借 run/step/block；当前旧仓库不作基座。 |
| Mem0 | 外部 memory layer | 深读，后期离线对照。 |
| Eclipse Ditto | digital twin | 深读，借 reported/desired/policy/event。 |
| OPA | 通用 policy engine | 对照，多服务阶段候选。 |
| Casbin Node | RBAC/ABAC | 首期依赖候选。 |
| MCP TypeScript SDK | 标准工具协议 | 后期 adapter；生产暂用 v1.x。 |
| OTel GenAI conventions | trace 语义 | 对照，先稳定内部 trace contract。 |

## 19. Hermes Home Assistant 插件的真实边界

- 固定源码：https://github.com/NousResearch/hermes-agent/tree/f8361d29c8e2a2be6ba9ada32f1d694bf47a4b6a/plugins/platforms/homeassistant
- `adapter.py` 通过 Home Assistant WebSocket 订阅 `state_changed`，把变化转换成文本 `MessageEvent` 交给 Hermes agent；出站只调用 `persistent_notification.create` 或 `notify.notify`。
- 该插件有 domain/entity filter、每实体 cooldown、重连退避和 4096 字符通知上限，但它不是实体能力发现、service call 控制、事务 ACK 或 reported-state reconciliation adapter。
- 因此不能用“借 Hermes Home Assistant 插件”替代 ESP-111 的 typed tool、policy 和命令闭环。未来 Home Assistant 接入应单独实现受限 adapter，Hermes 插件仅作事件网关/通知模式参考。
- 本轮固定的 Hermes `main` 提交为 `f8361d29c8e2a2be6ba9ada32f1d694bf47a4b6a`，最终文档应注明这是 2026-07-10 的研究快照，不承诺未来目录稳定。

## 20. AgentDojo 与 tau 评测体系

### 20.1 AgentDojo

- 仓库：https://github.com/ethz-spylab/agentdojo
- 文档：https://agentdojo.spylab.ai/concepts/task_suite_and_tasks/
- 版本：文档显示 v0.1.35，MIT；API 仍标注 under development。
- TaskSuite 明确绑定 environment type、可用 tools、初始 `environment.yaml` 和可植入不可信内容的 `injection_vectors.yaml`。
- UserTask 同时定义自然语言任务、utility 判定和 ground-truth tool calls；InjectionTask 定义攻击者目标、security 判定和 ground-truth attack calls。utility/security 既可看最终环境 diff，也可看调用 trace，能识别“最终回答安全但中间已经执行危险动作”。
- 对 ESP-111 最合适的用法是自建 `smart_home` suite：数字孪生环境保存房间、设备、reported state、权限和命令计数；正常任务验证完成度；注入任务验证跨家庭、跨设备、未授权 R2-R4 动作、记忆污染和工具输出注入。
- AgentDojo 适合离线安全基准，不进入生产 runtime，也不能代替确定性 policy。

### 20.2 tau-bench / tau³-bench

- 旧仓库：https://github.com/sierra-research/tau-bench 明确警告 airline/retail tasks 已过时，不应继续作为当前任务基线。
- 当前仓库仍名为 `tau2-bench`，但 README 和 release 已升级为 tau³-bench 1.0.0：https://github.com/sierra-research/tau2-bench ，MIT。
- tau³-bench 把每个领域建模为 policy + tools + tasks + 可选 user tools，并支持文本 half-duplex、语音 full-duplex、knowledge retrieval、轨迹浏览和多次 trial。
- 可借鉴点不是它的客服领域数据，而是“政策遵从 + 正确工具轨迹 + 最终环境状态”的联合评测，以及重复 trial 的 `pass^k` 思维。智能家居验收不能只看一次自然语言回答。
- 推荐建立 golden household scenarios，并对每个模型/提示版本重复运行；关键安全不变量按最坏一次失败判定，不用平均成功率掩盖偶发危险动作。

## 21. Promptfoo 与 OWASP Excessive Agency

- Promptfoo agent guide：https://www.promptfoo.dev/docs/red-team/agents/
- OWASP 2025 LLM06：https://genai.owasp.org/llmrisk/llm062025-excessive-agency/
- Promptfoo 把 agent 测试分为黑盒端到端、组件级 direct hook 和基于 trace 的 glass-box；现成 plugin 覆盖 RBAC、BOLA、BFLA、SSRF、tool discovery、excessive agency、cross-session leak、RAG/memory poisoning。
- 轨迹断言可检查 tool used、tool args、tool sequence、step count 和 goal success。对 ESP-111 而言，必须检查 policy deny 是否发生在敏感 tool/command 之前，不能只检查最终回复有没有说“拒绝”。
- OWASP 将 excessive agency 根因归纳为 excessive functionality、permissions、autonomy；直接要求最小扩展、最小功能、避免 shell/URL 等开放工具、用户上下文执行、高影响动作 HITL、下游 complete mediation。
- 这直接支持本设计：普通家居 agent 不提供 shell、文件、浏览器、任意 HTTP、管理、ACK、ingest 或 token 工具；授权必须在 tool facade 和下游 command adapter 再验证一次，不能依赖 system prompt。

## 22. Liteque 轻量队列参考

- 仓库：https://github.com/karakeep-app/liteque
- 当前 npm 版本：0.8.0；MIT；定位是 Node.js typesafe SQLite job queue。
- `queue.ts` 使用 `(queue,idempotencyKey)` 冲突忽略、priority + createdAt 排序、delay/availableAt、有限 retry、running expiry 和随机 `allocationId` CAS；领取时事务内检查旧 allocation，再更新新 allocation/expiry。
- 可借鉴：SQLite 单机队列、lease token/CAS、超时重新领取、delay、priority 和幂等键，适合 ESP-server 的第一阶段。
- 不足：完成任务默认删除，缺少完整 attempt/audit history；没有 heartbeat 续租；取消只覆盖非 running；没有 command ACK/reported-state reconciliation、transactional outbox 或人工审批。不能把它原样当作 Agent runtime。
- 结论：借语义并自建 `job_runs + lease_token + attempts + outbox`；是否引入 npm 包留到实现 spike，计划不先锁定依赖。

## 23. Debezium Outbox 参考

- 官方 3.6 文档：https://debezium.io/documentation/reference/stable/transformations/outbox-event-router.html
- 页面固定修订：`233adb44b0a4d97489c054fe02f28d837514c2db`。
- 默认 outbox 字段为唯一 event `id`、`aggregatetype`、有序分区键 `aggregateid`、事件 `type` 和 JSON `payload`；事件 ID供消费者去重，aggregate ID用于保持同一聚合的消息顺序。
- outbox 只接受 INSERT 语义，业务状态与 outbox row 必须在同一数据库事务提交；发布仍是 at-least-once，消费者必须幂等。
- ESP-111 不需要在单机首期引入 Kafka/Debezium，但应采用同样的 `event_id/aggregate_type/aggregate_id/event_type/schema_version/correlation_id/causation_id/payload` 合同，并由 SQLite dispatcher 提供 SSE replay 和后续外部投递。

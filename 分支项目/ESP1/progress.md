# 进度日志

## 会话：2026-07-10 项目熟悉

### 阶段 1：仓库与入口盘点
- **状态：** complete
- 执行的操作：
  - 读取并沿用已有 `task_plan.md`。
  - 运行规划会话恢复检查；未发现需要恢复的输出。
  - 查询历史记忆索引，仅作为待 live source 验证的线索。
  - 确认顶层与嵌套后端 Git 根、分支和工作区状态。
  - 盘点顶层目录、固件工程配置、工具与主要文档。
  - 读取三套固件 `main/main.c` 与根/主组件 CMake 入口。
  - 识别 2026-07-05 总览中已漂移的顶层 Git 描述。
- 创建/修改的文件：
  - `task_plan.md`：追加本轮只读熟悉计划。
  - `findings.md`：创建本轮发现记录。
  - `progress.md`：创建本轮进度记录。

### 阶段 2：并行梳理三层实现
- **状态：** complete
- 执行的操作：
  - 已并行派发 C51/C52、S3、ESP-server 三个只读源码扫描。
  - 核对 active 协议头哈希和 S3 Server route 边界。
  - 读取 2026-07-09 C5/S3 code-smell 审计并对照当前工作区。
  - 确认 `tools/csi-debug-web` 的定位、入口和隔离边界。
  - 识别顶层仓库与嵌套仓库对 `ESP-server` 文件的双重追踪。
  - 核对 C5 当前身份、功能开关、CSI 周期和 `/local/v1` URL guard。
  - 核对 S3 当前启动顺序、本地 HTTP/UDP 面、Server client 路由和网络 worker 分发。
  - 核对后端 live route surface、启动迁移顺序与 Canonical CSI route。
  - 识别 `ESP-server/docs/api.md` 对 CSI ingest 的合同漂移。
  - 盘点固件 build/compile database、PSRAM/CSI 默认配置和分区样板。
  - 跟踪 C5 CSI 特征链、BME 空气质量入口、C5 event bus 与 S3 priority event bus/worker 分工。
  - 核对 C5 register/heartbeat/status/BME/CSI/command 的实际 HTTP 与 UDP transport 分流。
  - 读取项目硬规则与最新架构快照，确认默认只读目录和禁止操作。
  - 核对 active partition table、ESP-IDF targets 和现存构建产物时间。
  - 核对 S3 BME RAM cache/replay、network stable gate、CSI latest/coalesce 和 child registry truth ownership。
  - 完成 ESP-server 技术栈、路由/服务/DB、鉴权、存储、前端托管和独立 Git 状态扫描。
  - 确认后端 smoke 测试会启动临时服务，本轮按只读边界未运行。
  - 完成 C51/C52 启动、组件、CSI/BME 算法、runtime、transport、parity 与 dirty state 扫描。
  - C5 agent 执行只读 `git diff --check`，当前差异未发现 whitespace error；未构建。
  - C5、ESPS3 与 ESP-server 三个子扫描已完成。
  - S3 agent 执行只读 `git diff --check -- ESPS3`，当前差异未发现 whitespace error；未构建。

### 阶段 3：端到端契约核对
- **状态：** complete
- 执行的操作：
  - 逐段核对 BME、CSI、状态、命令、语音和 smart-home 的 C5/S3/Server 入口与出口。
  - 区分源码事实、静态检查和必须由构建/硬件/在线服务验证的结论。

### 阶段 4：交付项目熟悉摘要
- **状态：** complete
- 执行的操作：
  - 汇总当前三层架构、五条主链、构建入口、Git 双重归属和高风险维护点。
  - 将 live source 与旧文档、未提交工作树、硬件运行结论分开。
  - 准备最终项目接管摘要。

## 验证结果

| 检查 | 预期结果 | 实际结果 | 状态 |
|------|---------|---------|------|
| 规划上下文恢复 | 识别旧计划且不覆盖 | 两轮旧计划均已完成，本轮已独立追加 | 通过 |
| Git 边界 | 区分顶层与后端仓库 | 顶层 `main`、嵌套后端 `api`，两处均有既有改动 | 通过 |
| 固件入口 | 入口保持薄层并交给 orchestrator | C5 两份入口同构；S3 入口交给 gateway orchestrator | 通过 |
| C5 协议 parity | C51/C52 active 协议头一致 | SHA-256 完全一致 | 通过 |
| 调试工具边界 | 不影响正式 Server 或固件 | 本地 8787、内存态、无正式后端连接 | 通过 |
| CSI 上云边界 | 由 S3 发送 CanonicalEvent，Server 不处理 raw CSI | `/kernel/csi_event` 专用 route，通用 ingest 拒绝 CSI | 通过 |
| C5/S3 静态差异 | 当前差异没有 whitespace error | 两个子扫描的 `git diff --check` 均通过 | 通过 |
| 业务源码范围 | 本轮不修改业务源码 | 仅更新根目录规划记录；固件/后端/前端/DB 未编辑 | 通过 |

## 错误日志

| 时间 | 错误 | 尝试次数 | 解决方案 |
|------|------|---------|---------|
| 2026-07-10 | 工作区大量既有改动，无法由本轮归因 | 1 | 只读检查并在结论中明确作为背景状态 |

## 五问重启检查

| 问题 | 答案 |
|------|------|
| 我在哪里？ | 阶段 4 已完成：接管摘要已准备 |
| 我要去哪里？ | 本轮已完成，等待后续明确任务 |
| 目标是什么？ | 基于当前源码熟悉 ESP-111 项目 |
| 我学到了什么？ | 见 `findings.md` |
| 我做了什么？ | 见上方记录 |

## 会话：2026-07-10 代码质量与逻辑优化审计

### 阶段 1：范围、度量与基线
- **状态：** complete
- 执行的操作：
  - 承接与用户原文一致的活动 `/goal`；未重复创建目标。
  - 完整读取 `using-superpowers` 与 `planning-with-files-zh`，并运行规划会话恢复脚本。
  - 读取现有规划记录，保留早先未完成的稳定性审计，不覆盖旧内容。
  - 查询 ESP-111 审计记忆索引和 2026-06-16 只读审计摘要，仅作为 live-source 定位线索。
  - 确认当前顶层与嵌套后端两个 Git 根及大量既有改动，不归因、不回退。
  - 建立本轮四阶段计划、证据分级和最终报告路径。
  - 排除生成物/依赖/归档后统计活跃 C/JS 文件体量，定位 S3、后端与 C5 的高复杂度候选。
  - 核对顶层与嵌套仓库 diff 规模，确认本轮审计对象为大量既有未提交改动的 live snapshot。
  - 读取项目硬规则和 2026-07-09 code-smell 报告；旧报告仅作候选导航。
  - 复核 S3 offline policy 与本地 health handler，确认健康语义混淆和无锁共享状态候选。
  - 复核 S3 本地 command ACK、RAM 槽复用与 Server 超时重领主链，确认重复执行窗口的前半段证据。
  - 核对 S3 默认公网 HTTP URL、固定 SoftAP 密码、空 auth token 及请求头注入位置，待结合后端强制策略定级。
  - 闭环复核 command ACK 入队、worker 失败处理、Server 重领和本地成功返回，确认无限阻塞与重复执行窗口。
  - 读取后端 gateway auth，确认未配置 secret 时 fail-open，并与 S3 默认空 token/公网 HTTP 组合成条件性高风险。
  - 盘点后端启动/关停与 route surface，确认缺少独立 liveness/readiness。
  - 统计顶层实际追踪 95 个 `ESP-server` 文件，确认双 Git 根对同一后端的版本归属分裂。
  - 核对四份协议头哈希，确认 C51/C52 一致、S3 与共享参考版有角色性差异，避免机械 parity 误报。
  - 复核 Canonical CSI validate/insert/event/broadcast 与数据库 schema，确认 trace/tick 未形成幂等键的重复事实风险。
  - 盘点后端 package scripts 和 3,749 行 smoke harness，确认覆盖集中在单一串行 `run()` 的结构债务。
  - 对照 live device route 与 `docs/api.md`，确认多处仍宣称通用 ingest 接收已迁出的 `csi.motion`。
  - 串联 S3 BME RAM cache/replay 与后端 sensor INSERT/schema，确认响应丢失会重复落库、刷新状态和重复告警。
  - 识别 BME cache 的正确边界：有界 RAM 离线缓冲而非重启可恢复的 durable queue，避免把合理限流/in-flight 设计误报。
  - 追踪 gateway auth/binding、device/module 状态、主事实与 event log 的每请求 SQL 链，确认固定写放大和多表非事务部分提交风险。
  - 读取公开 SSE subscribe/broadcast 实现，确认无连接上限、per-client timer 与忽略 write backpressure 的资源风险。
  - 横向核对 command、smart-home、event、voice、memory routes 与 server 挂载，确认控制/破坏性/隐私/计费端点缺统一用户或管理员授权。
  - 审计 unique-index 迁移和 duplicate-key smoke，确认历史重复时跳过不变量、caller 忽略并继续启动的降级路径。
  - 抽样审计 Dashboard 动态 HTML、timer 与本地 CSI 工具；确认前端转义/清理和工具 loopback/请求体/history 边界等合理设计。
  - 记录 Dashboard 全局脚本单体与 CSI 工具无换行串口 buffer/设备 key 数量未封顶的低中优先级债务。
  - 通过函数职责扫描确认 S3 network worker/scheduler/protocol adapter 和后端 dashboard/state store 的结构性上帝模块，而非只按行数判断。
  - 核对 `.env.example`，确认未列 gateway auth/admin 安全部署项，默认示例会放大 fail-open 风险。
  - 抽查 C52 CSI tests，确认整段仍硬编码 C51/local-id-1/link-C51 且未进入常规 test harness，属于复制粘贴假覆盖。
  - 串联 S3 peer IP/MAC 学习、local pending 和 ACK handler，确认声明式设备身份可覆盖映射且命令读取/ACK 未绑定请求 peer/target。
- 创建/修改的文件：
  - `task_plan.md`：追加本轮代码质量与逻辑优化审计计划。
  - `findings.md`：追加范围、判定口径与当前基线。
  - `progress.md`：追加本轮进度。

### 阶段 2：四线并行深审
- **状态：** in_progress
- 执行的操作：
  - 已派发 C51/C52、ESPS3、ESP-server 三条只读子审计；根代理负责跨层、前端/工具和最终复核。
  - 子审计期间检测到用户侧仍在修改 S3 replay 源码，已要求所有 P0/P1 在结束前重读 live source。
  - ESP-server 子审计完成：18 条高信号发现，补充删除语义、Dashboard cache/DB 顺序、legacy 边界、共享事务、smart-home lease、状态 reconciliation、历史统计与 job 竞争问题。
  - 后端子审计末次复核未发现并发修复；非 public JS `node --check` 通过，未运行服务/smoke、未访问真实 DB。
  - C51/C52 子审计完成：12 条高信号发现，确认 voice/HTTP exclusive、CSI event/worker/gap、BME gas validity、placeholder ACK、link down、回压指标、手写 JSON、计数回绕和双份源码问题。
  - C5 子审计已按 live source 复核，未运行 build/flash/monitor；同时记录 URL guard、raw CSI 边界、锁外 handler 和 BME 有界等待等合理设计。
  - 本次上下文恢复确认活动 `/goal` 与代码质量审计原文一致；完整恢复根规划、发现和进度文件，未重新开始审计。
  - 重新确认顶层 `main` 与嵌套后端 `api` 均有大量既有改动，且 `ESP-server/db/database.db` 已 dirty；本轮不归因、不回退、不访问真实数据库。
  - 再次派发 C51/C52、ESPS3、ESP-server/跨层三条只读末次复核，统一要求当前行号、触发条件、影响、建议和证据等级，根代理负责去重与报告。
  - 确认稳定性报告已存在，但代码质量综合报告 `docs/project-code-quality-and-logic-audit-2026-07-10.md` 尚未生成；只复用经 live source 复核的重叠证据。
  - 末次重读确认后端 gateway token 缺失时仍 fail-open、SSE 忽略 backpressure、BME/CSI 均无稳定幂等唯一键且多步写入无事务；当前行号已重新采集。
  - 一次保护规则搜索范围误扩到工作区上级并触发 macOS 受保护目录权限错误；已停止该方式，后续严格限定工作区路径。
  - 主线程完成 S3 command ACK、resource lifecycle、server health/offline policy 的当前源码末次复核，并校正“cJSON 解析后仍以未转义字符串拼 JSON”的具体根因。
  - 重算主要结构热点行数，确认 S3 三个上帝模块与后端 service/test/frontend 单体仍存在，但不以行数直接定罪。
  - 主线程确认 C5 24 深度共享 FIFO、CSI callback 每帧入队和同 worker 串行 compute/report 的耦合；WiFi 文件路径假设错误已记录并改用文件索引定位。
  - 主线程完成 C5 voice quiescence/cancel、BME gas validity 与 C52 CSI 假覆盖的 live-source 复核，当前行号已固定。
  - 完成 C51/C52 live parity、双 Git 归属计数和 CSI API 文档漂移复核；仅两个身份头存在预期差异。
  - 一次安全配置文档扫描引用了不存在的 `ESP-server/README.md`；已记录并改为只扫描现有文件清单。
  - 主线程完成后端 migration、共享事务、S3 child stale、smart-home lease/ACK 与 SSE shutdown 的当前源码复核。
  - 对照 2026-06-16 与 2026-07-09 报告，区分已收敛、仍存在和本轮新增问题；旧报告只作为差异基线。
  - 无副作用静态验证通过：顶层及嵌套仓库 `git diff --check`、工作区 JS 全量 `node --check`、C51/C52 parity allowlist、C5 Server-route 与 raw-CSI 外发负面扫描。
  - 静态扫描只发现 C5 本地 S3 HTTP 基址和绝对 URL 拒绝逻辑；raw CSI 关键词只出现在 S3 拒绝名单，未发现正式序列化外发。

### 阶段 3：交叉验证与去重
- **状态：** complete

### 阶段 4：报告与静态验证
- **状态：** complete
  - 已创建 441 行综合报告，P0/P1/P2/P3 数量与总览一致，章节、路线图、限制和正向设计均齐全。
  - 首次引用校验发现 4 个缩写路径并已补全；同时发现 zsh 特殊变量 `path` 覆盖 `PATH`，已记录并准备使用修正版重跑行号上界检查。
  - 修正版引用校验通过：报告内所有完整源码路径存在，所有显式行号均未超过当前文件长度，已无 `...` 缩写路径。
  - 报告计数为 2 个条件性 P0、12 个 P1、7 个 P2、2 个 P3；无 TODO/TBD/FIXME、待补、待确认或候选项。
  - 报告及规划文件 `git diff --check` 通过；业务源码、前端、数据库和运行配置未由本轮编辑。
  - 最终交付：`docs/project-code-quality-and-logic-audit-2026-07-10.md`。

## 代码质量审计完成检查

| 要求 | 权威证据 | 结论 |
|---|---|---|
| 判断是否存在“屎山” | 报告第 1、5、12 节 | 已完成：定位 S3 三个结构性热点，并区分 C5 复制与后端单体 |
| 找出逻辑优化代码 | 报告第 3-6 节 | 已完成：按 P0/P1/P2/P3 给出触发、影响、改法与验收 |
| 写成报告 | 441 行最终报告文件 | 已完成 |
| 当前源码证据 | 引用文件存在/行号上界校验、live-source 复核 | 已完成 |
| 不误改项目 | 本轮仅报告与规划记录；真实 DB 未访问 | 已完成 |
| 说明验证边界 | 报告第 11 节 | 已完成：明确未 build、未 runtime、未真机 |

## 会话：2026-07-10 稳定性与问题处理流程审计

### 阶段 1：审计基线与证据框架
- **状态：** complete
- 执行的操作：
  - 承接与用户请求完全一致的活动 `/goal`。
  - 读取 `using-superpowers`、`brainstorming`、`planning-with-files-zh`；确认本轮是分析型审计，不进入功能实现审批流程。
  - 恢复并读取已有 `task_plan.md`、`findings.md`、`progress.md`，保留上一轮已完成记录。
  - 确认顶层与嵌套后端两个 Git 根均存在大量既有改动，不归因、不回退。
  - 追加本轮五阶段审计计划与证据框架。
  - 读取项目硬规则，确认业务源码、前端、数据库、运行服务和构建产物均不得修改。
  - 读取 2026-07-09 C5/S3 code-smell 报告，将其作为 live-source 复核导航，而非当前结论来源。
  - 并行派发 C5、S3、ESP-server 三条只读审计线，统一要求行号、触发条件、影响、建议和验收证据。
  - 对照 API 边界文档与结构总结，识别 CSI route、Git 根、CSI 开关/周期等会误导排障的文档漂移。
  - 扫描三层 trace/request/error/health/metrics 关键词，确认 CSI 关联能力较完整，但跨域关联标识和运行状态面仍需逐文件核验。
  - 读取 Server 生命周期和 S3 运行时公开状态，记录 graceful shutdown、无 readiness/liveness、无关停 deadline、S3 load 指标未形成统一诊断面等候选问题。
  - 扫描 C5 gateway link/WiFi 重连与 S3 local health/scheduler diagnostics，确认链路恢复保护存在，同时记录同步重连和健康探针信息不足候选风险。
  - 精读 C5 重连与 S3 health/offline policy，确认 C5 并发锁保护、无 jitter，以及 S3 把业务错误等同 Server 不可用且全局状态无锁的问题。
  - 扫描三层 command pending/dispatch/ACK 语义，记录异步 ACK 与 Server 超时重发之间可能产生重复执行的可靠性边界，待逐段确认。
  - 精读 command ACK 主链，确认 S3 先标 ACKED、入队失败仍向 C5 成功，以及 Server 超时重发带来的重复执行窗口；Server 终态幂等保护有效但覆盖不到该窗口。
  - 核对 S3 command queue 分配与去重，确认 ACKED 槽可立即复用、去重记录不持久，重复执行窗口成立。
  - 发现 S3 live source 在审计期间并发变化；已向三个只读子审计确认均未编辑，并把最终 live-source 复核设为报告门禁。
- 创建/修改的文件：
  - `task_plan.md`：追加稳定性审计计划。
  - `findings.md`：追加审计范围与初始基线。
  - `progress.md`：追加本轮进度。

### 阶段 2：并行审计三层实现
- **状态：** complete
  - 精读 ESP-server SQLite helper、event persistence 与 SSE client 管理，确认缺少 DB runtime PRAGMA、SSE backpressure 和集中关停能力。
  - 扫描后端路由鉴权并精读 gateway auth/binding，确认 gateway token 未配置时 fail-open，以及多组管理/写接口缺少统一 admin guard。
  - 扫描 BME cache/replay、CSI trace 与 Server 表唯一键，记录离线数据 RAM 易失和 ingest 幂等不足候选问题。
  - 精读 BME/CSI schema 与写入顺序，确认无幂等唯一键、跨多步写入无事务、失败重试可重复，以及 S3 BME ring 重启清空/满时覆盖仅日志的问题。
  - 搜索现有 runbook/incident/排障资料，确认相关想法分散在迁移计划中，尚未形成当前可执行的事故处理流程。
  - 对照部署健康检查和最小联调清单，确认 readiness 覆盖不足、CSI 拓扑漂移、破坏性操作冲突与配置文档化风险。
  - 扫描固件 Task WDT、reset reason 与 coredump 路径，确认 S3 关键 worker 已有 WDT，C5 和跨设备 crash 证据链仍不足。
  - 盘点 ESP-server 测试入口与 smoke 覆盖，确认已有业务回归基础，但稳定性故障注入和分层测试入口不足。
  - 扫描 CI/parity/release gate，确认当前没有自动工作流或脚本，验证依赖历史文档中的手工命令。
  - 核对三套分区、rollback/coredump/security 配置与版本标识，确认 C5 无 OTA、S3 rollback/coredump 未启用、固定版本和同名 C5 产物风险。
  - ESP-server 并行深审完成：补充 legacy auth bypass、binding ownership、共享事务、migration 假成功、stale child truth、命令语义分叉、启动/恢复与 retention 风险。
  - 后端只读 JS 全量语法与 diff whitespace 检查通过；按规则未启动服务或运行 smoke。
  - C5 并行深审完成：补充 CSI 共享总线击穿、短抖动假 READY、voice quiescence/abort、BME validity/recovery、CSI recalibration 与 subsystem health 风险。
  - C51/C52 parity、diff whitespace 和 raw CSI 负面扫描通过；未构建、未做真机验证。
  - 本地精读 S3 `resource_manager` 与 lifecycle design：确认 generation/锁外 release/失败回收保护，并发现 stale ingress 校验前先更新 peer identity 的时序缺口。
  - 跟踪 AP station disconnect 映射，确认 mapping miss 会 release-all，可能因未知 station 断开误释放全部合法 C5 资源。
  - 扫描 S3 network/offline shared state，确认多个 worker 无锁读写 server health、错误计数和 pending flags，存在双核任务竞态与语义混合。
  - 最终复核 S3 command ACK：确认 protocol worker 无限入队等待、失败后释放不重试、短暂 ACKED 去重、8 槽 DISPATCHED 无回收；手工 JSON 的字符串触发面已按官方 C5 payload 收窄。
  - 精读 S3 voice/local HTTP：确认长 voice 同步 handler 阻塞双 C5 控制面，以及只校验 allowlist、不校验 peer binding 的本地身份缺口。

### 阶段 3：端到端故障链与问题处理流程
- **状态：** complete
  - 串联 BME、CSI、状态、命令、voice 五条主链的失败传播、重试、部分成功、重复副作用和状态分叉。
  - 建立统一 event/trace/boot/seq 身份合同、错误语义、数据真源和六步事故闭环。
  - 完成按现象分层的故障分诊表和不可仅凭“日志不再报错”的事故关闭条件。

### 阶段 4：优先级与改进路线
- **状态：** complete
  - 最终归并为 3 个 P0、15 个 P1、5 个 P2，并保留当前正确设计和明确不建议改法。
  - 给出 0-3 天、1-2 周、3-6 周、6 周以后四阶段路线，配套指标阈值和 14 个故障注入/soak 场景。

### 阶段 5：报告与静态验证
- **状态：** complete
  - 最终报告写入 `docs/c5-s3-espserver-stability-audit-2026-07-10.md`，包含优先级、故障链、可靠性合同、事故闭环、分诊表、监控阈值、路线图和验收矩阵。
  - C5、S3、ESP-server 所有 P0/P1 证据均回到最终 live source 复核；S3 并发演进期间的旧行号和过时判断已校准。
  - 报告最终为 565 行；P0/P1/P2 编号唯一，无 TODO/TBD/待补/待确认或尾随空白。
  - C51/C52 parity、raw CSI 负面扫描、后端全量 JS syntax、顶层目标范围与嵌套 Server `git diff --check` 均通过。

## 本轮错误日志

| 时间 | 错误 | 尝试次数 | 解决方案 |
|------|------|---------|---------|
| 2026-07-10 | 审计期间 S3 源码持续并发变化，早期行号和候选结论可能失效 | 1 | 保持只读，最终报告前重新打开关键文件并只引用最终 live source |
| 2026-07-10 | 一次规划记录补丁因历史章节同名、上下文未唯一匹配而失败 | 1 | 使用本轮尾部章节和精确文本锚点重新追加，未重复原失败补丁 |
| 2026-07-10 | S3 子审计汇总时远端响应 502 | 1 | 复用已收集上下文，要求子审计重新交付简明结果，不重复全量扫描 |
| 2026-07-10 | S3 子审计压缩汇总连续三次远端 502 | 3 | 停止重复代理调用，由根代理对最终 live source 做针对性本地复核 |
| 2026-07-10 | ESP-server 文档扫描包含不存在的 zsh `deploy*` glob，命令被 nomatch 拒绝 | 1 | 改用 `rg --files` 的真实文档列表后定向扫描 |
| 2026-07-10 | voice identity 定向扫描包含不存在的 `src/services/voice*` glob | 1 | 改为指定 `voiceRoutes.js` 和身份 helper，不重复空 glob |

### 最终收口恢复
- **状态：** complete
- 已读取活动 goal、根规划文件和 485 行报告初稿，确认当前工作从阶段 2/5 继续，不重做已完成扫描。
- 已并行派发 C5、S3、ESP-server 三条末次只读 live-source 复核；要求结束前重读引用文件，防止并发源码变化造成陈旧行号。
- 初稿缺口已全部收口：S3 独立风险条目、静态验证表述、高优先级证据和无副作用检查均已完成。
- 根代理已完成 S3 resource/command/voice live-source 复核，确认 unknown station `release_all`、ACK 非持久/infinite-wait、单 HTTP task voice 阻塞和 peer binding 缺口。
- C5 最终复核确认 P1-2/3/4/5/6 仍成立，并把 P1-2 收窄为可复现的 shared-bus + worker-timeout + dispatcher head-of-line；原“官方 C5 ACK 未转义”判断更正为“已转义但字段和 ack_seq 未进入 payload”。
- S3 第二轮复核确认 registry/resource voice 分叉、8 槽 DISPATCHED 无回收、critical path unbounded wait、WDT 非 panic，以及 local peer->device 未绑定；这些将作为独立 P1 条目和故障注入场景进入最终稿。
- S3 lifecycle 最终追踪确认首次 generation 不强制 REGISTER：heartbeat/sensor/CSI 可直接激活 RELEASED session，未 registered registry view 又绕过 timeout release；最终合同需改为 register-first + peer-bound + generation-bound。
- 已把 C5 dispatcher 头阻塞、S3 register/peer/resource 门禁、unknown disconnect、voice 状态分叉、command 槽耗尽、critical path bounded progress 写入报告，并同步扩展指标、路线图与故障注入验收。
- 中间修订版 522 行时已通过编号、占位符和 whitespace 检查，随后按独立 QA 继续补齐验收参数与证据。
- ESP-server live source 再确认 auth fail-open/RBAC/SQLite/SSE/shutdown 和文档漂移；C5 假 READY 控制流行号已校准。smart-home command ACK/lease 条件仍做最后一段复读。
- smart-home command 最终复读完成：claim 无 lease/reclaim，ACK 无旧状态条件；部署清单漏 `src/` 和 time-status 假 health 均确认。C5 voice/BME/CSI 证据仍与报告结论一致。
- 重新执行 C51/C52 `runtime`、`sensor_domain`、`server_voice`、`command_domain` 逐目录 parity，全部无差异；BME driver validity flags 存在但 AQ 更新未使用，P1-5 保持成立。
- 后端 `server.js`、`src/`、`server-time-sync/`、`scripts/` 全量 `node --check` 通过；顶层目标范围和嵌套 Server tracked diff 的 `git diff --check` 通过。
- raw CSI/IQ/subcarrier 负面扫描只命中 S3 显式 reject keys、注释和 HTTP phase 变量，未发现 production serializer/client 外发；报告尾随空白已清理。
- Server 高优先级证据复读确认：legacy 写入口无统一认证；BME/CSI 无 unique idempotency 且多步非事务；S3 来源 child 被 timeout scan 排除；控制/RBAC、SSE/shutdown/migration 结论仍成立。
- 最终完成审计要求逐项核对：C5、S3、ESP-server 均有 live-source 发现和保留项；稳定性改进、问题处理流程、路线图、验收矩阵和验证限制均已写入最终报告。
- 独立最终 QA 提出的 9 项均已修复；复核结论为“无阻塞或应修项”。最终报告 565 行，SHA-256 `f39614ddd44cc2b2896705f689acb47e4308c2435a4ac2288f4361c7ec948acf`。

# ESP-111 智能家居智能体研究进度

## 2026-07-10 13:13:42 +0800 - 目标启动

- 承接系统已创建的同名活动 goal。
- 读取 `using-superpowers`、`brainstorming`、`planning-with-files` 和应用内浏览器技能。
- 确认本轮需要先探索、比较路线和确认设计，再完成最终实施计划。

## 2026-07-10 13:14-13:21 +0800 - 第一批项目与网页研究

- 只读检查 Git 状态、现有 docs、根/嵌套规划文件和最近提交。
- 启动三条本地并行审计：Server 能力、设备/网关契约、既有文档/历史约束。
- 应用内浏览器成功连接并深读 Hermes 官方仓库与架构、agent loop、memory、skills、cron、security、messaging 文档。
- 深读 Home Assistant LLM API、Conversation API、Architecture。
- 深读 LangGraph persistence/interrupts、Temporal Activity、Eclipse Ditto overview。
- 发出单一澄清问题：Hermes 是否指 Nous Research Hermes Agent；默认继续该解释。

## 当前可恢复状态

- 活动 goal 开始：2026-07-10 13:13:42 +0800。
- 计划最早收口：2026-07-10 16:13:42 +0800。
- 当前仍处于研究阶段，不得标记 goal complete。
- 本地源码审计已确认：有数据底座，无自主 runtime；安全与命令可靠性是自治前置门槛。
- 浏览器绑定在主代理持久会话中，研究标签变量名为 `hermesTab`；恢复后若失效，应从现有 iab 绑定新建标签，不重新选择浏览器。
- 已知相关浏览器内变量：`hermesArchitecture`、`hermesFeaturePages`、`hermesFeatureSummaries`、`hermesSafetyPages`、`homeAssistantPages`、`langGraphPages`、`workflowDomainRaw`。
- 不修改 `.planning/.active_plan`，它属于另一项 `esp32-c5-s3-github-study`。

## 下一批

1. 收齐 Server 与设备/网关审计完整行号。
2. 深读 Hermes tool/session/plugin 源码或开发文档，固定版本/提交。
3. 深读 openHAB、Node-RED、ESPHome、Matter/esp-matter、Wyoming/Rhasspy。
4. 形成 18+ 项目池和 10+ 深读矩阵。
5. 向用户呈现三种路线与推荐设计，等待确认。
6. 在设计确认后写两份最终文档。

## 2026-07-10 13:21-13:40 +0800 - 第二批领域与基础设施研究

- 收齐 Server、设备/网关、历史文档三条只读审计的完整结论和路径/行号。
- 建立独立 `.planning/smart-home-agent-open-source-study/`，未触碰根规划和 `.planning/.active_plan`。
- 深读 Hermes tools runtime、SQLite session storage、context compression、plugins。
- 深读 openHAB concepts/semantic model/rules、Node-RED context/errors、json-rules-engine。
- 深读 ESPHome component/safe mode、ESP-Matter controller/access/command/read/write/subscribe。
- 核对 Home Assistant voice pipeline、Wyoming、Rhasspy Hermes，并消除两个 Hermes 的同名歧义。
- 深读 Letta、Mem0、OPA、Casbin、MCP tools/security/TS SDK、OTel GenAI 新仓库。
- GitHub API 因共享出口达到未认证限额，只成功固定 Hermes metadata；后续改用仓库 HTML 与官方文档，不重复 API 请求。
- 已向用户提出三路线与推荐路线 1，等待方向确认；不依赖选择的研究继续。

## 下一恢复点（更新）

1. 把三条本地审计中的 P0/P1 门槛映射到推荐架构。
2. 深读一个成熟 agent 评测/安全测试方向和一个 scheduler/outbox 实现参考。
3. 形成 tool catalog、risk matrix、run state machine、data model 与阶段验收草案。
4. 用户确认路线后写设计规格并自审。

## 2026-07-10 13:40-13:54 +0800 - 评测、安全与耐久语义研究

- 收齐 tool/policy、run data model 和 roadmap/test 三条只读设计审计；未修改运行代码。
- 用应用内浏览器固定 Hermes `f8361d2` Home Assistant 插件，确认其是 `state_changed -> agent` 与通知通道，不是设备控制 adapter。
- 深读 AgentDojo v0.1.35 的 TaskSuite、UserTask、InjectionTask、utility/security 和 trace 判定，形成智能家居专用安全 benchmark 映射。
- 核对旧 tau-bench 已明确过时；当前 `tau2-bench` 仓库已发布 tau³-bench 1.0.0，适合借 policy/tools/tasks/trajectory 与多次 trial 评测模式。
- 深读 Promptfoo agent red team、trace assertions 和 OWASP 2025 LLM06 Excessive Agency，固定 RBAC/BOLA/BFLA/SSRF/过度代理/记忆污染测试面。
- 深读 Liteque 0.8.0 `queue.ts` 的 allocationId CAS、lease expiry、retry、priority、delay、idempotency，以及缺少 heartbeat/audit/running cancel 的边界。
- 深读 Debezium 3.6 Outbox Event Router，固定 event ID 去重、aggregate key 顺序和同事务 outbox 语义。

## 下一恢复点（13:54 更新）

1. 固定其余项目许可证、默认分支提交和版本漂移提示。
2. 把本地 tool catalog、R0-R4、run/step/tool/approval 状态机和表设计写入设计规格。
3. 起草学习文档，先覆盖项目对照、模式、反模式、选型和阅读路线。
4. 起草八阶段实施计划；所有阶段都写依赖、非目标、回滚、验证和验收门。
5. 最早 16:13:42 后才做最终收口审计。

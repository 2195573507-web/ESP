# ESP Backend API Integration Work Plan

## Goal

Complete P0/P1/P2 firmware-side integration from `docs/esp-backend-api-integration-plan.md` while preserving the C5-lightweight/S3-server-facing boundary.

## Constraints

- Primary edits are in `ESPS3`.
- `ESPC51` and `ESPC52` edits are limited to C5->S3 lightweight frame fields and disconnect/reconnect priority.
- C5 must not connect to home WiFi or ESP-server directly.
- Do not upload raw CSI.
- Do not fake real smart-home execution; when no device exists, ACK commands as `failed`.
- Do not add `gateway.heartbeat` to `/api/device/v1/ingest`.
- Do not edit `ESP-server/public`.

## Phases

- [x] Phase 1: Read integration plan and current backend API contract.
- [x] Phase 2: Inspect current S3/C5 firmware modules and existing server-facing endpoints.
- [x] Phase 3: Implement S3 gateway snapshot, status, logs/alarms, smart-home, CSI summary upload, and non-blocking scheduling.
- [x] Phase 4: Add only required C5 lightweight frame/reconnect adjustments and keep C51/C52 aligned.
- [x] Phase 5: Update necessary docs and run syntax/build checks.
- [x] Phase 6: Final scope and completion report.

---

# Task Plan: CSI Three-Layer State Machine Refactor

## Goal

Refactor CSI into a C5 edge feature extractor, S3 gateway fusion/state-machine owner, and ESP-server state storage/streaming center.

## Constraints

- C5 never calls ESP-server `/api/...` routes.
- C5 emits feature-only CSI frames, not raw CSI, I/Q, selected subcarrier lists, or state decisions.
- S3 is the only CSI state and `motion_score` source.
- ESP-server accepts/stores/broadcasts canonical facts only and performs no signal processing.
- Dashboard may be edited only for canonical CSI score/energy/state views.

## Phases

- [x] Phase 1: Design/spec and current implementation handoff reviewed.
- [x] Phase 2: Fix compile/syntax issues from the in-progress refactor.
- [x] Phase 3: Run C51/C52 parity and firmware/server boundary scans.
- [x] Phase 4: Run backend syntax, tests, smoke checks, and dashboard validation where possible.
- [x] Phase 5: Record final validation evidence and remaining caveats.

---

# Task Plan: 熟悉 ESP-111 当前项目

## 目标

基于当前仓库源码完成一次只读项目接管扫描，明确仓库边界、三层架构、关键数据流、构建验证入口、当前改动状态与后续修改的安全边界。

## 约束

- 本轮只读取与分析，不修改固件、后端或前端业务代码。
- 当前源码与构建配置优先于历史文档和记忆。
- 顶层仓库与嵌套 `ESP-server` 仓库分开检查、分开归因。
- 明确区分源码可确认结论与需要硬件联调才能确认的结论。

## 当前阶段

完成

## 各阶段

### 阶段 1：仓库与入口盘点
- [x] 确认 Git 根、工作区状态和顶层目录职责
- [x] 读取总览文档、构建清单与应用入口
- [x] 将第一批发现记录到 `findings.md`
- **状态：** complete

### 阶段 2：并行梳理三层实现
- [x] 梳理 ESPC51/ESPC52 采集、算法、调度和上报
- [x] 梳理 ESPS3 网关、协议、融合、事件总线和 Server 通信
- [x] 梳理 ESP-server 路由、服务、存储、实时事件和测试
- **状态：** complete

### 阶段 3：端到端契约核对
- [x] 串联 C5 -> S3 -> Server 数据流与控制流
- [x] 核对 CSI、环境传感、设备状态、智能家居和语音边界
- [x] 标注源码事实、文档漂移和硬件验证缺口
- **状态：** complete

### 阶段 4：交付项目熟悉摘要
- [x] 汇总架构、关键路径、构建方式和风险点
- [x] 更新 `findings.md` 与 `progress.md`
- [x] 向用户给出可直接用于后续工作的接管结论
- **状态：** complete

## 已做决策

| 决策 | 理由 |
|------|------|
| 采用只读并行扫描 | 用户当前要求是熟悉项目，尚未授权功能改动 |
| 以 live source 为主 | 项目近期 CSI 与 runtime 架构变化较多，旧文档可能漂移 |
| 分开检查两个 Git 根 | 顶层固件仓库和嵌套后端仓库可能有独立改动历史 |

## 遇到的错误

| 错误 | 尝试次数 | 解决方案 |
|------|---------|---------|
| 暂无 | 0 | - |

---

# Task Plan: C5 / S3 / ESP-server 稳定性与问题处理流程审计

## 目标

基于 2026-07-10 当前实时源码，审计 C5、S3、ESP-server 的稳定性风险、故障发现与恢复能力、跨层问题定位链路，并交付一份带证据、优先级和落地路线的中文改进报告。

## 约束

- 本轮不修改 C5、S3、ESP-server、前端、数据库或运行配置，只新增审计文档并更新规划记录。
- 以 live source 为主；历史文档和记忆只作为定位线索，结论必须回到当前文件核验。
- C51/C52 作为同一 C5 角色审计，同时检查镜像一致性与身份差异。
- 顶层 Git 与嵌套 `ESP-server` Git 分开归因，不回退、不覆盖既有改动。
- 明确区分源码可证、静态检查可证、构建可证和必须真机/在线闭环验证的内容。
- 不运行 flash、monitor、fullclean，不启动真实服务，不写 `ESP-server/db/database.db`。

## 当前阶段

阶段 5（已完成）

## 各阶段

### 阶段 1：审计基线与证据框架
- [x] 读取项目保护规则、近期审计文档和关键入口
- [x] 固化稳定性、可恢复性、可观测性、问题处理流程的审计维度
- [x] 将基线记录到 `findings.md`
- **状态：** complete

### 阶段 2：并行审计三层实现
- [x] 审计 C51/C52 的任务、队列、网络重连、资源边界和诊断能力
- [x] 审计 S3 的调度、缓存/重放、协议融合、网络状态机和资源生命周期
- [x] 审计 ESP-server 的路由、鉴权、存储、事务、错误处理、SSE 和运维能力
- **状态：** complete

### 阶段 3：端到端故障链与问题处理流程
- [x] 对照 BME、CSI、状态、命令、语音链路识别故障传播和恢复盲区
- [x] 核对 trace/identity/time/schema/error reason 是否能跨层关联
- [x] 形成现状流程与目标闭环流程
- **状态：** complete

### 阶段 4：优先级与改进路线
- [x] 对发现按 P0/P1/P2、收益、成本和依赖去重排序
- [x] 给出立即止损、短期加固、中期治理的实施顺序与验收门槛
- [x] 标注不建议改动和需要真机验证的边界
- **状态：** complete

### 阶段 5：报告与静态验证
- [x] 写入 `docs/c5-s3-espserver-stability-audit-2026-07-10.md`
- [x] 核对所有高优先级结论的实时源码证据和行号
- [x] 运行不产生业务副作用的静态检查并记录结果
- [x] 自检报告完整性、矛盾、占位符和范围
- **状态：** complete

## 已做决策

| 决策 | 理由 |
|------|------|
| 三层并行审计、根代理负责跨层串联 | 各层源码边界清楚，稳定性问题需要独立深挖后统一去重 |
| 最终报告采用“现状证据 -> 风险 -> 改法 -> 验收”结构 | 避免只列问题而无法执行或验证 |
| 不把当前脏工作区差异归因于本轮 | 现有改动属于用户，审计必须尊重其来源与状态 |

## 遇到的错误

| 错误 | 尝试次数 | 解决方案 |
|------|---------|---------|
| 新建 `/goal` 时发现线程已有同名活动目标 | 1 | 读取现有目标并确认完全一致，直接承接 |
| zsh 对不存在的 `deploy*` 文档 glob 报 `no matches found` | 1 | 改用 `rg --files` 生成真实文件清单后再扫描，不重复空 glob |
| zsh 对不存在的 `src/services/voice*` glob 报 `no matches found` | 1 | 直接扫描已确认存在的 `src/routes/voiceRoutes.js` 与身份 helper |

---

# Task Plan: ESP-111 代码质量与逻辑优化审计

## 目标

基于 2026-07-10 当前工作区源码，只读审计 ESPC51、ESPC52、ESPS3、ESP-server 与正式链路工具中的高复杂度、重复、职责混杂、状态/并发/错误语义缺陷及可量化的逻辑优化机会，输出一份带文件行号、触发条件、影响、优先级、修改建议和验收方法的中文报告。

## 约束

- 不修改固件、后端、前端、数据库、运行配置或现有用户改动；只更新规划记录并新增最终报告。
- 当前源码优先；历史审计和记忆仅作导航，进入最终报告前必须重新核对 live source。
- 顶层 Git 与嵌套 `ESP-server` Git 分开归因；不把既有脏工作区内容归因于本轮。
- C51/C52 同时检查镜像重复成本与真实行为漂移，不因文件体量大就直接判定为问题。
- 每条高优先级结论至少包含具体文件/行号、可复现触发条件、实际影响和可验证改法。
- 不运行 flash、monitor、fullclean，不启动真实服务，不写 `ESP-server/db/database.db`。

## 当前阶段

完成

## 各阶段

### 阶段 1：范围、度量与基线
- [x] 盘点 active source、生成物/依赖排除项、Git 边界和既有改动
- [x] 统计大文件、长函数、重复实现、TODO/FIXME、危险并发/阻塞/错误吞噬模式
- [x] 建立“确认缺陷 / 结构性债务 / 逻辑优化 / 无需改”判定标准
- **状态：** complete

### 阶段 2：四线并行深审
- [x] 审计 C51/C52 算法、runtime、网络、复制一致性与资源边界
- [x] 审计 ESPS3 调度、队列、状态机、缓存/重放、协议和资源生命周期
- [x] 审计 ESP-server 路由、服务、数据库、事务、鉴权、缓存与错误语义
- [x] 审计跨层合同、可靠性、身份/时间/重试语义与调试工具边界
- **状态：** complete

### 阶段 3：交叉验证与去重
- [x] 根代理逐条复核高优先级证据和当前行号
- [x] 区分确定 bug、维护风险、性能优化和仅需真机验证的假设
- [x] 与已有 2026-06-16、2026-07-09 报告对照，标记已修复、仍存在和新增项
- **状态：** complete

### 阶段 4：报告与静态验证
- [x] 写入 `docs/project-code-quality-and-logic-audit-2026-07-10.md`
- [x] 给出 P0/P1/P2/P3 排序、收益/成本/依赖和分阶段治理路线
- [x] 运行只读或无业务副作用的报告完整性、链接/行号、重复与占位符检查
- [x] 更新规划记录并完成活动 `/goal`
- **状态：** complete

## 已做决策

| 决策 | 理由 |
|------|------|
| 只新增一份综合报告，不改业务代码 | 用户要求审计并写报告，未授权修复 |
| 四条独立证据线并行、根代理交叉复核 | 提高覆盖面，同时避免把单个扫描结果直接当结论 |
| 复杂度指标只用于定位，不按行数定罪 | 大文件可能由协议表、常量或必要状态机导致，必须结合职责和控制流判断 |
| 复用既有稳定性审计证据但重新核对 live source | 现有规划文件已有相关发现，可节省重复定位但不能替代本轮验证 |

## 遇到的错误

| 错误 | 尝试次数 | 解决方案 |
|------|---------|---------|
| `create_goal` 返回已有未完成目标 | 1 | `get_goal` 确认现有目标与用户本轮原文一致，直接承接 |
| 初次并行读取输出被合并截断 | 1 | 改用分段 `sed`、定向 `rg` 和单文件尾部读取，避免依赖截断内容 |
| 保护规则搜索误用了上级目录 `find ..`，命中 macOS 隐私目录权限错误 | 1 | 后续只在 `/Users/zhiqin/ESP-111` 工作区内用 `rg --files`/定向 `find` 搜索；该错误不影响源码证据 |
| 读取 C5 WiFi 事件时假定了不存在的 `components/wifi/wifi_manager.c` 路径 | 1 | 改用 `rg --files ESPC51 | rg 'wifi_manager\\.c$'` 定位真实路径后再读取 |
| 文档安全配置扫描包含不存在的 `ESP-server/README.md` | 1 | 后续只扫描 `rg --files ESP-server` 返回的现有 Markdown 文件；已有 `docs/` 证据不受影响 |
| 阶段状态补丁因 `progress.md` 在中断期间追加而上下文失配 | 1 | 拆分为按当前行号匹配的独立小补丁，原失败补丁未产生写入 |
| 引用校验脚本在 zsh 中用变量名 `path` 覆盖特殊 `PATH`，导致 `wc/tr/sort/head` 找不到 | 1 | 变量改名为 `file` 后重跑；首次运行只采用文件存在性结果，不采用行号上界结果 |

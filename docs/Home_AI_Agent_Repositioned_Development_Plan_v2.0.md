# Home AI Agent 项目重新定位与分端开发总计划

> 文件名：`Home_AI_Agent_Repositioned_Development_Plan_v2.0.md`  
> 文档类型：总体软件设计与实施计划（SDD + Execution Plan）  
> 适用范围：ESPC51、ESPC52、ESPS3、ESP-server、现有 ESP-server Web  
> 目标读者：Codex、后续开发人员、联调与验收人员  
> 当前版本：V2.0  
> 编制原则：以现有仓库为基础增量改造，不重建已有链路，不牺牲语音、雷达、BME690、网络、告警和命令链路稳定性。

---

# 0. 文档目的

本文件用于重新定义整个 Home AI Agent 项目的产品定位、端侧职责、跨端协议、规则体系、语音交互、数据留存、联网工具、实施顺序和验收门禁。

Codex 必须把本文件视为本轮开发的最高层业务与架构约束。

本文件不是要求一次性完成所有未来能力，而是：

1. 明确最终架构；
2. 明确当前阶段必须完成的主线；
3. 明确不得破坏的既有功能；
4. 明确 C5、S3、ESP-server 的责任边界；
5. 明确各阶段的输入、输出、状态机、错误处理和验收条件；
6. 避免 Codex自行扩张范围、重复建设或把复杂逻辑放错设备。

---

# 1. 项目重新定位

## 1.1 核心产品定位

项目定位为：

> **以家庭环境感知和本地自治为核心的无交互智能家居系统。**

系统应尽量根据人体存在、稳定人数、环境数据、时间、场景、房间习惯和 Server 下发规则，在不要求用户频繁确认的情况下完成自动判断和虚拟设备控制。

用户明确命令拥有最高业务优先级。

当用户命令与普通系统判断相反时：

1. 执行用户命令；
2. 暂停或抑制冲突自动化；
3. 在房间有人且允许播报时，进行简短语音提醒；
4. 上传命令、冲突、抑制原因和最终状态；
5. 将明确纠正或反向操作作为反馈数据。

只有进入明确的极端危险状态时，系统才允许拒绝、撤销或覆盖用户命令。

## 1.2 产品核心体验

第一阶段需要形成以下闭环：

```text
环境与人体感知
→ S3 本地稳定状态判断
→ S3 本地规则决策
→ 虚拟灯/空调/风扇状态变化
→ 状态与原因上传 ESP-server
→ 现有 Web 展示
→ 房间有人且符合策略时由 C5 播报
→ 用户语音或 Web 纠正
→ Server 形成 Feedback / Habit / Rule 候选
→ 新规则试运行后重新同步到 S3
```

当前没有真实家电，因此第一阶段必须实现“虚拟设备执行器”，但所有命令、状态、规则、ACK、反馈和展示接口必须按照未来真实设备接入的方式设计。

---

# 2. 三端最终职责边界

## 2.1 C5：数据收取与用户交互端

C51 和 C52 的定位：

- 采集本房间 BME690 环境数据；
- 通过 BLE 接收并基础解析本房间 LD2450 雷达数据；
- 上报轻量雷达目标数据；
- 执行唤醒词检测；
- 录音、上传音频；
- 播放唤醒提示、普通回复、自动化提示和紧急播报；
- 接收 S3 下发的本地命令；
- 返回命令 ACK、语音播放 ACK 和健康状态；
- 在 S3 不可达时，仅保留最小安全和恢复能力；
- 不负责复杂规则判断；
- 不负责跨房间决策；
- 不负责长期习惯学习；
- 不直接访问互联网和 ESP-server。

## 2.2 S3：家庭总网关、本地运行面和主要决策中心

S3 是本项目的重点。

S3 负责：

- SoftAP + STA 网络；
- C51/C52 注册、心跳和健康管理；
- C51、C52、S3_LOCAL 三来源数据隔离；
- 雷达、BME、语音、设备状态聚合；
- 稳定有人/无人判断；
- 稳定单人/多人判断；
- 房间状态机；
- 本地规则引擎；
- 用户命令与自动规则冲突仲裁；
- 虚拟设备执行器；
- 紧急规则和安全策略；
- 本地短期历史；
- 断网继续运行；
- 联网补传；
- 规则版本同步、部分应用、回滚；
- 全局语音会话锁；
- 条件语音播报；
- 紧急语音抢占；
- 将所有决策和结果上传 Server。

## 2.3 ESP-server：数据、AI、联网工具和规则控制面

ESP-server 负责：

- 长期数据存储；
- 设备、房间、规则、记忆、习惯、反馈和决策管理；
- 上游 ASR、LLM、TTS；
- Tool Registry；
- 天气、新闻和后续联网搜索；
- 分层提示词；
- 复杂任务结构化计划；
- 规则配置、生成、试运行评估和自动发布；
- 规则版本控制；
- Web 增量配置与展示；
- 高频原始数据短期保存；
- 聚合数据长期保存；
- 数据清理作业；
- 将规则、房间配置、天气上下文等同步给 S3。

ESP-server 不应直接越过 S3 操作 C5 或虚拟设备。

---

# 3. 当前物理部署与房间模型

## 3.1 初始部署

```text
S3 + S3_LOCAL → 客厅
C51 → 卧室一
C52 → 卧室二
```

客厅没有 C5 语音终端。

卧室一和卧室二均有：

- C5；
- BME690；
- LD2450；
- 麦克风；
- 扬声器；
- 唤醒词和语音交互能力。

## 3.2 可配置要求

房间归属不得写死在固件。

ESP-server Web 必须能够配置：

- 设备唯一 ID；
- 房间 ID；
- 房间名称；
- S3 网关物理位置；
- 感知来源归属；
- 语音终端归属；
- 各房间规则参数；
- 各房间静音时间；
- 各房间有人/无人确认时间；
- 各房间多人确认时间。

修改房间归属后：

1. Server 生成新配置版本；
2. S3 拉取并原子切换；
3. 新数据使用新归属；
4. 历史数据保持采集时原归属；
5. 受影响规则进入迁移、暂停或重新绑定状态；
6. 不要求重新编译固件。

---

# 4. 全局优先级与安全边界

## 4.1 决策优先级

从高到低：

```text
1. 固件内置极端安全保护
2. 用户当前明确命令
3. 用户有效临时覆盖
4. 用户长期明确配置
5. 已启用基础自动化
6. 高置信度房间习惯规则
7. 多人共享保守规则
8. 系统默认规则
```

## 4.2 用户命令处理

用户命令按语义形成不同有效期：

- “打开灯”：短期保护；
- “先别关”：临时覆盖；
- “今晚别关”：到次日指定时间；
- “未来两小时保持打开”：明确 2 小时；
- “以后不要自动关卧室灯”：长期配置；
- “恢复自动控制”：清除相关覆盖。

每条覆盖必须包含：

```json
{
  "override_id": "string",
  "scope": {
    "room_id": "string",
    "device_id": "string or null"
  },
  "action": "keep_on | keep_off | pause_automation | mute | other",
  "source": "explicit_user_command",
  "created_at": "timestamp",
  "expires_at": "timestamp or null",
  "until_condition": "optional",
  "priority": 900,
  "allow_safety_override": true
}
```

## 4.3 极端危险

极端危险可以覆盖用户命令。

要求：

- 有明确阈值；
- 有持续时间；
- 尽量有多条件确认；
- 记录证据；
- 记录执行动作；
- 风险解除后不得自动恢复危险前状态；
- 需用户重新命令或规则明确允许；
- 单个失真传感器不得轻易触发强制动作。

---

# 5. C5 分端开发计划

## 5.1 C5 核心状态

每个 C5 维护：

```text
BOOTING
REGISTERING
ONLINE_IDLE
VOICE_WAKE_PENDING
VOICE_LOCK_GRANTED
VOICE_RECORDING
VOICE_WAITING
VOICE_PLAYING
VOICE_REJECTED
EMERGENCY_PLAYING
RECOVERING
OFFLINE
```

## 5.2 雷达链路

C5 保持现有 BLE-only 雷达接入方向。

要求：

- 每个 C5 固定绑定自己的雷达 MAC；
- 只连接白名单设备；
- 解析 30-byte 帧；
- 保留与 S3_LOCAL 一致的基础坐标和目标字段语义；
- C5 只做基础解析与轻量预处理；
- 不做区域判断；
- 不做跨帧复杂身份推断；
- 不做房间级自动化决策；
- 上报必须带 source、device_id、sequence、timestamp、validity；
- 断连时进入恢复状态，不阻塞语音和 BME。

## 5.3 BME690

要求：

- 保留现有采集链路；
- 不因规则、语音或雷达修改采样流程；
- 继续上报原始采样时间；
- 断网时由 S3 负责主要持久化；
- C5 仅保留必要的小容量临时缓存；
- 不在 C5 上运行复杂环境告警规则。

## 5.4 语音唤醒仲裁

采用“先唤醒者独占，后唤醒者拒绝”。

流程：

```text
C5 检测到唤醒词
→ 请求 S3 全局语音锁
→ 首个成功者获得会话
→ 后到者收到 VOICE_SESSION_BUSY
→ 后到者不录音、不上传、不播放正常回复
```

C5 必须验证：

- `voice_session_id`
- `owner_device_id`
- `generation`
- `lease_expires_at`

收到过期、非本机或错误 generation 的指令必须拒绝。

## 5.5 播报策略

普通自动化播报：

- 仅房间确认有人；
- 仅对应房间 C5；
- 不在静音状态；
- 不与用户会话冲突；
- 相同动作限频；
- 房间无人时不播报；
- 无人也必须上传执行状态。

紧急播报：

- 可抢占普通语音；
- 可在人体状态未知时兜底播报；
- 使用独立 `emergency_event_id`；
- 播放后返回独立 ACK；
- 恢复播报同样需要 ACK。

## 5.6 C5 不允许承担的能力

Codex 不得在 C5 加入：

- 复杂 LLM；
- Agent Loop；
- 长期习惯；
- 多房间状态；
- 新闻和天气工具；
- 规则自动学习；
- 通用工作流；
- 大量 Flash 历史；
- 复杂虚拟设备状态机。

---

# 6. S3 分端开发计划

## 6.1 S3 目标架构

建议新增或明确以下模块：

```text
ESPS3/components/
├── automation_rule_engine/
├── rule_store/
├── rule_sync_client/
├── room_state_engine/
├── occupancy_stabilizer/
├── target_count_stabilizer/
├── virtual_device_executor/
├── user_override_manager/
├── local_voice_command_router/
├── voice_session_manager/
├── emergency_voice_router/
├── decision_audit_log/
├── short_history_store/
└── config_snapshot_manager/
```

必须优先复用现有：

- gateway_orchestrator；
- local_http_server；
- network_worker；
- server_client；
- command_router；
- sensor_aggregator；
- smart_home_gateway；
- environment_alarm_engine；
- radar_domain 相关模块；
- 现有缓存、事件总线和调度器。

不得重复创建第二套网络栈、命令队列或告警引擎。

## 6.2 房间状态模型

每个房间维护：

```json
{
  "room_id": "bedroom_01",
  "presence_state": "occupied | vacant | unknown",
  "stable_target_count": 0,
  "occupancy_mode": "single | multiple | unknown",
  "environment_fresh": true,
  "radar_fresh": true,
  "quiet_state": "normal | scheduled | sleep_confirmed | temporary_awake",
  "scene_state": "none | wake_candidate | wake_confirmed | leave_candidate | other",
  "automation_profile": "room_personalized | shared_conservative | basic_only",
  "last_state_change_at": "timestamp"
}
```

## 6.3 有人/无人稳定化

初始建议：

- 连续有人 1.5 秒 → occupied；
- 连续无人 60 秒 → vacant；
- 数据过期 → unknown；
- unknown 不得作为无人；
- unknown 不执行自动关灯；
- 阈值按房间可配置；
- 后续根据日志调优。

## 6.4 人数稳定化

初始建议：

- 连续 `target_count >= 2` 3 秒 → multiple；
- 连续 `target_count == 1` 10 秒 → single；
- 数据过期或算法不可信 → unknown；
- multiple 与 unknown 使用共享保守规则；
- 重新启用单人习惯前需额外稳定期。

## 6.5 规则引擎

S3 负责大部分实时决策。

规则必须是受限 DSL 或固定 Schema，不允许执行任意代码。

建议规则结构：

```json
{
  "rule_id": "presence_light_bedroom_01",
  "version": 12,
  "rule_type": "basic_automation",
  "source": "manual | habit_learning | system",
  "room_id": "bedroom_01",
  "enabled": true,
  "priority": 500,
  "conditions": [
    {"field": "presence_state", "operator": "eq", "value": "occupied"},
    {"field": "time_window", "operator": "in", "value": "night"}
  ],
  "actions": [
    {"device_id": "bedroom_01_light", "action": "turn_on"}
  ],
  "cooldown_seconds": 120,
  "minimum_active_seconds": 60,
  "offline_policy": "continue",
  "expires_at": null,
  "probation": {
    "enabled": false,
    "until": null
  }
}
```

第一阶段仅支持：

- 条件 AND；
- 少量 OR 分组；
- 比较、范围、状态、时间窗；
- 简单持续时间；
- 顺序步骤；
- 每一步内部并行动作；
- 前一步成功后继续；
- 不支持任意循环、递归、脚本或复杂依赖图。

## 6.6 规则包同步

运行策略：

1. S3 开机加载最后有效规则；
2. 联网后立即检查 Server；
3. Server 新版本通知 S3；
4. S3 主动拉取完整规则包；
5. 每 15 分钟兜底检查；
6. 相同版本不重复应用；
7. 更新失败不影响旧规则运行；
8. 保留稳定版本和上一回滚版本。

当前阶段不做签名。

必须校验：

- schema_version；
- 版本；
- 长度；
- checksum；
- 参数范围；
- firmware 兼容性；
- 重复 rule_id；
- 资源预算；
- 非法优先级；
- 不支持的动作；
- 基础结构完整性。

## 6.7 部分规则应用

用户选择：

> 新规则包中单条规则失败时，跳过失败规则，启用其余规则。

要求：

- 规则集状态标记 `ACTIVE_PARTIAL`；
- 返回每条 accepted/rejected；
- 同 rule_id 新版本失败时，继续保留上一有效版本；
- 基础包结构损坏时整体拒绝；
- Server Web 显示实际 S3 生效内容，而不是只显示发布版本号。

## 6.8 固定资源上限

必须在编译期或 Kconfig 设置上限，例如：

```text
MAX_RULES
MAX_CONDITIONS_PER_RULE
MAX_ACTIONS_PER_RULE
MAX_RULE_STEPS
MAX_ACTIVE_TIMERS
MAX_PENDING_DECISIONS
MAX_USER_OVERRIDES
MAX_LOCAL_HISTORY_BYTES
MAX_RULE_PACKAGE_BYTES
```

资源不足时：

- 拒绝超限规则；
- 上传明确错误；
- 不抢占语音、雷达、BME、网络和告警资源；
- 不动态淘汰正在运行的高优先级链路；
- 不依赖分配失败后再补救。

## 6.9 虚拟设备执行器

第一阶段设备：

```text
light: ON / OFF
air_conditioner: ON / OFF
fan: ON / OFF
```

不支持：

- 灯亮度；
- 色温；
- 空调温度；
- 制冷制热模式；
- 风扇档位；
- 摇头。

状态结构：

```json
{
  "device_id": "bedroom_01_light",
  "device_type": "light",
  "room_id": "bedroom_01",
  "power": "on",
  "execution_mode": "virtual",
  "last_action": "turn_on",
  "action_source": "presence_automation",
  "decision_reason": "occupied_and_night",
  "verified": true,
  "updated_at": "timestamp"
}
```

`verified=true` 仅表示虚拟状态机写入成功，Web 必须显示“模拟执行”。

## 6.10 灯规则

开启：

- 必须确认有人；
- 结合时间；
- 可结合 Server 下发的有效天气明暗上下文；
- 结合房间习惯；
- 睡眠场景不自动开主灯；
- 用户刚关灯时保护期内不自动开；
- 天气不可用时跳过依赖天气的规则；
- 后续接入照度传感器后以真实照度为主。

关闭：

- 卧室无人延时 3～10 分钟；
- 客厅无人延时 1～3 分钟；
- 多人必须全部稳定离开；
- unknown 不关灯；
- 重新有人立即取消计时；
- 用户覆盖优先；
- 关闭时通常无人，因此不播报，只上传状态。

## 6.11 空调与风扇规则

分级协同：

- 轻度偏热：风扇；
- 明显偏热：空调；
- 极热或高湿：空调 + 风扇；
- 恢复过程中按规则依次关闭；
- 设置最小运行时间；
- 设置最小停机时间；
- 设置切换冷却期；
- 房间无人默认不启动；
- 高置信度起床/回家场景可短时预处理；
- 环境数据过期不新增开启动作。

预处理：

- 风扇提前 3～5 分钟；
- 空调提前 10～20 分钟；
- 按场景配置；
- 超时未确认有人自动取消；
- 用户进入后转为正常运行；
- 房间无人时不播报预处理开始。

## 6.12 舒适区学习

S3 不训练舒适区。

Server 负责学习后下发规则。

初期仅有：

- 安全阈值；
- 宽松默认范围；
- 已确认或高置信度规则。

用户无反馈不作为学习证据。

## 6.13 用户覆盖管理

S3 必须支持：

- 设备级；
- 房间级；
- 自动化级；
- 静音级；
- 临时和长期；
- 明确 expires_at；
- until_condition；
- 安全覆盖例外。

规则触发但被覆盖时，仍上传：

```text
suppressed_action
suppressed_by_override_id
original_rule_id
reason
```

## 6.14 本地离线语音命令

S3 只处理有限命令：

- 停止；
- 取消；
- 静音；
- 恢复播报；
- 暂停自动控制；
- 恢复自动控制；
- 打开灯；
- 关闭灯；
- 保持打开；
- 保持关闭；
- 撤销最近动作；
- 不要这样做。

不处理：

- 开放式聊天；
- 天气问答；
- 新闻问答；
- 原因解释；
- 复杂设备计划；
- 本地 LLM。

词表可由 Server 扩展，但固件内置的停止、取消、静音等不能远程删除。

## 6.15 语音会话管理

全局只允许一个普通语音会话。

状态：

```text
IDLE
LOCKED
RECORDING
WAITING_SERVER
PLAYING
ENDING
PREEMPTED
```

必须有租约和 generation。

后到 C5 返回 `VOICE_SESSION_BUSY`。

会话结束、C5 掉线、超时、异常时释放。

S3 重启后默认 IDLE。

## 6.16 紧急播报路由

客厅无 C5。

客厅紧急事件：

- 只有卧室一有人 → C51；
- 只有卧室二有人 → C52；
- 两边都有人 → 两边；
- 两边都无人 → 不播报，只上传；
- 两边都 unknown → 两边；
- 一边 unknown、一边有人 → 两边；
- 一边 unknown、一边无人 → unknown 侧。

仅 emergency 使用此兜底。

## 6.17 紧急提醒升级

状态：

```text
DETECTED
ACTIVE_UNACKNOWLEDGED
ACKNOWLEDGED
ESCALATED
RECOVERING
RESOLVED
```

策略：

- 首次立即；
- 未确认持续时 1、3、5 分钟等重复；
- 用户确认后降低频率；
- 不能关闭安全保护；
- 风险恶化重新升级；
- 恢复后播报恢复信息；
- 同一事件共享 event_id。

## 6.18 夜间静音

状态：

```text
NORMAL
QUIET_SCHEDULED
SLEEP_CONFIRMED
TEMPORARY_AWAKE
```

规则：

- Web 配置基础静音时段；
- 睡眠场景可提前静音；
- 起床高置信度可提前退出；
- 主动唤醒正常响应；
- 紧急播报始终允许；
- 主动唤醒后临时退出静音 5～15 分钟；
- 默认建议 10 分钟；
- 期间允许连续交互；
- 不开放无关主动简报；
- 无后续活动恢复静音。

## 6.19 短期历史与离线存储

RAM/PSRAM 仅保存当前状态和小缓存。

Flash 独立分区保存关键事件：

- presence 变化；
- 稳定人数变化；
- 规则触发；
- suppressed action；
- 虚拟设备变化；
- 用户覆盖；
- 播报结果；
- 规则更新；
- 紧急告警；
- 上传补传状态。

不得保存：

- 原始雷达高频帧；
- 高频目标坐标；
- PCM；
- 每秒调试日志。

保留策略：

- 24 小时强保证；
- 72 小时尽力保留；
- 未上传记录最高优先；
- 紧急事件优先；
- 先删除已上传、低价值、最旧普通事件；
- 80% 告警；
- 100% 受控淘汰；
- 不静默丢数据。

---

# 7. ESP-server 分端开发计划

## 7.1 总体原则

保持现有 Node.js、Express、SQLite、SSE、语音和 Web 基础。

不得为了“先进”重写为 FastAPI、PostgreSQL、MQTT 或全新前端。

所有新模块通过现有 service、route、db helper 和 command queue 增量集成。

## 7.2 建议模块

```text
ESP-server/src/
├── rules/
│   ├── ruleService.js
│   ├── ruleValidator.js
│   ├── rulePublisher.js
│   ├── ruleEvaluationService.js
│   └── ruleResourceEstimator.js
├── agent/
│   ├── agentOrchestrator.js
│   ├── intentService.js
│   ├── contextAdapter.js
│   ├── decisionService.js
│   └── speechPlanService.js
├── tools/
│   ├── registry.js
│   ├── weatherTool.js
│   ├── newsTool.js
│   ├── toolPolicy.js
│   └── providers/
├── feedback/
│   ├── feedbackService.js
│   ├── habitService.js
│   └── memoryCandidateService.js
├── scheduler/
│   ├── ruleJobs.js
│   ├── retentionJobs.js
│   └── aggregationJobs.js
└── notifications/
    ├── notificationService.js
    ├── webProvider.js
    ├── voiceProvider.js
    └── appPushProvider.placeholder.js
```

## 7.3 Web 改动原则

保持现有页面结构。

只增量加入：

- 规则配置入口；
- 规则版本；
- S3 同步状态；
- partial active 状态；
- 虚拟设备状态；
- 决策和执行结果；
- 用户反馈；
- 记忆和习惯管理；
- 告警确认；
- 天气和新闻工具配置；
- 本地存储和补传诊断。

不重做首页。

## 7.4 规则配置

Web 需支持：

- 房间；
- 条件；
- 持续时间；
- 动作；
- 播报策略；
- 冷却时间；
- 最小运行时间；
- 用户覆盖策略；
- 离线策略；
- 规则有效期；
- 优先级；
- 试运行；
- 禁用；
- 回滚。

规则生命周期：

```text
DRAFT
VALIDATING
READY
PUBLISHED
ACTIVE_ON_S3
ACTIVE_PARTIAL
PROBATION
ACTIVE
SUSPENDED
ROLLED_BACK
SUPERSEDED
DISABLED
```

## 7.5 规则自动生成

采用：

- 人工基础规则；
- Server 自动学习候选；
- 达到置信度后自动发布；
- 不逐条人工审批；
- 必须通过代码门禁；
- 新规则进入短期试运行；
- 异常自动暂停或回滚。

门禁：

- 最低样本数；
- 置信度；
- 数据新鲜度；
- Schema；
- 冲突；
- 资源预算；
- 历史回放；
- 安全边界；
- 影响范围；
- 触发频率预测。

## 7.6 试运行

建议：

- 3～7 天；
- 至少 5 次触发；
- 反向反馈低于阈值；
- 失败率低于阈值；
- 无高频开关；
- 无重复播报；
- 无持续冲突。

异常：

- 规则多次被用户覆盖；
- 用户短时反向操作；
- 触发频率异常；
- S3 执行失败；
- 传感器不可靠；
- 播报过度。

## 7.7 提示词分层

采用同一上游模型，但不同任务使用不同系统提示词和严格输出。

至少拆分：

1. 普通对话；
2. 意图识别；
3. Tool Calling；
4. 复杂命令计划；
5. 规则生成；
6. 规则安全审查；
7. 习惯归纳；
8. 记忆候选；
9. 天气回答；
10. 新闻回答；
11. 最终语音回复；
12. Web 解释摘要。

机器任务必须输出严格 JSON。

模型输出不得直接操作硬件。

## 7.8 命令与语音分离

复杂命令必须输出：

```json
{
  "response_type": "complex_command",
  "decision_id": "string",
  "steps": [],
  "speech_policy": {}
}
```

控制 JSON：

- 不转 TTS；
- 经 Schema 校验；
- 下发 S3；
- 等待实际结果。

语音：

- 只说用户可理解的简短提示；
- 开始时可播报“正在处理”；
- 最终根据 S3 结果生成；
- 部分失败必须说明；
- 不得提前声称成功。

## 7.9 复杂任务执行简化

第一阶段只支持：

- 顺序步骤；
- 步骤内并行；
- 简单前置条件；
- 前一步成功后继续；
- 不支持通用 DAG；
- 不支持循环；
- 不支持复杂补偿。

## 7.10 超时

建议：

- S3 本地动作：1～3 秒；
- C5 ACK：3～5 秒；
- Server 内部工具：3～5 秒；
- 天气/新闻：8～15 秒；
- LLM：15～30 秒；
- TTS：10～20 秒；
- 整体：30～60 秒。

超过 5～10 秒可播报一次中间进度。

已成功动作不因独立动作失败回滚。

## 7.11 Tool Registry

第一阶段：

- `get_environment`
- `get_device_state`
- `control_virtual_device`
- `query_history`
- `query_memory`
- `get_weather`
- `get_news`
- `save_feedback`

每个 Tool 必须定义：

- JSON Schema；
- 超时；
- 权限等级；
- 是否只读；
- 是否需要确认；
- 缓存策略；
- 错误码；
- 审计字段。

## 7.12 天气工具

家庭位置在 Web 固定配置：

- 城市；
- 经纬度；
- 时区。

S3 不直接访问天气服务。

天气可参与自动化，但：

- 失败时直接报错；
- 不使用旧缓存冒充实时；
- 不允许模型猜测；
- 依赖天气的规则本次跳过；
- 本地自动化继续运行。

## 7.13 新闻工具

新闻只用于：

- 用户询问；
- 场景简报；
- 主动简报。

新闻不得参与设备控制和安全规则。

失败时直接说明失败。

## 7.14 场景简报

采用场景触发，不固定时间强制播报。

支持：

- 起床；
- 准备出门；
- 用户主动请求；
- 严重天气变化。

触发需满足：

- 房间有人；
- C5 在线；
- 语音空闲；
- 非静音；
- 未重复；
- 数据有效。

置信度：

- ≥ 0.85 自动播报；
- 0.60～0.85 继续观察；
- < 0.60 放弃；
- 阈值可配置。

客厅无 C5，普通简报只上传 Web，不借用卧室播报。

## 7.15 记忆、习惯、规则区分

Memory：

- 稳定事实和偏好；
- 普通对话可生成候选；
- 候选不直接参与高影响控制；
- 可确认、修改、删除。

Habit：

- 多次明确行为形成的统计规律；
- 无反馈不参与学习；
- 明确命令、纠正、撤销、反向操作才是样本。

Rule：

- S3 可执行的条件和动作；
- 来自人工或已通过门禁的习惯候选；
- 版本化；
- 可试运行和回滚。

三者必须分表或分模型。

## 7.16 用户反馈

来源：

- 明确语音纠正；
- Web 标记；
- 立即反向命令；
- 参数修改；
- 撤销；
- “不要这样做”。

反馈类型：

```text
accepted
rejected
modified
cancelled
reverted
manual_override
```

用户无反馈：

- 不算接受；
- 不提高习惯置信度；
- 只做运行统计。

系统只在新规则试运行或长期不确定时偶尔询问反馈，并严格限频。

## 7.17 记忆 Web 管理

Web 支持：

- 查看候选；
- 确认；
- 修改；
- 删除；
- 禁止参与自动化；
- 查看来源；
- 查看置信度；
- 查看影响规则；
- 查看房间范围。

## 7.18 数据留存

平衡策略：

- 雷达坐标：7 天；
- 原始环境采样：90 天；
- 小时/每日聚合：长期；
- presence 事件：长期；
- 决策、告警、反馈：长期；
- 规则版本和回滚：长期；
- 原始音频：不保存；
- 转写、意图、命令和结果：长期。

清理作业：

- 先聚合；
- 再删除；
- 失败不影响接收；
- 记录清理日志；
- 容量告警；
- 配置化保留时间。

## 7.19 原始音频

强制规则：

- ASR 和当前请求处理完成后删除；
- 失败和超时也必须删除；
- 不默认调试留存；
- 不保存唤醒前环境音；
- 不长期保存 TTS；
- Web 不提供历史录音播放。

---

# 8. 跨端协议

## 8.1 通用字段

所有跨端消息建议包含：

```json
{
  "request_id": "string",
  "trace_id": "string",
  "gateway_id": "string",
  "source_device_id": "string",
  "room_id": "string",
  "timestamp": "timestamp",
  "sequence_no": 0,
  "schema_version": 1
}
```

## 8.2 C5 → S3

至少包括：

- register；
- heartbeat；
- status；
- BME sensor；
- radar result；
- wake request；
- audio chunk；
- playback ACK；
- command ACK；
- health。

## 8.3 S3 → C5

至少包括：

- voice lock granted/rejected；
- start/stop recording；
- play prompt；
- play TTS；
- stop playback；
- emergency playback；
- local command；
- config update；
- sync status。

## 8.4 S3 → Server

至少包括：

- sensor ingest；
- radar state；
- room state；
- virtual device state；
- decision event；
- suppressed action；
- rule sync result；
- voice session event；
- emergency event；
- playback ACK；
- offline buffer state；
- gateway state。

## 8.5 Server → S3

至少包括：

- rule update notification；
- rule package fetch；
- room config；
- weather context；
- complex command plan；
- feedback correction；
- memory-derived configuration；
- notification ACK；
- command cancel。

---

# 9. 数据库新增或扩展建议

## 9.1 规则相关

- `automation_rules`
- `rule_versions`
- `rule_deployments`
- `rule_deployment_items`
- `rule_probation_runs`
- `rule_evaluations`

## 9.2 决策相关

- `agent_decisions`
- `decision_steps`
- `decision_actions`
- `action_feedback`

## 9.3 虚拟设备

- `virtual_devices`
- `virtual_device_state_history`

## 9.4 房间状态

- `room_state_history`
- `occupancy_events`
- `target_count_events`

## 9.5 记忆与习惯

- `memory_candidates`
- `confirmed_memories`
- `room_habits`
- `habit_evidence`

## 9.6 通知

- `notification_deliveries`

当前 App 不开发，但保留 `app_push` channel 状态。

---

# 10. 现有 Web 增量功能

不重构首页。

新增入口：

1. 房间与设备配置；
2. 规则配置；
3. 规则发布状态；
4. S3 实际生效规则；
5. partial active 错误详情；
6. 虚拟设备状态；
7. 最近自动决策；
8. 被抑制动作；
9. 用户反馈；
10. 记忆和习惯管理；
11. 紧急告警确认；
12. 天气位置和工具配置；
13. 本地存储状态；
14. 补传状态；
15. 数据保留配置。

---

# 11. 登录与权限

当前阶段不新增登录系统。

要求：

- 保持现有访问方式；
- 保持现有 Gateway Token；
- 保持现有设备绑定；
- 保持现有管理员鉴权能力；
- 不新增账号、JWT、RBAC；
- 不删除未来扩展字段；
- 新接口沿用现有鉴权路径。

---

# 12. 实施顺序

## Phase 0：只读审计与基线冻结

Codex 必须先：

- 审计实际仓库；
- 列出现有文件；
- 确认当前已实现模块；
- 确认 C5/S3/Server 真实接口；
- 确认当前 partition table；
- 确认 S3 Flash；
- 确认 RAM/PSRAM；
- 确认现有 Web；
- 确认已有测试；
- 形成影响矩阵。

禁止在未完成审计前重写核心链路。

## Phase 1：跨端 Schema 与规则协议

完成：

- room config；
- rule schema；
- rule package；
- partial activation response；
- virtual device schema；
- decision event；
- suppressed action；
- voice session；
- emergency voice；
- user override。

## Phase 2：S3 状态稳定化

完成：

- presence stabilizer；
- target count stabilizer；
- room state；
- unknown 降级；
- 多人共享策略；
- 时间参数可配置；
- host tests。

## Phase 3：S3 规则引擎

完成：

- 固定上限；
- 规则加载；
- 单条拒绝；
- 旧规则保留；
- 优先级；
- cooldown；
- duration；
- user override；
- safety rules；
- host tests。

## Phase 4：虚拟设备

完成：

- light；
- AC；
- fan；
- 状态变化；
- 决策记录；
- Web 上报；
- 模拟 verified；
- 用户命令；
- 自动化；
- 回归测试。

## Phase 5：语音条件播报

完成：

- 全局锁；
- 后唤醒拒绝；
- 自动动作播报；
- 无人不播报；
- 静音；
- temporary awake；
- 紧急抢占；
- 客厅紧急路由；
- ACK；
- generation 防串话。

## Phase 6：规则同步与本地历史

完成：

- 启动加载；
- Server 通知；
- 15 分钟检查；
- partial activation；
- rollback；
- 短期历史；
- 24h 保证；
- 72h 尽力；
- 断网补传。

## Phase 7：ESP-server Web 和规则控制面

完成：

- 配置；
- 规则编辑；
- 发布；
- 实际部署状态；
- partial 错误；
- 虚拟状态；
- 决策解释；
- 反馈；
- 记忆管理。

## Phase 8：分层提示词和复杂命令

完成：

- Intent；
- Tool；
- Plan；
- Action/Speech 分离；
- 双阶段语音；
- 超时；
- partial completion；
- 简化步骤模型。

## Phase 9：天气与新闻

完成：

- 固定家庭位置；
- weather tool；
- news tool；
- 失败直接报错；
- 天气失败跳过自动化；
- 场景简报；
- 数据新鲜度。

## Phase 10：Feedback、Memory、Habit 与自动发布

完成：

- 明确反馈；
- 无反馈不学习；
- 候选记忆；
- 房间习惯；
- 规则候选；
- 自动门禁；
- probation；
- 自动回滚；
- Web 管理。

---

# 13. 关键测试

## 13.1 C5

- C51/C52 只能一个获得语音锁；
- 后到者不录音；
- owner 掉线锁释放；
- emergency 可抢占；
- playback ACK 正确；
- 雷达断线不影响语音；
- BME 不受规则改造影响。

## 13.2 S3

- 三来源严格隔离；
- presence 防抖；
- 多人防抖；
- unknown 不自动关灯；
- 用户命令覆盖自动化；
- 安全规则覆盖用户；
- 虚拟设备状态一致；
- partial rule package；
- 资源超限拒绝；
- 断网继续执行；
- 重启恢复；
- 24 小时本地历史；
- 语音 generation 防串话。

## 13.3 Server

- 规则 Schema；
- 资源预算；
- 自动发布门禁；
- probation；
- rollback；
- Action/Speech 分离；
- Tool 超时；
- 天气失败；
- 新闻失败；
- 原始音频删除；
- 数据清理；
- 反馈影响规则；
- 无反馈不影响习惯。

## 13.4 联调

```text
卧室一确认有人
→ 夜间规则触发
→ S3 虚拟灯 ON
→ Server 收到状态
→ Web 显示原因
→ C51 播报
→ 用户说“不要这样做”
→ S3 撤销
→ Server 记录反馈
→ 规则置信度下降
```

```text
C51 正在普通对话
→ 客厅极端告警
→ S3 抢占
→ 根据卧室 presence 路由
→ 播报 ACK
→ 告警持续重复
→ 用户确认后降频
→ 风险恶化再次升级
```

```text
Server 断网
→ S3 使用最后有效规则
→ 本地自动化继续
→ 历史落盘
→ 联网后补传
→ Server 不重复写入
```

---

# 14. 验收门禁

每个阶段必须满足：

- 构建无 warning；
- `git diff --check` 通过；
- 不修改无关模块；
- 不破坏现有 API；
- 不新增重复链路；
- host tests 通过；
- 资源报告完整；
- S3 内部 RAM、DMA、PSRAM 变化有对比；
- 语音链路回归；
- 雷达链路回归；
- BME 链路回归；
- SoftAP 和两个 C5 注册回归；
- Server API 回归；
- Web 现有功能回归。

---

# 15. 资源与性能要求

## 15.1 S3

规则引擎必须：

- 固定容量；
- 避免动态碎片；
- 规则包解析分阶段；
- 大 JSON 优先使用 PSRAM；
- 回调中不做阻塞 IO；
- Flash 写入异步或批处理；
- 不占用语音关键 DMA；
- 不降低雷达接收实时性。

## 15.2 C5

- BLE callback 只做轻量写入；
- 语音优先级不得被雷达阻塞；
- 不新增大栈；
- 不新增长期缓存；
- 扬声器和麦克风资源必须由现有资源管理器控制。

## 15.3 Server

- 外部工具统一超时；
- Tool 有并发限制；
- 数据清理独立调度；
- Web SSE 不被长任务阻塞；
- SQLite 写入短事务；
- 不保存原始音频。

---

# 16. Codex 强制执行规则

Codex 必须：

1. 先审计，后设计，后修改；
2. 每次改动给出文件清单；
3. 每次改动给出不影响现有功能的证明；
4. 所有新能力复用现有基础；
5. 所有跨端接口一次设计、双方同步；
6. 先提供协议和测试，再接入运行链路；
7. 对资源变化给出数字；
8. 对每个失败路径给出错误码；
9. 对所有异步任务加入超时和 generation；
10. 对所有自动动作保留可解释记录；
11. 对所有用户覆盖保留有效期；
12. 对所有规则更新保留回滚版本；
13. 对所有外部工具禁止猜测结果；
14. 对所有虚拟执行明确标识 virtual；
15. 不将命令 JSON 转成 TTS；
16. 最终语音以 S3 实际结果为准；
17. 无人时不播报普通动作；
18. 无论有人无人都上传执行状态。

Codex 禁止：

- 重写现有网络栈；
- 重写现有命令队列；
- 新建第二套环境告警引擎；
- 把复杂 AI 放到 C5；
- 让 S3 直接查询新闻或天气；
- 让 Server 绕过 S3 操作 C5；
- 无限制加载规则；
- 使用用户无反馈作为接受证据；
- 长期保存原始音频；
- 因一个新模块影响语音、雷达或 BME；
- 在未验证 Flash 分区前承诺 72 小时历史；
- 在未获得 S3 ACK 前播报“已完成”；
- 将部分规则发布误标记为完整成功；
- 擅自开发登录系统或 App；
- 重做现有 Web 首页；
- 引入 MQTT、PostgreSQL、FastAPI 等非当前主线重构。

---

# 17. 当前阶段完成定义

当前主线完成后，系统应达到：

```text
C51/C52/S3_LOCAL 三房间来源稳定
→ S3 本地判断有人/无人和稳定人数
→ S3 加载 Server 规则并离线运行
→ S3 控制虚拟灯、空调、风扇
→ 用户命令优先并形成覆盖
→ 自动动作状态始终上传
→ 有人时按策略播报
→ 紧急事件可抢占
→ Web 可配置规则和查看结果
→ 用户可纠正
→ Server 可形成候选记忆、习惯和规则
→ 新规则自动进入试运行并可回滚
```

---

# 18. 后续但非当前阶段

暂不实施：

- 真实灯具；
- 真实空调；
- 真实风扇；
- App；
- App Push；
- 多用户身份；
- 声纹；
- JWT/RBAC；
- Matter；
- Home Assistant；
- MQTT；
- OTA；
- 本地大模型；
- PostgreSQL；
- 多网关；
- 多家庭。

这些能力必须在当前 S3 本地运行面稳定后再评估。

---

# 19. 最终架构结论

```text
C5
= 感知 + 唤醒 + 录音 + 播放 + 基础雷达解析

S3
= 家庭总网关 + 房间状态 + 本地规则 + 用户覆盖
  + 虚拟执行 + 紧急保护 + 语音仲裁 + 离线自治

ESP-server
= 长期数据 + AI + 提示词 + 联网工具 + 规则控制面
  + Web 配置 + Feedback + Memory + Habit + 自动发布
```

项目成功标准不是“AI 回答更多问题”，而是：

> 在不依赖持续交互的情况下，S3 能稳定、可解释、可回滚地完成本地家庭自动化，同时保留用户最高控制权和极端安全边界。

---

# 20. 版本记录

| 版本 | 日期 | 内容 |
|---|---|---|
| V1.2 | 2026-07-19 | 原 Home AI Agent 开发设计文档 |
| V2.0 | 2026-07-19 | 根据项目重新定位和逐项确认，重构为 C5 感知交互、S3 本地自治、ESP-server 控制面架构 |

---

**文档结束**

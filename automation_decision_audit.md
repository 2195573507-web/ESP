# 自动决策链路完整性审计

## 审计边界

- 范围：当前 `/Users/zhiqin/ESP 部分开发` 工作树中的 ESPS3、ESPC51/ESPC52、ESP-server 源码、CMake、头文件、静态测试和静态文档。
- 方法：只追踪源码中可达的调用链；不把注释、需求、测试桩、日志或仅存在的协议常量当作执行能力。
- 未执行：代码修改、烧录、设备运行、服务启动、网络验收。
- 完成度含义：
  - `已实现`：该层的源码链路存在并可达。
  - `部分实现`：输入、状态、规则、事件或协议存在，但链路未到真实设备动作。
  - `未实现/空壳`：缺少生产调用链，或源码明确返回不支持/失败占位。
- 证据等级：`A` = 源码中存在可达实现及调用链；`B` = 源码/CMake 存在但接入不完整或只有静态证据；`C` = 文档、注释、占位接口或未接入代码。

## 结论摘要

严格按“传感器触发规则并导致真实设备动作”统计：

- 真正闭环的自动化数量：`0`。
- 已形成“传感器 -> 状态 -> 规则 -> 事件 -> 服务器上报”的决策族：`2`。
  - 雷达房间习惯规则：6 类规则。
  - BME690 环境告警规则：13 类告警类型。
- 半成品的传感器驱动决策链：`2`。
- 只有数据/事件、没有设备动作的主要决策族：`2`。
- 独立的 Server command -> C5 传输链：存在，但不是传感器规则自动化；其真实执行能力只有 `device.noop` 和部分 LCD 文本显示。
- 最大缺口：没有统一的 `Decision/Event -> Action Policy -> Command Contract -> Device Executor` 接口；本地规则事件不会进入 `command_router`，`smart_home_gateway` 也没有真实执行器。

## 当前已实现自动决策

这里的“已实现”表示规则判断和事件链已实现，不代表已经完成设备动作闭环。

|功能|输入|判断|输出动作|完成度|
|---|---|---|---|---|
|房间进入/离开规则|本地 S3 UART 雷达、C51/C52 远端雷达 -> `radar_home_snapshot`|房间 `occupied=false -> true` 或 `true -> false`|生成 `PERSON_ENTER_ROOM` / `PERSON_LEAVE_ROOM`，进入 FIFO，POST `/api/habit-events`|部分实现，A；无灯光、静音或其他设备动作|
|长停留/长期占用|雷达房间占用状态 + 单调时间|超过 `long_stay_ms` 或 `long_occupancy_ms`|生成 `PERSON_LONG_STAY` / `LONG_OCCUPANCY`，POST `/api/habit-events`|部分实现，A|
|无人超时|雷达房间空状态 + 单调时间|超过 `empty_timeout_ms`|生成 `ROOM_EMPTY_TIMEOUT`，POST `/api/habit-events`|部分实现，A；没有关闭设备动作|
|夜间活动|雷达进入房间 + wall-clock|判断是否处于配置的夜间时间窗|生成 `NIGHT_ACTIVITY`，POST `/api/habit-events`|部分实现，B；生产 wall-clock provider 未确认接入|
|环境温度/湿度告警|C51/C52 BME690 envelope|高低温、高低湿、温湿度快速变化及恢复阈值|生成 active/recovered alarm event，POST `/api/logs/v1/alarms`|部分实现，A；无空调、风扇、播报动作|
|空气质量告警|BME690 气体/湿度派生字段|warning、critical、deteriorating、pollution spike 等规则|生成 active/recovered alarm event，POST `/api/logs/v1/alarms`|部分实现，A；无净化器、风扇或 C5 动作|

### 已确认的两条真实决策链

#### 雷达习惯规则链

```text
S3 本地/远端雷达
  -> radar_spatial_state / radar_registry
  -> radar_home_snapshot
  -> habit_rule_adapter
  -> habit_rule_engine
  -> habit_event FIFO
  -> habit_event_reporter
  -> server_client_post_habit_event_json()
  -> ESP-server /api/habit-events
```

证据：

- 快照聚合：`/Users/zhiqin/ESP 部分开发/ESPS3/components/Middlewares/radar_domain/radar_local_adapter.c:85-133,174-244`、`radar_home_snapshot.c:40-93`。
- 规则判断与事件生成：`/Users/zhiqin/ESP 部分开发/ESPS3/components/habit_rule_engine/habit_rule_engine.c:176-249`。
- 适配器和 reporter：`/Users/zhiqin/ESP 部分开发/ESPS3/components/Middlewares/habit_rule_adapter.c:9-34`、`habit_event_reporter.c:72-148`。
- 上报请求：`/Users/zhiqin/ESP 部分开发/ESPS3/components/Middlewares/server_client/server_client.c:1372-1385`。

#### BME690 环境告警链

```text
C51/C52 BME690 envelope
  -> s3_scheduler
  -> sensor_aggregator_handle_envelope()
  -> environment_alarm_adapter_ingest()
  -> environment_alarm_engine
  -> active/recovered alarm event
  -> environment_alarm_reporter
  -> network_worker
  -> POST /api/logs/v1/alarms
```

证据：

- 输入分发：`/Users/zhiqin/ESP 部分开发/ESPS3/components/Middlewares/runtime/s3_scheduler.c:860-887`。
- 聚合和适配：`/Users/zhiqin/ESP 部分开发/ESPS3/components/Middlewares/sensor_aggregator/sensor_aggregator.c:823-905`、`environment_alarm_adapter.c:235-357`。
- 规则：`/Users/zhiqin/ESP 部分开发/ESPS3/components/environment_alarm_engine/src/environment_alarm_rules.c:43-53`。
- 重试和上报：`/Users/zhiqin/ESP 部分开发/ESPS3/components/Middlewares/environment_alarm_reporter/environment_alarm_reporter.c:220-280,397-441`。

## 输入源与状态清单

### 实际输入源

|输入|结论|证据|
|---|---|---|
|S3 本地雷达|已接入状态聚合和规则|`radar_local_adapter.c:174-244`，A|
|C51/C52 远端雷达|已经由 `radar_ingest` 进入 registry 和 HOME snapshot|`radar_ingest.c:240-280`、`radar_home_snapshot.c:45-80`，A|
|BME690|已进入环境告警适配器和环境规则|`s3_scheduler.c:860-886`、`sensor_aggregator.c:823-895`，A|
|单调时间|用于持续时间、长停留和空房超时|`habit_rule_engine.c:122-129,224-248`，A|
|wall-clock|规则接口存在，但生产 provider 仍是 unavailable|`/Users/zhiqin/ESP 部分开发/ESPS3/components/habit_rule_engine/time_provider.c:5-9`，B|
|网络状态|用于 reporter、重试和队列门控，不参与房间规则判断|`environment_alarm_reporter.c:397-429`，A|
|其他传感器|未发现接入当前自动决策规则的其他实际源|静态全局搜索，B|

### 已生成状态

- 雷达：`RADAR_OCCUPANCY_PRESENT`、`RADAR_OCCUPANCY_HOLD`、`RADAR_OCCUPANCY_VACANT_INFERRED`、`RADAR_MOTION_MOVING`、track/person count、room occupied、home occupied、home person count。
- 习惯运行态：room occupied/empty、occupied duration、empty duration、enter room、leave room、long stay、long occupancy、empty timeout。
- 环境：`WARMUP`、`READY`、`DEGRADED`、`UNKNOWN`、温湿度阈值状态、空气质量 warning/critical、恶化、污染突增、environment unstable、critical environment、active/recovered。
- 未发现完整的 sleep/wake、全局回家/离家、home arrival/home departure、夜间静音状态机。

主要证据：`/Users/zhiqin/ESP 部分开发/ESPS3/components/Middlewares/radar_domain/radar_local_adapter.c:85-152`、`radar_home_snapshot.h:15-31`、`/Users/zhiqin/ESP 部分开发/ESPS3/components/environment_alarm_engine/include/environment_alarm_engine.h:37-49`。

## 部分实现

|功能|缺少环节|证据|
|---|---|---|
|规则编辑与保存|已能 Web CRUD 和写 SQLite，但保存不会触发评估、发布、设备状态变化|`/Users/zhiqin/ESP 部分开发/ESP-server/src/routes/habitRulesRoutes.js:29-108`、`src/db/habitRules.js:1-25`；文档 `docs/habit-rule-server-report.md:38-50`，A|
|规则 bundle|已能编译并 GET `habit-rule-bundle-v1`，但没有自动发布任务、设备下载或安装|`/Users/zhiqin/ESP 部分开发/ESP-server/src/services/habitRuleBundleCompiler.js:40-126`，A|
|version/checksum|可派生版本和 checksum，但 `/version` 与 `/bundle` 的 checksum 输入不同；没有独立发布记录或设备版本状态|`/Users/zhiqin/ESP 部分开发/ESP-server/src/services/habitRulesService.js:162-181`，A|
|S3 规则加载|C 端有 `rule_loader_load_json()` 和 bundle schema 解析，但生产启动使用 defaults；远程函数明确返回 `ESP_ERR_NOT_SUPPORTED`|`/Users/zhiqin/ESP 部分开发/ESPS3/components/habit_rule_engine/rule_loader.c:139-178`、`habit_rule_engine.c:146-154,283-315`，A/B|
|规则下发|Server 有通用 pending command API；habit rule bundle 没有转换成 command，也没有 S3 定时拉取、checksum 校验和安装链|`/Users/zhiqin/ESP 部分开发/ESP-server/src/routes/commandRoutes.js:162-224`、`ESPS3/components/Middlewares/server_client/server_client.c:1557-1627`，B|
|S3 -> C5 command contract|pending command、协议字段、poll/ack 链存在；C5 实际只执行 noop 和部分 LCD 文本，其他协议常量并不等于执行能力|`/Users/zhiqin/ESP 部分开发/ESPS3/components/Middlewares/command_router/command_router.c:160-199,405-527`、`/Users/zhiqin/ESP 部分开发/ESPC52/components/Middlewares/command_domain/system_command/system_server_client.c:691-735`，A/B|
|LCD 文本|可进入 `screen_service_show_text()` 和 LCD service；LCD 未就绪时仍可能成功 ack，另有 placeholder bridge 只记日志|`/Users/zhiqin/ESP 部分开发/ESPC52/components/Middlewares/command_domain/system_command/system_server_client.c:708-735`、`ESPC52/components/Middlewares/display_placeholder/ai_screen_bridge.c:25`，A/B|
|智能家居命令|S3 能拉取 pending command，但没有真实执行器，收到命令会失败 ack|`/Users/zhiqin/ESP 部分开发/ESPS3/components/Middlewares/smart_home_gateway/smart_home_gateway.c:24-27,52-105`，A|

## 未实现

|需求|缺失模块或证据|
|---|---|
|起床自动化|没有 wake/sleep 状态机、起床规则、事件和动作；C|
|离开家|现有 `PERSON_LEAVE_ROOM` 只表示单房间变空，没有全屋离家聚合；`habit_rule_engine.c:193-221`，B|
|回家|没有 home-arrival 规则类型或进入家庭事件；`rule_loader.c:21-30`，B|
|睡眠|没有 sleep state、bedtime 状态机或睡眠规则；C|
|夜间静音|`NIGHT_ACTIVITY` 只生成事件，没有音量或静音策略调用；`habit_rule_engine.c:207-215`，A|
|无人关闭|有 `ROOM_EMPTY_TIMEOUT` 事件，但没有 `event -> command -> actuator`；`habit_rule_engine.c:241-248`，A|
|有人开启|有 `PERSON_ENTER_ROOM` 事件，但没有灯/设备开启命令；`habit_rule_engine.c:201-205`，A|
|灯控制|未发现灯/继电器命令、驱动或执行适配器；B|
|空调控制|未发现命令、协议或真实驱动；B|
|风扇控制|未发现命令、协议或真实驱动；B|
|紧急播报|`alert.play_tone` 可被 S3 映射为 `speaker.play_audio`，但 C5 执行端返回 unsupported；`command_router.c:168-170`、`system_server_client.c:691-735`，A|
|主动提醒|没有自动调度器、规则动作字段或 C5 自动提醒执行模块；C|
|主动简报|没有由传感器/占用状态触发的简报生成与播报链；C|

## C5 当前执行能力边界

### 已有但不属于自动决策的能力

语音应答播放存在真实链路：

```text
Mic/VAD -> voice_chain -> /local/v1/voice/turn
  -> PCM response -> audio_player_stream_open/write/finish
  -> Speaker/IIS
```

证据：`/Users/zhiqin/ESP 部分开发/ESPC52/components/Middlewares/server_voice/server_voice_client.c:342-385`、`voice_domain/voice_chain.c:418`、`speaker/speaker_player.c:1048`，A。

这证明 C5 有语音响应执行能力，但当前没有证据表明 habit/environment 规则会自动触发该语音链。

### 传感器采集能力

C5 的雷达、BME690、语音输入和 LCD 相关代码均存在；其中雷达/BME 主要承担采集、计算、上传或状态展示。采集成功不等于自动化动作完成。

### 明确未形成的设备执行

- `speaker.play_audio`、`speaker.set_volume`、`config.set` 在 C5 command consumer 侧未形成执行分支，最终为 unsupported。
- 灯、空调、风扇没有真实驱动和命令链。
- `smart_home_gateway` 初始化日志明确为 `real_device_attached=0`，pending command 直接以 `no_real_smart_home_device_attached` 失败 ack。

## 当前架构图

```text
Sensor
  |
  +-- S3 UART/远端 Radar -------------------+
  |                                         |
  +-- C51/C52 BME690 -----------------------v
  |                              State Engine
  |                         radar_registry / HOME snapshot
  |                         sensor_aggregator / alarm state
  |                                         |
  |                                         v
  |                                  Rule Engine
  |                         habit_rule_engine / environment_alarm_engine
  |                                         |
  |                                         v
  |                                  Event / Report
  |                         habit_event_reporter / alarm_reporter
  |                                         |
  |                                         v
  |                                  ESP-server API
  |
  +-- Server pending commands -> network_worker -> command_router
                                                |
                                                v
                                         C5 poll / ACK
                                                |
                              +-----------------+----------------+
                              |                                  |
                         noop / LCD                    unsupported / failed
                              |                                  |
                         C5 display                    no smart-home actuator
```

当前缺失的真实主干是：

```text
Rule Event
  -> Action Policy
  -> Command Contract
  -> C5/Smart-home Executor
  -> Real Device Action
  -> Result/ACK
```

`gateway_orchestrator` 已启动相关模块，CMake 也注册了它们：`/Users/zhiqin/ESP 部分开发/ESPS3/components/Middlewares/gateway_orchestrator/gateway_orchestrator.c:62-162`、`components/Middlewares/CMakeLists.txt:1-45,76-102`。因此当前主要问题不是模块完全未编译，而是状态/规则事件与执行端之间没有接线。

## 数量口径

|统计项|数量|口径|
|---|---:|---|
|真正闭环的传感器驱动自动化|0|必须从传感器规则到真实设备动作并有执行结果|
|半成品传感器驱动决策链|2|雷达习惯规则、BME690 环境告警；都到服务器事件为止|
|只有数据/事件没有设备动作的主要决策族|2|习惯事件族、环境告警族|
|习惯规则类型|6|进入、离开、长停留、空房超时、夜间活动、长期占用|
|环境告警类型|13|以 `environment_alarm_engine.h:37-43` 的告警枚举为准|
|独立的 Server command -> C5 控制链|1|不是传感器自动化；只实现 noop 和部分 LCD|
|已确认真实设备执行但非自动规则|1|语音应答播放链|

## 下一阶段建议

1. 先定义并固定统一的 action contract：至少包含 `action_id`、`source_event_id`、目标设备、动作类型、参数、TTL、幂等键、执行状态和失败原因。
2. 在 S3 建立唯一的 `event -> action policy` 层，使 habit event 和 environment alarm 不再只进入 reporter，而能按策略进入 `command_router`；仍需保留事件上报。
3. 将 server bundle 的发布、S3 version 查询、checksum 校验、持久化安装和生效回报补成一条生产链；不要把 `habit_rule_engine_load_json()` 的解析能力当作已下发。
4. 为 C5 分别补齐并验证 LCD、播报、主动提醒的真实执行结果；协议常量、ACK 或日志不能替代执行成功。
5. 接入真实智能家居执行器适配层后，再实现无人关闭、有人开启、环境告警联动、夜间静音等场景；在此之前应保持这些场景标为未实现。
6. 最后再定义全屋聚合状态：起床、睡眠、回家、离家、夜间模式，并明确房间级占用与全屋状态的去抖、超时和多设备一致性。

## 审计限制

本报告是静态源码审计。没有执行 build、flash、monitor、真实 ESP-server 交互或硬件压力验证，因此不能据此声明设备端运行成功、网络重试成功或真实执行器已验收。

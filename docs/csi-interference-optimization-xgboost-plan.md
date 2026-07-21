# WiFi CSI 人体运动检测与环境干扰抑制 XGBoost 最终实施计划

> 状态：合同冻结前的最终计划。本轮只修订本文件；未修改业务源码、配置、CMake、sdkconfig、前端、数据库或 ESP-server。
>
> live-source 复核日期：2026-07-13。当前 C5 callback 是 latest-pending 有界覆盖模型，feature/report worker 分开调度但 report tick 同步 HTTP；S3 CSI trigger、CSI ingress 与 `CSI_FUSION_TICK_MS` flush 是独立时序。它们不能被解释为固定 callback 采样率或一一对应关系。

## 1. 目标、产品语义和不可突破边界

本计划只检测人体**运动**并抑制机械/环境干扰；不是 occupancy、presence、absence 或无人检测器。对外 Canonical CSI v2 正式状态只允许：

- `IDLE`：有效新稳定证据已确认；不表示无人、absence 或静止人体不存在。
- `MOTION`：有效新人体运动证据已确认。
- `HOLD`：证据不足、干扰、重校准、单链路退化、资源恢复或故障安全状态。

`INVALID`、`RECALIBRATING`、`RULE_ONLY`、`MODEL_SHADOW`、`MODEL_ACTIVE` 和诊断标签只在 C5/S3 本地使用，绝不新增 Canonical CSI v2 状态或 Server 字段。

固定边界如下：

1. raw CSI、I/Q、phase、amplitude 数组、子载波序列和完整 feature vector 只在 C5 callback/feature 链短生命周期内存在，绝不离开 C5、持久化或进入 ESP-server。
2. C5 只输出固定大小的低维摘要；S3 是唯一模型推理、双链路配对、融合和正式状态决策点；ESP-server 仍只接收 Canonical CSI v2 最终事实。
3. C51/C52 的共享 CSI 运行逻辑必须 byte-identical；仅现有身份、`link_id` 和板级配置可不同。每一阶段的验收都检查共享文件 parity。
4. 任何 v3 扩展都是 local v2 的加法超集，规则路径必须可消费 v3。模型失败时不得请求第二种 C5 payload。
5. 本计划不修改 ESP-server、前端、数据库或 Canonical CSI v2；不改变语音、BME、heartbeat、command 的优先级或业务合同。

## 2. 当前 live source 基线

| 层 | 已确认事实 | 计划约束 |
| --- | --- | --- |
| C5 callback | `csi_service_rx_cb()` 只复制最近一份 pending CSI；已有 pending 时增加 overwrite 计数并投递 `C5_EVENT_CSI_READY`。callback 不是定时采样器。 | 不得用 S3 trigger 或 report interval 推导 callback 数。callback 保持有界复制、计数和唤醒。 |
| C5 worker/report | `csi_service_process_tick()` 消费一份 latest pending 样本并生成 feature；`csi_service_report_tick()` 目前直接调用同步 `server_comm_http_post_json()`。 | v3 必须把 HTTP 移出 feature/CSI worker。 |
| C5 调度/资源 | C5 CSI process/report 已受 `c5_should_run()`、voice/backpressure 和资源暂停恢复路径影响。 | 新 report owner 必须接入同一 voice/resource/backpressure gate，不能绕过资源仲裁。 |
| S3 ingress/fusion | `/local/v1/csi/result` 经 parser/gateway 进入独立 CSI fusion worker；S3 scheduler 以 `CSI_FUSION_TICK_MS` 触发 flush，并独立发送 CSI trigger。 | trigger、receive、flush 与 C5 callback/window 是不同事件；tick 不得制造新证据。 |
| S3/Server 协议 | local schema 当前为 v2；S3 产出 Canonical CSI v2，外部状态为 `IDLE/MOTION/HOLD`。 | local v3 仅限 C5--S3；Canonical CSI v2 原样保持。 |

旧文档只可提供历史背景，实施前必须重新读取触及模块的 live source。

## 3. 唯一数据流、baseline 所有权和模式

```text
C5 raw CSI/IQ (short lifetime only)
  -> C5 feature_baseline + fixed edge_feature_v3 window
  -> latest-only nonblocking C5 report owner
  -> S3 local v2/v3 parser and window admission
  -> S3 pairing/gate/reliability/decision_baseline
  -> two binary XGBoost heads (when allowed)
  -> evidence state machine
  -> Canonical CSI v2 final fact
  -> ESP-server
```

### 3.1 两类 baseline 的唯一所有权

| baseline | 唯一所有者 | 内容 | 禁止行为 |
| --- | --- | --- | --- |
| `feature_baseline` | C5 | 原始振幅归一化、稳定子载波选择、noise floor、慢速特征基线及其更新/冻结。 | S3 不拥有 raw CSI，也不重算或决定该 baseline。 |
| `decision_baseline` | S3 | 模型 score 阈值、可靠度参考、双链路融合参考、迟滞/状态机参考。 | C5 不做模型推理、双链路融合或最终状态。 |

local v3 每个窗口必须带 `calibration_epoch` 和 `feature_baseline_epoch`。C5 在一次重新校准完成、特征 baseline 重置或影响其统计语义的 BSSID/channel 变化后递增相应 epoch；二者均属于 C5 声明的窗口事实。

S3 观察到任一 epoch 改变时，必须原子地清除该 link 的 motion/idle/interference streak、待配对窗口、模型共识、`decision_baseline` 观察和旧 baseline 观察，冻结该 link 的 decision-baseline 更新，并进入 warmup/`HOLD`。在同一 epoch 已满足 warmup 和全部 gate 前，任何窗口不得成为模型或稳定证据。不得出现“S3 是唯一 raw baseline 决策点”或同义表述。

### 3.2 运行模式、启动和锁存

| 模式 | 正式状态来源 |
| --- | --- |
| `RULE_ONLY` | 规则 gate 和规则状态机；可记录质量诊断。 |
| `MODEL_SHADOW` | 规则仍是唯一正式来源；模型仅记录拟议结果。 |
| `MODEL_ACTIVE` | 通过所有 gate、双链路配对和状态机的模型证据可影响正式状态。 |

开机默认 `RULE_ONLY`。只有 bundle 通过 CRC、schema、feature count、golden vector 和资源预算检查后才能进入 `MODEL_SHADOW`。`MODEL_ACTIVE` 只能由明确配置或受控 canary 启用，不能因 bundle 存在或 shadow 健康而自动开启。

模型硬故障（identity/schema/golden/资源/推理不变量失败）立即转为 `RULE_ONLY` 并在**当前 link generation**锁存；同 generation 内不得自动重复 active/fallback。只有新的合法 generation 或受控人工重新启用流程能解除锁存，且解除前必须再次通过完整 bundle 自检。v2 parser 至少保留到 P8 后的独立移除决策；现场仍需回滚时绝不删除。

## 4. local v3 窗口合同、身份和完整性

### 4.1 v2/v3 协商

S3 先支持 v2/v3 双解析，C5 才可发送 v3。C5 仅在 S3 明确声明 capability 后发送 v3；否则继续 v2。mixed v2/v3 只允许 `RULE_ONLY`，两链路均确认 v3 也仍必须先通过 P0D 合同冻结才可 shadow。v3 缺字段、bitmap/schema 不匹配或范围不合法时拒绝该窗口并进入 `RULE_ONLY/HOLD`，不得猜测或部分解释。BSSID/channel、注册、断链、C5 boot、capability 收回都会清除该 link 的 v3 parser/pairing/baseline 状态。

v3 固定字段至少为：

```text
feature_schema_version
local_protocol_version
feature_capability_bitmap
link_generation
window_seq
window_start_monotonic_ms
window_end_monotonic_ms
window_duration_ms
observed_frame_count
valid_frame_count
callback_overwrite_count
callback_drop_count
calibration_epoch
feature_baseline_epoch
```

`frame_seq_start`、`frame_seq_end`、`trigger_epoch`、`trigger_seq_start`、`trigger_seq_end` 可作为附加字段，但不替代上述字段或新证据身份。

### 4.2 窗口身份和接收序号

一个 C5 窗口的主键是 `(link_generation, window_seq)`。`window_seq` 对每个 generation 单调递增；有限位宽实现必须使用明确的无符号回绕比较并有测试。C5 timestamp 只用于诊断、范围检查和窗口覆盖描述，不能作为去重主键。

每个 S3 link 维护：

```text
last_observed_window_seq
last_consumed_window_seq
```

接收后、通过基础 envelope/schema/generation 检查的严格新 `window_seq` 才更新 `last_observed_window_seq`。同 generation 下 `window_seq <= last_observed_window_seq` 一律以 duplicate、out-of-order 或 replay 拒绝；不得进入模型、配对、baseline 或任何 streak。仅当窗口通过**所有** schema、epoch、完整性、freshness、RF、resource、pairing/降级和模式 gate，且实际进入规则或模型状态机时，才更新 `last_consumed_window_seq`。因此一个窗口只能被消费一次，同一窗口不得重复增加 motion/interference/idle streak。

generation 改变、BSSID/channel 变化、重新注册或 C5 reboot 时：先清空两个 seq、水位、pending pair、streak、共识和 baseline 观察，再接受新 generation 的第一个窗口。旧 generation 延迟包丢弃并计数；同 generation 序号重置是协议异常而非重新开始。

### 4.3 窗口完整性

不得假设 S3 的 50 ms trigger 与 C5 CSI callback 一一对应，也不得把 callback 视为严格固定采样率。`window_duration_ms` 由 C5 monotonic 起止时间计算；`observed_frame_count` 是窗口内 callback 观察总数；`valid_frame_count` 是实际进入特征统计的有效帧数；`callback_overwrite_count` 和 `callback_drop_count` 都是该窗口相对起点的增量。

只有采样机会在目标 C5 硬件/驱动路径中可可靠计数时，才允许增加 `capture_opportunity_count`，并使用它计算完整性。否则不得出现或使用虚假的 `expected_frame_count`。无 opportunity 计数时，完整性 gate 必须同时检查：

1. `window_duration_ms` 位于冻结的最小/最大范围；
2. `valid_frame_count` 达到冻结的绝对下限；
3. 实际 callback/sample 间隔的 min/median/max 或分位数分布未超出冻结范围；
4. `observed_frame_count`、overwrite/drop 增量、seq gap、RF quality 和 calibration 全部合格。

所有数字阈值在 P0D 写入版本化 schema/manifest；未冻结前不得开始 v3、训练或 active。

## 5. 双链路确定性配对合同

每个 S3 双链路 pair 状态必须包含：

```text
fusion_pair_id
pair_new_link_mask
pair_skew_ms
pair_alignment_state
last_observed_window_seq
last_consumed_window_seq
```

`fusion_pair_id` 是本次成对消费的稳定 ID（由两个 `(generation, window_seq)` 及 pair counter 构成）；`pair_new_link_mask` 只标记本 pair 实际新且尚未消费的 link；`pair_skew_ms` 是用于配对的 S3 monotonic receive time 或已验证的 trigger 对齐时间之差；`pair_alignment_state` 固定为 `WAITING`、`PAIRED`、`TIMEOUT_DEGRADED`、`REJECTED` 之一。

1. 各 link 先按第 4.2 节用 `last_observed_window_seq` 拒绝 duplicate/out-of-order/replay；未通过者不能改变 pair state。
2. 双链路共识只能由两个均为新、尚未消费、同一有效 epoch、各自通过单链路 gate 的窗口构成。任一旧窗口可作为 `context_only` 诊断，但不得填入 `pair_new_link_mask`、不得伪装新证据、不得使 streak 递增。
3. 两个候选的 `pair_skew_ms` 必须不超过唯一配置 `MAX_PAIR_SKEW_MS`；首个候选最多等待 `PAIR_WAIT_TIMEOUT_MS`。这两个值由 P0D 根据真实 receive-jitter 冻结，写入 config/manifest/golden replay，未冻结不得进入 P1。
4. 合格 pair 原子消费两个窗口，生成唯一 `fusion_pair_id`，更新两个 `last_consumed_window_seq`，然后最多各增加一次对应 evidence streak。
5. skew 超限、epoch/generation 不同或等待超时必须清除 pending pair 并记 reason。首版单链路退化固定为运行模式 `RULE_ONLY`、正式状态 `HOLD`；双链路模型不得用缺失输入推理。未来单链路模型必须独立训练、导出、golden test 和验收，不能复用双链路 bundle。
6. fusion tick 只检查 freshness、timeout 和安全看门狗；无新 pair/window 时不得增长任何 streak、不得产生 `MOTION`/`IDLE` 或 baseline 更新。

P0D 优先评估并决定是否把 S3 `trigger_epoch/trigger_seq` 发给 C5，并由 C5 回传覆盖的 `trigger_seq_start/end`。若首版不实现，该决定必须记录为“receive-time pairing”；其限制是 receive time 仅表示到达顺序而非采集同步。此模式必须禁止以 receive time 声称物理同步/采样覆盖，使用更保守的 `MAX_PAIR_SKEW_MS`/timeout，在 queue overload 或任一 link lateness 异常时直接 `HOLD`，并把该限制列为 P7/P8 风险。

## 6. C5 nonblocking latest-only report transport

feature/CSI worker 禁止同步等待 HTTP。P1 的固定模型为：

```text
feature worker -> fixed-size summary -> latest/next slots -> low-priority report owner -> HTTP
```

- feature worker 只构造固定大小摘要并发布到 latest slot；不序列化、不 HTTP、不重试、不等待 report owner。
- report owner 是独立低优先级执行者，唯一可调用 HTTP 的 CSI 组件，`MAX_REPORT_INFLIGHT = 1`。
- 采用两个固定大小 slot：一个当前发送，一个 `next`。发送期间新摘要覆盖 `next`，增加 overwrite 诊断计数；不建立无界队列、不保存高频历史。
- 网络失败、超时或 HTTP 拒绝只记录 report failure；不阻塞 callback/feature worker、不重放高频历史、不更新 S3 模型证据，也不把失败窗口重新注入状态机。
- owner 每次取 slot、开始 HTTP、完成 HTTP 和再次发送前均必须通过 voice lease、C5 resource、gateway readiness 和 backpressure gate。语音独占/资源恢复/高压时只保留最新 `next` 或丢弃并计数。

P0B 在具体 C5/S3 目标板上冻结下列全部数值并将其纳入 schema/manifest/测试，未同时冻结不得进入 P1：

```text
MAX_FEATURE_COUNT
MAX_LOCAL_CSI_BODY_BYTES
MAX_SERIALIZATION_TIME_US
MAX_PARSE_HEAP_BYTES
MAX_REPORT_INFLIGHT = 1
```

首版 wire 格式优先固定顺序数值数组、bitmap 和短字段；body、解析和 heap 必须有硬上限。禁止几十个长 JSON 字段名、可变长度 feature object、无界 debug 字符串或动态数组造成 payload/解析时间/heap 无界增长。

## 7. 特征、模型和状态决策合同

### 7.1 特征和模型输入

C5 `edge_feature_v3` 只包含固定大小低维摘要和 valid bitmap，例如归一化后能量/鲁棒分位数/慢快比/时域变化/载波选择质量/RSSI 汇总/窗口质量。C5 的 raw amplitude 归一化、stable carrier、noise floor 和慢速基线都属于 `feature_baseline`。S3 只可在通过 gate 后构造固定顺序的双链路关系特征。

queue depth、CPU、HTTP 延迟、drop/overwrite、heap/stack 等系统负载指标只用于 gate/reliability/诊断，**不得**作为人体运动或干扰模型 feature。manifest 固定 feature index/name/unit/range、clipping/normalization、missing/sentinel/valid-bit、聚合定义、float 精度、schema、vector hash 和最大 feature count；NaN、Inf、`-0.0`、范围外值不可静默转成 0。

### 7.2 两个 binary head 和标签

模型固定为两个独立 binary XGBoost head：`human_motion_score` 和 `interference_score`。每个 head 有独立训练 target、权重、校准、threshold、artifact identity、普通 C 导出和 golden vector。未校准输出只称 score，不称概率。

采集/训练标签固定为：

```text
STABLE_NO_PERSON
STABLE_PERSON_STILL
HUMAN_MOTION
ENVIRONMENT_INTERFERENCE
HUMAN_MOTION_WITH_INTERFERENCE
UNKNOWN
```

| 标签 | human target | interference target | 训练用途 |
| --- | ---: | ---: | --- |
| `STABLE_NO_PERSON` | 0 | 0 | 稳定负样本，单独统计。 |
| `STABLE_PERSON_STILL` | 0 | 0 | 有人静止负样本，单独统计，绝不解释为正式 presence 状态。 |
| `HUMAN_MOTION` | 1 | 0 | 两 head 的正/负组合。 |
| `ENVIRONMENT_INTERFERENCE` | 0 | 1 | 两 head 的负/正组合。 |
| `HUMAN_MOTION_WITH_INTERFERENCE` | 1 | 1 | 重叠正样本，绝不压成互斥类别。 |
| `UNKNOWN`/transition/invalid | 不训练 | 不训练 | 保留 reject reason 和质量审计。 |

每条采集记录至少包含：人员位置、距离、运动类型、速度、方向、干扰设备、干扰档位、房间、摆位、日期、人员标识和采集 `run_id`。`rf_invalid_transition` 只用于 gate/故障注入，不能标成普通干扰正样本。

### 7.3 score 同时高的唯一决策表

所有行都以前置条件为“新且未消费、双链路 pair 合格、RF/calibration/resource gate 合格”。否则直接 `HOLD`，不累计模型 streak。

| human motion | interference | 双链路人体一致性和质量 | 正式状态 | 本地诊断 |
| --- | --- | --- | --- | --- |
| 高 | 低 | 达到 normal human threshold | `MOTION`，经 motion hysteresis | `human_motion` |
| 低 | 高 | 任意 | `HOLD`，经 interference hysteresis | `environment_interference` |
| 高 | 高 | 两链路均达到更高 `MOTION_WITH_INTERFERENCE_HUMAN_THRESHOLD`、方向/时间一致、RF 质量合格 | `MOTION`，经专用 hysteresis | `motion_with_interference` |
| 高 | 高 | 人体证据不足、链路不一致或 RF 质量不足 | `HOLD` | `motion_with_interference_unresolved` |
| 低 | 低 | 稳定 gate 和稳定时间都合格 | `IDLE`，经 idle hysteresis | `stable_candidate` |

interference 高分不能无条件覆盖强且双链路一致的人体运动证据。`HUMAN_MOTION_WITH_INTERFERENCE` 必须单独报告 human-head recall、interference-head recall、最终 `MOTION` recall、false motion rate 和 detection latency。

### 7.4 证据状态机和 decision baseline

证据驱动转换只由实际消费的新 window/pair 触发；安全看门狗可无新证据将 `MOTION` 或 `IDLE` 转为 `HOLD`。无新窗口时只能保持状态或安全进入 `HOLD`，不能增加 streak、不能进入 `MOTION`/`IDLE`、不能更新任一 baseline。

S3 `decision_baseline` 更新必须同时满足：新且已消费的稳定双链路 pair、两 score 低、规则稳定、RF/calibration/resource gate 合格、稳定时间满足、近期没有人体/干扰/voice/recovery、pairing/完整性合格。更新有最大步长和最大速率；motion/interference/epoch/reconnect/BSSID/channel/placement 显著变化后冻结并重建观察。记录 update/freeze/reject/recalibrate reason、link、generation、window/pair、epoch 和版本，但不记录 raw CSI。

## 8. 数据集隔离、校准和事件级指标

数据必须严格隔离为：

```text
train
validation / hyperparameter tuning
calibration / score calibration / threshold selection
final untouched test
```

按 `run_id/date/room/placement/person` 分组，整个组只能落入一个集合，防止同一次采集或相邻窗口泄漏。final untouched test 不得参与 feature 选择、XGBoost 搜索、Platt/Isotonic 校准、head threshold 选择、迟滞阈值、连续窗口数或任何状态机参数调整。所有 split manifest、随机种子、组成员和排除原因可重现。

事件级评估在 P0C 冻结下列唯一规则：

1. ground truth 有明确 onset/offset；短于 `MIN_EVENT_DURATION_MS` 的标注只进 transition/uncertain，不能充当正式事件。
2. 同类预测间隔不大于 `PREDICTION_MERGE_GAP_MS` 时先合并；ground truth 是否合并使用独立、预注册规则。
3. 一个预测事件至多匹配一个 ground-truth event，且一个 truth 至多匹配一个预测；匹配使用冻结的 overlap/onset 条件。
4. 允许提前 `EARLY_TOLERANCE_MS`、延迟 `LATE_TOLERANCE_MS`；detection latency 从 ground-truth onset 到第一个匹配 `MOTION` 的 monotonic 时间计算。
5. 标注边缘 `TRANSITION/UNCERTAIN` 区不计 TP/FP/FN，单列时长和排除原因；禁止静默计作稳定负样本。
6. false motion 同时报 events/hour 和 events/run；并分别报告 `STABLE_NO_PERSON`、`STABLE_PERSON_STILL`、`HUMAN_MOTION`、`ENVIRONMENT_INTERFERENCE`、`HUMAN_MOTION_WITH_INTERFERENCE`。

每个报告给出独立 event/run、总时长、人员/房间/摆位/日期数、95% CI、模式分层和单/双链路分层。高重叠窗口不能充当独立样本量。

## 9. bundle、回退、采集和日志

普通 C bundle 使用静态 const、有界 traversal、无 malloc/递归/运行期树构建；两个 head 的 feature ordering、missing branch、float 语义、校准、SHA-256/CRC、schema、feature count、资源上限都进入 manifest。单窗口 golden vector 覆盖 summary 到 model vector、tree margin 到 calibrated score、缺失/边界/NaN/Inf/`-0.0`/非法 index；任一失败不加载模型。

高频逐窗口日志必须限速并可计数汇总；模型 SHA、schema、版本等固定信息只在启动、模式切换和故障时输出。C5/S3 本地诊断可包含窗口/配对/gate/epoch/score/资源原因，但绝不进入 Server payload。

离线采集文件只存允许的低维摘要、标签和元数据，必须有最大文件数/字节数、轮转、最小剩余空间阈值和空间不足自动停止采集。停止采集必须只影响 debug collector，不能影响正式 CSI、语音、BME、heartbeat 或 command 链路。debug label route 默认不注册；启用时只可在本地受认证接口、限速、大小上限下运行，不能进入 ESP-server。

## 10. sequence replay 合同

除单窗口 golden vector 外，P0D/P2/P5 的确定性 sequence replay 必须覆盖：

```text
duplicate
out-of-order
gap
generation change
calibration_epoch change
feature_baseline_epoch change
BSSID/channel change
single-link loss and recovery
mixed v2/v3
RULE_ONLY -> MODEL_SHADOW -> MODEL_ACTIVE
MODEL_ACTIVE hard failure -> latched RULE_ONLY
human motion with environmental interference
no new window across ticks
same window submitted repeatedly
```

每条 replay 断言 accepted/rejected reason、`last_observed_window_seq`、`last_consumed_window_seq`、`fusion_pair_id`、pair mask/skew/state、streak、mode、正式状态、baseline action 和 latch 状态。特别断言：无新窗口时 tick 不增加 streak；同一窗口绝不第二次消费；epoch/generation 改变清空旧观察；单链路永远不会进入双链路模型推理。

## 11. 阶段门禁和唯一顺序

在 P0C/P0D 全部合同冻结前，禁止 local v3 实现、模型训练或 active 部署。每阶段只可在该阶段范围内修改，C51/C52 公共逻辑仍须 parity，raw CSI 不得外传，Canonical CSI v2 不得变更。

| 阶段 | 产物 | 进入下一阶段的门禁 |
| --- | --- | --- |
| P0A live-source 复核 | 触及模块的源码事实、边界和差异清单。 | C5 callback/worker、S3 ingress/fusion、local v2、Canonical v2、resource gate 均已复核。 |
| P0B 运行基线与非阻塞 transport 诊断 | callback/worker/report/heap/parse/HTTP/voice/backpressure 实测和五项预算候选。 | 五项预算均有目标板数值，且同步 HTTP 风险和资源 gate 已量化。 |
| P0C 数据/标签/事件指标合同冻结 | 六标签、元数据、四集合隔离、event matching 和验收报告模板。 | split/event 规则可复现，final test 隔离被审计。 |
| P0D v3 schema、窗口身份、pairing、baseline epoch 冻结 | v3 字段、完整性、seq、epoch、pairing、trigger 决策、max skew/timeout、sequence assertions。 | 全部字段/参数/单链路行为有唯一语义并通过设计审查。 |
| P1 C5 feature_v3 与异步 latest-only report | C5 feature baseline、窗口字段、双 slot report owner、固定 body。 | 不同步 HTTP、`inflight <= 1`、预算/parity/资源 gate 通过。 |
| P2 S3 v2/v3 parser、gate、pairing、规则降级 | 双 parser、epoch reset、确定性 pairing、`RULE_ONLY/HOLD` 降级和 replay。 | mixed v2/v3 安全、重复不消费、单链路不推理。 |
| P3 数据采集与数据质量审计 | 有限额 collector、标签和质量/隔离审计。 | 元数据完整、采集不干扰正式链路、group split 合格。 |
| P4 两个 binary XGBoost head 训练、校准和导出 | 两 head、校准/threshold、final test 报告、bundle。 | final test 未泄漏，所有分层指标和 overlap 类别单列。 |
| P5 S3 shadow | bundle 自检后 shadow、分歧/资源诊断。 | 无硬故障、shadow 只记录不改正式状态。 |
| P6 受控 canary active | 明确配置的受限 active。 | 双链路 v3、canary/回退锁存、真机观察门禁全部通过。 |
| P7 长时间与跨环境验收 | 独立 event/run、跨环境、长时 false-motion 统计。 | 预注册 CI/时长/分层门禁通过或保持 RULE_ONLY/SHADOW。 |
| P8 是否允许移除兼容路径的独立决策 | v2 parser/兼容路径移除或保留决定。 | 现场回滚需求、部署覆盖和长期验收明确证明可移除；否则保留。 |

## 12. 实施前一致性检查

1. 术语只使用 `IDLE/MOTION/HOLD` 作为正式状态；不混用 occupancy、presence、vacant、empty 等结论。
2. C5 只拥有 `feature_baseline`，S3 只拥有 `decision_baseline`；epoch 改变会清理 S3 旧观察并 warmup/HOLD。
3. trigger、callback、window、receive、fusion tick 被视为不同事件；没有固定 callback 数或一一对应假设。
4. pair、模型 gate、规则 gate、状态机、fallback/latch 的顺序按本计划唯一执行。
5. raw CSI/IQ/phase/amplitude 数组/feature vector 从不进入 ESP-server；Canonical CSI v2、ESP-server 前端和数据库不变。
6. C51/C52 公共逻辑保持一致；voice/resource/backpressure 优先级不被 CSI report owner 绕过。
7. 本轮没有修改业务源码，没有 build、flash、monitor、fullclean、erase-flash、commit、reset、checkout、pull、清理命令，也没有启动 ESP-server。

## 13. 必须由真机数据冻结的参数

P0B/P0D/P0C 必须从目标 C51/C52/S3 实测后冻结以下数值，未冻结不进入其后阶段：`MAX_FEATURE_COUNT`、`MAX_LOCAL_CSI_BODY_BYTES`、`MAX_SERIALIZATION_TIME_US`、`MAX_PARSE_HEAP_BYTES`、窗口持续时间与有效帧下限、callback 间隔分布、overwrite/drop 门限、`MAX_PAIR_SKEW_MS`、`PAIR_WAIT_TIMEOUT_MS`、fresh/stale/RF/reliability 门限、warmup、迟滞和连续窗口数、baseline 速率限制、模型/校准阈值、event matching 容差、bundle flash/stack/CPU 预算和长时验收样本量。计划固定其测量、冻结位置和消费语义，但不以未验证的纸面常数替代真机结果。


# ESPS3 LD2450 雷达链路优化与后续 C5 接入准备实施计划

更新日期：2026-07-17  
适用工程：`/Users/zhiqin/ESP 雷达/ESPS3`  
执行对象：Codex  
实施性质：仅修改 ESPS3 的本地 LD2450 雷达链路；不修改 C51、C52、ESP-server、Dashboard、数据库及其他业务链路。

---

## 0. 文档用途与执行规则

本文是 ESPS3 雷达优化的唯一实施依据。Codex 必须先读取当前 live source，再按本文阶段顺序执行。历史文档、旧行号、旧宏值和旧结论只能用于导航，不能替代当前源码事实。

执行时必须遵守以下规则：

1. 不得凭文件名、历史报告或推测直接修改。
2. 每一阶段开始前必须确认实际文件、公开接口、调用关系、线程所有权和当前构建状态。
3. 每一阶段完成后必须进行对应验证，验证失败不得继续进入下一阶段。
4. 不得把“编译通过”写成“硬件验证通过”。
5. 不得擅自扩大范围，不得顺手重构无关代码。
6. 不得为了让测试通过而删除诊断、放宽错误条件、隐藏异常或伪造结果。
7. 数值参数均为首轮候选值，必须集中配置并保留真机标定入口，不得散落为魔法数字。
8. 若 live source 与本文描述不一致，优先保持本文的目标和边界，同时在执行日志中记录差异；不得自行改变总体架构。
9. 任何无法在当前环境验证的项目必须标记为 `BLOCKED_BY_HARDWARE` 或 `BLOCKED_BY_MISSING_DATA`，不得声称完成。

---

## 1. 最终目标

在不破坏现有 ESPS3 UART 雷达收帧、LD2450 固定帧解析、坐标转换、区域接口、目标跟踪、空间状态和其他业务链路的前提下，将当前 S3 本地雷达处理链路增量强化为：

```text
LD2450 UART
  -> radar_service
  -> ld2450_parser
  -> immutable raw frame
  -> radar_local_adapter
  -> normalized observation frame
  -> raw target validation
  -> coordinate calibration
  -> target association
  -> stable target tracker
  -> spatial state
  -> occupancy / motion state
  -> latest snapshot / registry / diagnostics
```

本轮最终必须解决以下问题：

1. 固定 `900 mm` 最近邻门限在快速移动和多人交叉时容易误配或断轨。
2. 当前贪心匹配在多个目标接近时不是全局最优。
3. 新检测立即建成正式轨迹，单帧杂波可能造成目标数量跳变。
4. 固定 `EMA alpha=0.5` 对静止目标偏灵敏。
5. 单目标突跳保护不能覆盖多人场景。
6. 原始目标数、空间接受目标数、可见轨迹数和业务有效轨迹数容易混淆。
7. UART 离线、输入过期和无目标必须严格区分。
8. 后续 C51/C52 接入时不能再复制一套 S3 空间算法或破坏现有本地雷达算法。

本轮完成后，S3 本地雷达应具备：

- 稳定的三目标定长处理；
- 基于预测位置的目标关联；
- 确定性全局最优小规模分配；
- `TENTATIVE / CONFIRMED / HOLD / EMPTY` 轨迹生命周期；
- 自适应 EMA；
- 通用异常点隔离；
- 正确的 occupancy、motion 和 freshness 语义；
- 完整可回放、可诊断、可量化的测试基础；
- 为未来 C51/C52 输入预留统一内部观测合同，但本轮不激活远端雷达输入。

---

## 2. 本轮范围

### 2.1 允许修改

仅允许修改或新增 `/Users/zhiqin/ESP 雷达/ESPS3` 内与雷达直接相关的代码和文档，预计包括但不限于：

```text
ESPS3/components/radar_ld2450/
ESPS3/components/Middlewares/radar_domain/
ESPS3/components/Middlewares/CMakeLists.txt
ESPS3/components/radar_ld2450/CMakeLists.txt
ESPS3/tests/                         # 若当前工程已有或适合新增隔离测试
ESPS3/docs/                          # 仅本次执行记录和测试说明
```

允许在确认 live source 后增量修改以下职责对应文件：

```text
ld2450_parser.*
radar_service.*
radar_local_adapter.*
radar_coordinate_transform.*
radar_zone_map.*
radar_target_tracker.*
radar_spatial_state.*
radar_diagnostics.*
radar registry/latest snapshot 相关文件
```

允许新增窄职责模块，例如：

```text
radar_observation.*
radar_target_association.*
radar_pipeline_config.*
radar_replay_test.*
```

实际文件名必须以 live source 为准。不得为了匹配本文示例而重复创建已有职责模块。

### 2.2 明确禁止修改

本轮禁止修改：

```text
/Users/zhiqin/ESP 雷达/ESPC51
/Users/zhiqin/ESP 雷达/ESPC52
/Users/zhiqin/ESP 雷达/ESP-server
/Users/zhiqin/ESP 雷达/ESP-server/public
/Users/zhiqin/ESP 雷达/ESP-server/db
任何 managed_components
任何第三方组件源码
LD2450 模组内部固件
```

本轮禁止修改或重构以下业务链路：

```text
BME690
voice
WakeNet
Mic
Speaker
command
Wi-Fi/APSTA
server_client
Dashboard snapshot
ESP-server API
数据库
LCD/LVGL
```

如果雷达结果当前被其他模块读取，只允许保持兼容所需的最小适配，不允许修改消费方业务逻辑。

---

## 3. 冻结决策

以下决策已经冻结，Codex 不得擅自改变。

### 3.1 仅激活 S3 本地雷达

本轮只处理：

```text
RADAR_SOURCE_S3_LOCAL
```

允许在 S3 内部类型中预留：

```text
RADAR_SOURCE_C51
RADAR_SOURCE_C52
```

但本轮不得：

- 新增 C51/C52 雷达 HTTP handler；
- 新增 C5 雷达网络协议；
- 生成伪造的 C51/C52 雷达状态；
- 将 C51/C52 标记为在线或具备雷达数据；
- 修改 child registry 以假装远端雷达已接入；
- 修改 C5；
- 声称三源联调完成。

预留枚举的唯一目的，是让 S3 内部算法上下文从第一天就按 `source` 隔离，避免后续 C5 接入时重写算法。

### 3.2 不进行跨房间融合

未来三个雷达分别位于不同房间。即使本轮预留多源类型，也必须保持：

```text
S3_LOCAL tracker 独立
C51 tracker 独立
C52 tracker 独立
```

禁止：

- 跨源目标合并；
- 跨房间 track ID 关联；
- 多源坐标融合；
- 多源 occupancy 加权；
- 一个源的 freshness 重置另一个源；
- 一个源离线导致另一个源状态清空。

### 3.3 不重写当前有效模块

现有以下能力必须保留并增量增强：

```text
radar_coordinate_transform
radar_zone_map
radar_target_tracker
radar_spatial_state
```

要求：

- 优先保持现有 public API；
- 如接口不足，新增 adapter 或内部扩展接口；
- 不得删除现有原始坐标、过滤坐标和诊断字段；
- 不得将全部算法替换成新框架；
- 不得引入通用 Kalman 库；
- 不得引入深度学习；
- 不得引入动态目标容器。

### 3.4 不新增房间区域划分

本轮不新增家具区、活动区、床区、门区等空间语义。

允许保留并继续使用：

- 最大有效距离；
- 已有房间矩形边界；
- 已有 zone map 接口；
- 坐标标定参数。

默认不改变当前区域配置。没有实物标定证据时，不得改变镜像、旋转、偏移和边界。

### 3.5 不自动写 LD2450 持久化配置

本轮只优化上位机算法，不自动执行：

- 恢复出厂；
- 改波特率；
- 改蓝牙开关；
- 改单目标/多目标模式；
- 改区域过滤；
- 改模组参数。

若当前工程已有查询能力，可以保留查询和日志；不得因为查询失败阻断其他系统功能。

---

## 4. 官方协议与不可违反的解析边界

LD2450 实时上报帧固定为 30 字节：

```text
Byte 0..3    AA FF 03 00
Byte 4..11   target slot 1
Byte 12..19  target slot 2
Byte 20..27  target slot 3
Byte 28..29  55 CC
```

每个目标槽固定 8 字节：

```text
X                  u16 little-endian，方向编码
Y                  u16 little-endian，方向编码
Speed              u16 little-endian，方向编码，单位 cm/s
Distance resolution u16 little-endian，单位 mm
```

必须保持以下事实：

1. 实时上报帧没有 length 字段。
2. 实时上报帧没有 command 字段。
3. 实时上报帧没有官方 CRC/checksum。
4. 完整性判断只能依赖固定长度、帧头、帧尾、流重同步和合理范围检查。
5. 配置/应答帧与实时上报帧不是同一种格式。
6. 全零 8 字节槽表示无目标。
7. `slot 1..3` 不是持久人物 ID。
8. 业务 `track_id` 只能由 S3 tracker 生成。
9. 不得使用 `slot` 作为长期颜色、人物或业务身份。
10. 距离可由 `sqrt(x²+y²)` 派生；`resolution` 不是欧氏距离。
11. 坐标正负方向必须由实物标定确认，不能凭字段名称猜测。

官方示例目标槽必须加入解码测试：

```text
0E 03 B1 86 10 00 40 01
```

预期：

```text
X = -782 mm
Y = 1713 mm
Speed = -16 cm/s
Resolution = 320 mm
```

---

## 5. 当前实现基线与本轮修改原则

根据现有审查结论，当前 ESPS3 已具备：

- 30 字节固定帧缓冲；
- `AA FF 03 00` 帧头识别；
- `55 CC` 帧尾验证；
- 半包和粘包处理；
- 坏头、坏尾逐字节滑动重同步；
- 全零槽过滤；
- X/Y/Speed/Resolution 解码；
- 整数欧氏距离；
- RX task 独占 UART 正常数据读取；
- latest frame 深拷贝；
- 恢复后连续有效帧门禁；
- 镜像、旋转、平移坐标变换；
- 最大距离和矩形边界准入；
- 最多 3 条 track；
- 最近邻匹配；
- 固定 `EMA alpha=0.5`；
- HOLD 与超时释放；
- presence 与 motion 分离；
- 运动进入/退出滞回。

因此本轮原则是：

```text
保留 parser
保留 UART owner
保留坐标模块
保留空间状态模块
新增统一 observation 边界
拆出或强化 association
增量强化 tracker
修正 spatial state 输入语义
补齐诊断和回放测试
```

禁止以“代码更整洁”为理由重写完整雷达链路。

---

## 6. 目标软件架构

### 6.1 分层职责

| 层 | 输入 | 输出 | 唯一职责 | 明确禁止 |
|---|---|---|---|---|
| `radar_service` | UART bytes/events | parser feed、service health | UART 生命周期、读取、恢复、统计 | 不做跟踪、区域、业务状态 |
| `ld2450_parser` | byte stream | immutable raw frame | 帧界定、解码、重同步 | 不做 track、presence、motion |
| `radar_local_adapter` | latest raw frame | observation frame | 复制、source、seq、timestamp、freshness | 不做人物语义 |
| raw validation | observation slots | candidate list | 全零、结构、距离和拒绝原因 | 不改原始数据 |
| coordinate transform | candidate raw point | calibrated point | 镜像、旋转、平移、边界 | 不覆盖 raw 坐标 |
| association | tracks + candidates | assignment result | 预测、gate、确定性最优分配 | 不直接输出 occupancy |
| tracker | assignments | stable tracks | 生命周期、滤波、速度、ID | 不依赖 slot 作为 ID |
| spatial state | stable tracks | presence/motion | 房间准入后的业务语义 | 不把离线当无人 |
| snapshot/registry | pipeline result | latest snapshot | 深拷贝发布和读取 | 不持有瞬态指针 |
| diagnostics | counters/snapshot | bounded logs | 可观测性和限频日志 | 不改变算法状态 |

### 6.2 单一写入者原则

必须保证：

- UART RX task 是 parser/service 的唯一写入者；
- 雷达 pipeline worker 或当前 `radar_local_adapter` 所在线程是 tracker、association、spatial state 的唯一写入者；
- 其他任务只能读取深拷贝 latest snapshot；
- 不允许多个任务直接修改同一个 tracker；
- 不允许诊断任务持有可失效的字符串或结构体指针；
- 不允许通过日志回调反向改变算法状态。

### 6.3 固定容量与内存原则

LD2450 最多 3 个目标，因此所有运行时结构必须定长：

```text
最多 3 个 raw slots
最多 3 个 candidates
最多 3 个 tracks
最多 3 x 3 个 association costs
最多固定数量的历史点
```

硬性要求：

- parser 每帧零 heap allocation；
- association 每帧零 heap allocation；
- tracker 每帧零 heap allocation；
- spatial state 每帧零 heap allocation；
- 不创建新常驻任务；
- 不创建无界队列；
- 不在每帧路径构造 JSON；
- 不在每帧路径使用 `malloc/calloc/realloc/free`；
- 不在任务栈上声明大型数组；
- 所有新增静态上下文总量必须记录；
- 预期新增静态内存不超过 8 KiB，超过必须说明原因并暂停实施评审。

---

## 7. 内部数据合同

实际字段名可按当前工程命名风格调整，但语义必须保持一致。

### 7.1 雷达源

```c
typedef enum {
    RADAR_SOURCE_S3_LOCAL = 0,
    RADAR_SOURCE_C51 = 1,
    RADAR_SOURCE_C52 = 2,
    RADAR_SOURCE_COUNT = 3
} radar_source_id_t;
```

本轮只有：

```text
RADAR_SOURCE_S3_LOCAL = ACTIVE
RADAR_SOURCE_C51 = RESERVED_NOT_ACTIVE
RADAR_SOURCE_C52 = RESERVED_NOT_ACTIVE
```

不得发布 C51/C52 的空快照作为有效状态。

### 7.2 Observation target

```c
typedef struct {
    uint8_t slot_index;             // 0..2，仅表示本帧槽位
    bool raw_valid;
    int16_t raw_x_mm;
    int16_t raw_y_mm;
    int16_t raw_speed_cm_s;
    uint16_t resolution_mm;
    uint32_t distance_mm;
} radar_observation_target_t;
```

### 7.3 Observation frame

```c
typedef struct {
    radar_source_id_t source;
    uint32_t frame_seq;
    int64_t captured_at_ms;
    uint32_t source_age_ms;
    bool source_valid;
    uint8_t raw_target_count;
    radar_observation_target_t targets[3];
} radar_observation_frame_t;
```

规则：

- `frame_seq` 由 S3 本地适配层对每个完整有效帧单调递增；
- 使用无符号 wrap-safe 比较；
- `captured_at_ms` 使用 monotonic time；
- 不使用系统墙钟参与 freshness；
- observation 必须是深拷贝；
- parser 内部缓冲地址不得向下游泄漏；
- 处理完成后不得继续引用 RX 临时缓冲。

### 7.4 Candidate

```c
typedef struct {
    uint8_t slot_index;
    int16_t raw_x_mm;
    int16_t raw_y_mm;
    int16_t raw_speed_cm_s;
    uint16_t resolution_mm;
    int32_t calibrated_x_mm;
    int32_t calibrated_y_mm;
    uint32_t distance_mm;
    bool accepted;
    radar_reject_reason_t reject_reason;
} radar_candidate_t;
```

原始坐标和标定坐标必须同时保存。任何滤波和坐标变换不得覆盖原始值。

### 7.5 Track

每条轨迹至少保存：

```c
typedef enum {
    RADAR_TRACK_EMPTY = 0,
    RADAR_TRACK_TENTATIVE,
    RADAR_TRACK_CONFIRMED,
    RADAR_TRACK_HOLD
} radar_track_state_t;

typedef struct {
    uint16_t track_id;
    radar_track_state_t state;

    int32_t measurement_x_mm;
    int32_t measurement_y_mm;

    int32_t filtered_x_mm;
    int32_t filtered_y_mm;

    int32_t predicted_x_mm;
    int32_t predicted_y_mm;

    int32_t velocity_x_mm_s;
    int32_t velocity_y_mm_s;

    int16_t reported_speed_cm_s;

    uint32_t created_at_ms;
    uint32_t last_hit_ms;
    uint32_t last_update_ms;
    uint32_t miss_age_ms;

    uint8_t consecutive_hits;
    uint8_t consecutive_misses;
    uint8_t confidence;

    bool visible_this_frame;
    uint8_t last_slot_index;
} radar_track_t;
```

具体类型宽度可按当前代码调整，但必须避免：

- `x*x + y*y` 的 16 位溢出；
- `velocity * dt` 的 32 位中间值溢出；
- 时间戳混用 signed/unsigned；
- 通过裸指针暴露内部 track 数组。

### 7.6 拒绝原因

必须使用有限枚举：

```c
typedef enum {
    RADAR_REJECT_NONE = 0,
    RADAR_REJECT_ZERO_SLOT,
    RADAR_REJECT_FRAME_INVALID,
    RADAR_REJECT_SOURCE_STALE,
    RADAR_REJECT_OUT_OF_MAX_DISTANCE,
    RADAR_REJECT_OUTSIDE_ROOM,
    RADAR_REJECT_ASSOCIATION_GATE,
    RADAR_REJECT_TRACK_CAPACITY,
    RADAR_REJECT_UNCONFIRMED,
    RADAR_REJECT_SEQUENCE,
    RADAR_REJECT_INTERNAL
} radar_reject_reason_t;
```

不得把动态字符串作为状态存入 track、snapshot 或异步日志队列。

### 7.7 Snapshot 计数语义

最终 snapshot 必须明确区分：

```text
raw_target_count
accepted_target_count
tentative_track_count
confirmed_visible_track_count
hold_track_count
business_active_track_count
```

定义：

- `raw_target_count`：完整帧中非全零目标槽数量；
- `accepted_target_count`：通过结构、坐标和空间准入的候选数量；
- `tentative_track_count`：尚未确认的新轨迹；
- `confirmed_visible_track_count`：本帧成功匹配的 confirmed 轨迹；
- `hold_track_count`：已确认但当前短时未匹配、仍为关联保留的轨迹；
- `business_active_track_count`：允许参与 occupancy/UI 的轨迹数量，不含 tentative。

任何旧接口若只有一个 `target_count`，必须确认其历史语义并保持兼容。新增计数优先放入内部 snapshot 和诊断，不得无评审改变外部数据合同。

---

## 8. Parser 与 radar_service 修改计划

### 8.1 必须保留的行为

`ld2450_parser` 必须继续支持：

- 半帧；
- 粘包；
- 任意起始偏移；
- 坏头重同步；
- 坏尾重同步；
- 多帧连续输入；
- 一次 100 ms 无数据不清空有效半帧；
- 长期不增长半帧的有限超时清理；
- 全零槽；
- 三槽解码。

### 8.2 允许新增

仅允许补充：

- 完整有效帧计数；
- `frame_seq`；
- 帧完成 monotonic timestamp；
- bad header 计数；
- bad tail 计数；
- skipped byte 计数；
- resync 计数；
- partial frame timeout reset 计数；
- latest complete raw frame 调试副本；
- 离线回放 feed 接口，复用同一 parser，不复制算法。

### 8.3 明确禁止

不得：

- 虚构 CRC；
- 把 30 字节实时帧改成 length-based parser；
- 把配置应答帧混入实时 parser；
- 在 parser 中做坐标翻转；
- 在 parser 中做 EMA；
- 在 parser 中做 presence/motion；
- 在 parser 中生成持久人物 ID；
- 在 parser 中打印每字节日志；
- 在 UART ISR 中执行算法；
- 因一次 read timeout 清空半帧；
- 使用动态扩容缓冲区。

### 8.4 Parser 验收

必须通过：

1. 官方目标槽解码；
2. 单帧一次性输入；
3. 逐字节输入；
4. 两帧粘包；
5. 半帧后续补齐；
6. 错误前缀后有效帧；
7. 错误尾部后下一个有效帧；
8. 帧头跨 read 边界；
9. 连续空 read 不破坏半帧；
10. 长期半帧超时清理；
11. 三个全零槽；
12. 一、二、三个有效槽；
13. 最大和边界数值；
14. 回放接口与 UART feed 产生相同输出。

---

## 9. Observation adapter 与 source context

### 9.1 新增统一 observation 边界

`radar_local_adapter` 不应直接把 parser 私有结构传给 tracker。应新增一个窄适配过程：

```text
latest raw frame
  -> deep copy
  -> assign source=S3_LOCAL
  -> assign frame_seq
  -> assign captured_at_ms
  -> count non-zero slots
  -> build immutable observation
```

### 9.2 多源上下文

内部可定义：

```c
typedef struct {
    radar_source_id_t source;
    radar_pipeline_config_t config;
    radar_target_tracker_t tracker;
    radar_spatial_state_context_t spatial;
    radar_source_diagnostics_t diagnostics;
    radar_snapshot_t latest_snapshot;
    bool active;
} radar_source_context_t;
```

静态保存：

```c
static radar_source_context_t s_sources[RADAR_SOURCE_COUNT];
```

本轮初始化要求：

```text
S3_LOCAL.active = true
C51.active = false
C52.active = false
```

对 inactive source：

- 不运行 tick；
- 不生成业务状态；
- 不刷新 freshness；
- 不出现在有效源列表；
- 不发布 `VACANT`；
- 不输出周期性误导日志。

### 9.3 时序与 freshness

必须统一使用 monotonic ms。

首轮候选：

```text
expected processing cadence: 100 ms
source stale threshold: 保持当前有效值，若当前无集中配置则候选 500 ms
source offline threshold: 保持 radar_service 当前恢复状态机语义
```

不得仅因为一帧没有目标判 source stale。

source 状态与 occupancy 必须正交：

```text
source invalid/stale/recovering -> occupancy UNKNOWN
source valid + no confirmed target -> VACANT_INFERRED
source valid + confirmed visible target -> PRESENT
source valid + recent confirmed target short miss -> HOLD
```

---

## 10. Raw validation 与坐标处理

### 10.1 Raw validation

第一阶段只做有明确依据的检查：

- 全零槽丢弃；
- frame 结构无效丢弃；
- source stale 不进入新一轮业务更新；
- 欧氏距离超过当前配置最大距离时拒绝；
- 坐标变换后超出现有房间边界时拒绝。

不得未经证据新增任意速度上限、分辨率上限或人体分类规则。

### 10.2 坐标变换

必须保持当前顺序：

```text
raw point
  -> flip
  -> rotation
  -> translation
  -> max distance
  -> room rectangle / existing zone admission
```

默认保持当前值：

```text
flip_x = false
flip_y = false
rotation = 0
offset_x = 0
offset_y = 0
max_distance = 当前值，审查结论为 6000 mm
```

除非 live source 已经有用户配置，否则不得改变。

### 10.3 数值安全

计算距离时使用至少 64 位中间值：

```text
d2 = int64(x) * x + int64(y) * y
```

旋转和速度预测同样必须检查中间值宽度。不得因整数溢出产生负距离或错误 gate。

---

## 11. 目标关联算法

### 11.1 目标

将当前固定位置最近邻贪心匹配升级为：

```text
track prediction
  -> pair cost matrix
  -> per-pair dynamic gate
  -> enumerate all legal assignments
  -> deterministic minimum total cost
```

最多 3 个 track 和 3 个 candidate，无需通用 Hungarian 库。

### 11.2 预测位置

对 `TENTATIVE / CONFIRMED / HOLD` 轨迹计算：

```text
dt_ms = now_ms - last_update_ms
```

规则：

- `dt_ms <= 0`：不预测，使用 filtered position；
- 正常范围：使用速度预测；
- 超过 gap 上限：不继续长距离外推，使用 filtered position 并增加 gate 谨慎处理；
- 禁止无限外推 HOLD 轨迹。

建议：

```text
normal_dt_min = 20 ms
normal_dt_max = 500 ms
```

预测：

```text
pred_x = filtered_x + velocity_x * dt_ms / 1000
pred_y = filtered_y + velocity_y * dt_ms / 1000
```

中间计算必须使用 64 位。

### 11.3 动态 gate

首轮候选公式：

```text
motion_budget_mm = speed_mm_s * dt_ms / 1000
gate_mm = max(800, motion_budget_mm + 400)
gate_mm = min(gate_mm, 1200)
```

说明：

- `800 mm` 是首轮基础门限；
- `400 mm` 是观测噪声和预测误差预算；
- `1200 mm` 防止长 gap 时无界匹配；
- `speed_mm_s` 优先使用 tracker 估计速度；
- tracker 速度无效时可参考 LD2450 reported speed 的绝对值；
- 所有参数必须放入 `radar_pipeline_config`；
- 不得散落在函数内部。

`TENTATIVE` 轨迹可使用相同 gate，但必须通过连续命中确认，不得因为 gate 较大直接成为业务轨迹。

### 11.4 Cost

基础 cost：

```text
cost = squared_distance(predicted_position, candidate_position)
```

首轮不加入未经验证的复杂权重。禁止直接加入：

- 人体类别权重；
- 未标定区域概率；
- App 私有算法猜测；
- 随机 tie-break；
- 动态分配库。

### 11.5 全局最优分配

最多 3 x 3，直接枚举所有合法 one-to-one assignment。

比较顺序必须确定：

1. 最小总 cost；
2. 若相同，优先匹配更多已有 `CONFIRMED/HOLD` 轨迹；
3. 若仍相同，优先更低 track_id；
4. 若仍相同，优先更低 slot_index。

保证同一输入始终产生相同输出，避免日志和测试随机漂移。

### 11.6 未匹配处理

- 未匹配 confirmed track：进入或保持 HOLD；
- 未匹配 tentative track：按 tentative 超时规则快速释放；
- 未匹配 candidate：若有空 track，创建 TENTATIVE；
- 无空 track：记录 `RADAR_REJECT_TRACK_CAPACITY`；
- 超 gate candidate 不得强行匹配；
- 不得因一个异常点立即移动 confirmed track；
- 不需要额外动态 outlier 容器，新建 TENTATIVE 的两帧确认本身就是异常隔离。

---

## 12. Track 生命周期

### 12.1 状态转换

```text
EMPTY
  -> 第一次合法未匹配 candidate
TENTATIVE
  -> 连续命中达到确认门槛
CONFIRMED
  -> 当前帧未匹配
HOLD
  -> gate 内重新匹配
CONFIRMED
  -> 超过释放时间
EMPTY
```

### 12.2 首轮参数

集中配置：

| 参数 | 首轮值 |
|---|---:|
| tentative confirm hits | 2 帧 |
| tentative max age | 300 ms |
| tentative allowed misses | 1 帧或当前 cadence 下等价时间 |
| business HOLD | 400–500 ms，默认 500 ms |
| association retention | 1800 ms |
| track release | 1800 ms |
| initial confidence | 保持现有语义或映射为 40 |
| confirmed hit confidence increment | 有界增加 |
| miss confidence decrement | 有界减少 |

confidence 不能作为唯一生命周期依据；时间和连续命中必须是权威条件。

### 12.3 Track ID

要求：

- 仅 tracker 创建；
- 16 位或 32 位单调递增；
- 跳过 0；
- wrap 后不得与当前 active track 冲突；
- 轨迹释放后旧 ID 不立即复用；
- slot 变化不改变 track_id；
- source 不同的 track_id 不能被解释为同一人物。

### 12.4 速度估计

命中后根据 filtered position 和 `dt` 更新：

```text
measured_vx = (new_filtered_x - old_filtered_x) * 1000 / dt_ms
measured_vy = (new_filtered_y - old_filtered_y) * 1000 / dt_ms
```

可以使用有界一阶平滑，但不得引入通用 Kalman。

要求：

- `dt` 非法时不更新速度；
- HOLD 期间不继续积分位置；
- reacquire 后限制速度尖峰；
- 速度只服务预测和运动诊断，不替代 LD2450 reported speed；
- 两种速度都应可诊断。

---

## 13. 自适应 EMA

### 13.1 目标

降低静止目标漂移，同时保持移动目标响应。

首轮候选：

| 条件 | alpha |
|---|---:|
| 低速且预测残差小 | 0.25 |
| 正常移动 | 0.50 |
| 快速移动或残差明显但仍在 gate 内 | 0.60 |

建议使用定点：

```text
alpha_q8:
0.25 -> 64
0.50 -> 128
0.60 -> 154
```

更新：

```text
filtered += alpha * (measurement - filtered)
```

中间值必须使用足够位宽。

### 13.2 切换条件

首轮集中配置候选：

```text
static:
  abs(reported_speed) < 15 cm/s
  and residual < 120 mm

fast:
  abs(reported_speed) >= 60 cm/s
  or residual >= 400 mm

otherwise:
  normal moving
```

这些条件属于首轮参数，不是官方事实。必须通过回放和真机验证后调整。

### 13.3 三帧中值滤波

不默认强制启用。

实施规则：

1. 先完成预测、gate、确认期和自适应 EMA；
2. 使用回放指标确认是否仍有小范围尖峰；
3. 只有中值滤波能降低误运动且不显著增加位置延迟时才启用；
4. 若启用，只保存每 track 固定 3 点历史；
5. 中值滤波位于 association 成功后、EMA 前；
6. 不得对不同 slot 的点直接做中值；
7. 不得对未关联候选跨人混合。

---

## 14. 异常点处理

### 14.1 大突跳

不再保留只针对“一个 accepted、一个 visible”的特殊业务规则作为主要保护。

统一规则：

- confirmed track 只接受 gate 内 assignment；
- gate 外 measurement 不更新该 track；
- 原 track 进入 HOLD；
- gate 外 measurement 可创建 TENTATIVE；
- 该 measurement 只出现一帧时，tentative 快速释放；
- 连续两帧稳定出现时才确认新轨迹。

这同时覆盖：

- 单目标瞬时突跳；
- 多目标槽位交换；
- 多径单帧杂波；
- 真正新目标进入；
- 旧目标离开并在远处出现新目标。

### 14.2 突跳旧逻辑迁移

如果 `radar_spatial_state` 当前存在单目标 `1500 mm` 特殊拒绝：

1. 先添加 association/tracker 通用 gate；
2. 添加回放测试证明相同或更好；
3. 再将旧特殊逻辑降级为兼容保护或移除；
4. 不得在没有测试覆盖时直接删除；
5. 最终不得同时存在相互冲突的双重拒绝，导致真实快速移动被重复过滤。

执行日志必须说明旧规则最终状态。

---

## 15. Spatial state、Occupancy 与 Motion

### 15.1 传感器健康与业务状态分离

必须独立保存：

```text
sensor_health
source_freshness
occupancy_state
motion_state
```

禁止：

```text
UART 离线 -> VACANT
parser recovering -> VACANT
source stale -> IDLE
无有效帧 -> 无人
```

正确规则：

```text
source invalid/stale/recovering -> occupancy UNKNOWN
source valid + confirmed visible > 0 -> PRESENT
source valid + no visible confirmed but recent confirmed miss <= 500 ms -> HOLD
source valid + no visible confirmed and business HOLD expired -> VACANT_INFERRED
```

轨迹可为 association 保留至 1800 ms，但 occupancy HOLD 只保留约 500 ms。二者不得混为同一个超时。

### 15.2 参与 occupancy 的轨迹

仅以下轨迹参与：

```text
CONFIRMED + visible_this_frame
CONFIRMED/HOLD + business hold age <= hold threshold
```

不参与：

```text
TENTATIVE
EMPTY
source stale 时的任何旧 track
超出空间准入的 candidate
```

### 15.3 Motion 候选

仅使用 confirmed visible track 的稳定量：

```text
abs(LD2450 reported speed) >= 15 cm/s
or
filtered displacement >= 120 mm per正常处理周期
```

如果处理 cadence 不是稳定 100 ms，应改为基于 `dt` 的等价速度门限，不能直接沿用单帧位移。

### 15.4 Motion 滞回

首轮保持或统一为：

```text
连续 3 个有效观察进入 MOVING
连续 8 个有效低运动观察退出 MOVING
```

规则：

- TENTATIVE 不触发正式 motion；
- gate 外 measurement 不触发 motion；
- HOLD 不增加 moving counter；
- source stale 时 motion validity 置无效；
- 恢复后必须基于新有效帧重新确认；
- 不得把旧 motion 状态无限保留；
- 不得用 `speed == 0` 作为静止人体唯一条件。

### 15.5 当前外部接口兼容

如果现有 registry 或 snapshot 已向其他 S3 模块暴露：

- 保持字段名称和枚举值兼容；
- 内部增加 `valid/freshness/counts`；
- 外部接口没有 UNKNOWN 表达能力时，不得擅自改变枚举数值；
- 应新增 adapter 将内部状态映射到旧接口；
- 不修改 Server 或 Dashboard；
- 不新增雷达上云接口。

---

## 16. 诊断与日志安全

### 16.1 必须输出的摘要

每个周期摘要至少可获得：

```text
source
frame_seq
timestamp_ms
source_age_ms
sensor_health
raw_target_count
accepted_target_count
tentative_track_count
confirmed_visible_track_count
hold_track_count
business_active_track_count
occupancy_state
motion_state
parse_errors
sequence_rejects
freshness_expiry
association_rejects
track_creates
track_confirms
track_releases
id_switch_test_metric
```

### 16.2 每 track 调试字段

调试模式下可输出：

```text
source
frame_seq
slot_index
track_id
track_state
raw_x/raw_y
calibrated_x/calibrated_y
measurement_x/measurement_y
predicted_x/predicted_y
filtered_x/filtered_y
reported_speed
estimated_vx/estimated_vy
association_residual
gate
confidence
reject_reason
visible
timestamp
```

### 16.3 日志频率

默认：

- summary：不高于 1 Hz；
- 状态改变：立即输出一次；
- parser 错误：限频聚合；
- per-frame mapping：仅调试宏开启；
- raw 30-byte hex：仅诊断开关开启，并有明确限频或采样比例。

不得默认每帧打印大量日志。

### 16.4 字符串安全

鉴于 S3 诊断路径曾存在字符串生命周期风险，新增日志必须遵守：

- 枚举优先打印整数；
- 枚举转字符串必须返回静态常量；
- 对未知枚举返回固定 `"unknown"`；
- 不把栈上临时字符串指针存入异步结构；
- 不把可失效 `const char *` 交给延迟日志；
- 不对未验证 NUL 终止的缓冲使用 `%s`；
- 外部字符串使用长度限制格式；
- snapshot 中不保存动态字符串指针；
- 所有诊断结构深拷贝或保存数值枚举。

---

## 17. 并发、快照与恢复

### 17.1 最新帧读取

必须采用：

```text
producer 写完整 frame
-> 原子/互斥更新 generation
-> consumer 在锁内复制
-> 锁外处理
```

禁止在持锁状态执行：

- association；
- tracker；
- spatial state；
- 日志格式化；
- 网络操作；
- JSON；
- 长时间计算。

### 17.2 Latest snapshot 发布

pipeline 处理完成后：

```text
build local snapshot
-> lock
-> copy to published snapshot
-> increment snapshot generation
-> unlock
```

读取者只能获得完整一致快照，不得看到半更新状态。

### 17.3 Source 恢复

保持 radar_service 现有恢复状态机，不重写 UART 恢复。

恢复时：

1. source health 进入 recovering；
2. occupancy/motion 对外为 UNKNOWN；
3. parser 连续有效帧达到当前门槛后 source 重新 valid；
4. tracker 是否保留必须使用明确策略。

首轮策略：

- 短暂 input gap 且未超过 track release：保留 track 供 reacquire，但不输出 PRESENT；
- radar_service 完整重初始化、长时间离线或时间倒退：清空 source tracker；
- 不清空其他 source；
- 清空行为必须记录 reason。

---

## 18. 参数集中管理

新增或扩展 `radar_pipeline_config`，至少包含：

```text
max_distance_mm
association_base_gate_mm
association_noise_budget_mm
association_max_gate_mm
normal_dt_min_ms
normal_dt_max_ms
tentative_confirm_hits
tentative_max_age_ms
business_hold_ms
track_release_ms
ema_static_alpha_q8
ema_moving_alpha_q8
ema_fast_alpha_q8
static_speed_threshold_cm_s
fast_speed_threshold_cm_s
static_residual_mm
fast_residual_mm
motion_speed_threshold_cm_s
motion_displacement_threshold_mm
motion_enter_frames
motion_exit_frames
source_stale_ms
diagnostic_summary_period_ms
```

要求：

- 默认值唯一；
- 不在多个 `.c` 文件重复；
- 编译期常量或只读配置；
- 本轮不新增 NVS 写入；
- 不新增远程配置接口；
- 参数日志在启动时输出一次；
- 参数非法时 fail fast 或回退到编译默认值，并记录明确原因；
- 不允许运行时出现零除、负超时或 gate 上限小于下限。

---

## 19. 文件级任务

Codex 必须先核对 live source，随后按职责执行。以下是目标，不是强制文件名。

### 19.1 `radar_ld2450`

修改目标：

- 保持 parser 主状态机；
- 补齐 frame metadata 和 parser counters；
- 提供离线 feed/replay 能力；
- 保持 radar_service 单一 UART owner；
- 保持恢复状态机；
- 不添加业务算法。

### 19.2 `radar_local_adapter`

修改目标：

- 将 raw frame 转为 immutable observation；
- 标记 `source=S3_LOCAL`；
- 管理 frame sequence；
- 管理 captured timestamp；
- 执行 source freshness gate；
- 调用 source-specific pipeline；
- 不直接访问 Server；
- 不构造 C5 协议；
- 不新增任务。

### 19.3 `radar_observation`（可新增）

职责：

- observation/candidate/reject reason 类型；
- raw frame 到 observation 的纯转换；
- 固定数组；
- 无 heap；
- 可被单元测试直接调用。

### 19.4 `radar_target_association`（建议新增）

职责：

- prediction；
- gate；
- cost matrix；
- 合法 assignment 枚举；
- deterministic tie-break；
- 输出 matched/unmatched；
- 无 track 生命周期副作用，或副作用严格限定并文档化。

优先做成纯函数，便于测试。

### 19.5 `radar_target_tracker`

修改目标：

- 保留 public API 或增加兼容 wrapper；
- 增加 track state；
- 增加 velocity；
- 增加 predicted position；
- 增加 consecutive hit/miss；
- 增加 adaptive EMA；
- 使用 association 输出；
- 生成 track_id；
- 处理 tentative/confirmed/hold/release；
- 不做 occupancy。

### 19.6 `radar_spatial_state`

修改目标：

- 只消费 accepted candidates 和 stable tracks；
- 明确 raw/accepted/visible/active count；
- 移除或兼容迁移单目标特例；
- 实现 source invalid -> UNKNOWN；
- 区分 business HOLD 与 association retention；
- motion 只使用 confirmed visible stable track；
- 保持已有公开状态接口。

### 19.7 `radar_coordinate_transform` 与 `radar_zone_map`

原则：

- 不改默认坐标方向；
- 不改已配置空间范围；
- 仅补类型宽度、纯函数测试或接口适配；
- 不新增房间区域。

### 19.8 `radar_diagnostics`

修改目标：

- 新计数；
- 新 source/track 字段；
- 限频；
- 静态字符串；
- 深拷贝；
- 不使用危险 `%s`；
- 不影响算法时序。

### 19.9 CMake

只添加实际新增雷达文件和测试依赖。不得：

- 引入大库；
- 引入网络库；
- 引入通用矩阵库；
- 引入 Kalman 库；
- 修改无关组件依赖；
- 修改 managed component。

---

## 20. 实施阶段

## P0：Live source 基线

任务：

1. 确认工作根目录。
2. 确认是否为 git 仓库及 dirty 状态。
3. 列出 ESPS3 雷达相关文件。
4. 查清实际调用链。
5. 查清 task、queue、mutex 所有权。
6. 查清当前 public API 和消费方。
7. 查清当前参数。
8. 查清当前构建命令。
9. 记录当前 build 结果。
10. 记录当前 warning baseline。
11. 不修改源码。

输出：

```text
ESPS3/docs/s3-radar-optimization-baseline.md
```

门禁：

- live source 与调用链未确认，不得进入 P1；
- baseline build 失败时先记录旧失败，不能把旧失败算成本轮回归。

## P1：冻结内部合同

任务：

- 定义 source、observation、candidate、track state、reject reason、snapshot counts；
- 定义参数配置；
- 保持现有 public API；
- 添加必要 adapter；
- 添加编译期断言，如数组容量为 3。

门禁：

- 编译通过；
- 不改变运行行为；
- 不激活 C51/C52；
- 不新增任务和网络接口。

## P2：Observation 与 source context

任务：

- raw frame 深拷贝转 observation；
- S3_LOCAL source context；
- inactive C51/C52 context；
- frame seq；
- monotonic timestamp；
- freshness；
- snapshot generation。

门禁：

- parser 测试全通过；
- observation 转换测试全通过；
- 旧 S3_LOCAL 数据仍能进入现有 pipeline；
- inactive source 不产生业务状态。

## P3：Association

任务：

- prediction；
- dynamic gate；
- cost matrix；
- exhaustive assignment；
- deterministic tie-break；
- matched/unmatched 输出；
- 单元测试。

门禁：

- 1、2、3 目标所有组合确定性；
- slot swap 不导致不必要 track swap；
- gate 外不强配；
- 每帧零 heap。

## P4：Tracker 生命周期与滤波

任务：

- tentative；
- confirmed；
- hold；
- release；
- track ID；
- velocity；
- adaptive EMA；
- confidence 兼容；
- 通用异常隔离。

门禁：

- 单帧杂波不进入业务 count；
- 连续两帧新目标可确认；
- 短暂遮挡可 reacquire；
- 长时间丢失释放；
- 不因 slot 交换改变人物 track；
- 不新增动态内存。

## P5：Spatial、Occupancy、Motion

任务：

- source health/freshness 与 occupancy 分离；
- business HOLD；
- association retention；
- confirmed-only occupancy；
- stable motion；
- 旧单目标突跳逻辑迁移；
- count 语义。

门禁：

- UART offline 不输出 VACANT；
- source valid 且无目标才可 VACANT_INFERRED；
- tentative 不触发 PRESENT；
- gate 外异常不触发 MOVING；
- HOLD 不继续累加 motion。

## P6：诊断与日志安全

任务：

- counters；
- summary；
- per-track debug；
- static enum strings；
- rate limit；
- snapshot-safe logging；
- 栈高水位和处理耗时记录。

门禁：

- 默认日志量可控；
- 不出现瞬态 `%s`；
- 日志关闭时算法行为一致；
- 日志不持锁执行重计算。

## P7：回放与测试

任务：

- parser tests；
- observation tests；
- association tests；
- tracker tests；
- spatial tests；
- end-to-end replay；
- metrics。

门禁：

- 全部纯软件测试通过；
- 失败用例不能通过修改期望值掩盖；
- 每个测试说明输入和预期。

## P8：Build 与静态回归

任务：

- ESPS3 build；
- warning diff；
- heap allocation scan；
- network/API negative scan；
- protected path check；
- resource report；
- format/static check。

门禁：

- build 成功；
- 无本轮新增 warning；
- C51/C52/Server 零修改；
- 无新增 `/local/v1/radar` 网络入口；
- 无新增 `/api/`；
- 无新增 per-frame heap；
- 无新增 task；
- 无虚构 CRC；
- 无 slot-as-ID。

## P9：交付与用户真机清单

输出：

```text
ESPS3/docs/s3-radar-optimization-execution-log.md
ESPS3/docs/s3-radar-replay-test-report.md
ESPS3/docs/s3-radar-resource-report.md
ESPS3/docs/s3-radar-hardware-test-checklist.md
```

硬件部分必须标记为待用户执行。

---

## 21. 纯软件测试矩阵

### 21.1 Parser

| 编号 | 场景 | 预期 |
|---|---|---|
| PAR-01 | 官方目标槽 | 解码数值准确 |
| PAR-02 | 完整单帧 | 输出一帧 |
| PAR-03 | 逐字节输入 | 输出与单帧一致 |
| PAR-04 | 两帧粘包 | 输出两帧 |
| PAR-05 | 半帧补齐 | 不提前输出 |
| PAR-06 | 垃圾前缀 | 重同步到有效帧 |
| PAR-07 | 坏尾后有效帧 | 坏帧拒绝，后帧恢复 |
| PAR-08 | 帧头跨边界 | 正确识别 |
| PAR-09 | 短 timeout | 半帧保留 |
| PAR-10 | 长半帧 timeout | 计数并清理 |
| PAR-11 | 全零槽 | count=0 |
| PAR-12 | 三有效槽 | count=3 |

### 21.2 Association

| 编号 | 场景 | 预期 |
|---|---|---|
| ASC-01 | 单 track 单 point | 正确匹配 |
| ASC-02 | point 超 gate | 不匹配 |
| ASC-03 | 两 track 两 point 平行 | ID 稳定 |
| ASC-04 | 两人交叉 | 全局 cost 最优 |
| ASC-05 | slot 交换 | track 不随 slot 交换 |
| ASC-06 | 三 track 三 point | one-to-one |
| ASC-07 | cost tie | tie-break 确定 |
| ASC-08 | HOLD reacquire | 恢复原 track |
| ASC-09 | 长 gap | 不无限预测 |
| ASC-10 | 快速移动 | dynamic gate 允许合理匹配 |

### 21.3 Tracker

| 编号 | 场景 | 预期 |
|---|---|---|
| TRK-01 | 新目标一帧 | tentative，不进入业务 |
| TRK-02 | 新目标两帧 | confirmed |
| TRK-03 | tentative 单次 miss | 释放或按配置处理 |
| TRK-04 | confirmed 短 miss | HOLD |
| TRK-05 | HOLD 内 reacquire | 原 ID |
| TRK-06 | 超 1800 ms | release |
| TRK-07 | 静止抖动 | filtered 抖动下降 |
| TRK-08 | 正常移动 | 跟随延迟可接受 |
| TRK-09 | 单帧远跳 | 旧 track 不跳 |
| TRK-10 | 远处新目标连续出现 | 新 track 确认 |

### 21.4 Spatial / Motion

| 编号 | 场景 | 预期 |
|---|---|---|
| SPA-01 | source invalid | UNKNOWN |
| SPA-02 | valid 无目标 | VACANT_INFERRED |
| SPA-03 | tentative only | 不 PRESENT |
| SPA-04 | confirmed visible | PRESENT |
| SPA-05 | 短暂丢失 | HOLD |
| SPA-06 | HOLD 超时 | VACANT_INFERRED |
| SPA-07 | 速度连续超阈值 | 3 帧后 MOVING |
| SPA-08 | 位移连续超阈值 | 3 帧后 MOVING |
| SPA-09 | 低运动连续 | 8 帧退出 |
| SPA-10 | 异常 raw jump | 不触发 MOVING |
| SPA-11 | source stale | UNKNOWN，不是 VACANT |
| SPA-12 | 恢复 | 新帧重新确认 |

### 21.5 端到端回放

至少生成或使用以下序列：

1. 静止单目标加小幅噪声；
2. 单帧 1.5–3 m 突跳；
3. 单人匀速走动；
4. 单人快速走动；
5. 两人平行；
6. 两人相向交叉；
7. 一人短暂遮挡；
8. 两人槽位互换；
9. 一帧虚假第三目标；
10. 三人进入和离开；
11. source 短时无帧；
12. source 长时间离线后恢复；
13. 房间边界内外切换；
14. frame sequence wrap 附近；
15. 时间戳异常或重复处理保护。

---

## 22. 量化指标

回放报告必须至少提供：

```text
input frame count
valid frame count
parser reject count
raw target count
accepted target count
track create count
track confirm count
track release count
false confirmed track count
ID switch count
association reject count
occupancy transition count
motion false-positive count
motion enter delay
motion exit delay
position jitter before/after
processing time avg/max
static memory delta
heap allocation count per frame
```

验收要求：

1. 单帧杂波不得成为 confirmed 业务目标。
2. slot 交换不得直接造成 track ID 交换。
3. source stale/offline 不得输出 VACANT。
4. 已确认单目标短暂丢失后，应在 HOLD 窗口内保持业务连续性。
5. 处理路径每帧 heap allocation 为 0。
6. 不新增常驻任务。
7. 算法处理时间必须明显低于当前约 100 ms cadence。
8. 首轮硬门槛：关闭详细日志时，单帧三目标 pipeline 最大处理时间不超过 5 ms；若超出，标记失败并分析，不得通过降低 cadence 掩盖。
9. 新增静态内存不超过 8 KiB；超过必须暂停并报告。
10. build 无本轮新增 warning。

位置精度、最终人数准确率和真实 ID switch 率必须由用户真机数据确认，纯合成测试不得宣称代表真实环境。

---

## 23. 静态扫描与验证

Codex 应根据实际环境执行等价命令。

### 23.1 范围检查

确认只修改 ESPS3：

```bash
find /Users/zhiqin/ESP\ 雷达/ESPC51 -type f -newer <baseline_marker>
find /Users/zhiqin/ESP\ 雷达/ESPC52 -type f -newer <baseline_marker>
find /Users/zhiqin/ESP\ 雷达/ESP-server -type f -newer <baseline_marker>
```

若有 git 仓库，优先使用对应 `git status --short`。顶层不是 git 时不得伪造顶层 git 结果。

### 23.2 禁止项扫描

```bash
rg -n "malloc|calloc|realloc|free" ESPS3/components/radar_ld2450 ESPS3/components/Middlewares/radar_domain
rg -n "CRC|checksum" ESPS3/components/radar_ld2450
rg -n "/api/|/local/v1/radar" ESPS3/components/radar_ld2450 ESPS3/components/Middlewares/radar_domain
rg -n "slot.*track_id|track_id.*slot" ESPS3/components/radar_ld2450 ESPS3/components/Middlewares/radar_domain
rg -n "xTaskCreate|xTaskCreateWithCaps" ESPS3/components/radar_ld2450 ESPS3/components/Middlewares/radar_domain
```

结果必须人工判断，不能仅以“有匹配”为失败。例如测试代码可能检查 malloc 为零；注释可能说明无 CRC。执行日志应解释每个命中。

### 23.3 Build

```bash
source /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh >/dev/null
cd /Users/zhiqin/ESP\ 雷达/ESPS3
idf.py build
```

不得执行：

```text
flash
monitor
erase-flash
fullclean
启动 ESP-server
访问生产数据库
```

除非用户后续明确授权。

---

## 24. 用户真机验收清单

Codex 只生成清单，不执行。

### HW-S3-01 UART 健康

观察：

- valid frame rate；
- bad header/tail；
- resync；
- source age；
- recovery state；
- UART overflow。

通过标准：

- 正常环境持续收到有效帧；
- 无持续 parser 错误增长；
- 短时干扰可自动恢复；
- source offline 时 occupancy 为 UNKNOWN。

### HW-S3-02 已知点坐标标定

至少测试：

```text
正前方近点
正前方远点
左前方
右前方
房间边界附近
```

记录：

- raw X/Y；
- calibrated X/Y；
- 官方工具坐标；
- 真实测量位置。

未完成前不得修改默认 flip/rotation/offset。

### HW-S3-03 静止单人

检查：

- raw 坐标抖动；
- filtered 坐标抖动；
- false motion；
- track ID；
- occupancy。

目标：

- 静止时 filtered 抖动明显小于 raw；
- 不频繁产生新 track；
- 不频繁进入 MOVING；
- 短时丢失进入 HOLD。

### HW-S3-04 单人行走

检查：

- 跟踪延迟；
- dynamic gate；
- motion 进入/退出；
- 快速移动是否断轨；
- 大转向是否误建新 track。

### HW-S3-05 两人交叉

检查：

- slot 变化；
- track ID switch；
- target count；
- temporary merge/split；
- official tool raw count 对比。

该项是 association 优化的核心验收。

### HW-S3-06 离线恢复

操作：

- 暂停/断开雷达数据；
- 恢复雷达。

检查：

- 离线时 UNKNOWN；
- 不报告 VACANT；
- 恢复门禁；
- tracker 清理或 reacquire 行为；
- 无崩溃、无死锁。

### HW-S3-07 资源与稳定性

至少运行覆盖：

- Wi-Fi 正常；
- BME 正常；
- voice 空闲；
- 雷达持续；
- 周期日志；
- 服务器不可达降级。

检查：

- heap；
- largest block；
- task stack high-water；
- CPU cadence；
- watchdog；
- crash；
- 日志安全。

Codex 未执行这些测试时必须标记：

```text
HARDWARE_VALIDATION_PENDING
```

---

## 25. 回滚策略

实施前建议：

- 若 ESPS3 是 git 仓库，创建本地 checkpoint commit 或 patch；
- 不推送远程，除非用户明确要求；
- 若不是 git 仓库，生成修改前文件清单和 patch 备份；
- 不修改 build 产物作为回滚依据。

每阶段回滚必须可恢复到上一阶段，不得同时混入无关重构。

若 P3–P5 算法回归失败：

1. 保留 P0 baseline 和测试；
2. 回滚 association/tracker/spatial 行为修改；
3. 保留不改变行为的 parser 测试和观测诊断；
4. 不通过关闭错误检查掩盖回归。

---

## 26. 最终交付物

Codex 完成后必须交付：

1. 实际修改文件清单；
2. 实际新增文件清单；
3. 每个文件的职责；
4. baseline build 结果；
5. final build 结果；
6. warning 差异；
7. parser 测试报告；
8. association 测试报告；
9. tracker 测试报告；
10. spatial/motion 测试报告；
11. end-to-end replay 报告；
12. 参数默认值表；
13. 静态内存变化；
14. heap allocation 检查；
15. task/queue 变化；
16. protected paths 零修改证明；
17. 未完成事项；
18. 硬件待验收清单；
19. 风险和回滚说明；
20. 后续 C5 接入所需的内部 observation 合同说明。

不得在最终报告中声称：

- C51/C52 已接入；
- 三源已联调；
- 官方 App 算法已复现；
- Kalman 已实现；
- 真机精度已通过；
- 人体身份可以长期识别；
- slot 是人物 ID；
- UART 离线代表无人；
- Server/Dashboard 已支持雷达。

---

## 27. 完成定义

只有同时满足以下条件，S3 软件阶段才能标记为完成：

```text
live source 基线完成
+ parser 行为无回归
+ observation 合同完成
+ S3_LOCAL source context 完成
+ C51/C52 保持 inactive
+ deterministic association 完成
+ track lifecycle 完成
+ adaptive EMA 完成
+ occupancy/motion 语义完成
+ diagnostics 安全完成
+ 全部纯软件测试通过
+ ESPS3 build 通过
+ 无新增 warning
+ 每帧零 heap allocation
+ 无新增常驻任务
+ protected paths 零修改
+ 硬件项目明确标记 pending
```

软件完成不等于雷达真机优化完成。只有用户完成 HW-S3-01 至 HW-S3-07 并提供结果后，才能冻结最终坐标参数、gate、EMA、HOLD、release 和 motion 阈值。

---

## 28. 后续 C5 阶段边界

本轮完成后，只允许形成以下未来接口准备：

```text
C5 parsed lightweight targets
  -> S3 remote observation adapter
  -> same source-specific association/tracker/spatial pipeline
```

后续 C5 阶段必须继续保证：

- C5 只做 BLE 接收、基础帧解析、轻量目标输出；
- S3 做坐标标定、区域、association、tracker、occupancy、motion；
- C51/C52 各自使用独立 source context；
- 不复制 S3 算法到 C5；
- 不把 C5 slot 当人物 ID；
- 不跨房间融合。

本轮不得提前实现 transport、HTTP handler、codec 或 C5 firmware。

---

## 29. Codex 最终执行指令

```text
在 /Users/zhiqin/ESP 雷达 中严格执行《ESPS3 LD2450 雷达链路优化与后续 C5 接入准备实施计划》。

本轮只允许修改 ESPS3 的雷达相关目录、必要的雷达 CMake 和本次测试/执行文档；禁止修改 ESPC51、ESPC52、ESP-server、Dashboard、数据库、managed_components 及 BME、voice、command、Wi-Fi、LCD 等无关链路。

先读取 live source，完成调用链、线程所有权、公开接口、当前参数、dirty 状态和 baseline build 审计。不得以历史文档代替源码。

保留当前 LD2450 30 字节 parser、UART 单一读取者、坐标变换、zone map、tracker 和 spatial state 的有效能力，不重写整条链路。新增统一 immutable observation 边界和 source-specific context；本轮只激活 S3_LOCAL，C51/C52 仅可作为 inactive 预留枚举，不得新增远端雷达接口或伪造状态。

将当前固定门限贪心最近邻增量升级为预测位置、动态 gate、3 目标定长全局最优确定性分配；增加 TENTATIVE/CONFIRMED/HOLD/EMPTY 生命周期、连续两帧确认、约 500 ms 业务 HOLD、约 1800 ms 关联保留与释放、自适应 EMA、速度估计和通用异常点隔离。所有参数必须集中配置，禁止散落魔法数字。

严格区分 raw、accepted、tentative、confirmed visible、hold 和 business active count。严格区分 sensor health、freshness、occupancy 和 motion。UART 离线、恢复中或数据 stale 必须输出 UNKNOWN，不得输出 VACANT。TENTATIVE、异常 raw jump 和 gate 外 measurement 不得触发正式 PRESENT 或 MOVING。

所有每帧路径必须零 heap allocation，不新增常驻任务、不新增无界 queue、不引入 Kalman/深度学习/大库、不虚构 CRC、不把 slot 当 track ID、不修改坐标默认方向、不自动修改 LD2450 持久化配置。

完成 parser、observation、association、tracker、spatial/motion 和端到端 replay 测试，记录 ID switch、false track、状态延迟、抖动、处理耗时、静态内存和 allocation 指标。完成 ESPS3 build、warning 对比、禁止项扫描、protected path 零修改检查和资源报告。

禁止 flash、monitor、erase-flash、fullclean、启动 ESP-server或声称真机验收通过。所有硬件测试必须输出给用户执行，并标记 HARDWARE_VALIDATION_PENDING。
```

# ESP 雷达：HLK-LD2450 替换 WiFi CSI 最终实施计划

版本：V1.0  
日期：2026-07-16  
执行对象：Codex  
目标工程根目录：`/Users/zhiqin/ESP 雷达`  
允许修改：`ESPC51/`、`ESPC52/`、`ESPS3/`  
禁止修改：原项目备份、`ESP-server/`、Dashboard 前端、真实数据库、第三方组件源码、雷达模组内部固件

---

## 0. 文档定位

本文是“在新工程目录 `ESP 雷达` 中，用三颗 HLK-LD2450 替换现有 WiFi CSI 感知链路”的最终实施计划。

本计划不是只读审计，也不是概念草案。本计划授权 Codex 在以下三个固件工程内修改、删除和新增代码：

```text
/Users/zhiqin/ESP 雷达/ESPC51
/Users/zhiqin/ESP 雷达/ESPC52
/Users/zhiqin/ESP 雷达/ESPS3
```

本计划不授权 Codex 修改：

```text
/Users/zhiqin/ESP-111                    # 原项目备份或其他历史项目
/Users/zhiqin/ESP 雷达/ESP-server       # 即使目录存在也保持只读
/Users/zhiqin/ESP 雷达/ESP-server/public
/Users/zhiqin/ESP 雷达/ESP-server/db
任何 managed_components、第三方库源码、雷达模组内部固件
```

本计划覆盖并替代“现有代码一行不能修改，只能新增文件”的旧约束，但仅在新目录 `ESP 雷达` 的 `ESPC51`、`ESPC52`、`ESPS3` 范围内生效。其他边界继续有效。

Codex 必须以当前 live source 为事实依据。旧文档、旧行号、旧宏值和历史报告只能帮助导航，不得替代源码扫描。

---

## 1. 已冻结的项目决策

以下决策已经由用户确认，不得在实施过程中擅自改变：

1. 共使用三颗 HLK-LD2450。
2. C51、C52、S3 各连接一颗 LD2450。
3. 三块设备位于三个不同房间，探测区域不存在重叠。
4. 三个雷达源完全独立，不做跨房间融合，不做双链路加权。
5. 不修改 LD2450 内部固件；主控仅通过 UART 接收上报和按需发送配置命令。
6. 不需要 Codex 决定硬件接线和 GPIO；代码只能引用独立板级配置，不得猜测引脚。
7. 不进行雷达与 CSI 并行影子测试。
8. 直接从新工程中移除现有 WiFi CSI 正式链路。
9. 原工程已有备份，因此新工程不需要保留 CSI 回退开关或兼容运行路径。
10. 第一阶段只修改 C51、C52、S3，不修改 ESP-server、Dashboard 和数据库。
11. 雷达结果不得伪装成 WiFi CSI，不得继续调用 `/kernel/csi_event` 上报雷达数据。
12. BME690、Wi-Fi 基础连接、register、heartbeat、status、command、语音、WakeNet、Mic、Speaker、LCD/LVGL 等既有功能必须保持。
13. C5 继续只访问 S3 的 `/local/v1/*` 本地接口，C5 不得访问任何 Server `/api/*` 或公网地址。
14. S3 仍是唯一 Server-facing 网关，但本阶段不新增雷达 Server API。
15. 硬件烧录、串口 monitor 和真机验收由用户执行，Codex 不代替用户宣称硬件通过。

---

## 2. 最终目标

完成后，系统必须形成以下正式运行拓扑：

```text
房间 A
HLK-LD2450 -> UART -> ESPC51
                    -> 本地帧解析
                    -> 本地状态机
                    -> POST /local/v1/radar/state
                    -> ESPS3 radar registry[source=C51]

房间 B
HLK-LD2450 -> UART -> ESPC52
                    -> 本地帧解析
                    -> 本地状态机
                    -> POST /local/v1/radar/state
                    -> ESPS3 radar registry[source=C52]

房间 C
HLK-LD2450 -> UART -> ESPS3
                    -> 本地帧解析
                    -> 本地状态机
                    -> ESPS3 radar registry[source=S3_LOCAL]

ESPS3 radar registry
  -> 分别维护三个房间的最新状态
  -> 不融合、不互相覆盖
  -> 提供本地诊断和后续 Server 接入边界
```

替换完成后，active firmware 中不得再运行以下 WiFi CSI 行为：

```text
C5 WiFi CSI callback
C5 I/Q 拷贝和幅值计算
C5 CSI 校准和特征窗口
C5 CSI edge detector
C5 /local/v1/csi/result 上报
S3 UDP CSI trigger
S3 CSI local ingress
S3 CSI worker queue
S3 C51/C52 CSI fusion
S3 CSI latest cache
S3 /kernel/csi_event 上传
```

---

## 3. 非目标与能力边界

本轮明确不实现：

- 不修改雷达内部算法或固件。
- 不使用 LD2450 BLE 作为正式通信链路。
- 不使用 PA9 作为正式状态真源。
- 不做人员身份识别。
- 不做跨房间人员轨迹关联。
- 不把三个目标槽位当作固定的三个人。
- 不承诺精确人数统计。
- 不承诺对长时间完全静止人员做可靠存在检测。
- 不做跌倒、姿态、呼吸、心率、睡眠或生命体征检测。
- 不上传 10 Hz 原始 UART 字节流。
- 不上传每一帧完整目标历史。
- 不修改 ESP-server 接口、数据库结构或 Dashboard。
- 不将雷达数据编码成 CSI CanonicalEvent。
- 不重构与本任务无关的 S3 大文件或现有网络架构。
- 不顺便修复其他无关问题。

### 3.1 静止人员边界

HLK-LD2450 是运动目标跟踪模组。人在走动、转身、抬手或身体存在微动时可能持续被检测；长时间完全静止时不能保证一直上报目标。

因此正式业务状态不得只使用“有人/无人”二值语义。系统必须区分：

```text
RADAR_UNKNOWN
RADAR_MOTION
RADAR_HOLD
RADAR_VACANT_INFERRED
```

`RADAR_VACANT_INFERRED` 只表示“超过配置保持时间未检测到运动目标”，不表示安全级确认无人。

---

## 4. HLK-LD2450 协议事实

实现必须依据 `LD2450 串口通信协议 V1.03`，并以项目资料中的开发指南校对。

### 4.1 UART 基线

```text
波特率：256000
数据位：8
校验位：None
停止位：1
字节序：Little Endian
标称上报频率：10 Hz
每帧长度：30 bytes
最大目标数：3
```

### 4.2 数据帧

```text
AA FF 03 00
+ target_1 8 bytes
+ target_2 8 bytes
+ target_3 8 bytes
+ 55 CC
```

目标字段顺序：

```text
X          uint16 little-endian，特殊方向编码，单位 mm
Y          uint16 little-endian，特殊方向编码，单位 mm
speed      uint16 little-endian，特殊方向编码，单位 cm/s
resolution uint16 little-endian，直接值，单位 mm
```

全零的 8 字节目标槽位表示目标不存在。

### 4.3 特殊符号解码

不得直接把 X、Y、speed 强制转换为普通 `int16_t` 二补码。

```text
X:
  raw = lo | (hi << 8)
  hi bit7 = 1 -> raw - 32768
  hi bit7 = 0 -> -raw

Y:
  raw = lo | (hi << 8)
  value = raw - 32768

speed:
  raw = lo | (hi << 8)
  hi bit7 = 1 -> raw - 32768
  hi bit7 = 0 -> -raw

resolution:
  raw = lo | (hi << 8)
  直接使用
```

必须用协议示例验证：

```text
0E 03 B1 86 10 00 40 01
-> X = -782 mm
-> Y = 1713 mm
-> speed = -16 cm/s
-> resolution = 320 mm
```

### 4.4 配置命令

正式运行默认只依赖数据上报。任何写配置动作必须显式开启，不能在每次启动时无条件写入雷达。

配置事务必须是：

```text
进入配置模式
-> 等待并校验 ACK
-> 发送单个配置命令
-> 等待并校验 ACK
-> 结束配置模式
-> 等待并校验 ACK
-> 等待正常数据帧恢复
```

默认行为：

- 可以查询固件版本和追踪模式。
- 不自动恢复出厂。
- 不自动改变波特率。
- 不自动修改蓝牙状态。
- 不自动修改区域配置。
- 不因查询失败阻断主系统其他功能。

---

## 5. 最终软件架构

## 5.1 公共雷达核心

C51、C52、S3 必须使用语义一致的 LD2450 解析器和状态机，禁止三套独立算法漂移。

优先方案：每个固件工程内保留同名雷达组件副本，并通过 parity 检查保证核心文件一致。不得重新引入顶层外部共享组件依赖，除非当前三个工程已经明确支持且不破坏独立构建。

建议目录：

```text
components/radar_ld2450/
├── CMakeLists.txt
├── include/
│   ├── ld2450_types.h
│   ├── ld2450_parser.h
│   ├── ld2450_uart.h
│   ├── ld2450_config.h
│   ├── radar_presence.h
│   ├── radar_service.h
│   └── radar_board_config.h
├── ld2450_parser.c
├── ld2450_uart.c
├── ld2450_config.c
├── radar_presence.c
├── radar_service.c
└── tests/
    ├── test_ld2450_parser.c
    ├── test_ld2450_resync.c
    ├── test_ld2450_decode.c
    └── test_radar_presence.c
```

### 5.1.1 职责边界

`ld2450_uart.c`

- 初始化 UART。
- 接收字节流。
- 将字节交给 parser。
- 记录 UART overflow、buffer full、timeout、driver error。
- 不做 HTTP。
- 不做 JSON。
- 不做浮点运算。
- 不在 ISR 中执行状态机。

`ld2450_parser.c`

- 搜索帧头。
- 支持任意分片输入。
- 支持连续多帧输入。
- 校验固定长度和帧尾。
- 逐字节恢复同步。
- 解析最多 3 个目标。
- 只发布完整有效帧。
- 不分配 heap。

`ld2450_config.c`

- 实现配置帧和 ACK 解析。
- 串行化配置事务。
- 默认不自动写配置。
- 配置期间显式暂停普通数据发布，完成后恢复。

`radar_presence.c`

- 执行目标有效性过滤。
- 执行 N-of-M 防抖。
- 维护 `UNKNOWN/MOTION/HOLD/VACANT_INFERRED`。
- 不访问 UART、Wi-Fi、HTTP 或 Server。

`radar_service.c`

- 组合 UART、parser 和 presence。
- 保存 latest-only 快照。
- 暴露线程安全只读接口。
- 输出诊断计数。

`radar_board_config.h`

- 只放 UART port、TX、RX、缓冲大小和板级启用开关。
- C51/C52/S3 可以不同。
- Codex 不得猜测 GPIO。
- 若工程中没有已确认引脚，必须保留明确的编译期配置入口，并在报告中标记“等待用户填写”，不得用随机空闲引脚伪造完成。

---

## 5.2 C51 / C52 终端层

C51 与 C52 必须保持同构。除以下项目外，公共雷达源码必须一致：

- local device ID。
- device_id/alias/room 映射。
- UART port 和 GPIO 板级配置。

每块 C5 的正式数据流：

```text
LD2450 UART
-> radar RX task
-> fixed parser
-> latest valid frame
-> room-local presence state machine
-> latest radar snapshot
-> radar runtime/report worker
-> POST /local/v1/radar/state
-> S3
```

C5 只负责本房间本雷达，不知道另外两个房间状态。

---

## 5.3 S3 网关层

S3 必须维护三份完全独立的雷达源：

```c
RADAR_SOURCE_C51
RADAR_SOURCE_C52
RADAR_SOURCE_S3_LOCAL
```

建议新增：

```text
ESPS3/components/Middlewares/radar_domain/
├── CMakeLists.txt
├── include/
│   ├── radar_protocol.h
│   ├── radar_registry.h
│   ├── radar_remote_ingest.h
│   ├── radar_local_adapter.h
│   └── radar_diagnostics.h
├── radar_protocol.c
├── radar_registry.c
├── radar_remote_ingest.c
├── radar_local_adapter.c
└── radar_diagnostics.c
```

S3 本地雷达解析必须复用 `radar_ld2450` 核心，不得另写一套字节解码。

S3 不再执行任何两房间融合算法。S3 registry 只保存：

- source ID。
- 对应 device ID。
- 对应 room ID。
- source online/unknown 状态。
- 当前 presence state。
- 当前目标数。
- 最新最多 3 个目标。
- 最近有效帧时间。
- 最近 motion 时间。
- 最近状态变化时间。
- sequence/session 信息。
- 诊断计数。

三个 source 的更新不得互相覆盖。一个雷达离线只影响对应房间。

---

## 6. 内部数据结构

建议使用固定大小结构，不使用每帧动态内存。

```c
typedef enum {
    RADAR_STATE_UNKNOWN = 0,
    RADAR_STATE_VACANT_INFERRED = 1,
    RADAR_STATE_HOLD = 2,
    RADAR_STATE_MOTION = 3,
} radar_presence_state_t;

typedef struct {
    bool valid;
    int16_t x_mm;
    int16_t y_mm;
    int16_t speed_cm_s;
    uint16_t resolution_mm;
} radar_target_t;

typedef struct {
    uint32_t frame_seq;
    uint64_t received_at_ms;
    uint8_t target_count;
    radar_target_t targets[3];
} radar_frame_t;

typedef struct {
    radar_presence_state_t state;
    uint8_t current_target_count;
    uint32_t state_seq;
    uint64_t state_since_ms;
    uint64_t last_valid_frame_ms;
    uint64_t last_motion_ms;
    bool uart_online;
    bool frame_fresh;
    radar_target_t targets[3];
} radar_snapshot_t;
```

约束：

- `target_count <= 3`。
- 所有计数使用饱和递增，避免溢出影响诊断。
- timestamp 使用本机单调时间，不依赖 Server 时间同步。
- 不在内部结构保存原始 30 字节历史队列。
- 可保存当前原始帧用于调试，但只能 latest-only，且默认日志不得打印全部原始字节。

---

## 7. 房间状态机

## 7.1 输入

状态机输入是经过以下处理后的当前有效目标结果：

1. 完整帧头和帧尾校验通过。
2. 目标槽位不是全零。
3. 数值在协议和实现允许范围内。
4. 可选软件区域过滤通过。
5. 当前帧未过期。

第一阶段不使用复杂目标轨迹关联。

## 7.2 初始参数

以下为代码默认值，必须集中到配置结构，不得散落 magic number：

```text
RADAR_FRAME_PERIOD_EXPECTED_MS       = 100
RADAR_ONLINE_TIMEOUT_MS              = 3000
RADAR_ENTER_WINDOW_FRAMES            = 3
RADAR_ENTER_REQUIRED_FRAMES          = 2
RADAR_SHORT_GAP_MS                   = 1000
RADAR_HOLD_TIMEOUT_MS                = 900000   # 15 min
RADAR_REPORT_HEARTBEAT_MS             = 1000
RADAR_TARGET_REPORT_MIN_INTERVAL_MS  = 500
RADAR_MAX_TARGETS                     = 3
```

15 分钟保持时间只是初始业务值，必须支持按房间配置。用户真机测试后可以调整。

## 7.3 转换规则

### 启动

```text
boot
-> RADAR_STATE_UNKNOWN
```

在收到足够连续有效帧前不得直接输出 VACANT。

### 检测到运动目标

最近 3 个有效帧中至少 2 帧存在一个或多个有效目标：

```text
ANY STATE
-> RADAR_STATE_MOTION
```

重新检测到目标时应立即退出 HOLD 或 VACANT_INFERRED。

### 短暂目标消失

```text
MOTION
+ 连续无目标时间 < 1 s
-> 保持 MOTION
```

用于过滤短丢帧和目标槽位闪烁。

### 进入保持

```text
MOTION
+ 连续无目标时间 >= 1 s
-> HOLD
```

HOLD 表示最近检测到活动，当前不能确认目标仍被跟踪。

### 推断空房

```text
HOLD
+ 距离 last_motion_ms >= hold_timeout
+ UART 在线
+ 有持续有效空帧
-> VACANT_INFERRED
```

不得仅因为 UART 无数据或解析失败进入 VACANT_INFERRED。

### 雷达异常

以下任一情况进入 UNKNOWN：

- 启动后未收到有效帧。
- 连续 3 秒无有效帧。
- UART driver 错误且尚未恢复。
- 帧错误率持续超过冻结门限。
- 雷达配置事务未正确结束。
- 软件判断数据 freshness 不足。

```text
ANY STATE
+ sensor unhealthy
-> UNKNOWN
```

UNKNOWN 不得被 Dashboard 或上层解释成 VACANT。

---

## 8. C5 -> S3 本地协议

## 8.1 新接口

```text
POST /local/v1/radar/state
```

该接口是 C5 到 S3 的本地轻量接口，不属于 Server API。

S3 必须只接受已注册且身份绑定正确的 C51/C52。雷达报文不得建立新身份，不得覆盖 peer mapping，不得替代 register/heartbeat。

## 8.2 报文原则

- 只发送最新状态。
- 不发送原始 UART 流。
- 不发送历史数组。
- 不发送浮点距离或角度作为必要字段。
- 最多发送 3 个固定目标。
- body 编码最坏情况必须有固定上限。
- 建议最大 body `<= 768 bytes`。
- C5 网络断开时不积压历史，只保留 latest snapshot。
- 重连后只发送当前 snapshot 一次。

## 8.3 建议 JSON v1

为保持可读和便于调试，第一版允许使用明确字段名；后续有证据表明需要压缩时再缩短。

```json
{
  "schema_version": 1,
  "local_id": 1,
  "sequence": 1234,
  "uptime_ms": 456789,
  "state": "motion",
  "target_count": 2,
  "uart_online": true,
  "frame_fresh": true,
  "last_motion_age_ms": 0,
  "targets": [
    {"x_mm": -782, "y_mm": 1713, "speed_cm_s": -16, "resolution_mm": 320},
    {"x_mm": 230, "y_mm": 2400, "speed_cm_s": 12, "resolution_mm": 300}
  ]
}
```

允许状态字符串：

```text
unknown
vacant_inferred
hold
motion
```

## 8.4 协议校验

S3 必须校验：

- Content-Length 上限。
- JSON 完整性。
- schema version。
- local_id 只允许 1 或 2。
- local_id 与已认证/已绑定 peer 一致。
- state 在白名单内。
- `target_count` 范围 0..3。
- `targets` 数量与 `target_count` 一致。
- 数值不超出结构范围。
- sequence 在当前 child session 内单调。
- uptime 明显倒退时按重启/session 处理，不误判为攻击或永久拒绝。
- 不允许 `raw_csi`、`iq`、`phase`、`subcarrier` 等旧 CSI 字段。

建议响应：

```text
202 accepted              # 已进入 S3 radar registry 更新路径
400 invalid payload
403 identity mismatch
409 sequence conflict
413 payload too large
503 local queue unavailable
```

202 只表示 S3 本地接受，不表示 Server 已持久化。本阶段没有雷达 Server 上传。

---

## 9. 上报策略与背压

C5 本地 UART 解析按约 10 Hz 运行，但 HTTP 不按 10 Hz 无条件发送。

发送条件：

1. presence state 变化时立即发送。
2. target_count 变化时发送，但受最小 500 ms 限制。
3. 目标坐标发生显著变化时可发送，但最高 2 Hz。
4. 状态稳定时每 1 秒发送一次 current-state heartbeat。
5. S3 不可达时只保留最新 snapshot。
6. 语音独占或网络压力时暂停 radar HTTP，上层状态机继续运行。
7. 恢复后立即发送一份最新 snapshot，不回放中间历史。

必须使用 latest-only/coalescing 语义：

```text
旧待发送 snapshot + 新 snapshot
-> 覆盖旧 snapshot
-> 只保留新 snapshot
```

禁止为雷达状态创建无界 queue 或离线历史 spool。

---

## 10. 与现有系统的资源关系

## 10.1 C5

雷达 UART 接收不得因语音进入独占而停止，否则长语音后无法判断房间当前状态。

语音期间：

```text
继续：UART RX、帧解析、latest snapshot、presence state
暂停或降频：radar HTTP 上报、非必要诊断日志
```

Wi-Fi 断开期间：

```text
继续：本地雷达状态判断
停止：HTTP 尝试风暴
保留：仅最新 snapshot
```

BME、heartbeat、status、command、voice 的优先级和行为不得因雷达接入被改变。

## 10.2 S3

S3 自身雷达 UART 必须独立于 STA/Server 状态运行。

S3 即使 Server 离线，也应：

- 接收 C51/C52 radar state。
- 更新三个本地房间 registry。
- 继续解析 S3 本地雷达。
- 不把本地失败返回为 Server 失败。

语音期间，S3 不应停止本地 radar registry 更新。

## 10.3 初始资源预算

以下为上限目标，不是允许无条件分配的额度：

### C5 每块板

| 项目 | 目标上限 |
| --- | ---: |
| UART RX ring | 1024 bytes |
| parser buffer/state | 128 bytes |
| latest frame + snapshot | 512 bytes |
| report JSON buffer | 768 bytes |
| radar RX task stack | 4096 bytes 初始值，运行后按 HWM 调整 |
| radar worker新增长期 heap | 4096 bytes 以内 |
| 每帧 heap allocation | 0 |

### S3

| 项目 | 目标上限 |
| --- | ---: |
| S3 local UART RX ring | 1024 bytes |
| 三源 radar registry | 4096 bytes 以内 |
| 单个 ingress body | 768 bytes 上限 |
| radar local task stack | 4096 bytes 初始值 |
| radar registry task/worker | 优先复用现有调度，不无依据增加大栈任务 |
| 每帧 heap allocation | 0 |

移除 CSI 后释放的任务栈、队列和特征内存不得立即被无关功能全部占满。必须保留内部 RAM 和 DMA 安全余量。

不得为了“看起来优化”而在没有 high-water mark 的情况下盲目缩减通用网络、语音和调度任务栈。

---

## 11. WiFi CSI 完整移除范围

Codex 必须先执行引用扫描，再删除 CSI-only 文件。不得因文件名包含 CSI 就机械删除共享代码。

## 11.1 C51 / C52 必须移除

按 live source 定位并清除：

- `esp_wifi_set_csi_rx_cb()`。
- `esp_wifi_set_csi(true/false)`。
- `wifi_csi_info_t` 使用。
- CSI callback 和 pending I/Q buffer。
- amplitude、subcarrier、calibration、baseline、variance、CV、motion score 等 CSI 专用计算。
- CSI event 类型、CSI worker queue、CSI worker 分支。
- `MAIN_ENABLE_CSI_SERVICE` 及相关宏。
- `CONFIG_ESP_WIFI_CSI_ENABLED` 默认配置。
- `sensor_domain/csi_placeholder`。
- `sensor_domain/csi_phase_a` 或其他 active CSI 算法目录。
- `/local/v1/csi/result` 客户端。
- CSI report cadence、日志、metrics。
- CMake 中所有 CSI source/include/requires。

如果某个 runtime worker 同时承担 BME 或 system，必须只移除 CSI 分支，不得删除共享 worker。

## 11.2 S3 必须移除

按 live source 定位并清除：

- `csi_placeholder_gateway`。
- `csi_fusion`。
- UDP CSI trigger 和 `ping trigger csi`。
- CSI trigger interval/config。
- `/local/v1/csi/result` handler。
- CSI protocol adapter parsing。
- CSI worker queue、lock、task、flush tick。
- per-link CSI latest cache。
- fusion state、MOTION/HOLD/IDLE CSI state machine。
- `sensor_aggregator_handle_csi_fact()` 及 CSI-only path。
- network worker CSI latest upload/coalescing。
- `server_client_post_csi_event_json()`。
- `/kernel/csi_event` firmware endpoint constant。
- CSI Server JSON work type。
- S3 CMake 中所有 CSI source/include/requires。

如果 raw/IQ/phase forbidden-key 检查是通用安全校验的一部分，可以保留通用拒绝逻辑；不得因移除 CSI route 而削弱其他输入校验。

## 11.3 删除后静态要求

在 active firmware 源码中执行负向扫描，排除 `build/`、`managed_components/`、文档和归档目录：

```text
esp_wifi_set_csi
wifi_csi_info_t
CONFIG_ESP_WIFI_CSI_ENABLED
MAIN_ENABLE_CSI_SERVICE
/local/v1/csi/result
/kernel/csi_event
csi_fusion
csi_placeholder
ping trigger csi
raw_csi
subcarrier_data
```

预期：

- active 业务代码中不再存在 CSI 启动、采集、处理、接口或上传。
- 如果只剩禁止字段字符串或迁移说明，必须逐项解释。
- 不能只关闭宏而保留 active CMake 编译和后台任务。

---

## 12. C5 实施步骤

## C5-P0：基线扫描

Codex 必须：

1. 确认项目根目录确实是 `/Users/zhiqin/ESP 雷达`。
2. 分别读取 C51/C52 当前启动链、CMake、sdkconfig.defaults、runtime workers、Wi-Fi、server_comm。
3. 列出所有 CSI active files、引用者、CMake entry、宏和运行时入口。
4. 确认 C51/C52 当前差异，只允许身份和板级配置差异。
5. 记录修改前 build 状态；若基线本来失败，必须记录原始失败，不得归因于雷达。
6. 不修改原项目备份。

输出：`docs/radar-migration-baseline.md` 或项目允许的同类文档。

## C5-P1：删除 CSI

1. 先断开 app startup 和 runtime 引用。
2. 删除 CSI event/worker 分支。
3. 删除 CSI CMake entry。
4. 删除 sdkconfig.defaults 中 WiFi CSI 开关。
5. 删除 CSI-only 文件和目录。
6. 运行负向扫描。
7. 此阶段不新增雷达，先确保普通 Wi-Fi、BME、voice、command 编译链没有被误删。

验收：C51/C52 不包含 active CSI；其他链路引用完整。

## C5-P2：加入公共 LD2450 核心

1. 新增 fixed parser。
2. 新增特殊符号解码函数。
3. 新增 UART adapter。
4. 新增可选配置命令事务。
5. 新增 presence state machine。
6. 新增 host/offline tests。
7. C51/C52 核心源码保持一致。

验收：协议样例解析正确，噪声/分片/连续帧测试通过。

## C5-P3：接入 runtime

1. 在 scheduler/worker ready 后启动 radar UART，避免早期事件无消费者。
2. radar RX 只发布 latest frame 或 task notification，不向全局 queue 每字节投事件。
3. presence worker 生成 latest snapshot。
4. 语音 gate 只暂停网络发送，不暂停 UART RX。
5. Wi-Fi link 未 ready 时不反复创建 HTTP 请求。
6. radar 启动失败必须进入 UNKNOWN，但不能阻断 BME/voice/system 启动，除非用户后续明确将雷达定义为系统关键启动项。

## C5-P4：新增 radar local client

1. 新增 `/local/v1/radar/state` codec/client。
2. 使用固定 JSON buffer。
3. 校验实际最坏编码长度。
4. 状态变化优先、稳定状态 1 秒 heartbeat。
5. 失败采用 latest-only，下次覆盖，不重放历史。
6. 不使用 `/api/*`。

## C5-P5：C51/C52 parity

执行逐文件差异检查：

- 公共 parser/state/client 必须一致。
- 允许差异仅限身份和 `radar_board_config.h`。
- 任何单边修复必须同步另一块 C5。

---

## 13. S3 实施步骤

## S3-P0：基线扫描

必须读取：

- gateway orchestrator。
- local HTTP server route 注册。
- protocol adapter。
- child registry / resource manager。
- scheduler 和 worker queue。
- network worker。
- sensor aggregator。
- server client。
- CSI trigger、fusion、upload 的完整调用链。

输出所有 CSI 删除点和共享依赖，避免破坏 BME、status、voice、command。

## S3-P1：删除 CSI

按第 11.2 节执行。

删除后：

- S3 不再创建 CSI fusion worker。
- S3 不再创建 CSI queue/lock。
- scheduler 不再有 CSI 100 ms flush。
- S3 不再向 C5 发送 CSI trigger。
- local HTTP 不再注册 `/local/v1/csi/result`。
- network worker 不再维护 CSI latest body。
- server client 不再调用 `/kernel/csi_event`。

## S3-P2：新增 radar protocol 和 registry

1. 定义 radar local payload parser。
2. 定义三源 registry。
3. 定义 source-to-device/room 固定映射。
4. registry 更新必须线程安全。
5. remote C5 source 必须通过 child/session/peer identity gate。
6. S3 local source不经过 HTTP，直接调用相同 registry API。
7. 每个 source 单独维护 freshness 和 UNKNOWN。

## S3-P3：新增 `/local/v1/radar/state`

1. 注册新 handler。
2. 限制 body 大小。
3. 读取 peer/session 信息。
4. parse 后再更新 registry。
5. invalid body 不更新 freshness。
6. duplicate same sequence/same body 可作为 no-op。
7. same sequence/different body 返回 conflict 并计数。
8. backward sequence 不更新当前状态。
9. 一个 C5 的错误不得使另一房间状态失效。

## S3-P4：接入 S3 本地雷达

1. 使用公共 `radar_ld2450`。
2. 建立 S3 本地 UART task。
3. 将 local snapshot 适配到 `RADAR_SOURCE_S3_LOCAL`。
4. 不经过 local HTTP，不伪造 C5 local_id。
5. S3 STA/Server 离线不影响本地雷达。

## S3-P5：诊断和本地状态

必须提供受限日志或只读诊断接口，至少能看见：

- 三个 source 的 state。
- target_count。
- last_valid_frame_age_ms。
- last_motion_age_ms。
- UART online。
- parse error count。
- sequence reject count。
- source identity mismatch count。
- current room mapping。

不得高频打印每个目标坐标。正常日志建议 5 至 10 秒汇总一次，状态变化立即打印一次。

本阶段不新增 Server 上传。不得为“保持 Dashboard 有数据”而继续伪装调用 CSI API。

---

## 14. 构建系统修改规则

允许修改三套固件中的：

- `CMakeLists.txt`。
- `idf_component.yml`，仅在确实需要依赖且有证据时。
- `sdkconfig.defaults`。
- app config header。
- orchestrator/runtime/source/header。

不建议手工长期维护生成的 `sdkconfig`。优先修改 `sdkconfig.defaults` 和源码配置，再使用正常 reconfigure/build 流程。

禁止：

- 修改 `managed_components/`。
- 为解析 30 字节协议引入大型第三方 JSON、DSP 或雷达库。
- 复制 Android、iOS 或 STM32 demo 整个工程进入固件。
- 从零实现浮点 DSP 算法冒充雷达内部处理。

---

## 15. 测试计划

## 15.1 Parser 单元测试

必须覆盖：

1. 官方示例帧。
2. 三个目标全有效。
3. 目标 2/3 全零。
4. 每个可能的 UART 分片位置。
5. 两帧连续无间隔。
6. 帧前随机噪声。
7. 帧中出现伪帧头。
8. 错帧尾后下一帧恢复。
9. 只有半帧后超时。
10. 超长垃圾输入，不越界。
11. X 正负方向。
12. Y 编码。
13. speed 正负方向。
14. resolution 最大边界。
15. 输入 NULL、零长度和重复 feed。

要求：

- 无 heap allocation。
- 无数组越界。
- 错帧可恢复。
- 完整帧只发布一次。

## 15.2 Presence 状态机测试

必须覆盖：

1. 启动保持 UNKNOWN。
2. 2-of-3 进入 MOTION。
3. 单帧目标不误触发。
4. 1 秒短 gap 保持 MOTION。
5. 超过短 gap 进入 HOLD。
6. HOLD 期间重新检测立即进入 MOTION。
7. 15 分钟后进入 VACANT_INFERRED。
8. UART timeout 从任意状态进入 UNKNOWN。
9. UART 恢复后必须经过有效帧确认，不直接恢复旧状态。
10. 空帧不能在 UART offline 时推进 VACANT。
11. 计时器回绕或时间倒退保护。
12. 每个房间独立 profile。

## 15.3 C5 codec/client 测试

- 最坏 3 目标 body 小于冻结上限。
- target_count 与数组一致。
- 不包含 raw bytes、CSI、I/Q、phase、subcarrier。
- 网络失败只保留 latest。
- state transition 不被 1 秒 heartbeat 节流吞掉。
- 重连后只发送当前状态。

## 15.4 S3 ingest 测试

- C51 正常报文更新 C51 source。
- C52 正常报文更新 C52 source。
- C51 报文不得更新 C52。
- 伪造 local_id 被拒绝。
- 未注册 peer 被拒绝。
- oversized body 被拒绝。
- sequence 重复和回退按合同处理。
- invalid body 不刷新 freshness。
- C51 offline 不影响 C52 和 S3 local。
- S3 local radar 不经过 HTTP。

## 15.5 静态回归

必须执行：

- 三固件 build。
- C51/C52 parity check。
- CSI 负向扫描。
- C5 `/api/` 和公网 URL 负向扫描。
- ESP-server diff 为空检查。
- `ESP-server/public` diff 为空检查。
- `managed_components` diff 为空检查。
- `git diff --check` 或对应静态格式检查。

---

## 16. Codex 构建权限和硬件边界

Codex 可以在用户现有 ESP-IDF 环境中执行：

```text
idf.py reconfigure
idf.py build
静态 rg/find/diff 检查
host/offline 单元测试
```

Codex 不得执行：

```text
idf.py flash
idf.py monitor
idf.py erase-flash
idf.py fullclean，除非用户单独明确授权
启动或修改 ESP-server
访问真实生产数据库
声称真机雷达已通过
```

若 UART GPIO 尚未提供导致 firmware 无法完成最终 build，Codex 必须：

1. 完成所有与引脚无关的 parser、state、protocol 和 integration 代码。
2. 将板级配置缺口明确标记为 blocked。
3. 不猜测引脚。
4. 不把 blocked 写成 passed。

---

## 17. 用户真机验收计划

以下由用户执行，Codex只提供日志说明和验收表。

## HW-P1：单板单雷达

分别测试 C51、C52、S3：

- 上电持续收到约 10 Hz 有效帧。
- 人进入后 500 ms 内进入 MOTION。
- 人离开后进入 HOLD。
- HOLD 到配置时间后进入 VACANT_INFERRED。
- 雷达断电后 3 秒内进入 UNKNOWN。
- 雷达恢复后自动重新同步。
- 目标坐标方向与实际左右一致。
- 最多 3 目标解析无异常。

## HW-P2：三房间并行

- 三个房间状态完全独立。
- C51 房间运动不改变 C52 和 S3 local。
- C52 离线不影响另外两个 source。
- S3 重启后 C5 能重新上报 current snapshot。
- 不存在旧 CSI trigger 流量。

## HW-P3：全功能回归

- Wi-Fi、register、heartbeat、status 正常。
- BME690 正常。
- WakeNet、Mic、VAD、voice turn 正常。
- Speaker 正常。
- LCD/LVGL 正常。
- command poll/ACK 正常。
- 语音期间 radar UART 不溢出。
- 网络断开期间本地 radar state 继续更新。
- 网络恢复后只发送最新 radar state。

## HW-P4：稳定性

最低执行：

- 8 小时连续运行。
- 多轮语音。
- Wi-Fi 断开/恢复。
- 单个雷达断电/恢复。
- 三颗雷达同时运行。

发布前建议：24 小时运行。

观察：

- 无 Guru Meditation。
- 无 watchdog reset。
- 无 UART overflow 持续增长。
- 无 heap 持续下降。
- 无 task stack overflow。
- 无 BME/voice/LCD/heartbeat 永久停止。

---

## 18. 验收门槛

## 18.1 功能门槛

- 三颗 LD2450 均形成独立状态源。
- C51/C52 能向 S3 上报 radar state。
- S3 能解析自身雷达。
- S3 registry 同时维护三源。
- 不存在跨房间融合。
- UNKNOWN、MOTION、HOLD、VACANT_INFERRED 转换符合合同。

## 18.2 CSI 移除门槛

- C5 不启用 WiFi CSI。
- C5 不存在 CSI callback/feature/report runtime。
- S3 不发送 CSI trigger。
- S3 不存在 CSI fusion worker。
- S3 不注册 CSI local route。
- S3 不调用 CSI Server endpoint。
- active CMake 不编译 CSI-only 文件。
- active firmware 日志不再出现 CSI 启动、触发、融合和上传。

## 18.3 兼容门槛

- C5 仍只访问 `/local/v1/*`。
- ESP-server 无修改。
- Dashboard 无修改。
- BME、voice、command、LCD 链路不改变外部合同。
- C51/C52 除身份和板级配置外保持同构。

## 18.4 资源门槛

- radar parser 每帧零 heap allocation。
- 无无界 queue。
- report body 有固定最大值。
- 移除 CSI 后内部 RAM 最大连续块不得下降到比修改前更差且无解释。
- build 无新增 warning；无法消除的旧 warning必须记录为 baseline。

---

## 19. 回滚策略

本项目的主要回滚点是原工程备份，而不是在新工程中保留 CSI 双运行代码。

回滚方式：

```text
停止使用 /Users/zhiqin/ESP 雷达 的固件
恢复使用用户已保存的原 CSI 项目/固件
```

新工程中：

- 不保留 CSI runtime 开关。
- 不保留 CSI trigger 兼容任务。
- 不保留 radar-as-CSI adapter。
- 可以保留迁移报告和删除清单，但不得重新编译旧 CSI 源码。

建议 Codex 在修改前创建明确的 Git commit 或 patch checkpoint，但不得替用户提交到远程仓库，除非用户明确要求。

---

## 20. 最终交付物

Codex 完成任务时必须提交以下结果：

1. C51 雷达实现。
2. C52 同构雷达实现。
3. S3 本地雷达实现。
4. C5 -> S3 `/local/v1/radar/state`。
5. S3 三源 radar registry。
6. CSI active 代码和配置删除。
7. Parser/state/codec/ingest 测试。
8. 三固件 build 结果。
9. C51/C52 parity 报告。
10. CSI 负向扫描报告。
11. 资源变化报告。
12. 修改文件清单。
13. 删除文件清单。
14. 未完成或 blocked 项清单。
15. 用户真机测试步骤。

建议生成文档：

```text
docs/radar-migration-baseline.md
docs/radar-migration-execution-log.md
docs/radar-migration-hardware-test-checklist.md
```

---

## 21. Codex 最终执行指令

```text
在 /Users/zhiqin/ESP 雷达 中严格按《ESP 雷达：HLK-LD2450 替换 WiFi CSI 最终实施计划》执行。

只允许修改 ESPC51、ESPC52、ESPS3；禁止修改原 ESP-111、ESP-server、Dashboard、数据库、managed_components 和雷达内部固件。先读取 live source 并建立基线，再完整移除 C5 WiFi CSI 采集/算法/上报和 S3 CSI trigger/ingest/fusion/upload，不能只关闭宏，也不能保留 active CSI 兼容路径。

新增三颗 HLK-LD2450 的 UART 驱动、固定长度帧解析、特殊符号解码、UNKNOWN/MOTION/HOLD/VACANT_INFERRED 状态机。C51、C52分别处理本房间雷达并通过 POST /local/v1/radar/state 向 S3 上报 latest-only 状态；S3解析自身雷达并维护 C51、C52、S3_LOCAL 三个完全独立的 room source，不做跨房间融合。

雷达不得伪装为 CSI，不得调用 /kernel/csi_event，本阶段不新增 Server 或 Dashboard 接口。必须保持 C5 只访问 S3 /local/v1、保持 BME、WiFi、register、heartbeat、status、command、voice、WakeNet、Mic、Speaker、LCD/LVGL 原合同和功能。

不猜测 UART GPIO；使用独立板级配置。完成 parser/state/protocol/ingest 测试、三固件 build、C51/C52 parity、CSI 负向扫描、ESP-server 未修改检查和资源变化报告。禁止 flash、monitor、erase-flash、启动 Server 或声称硬件验收通过。硬件项由用户测试，未具备引脚或真机条件时必须明确标记 blocked，不得虚报。
```

---

## 22. 完成定义

只有同时满足以下条件，Codex 才能将软件实施标记为完成：

```text
三套固件静态/build 验证完成
+ C51/C52 radar 核心 parity 完成
+ S3 三源 registry 完成
+ /local/v1/radar/state 合同完成
+ active CSI 链路全部移除
+ ESP-server 和前端零修改
+ 既有主链路静态回归通过
+ 所有未验证硬件项明确列为待用户验收
```

软件完成不等于硬件完成。只有用户完成 HW-P1 至 HW-P4 后，项目才能标记为“雷达替换真机验收完成”。

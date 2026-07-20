# LD2450 官方解析与 ESP 雷达算法优化参考

> 范围：本文是资料学习和静态代码审查结论，不修改雷达代码、配置或工程结构。  
> 证据标签：**[A 官方明确]** 来自海凌科资料或随附示例；**[B 源码确认]** 来自本工程当前 ESPS3 源码；**[C 工程建议]** 是面向 ESP32-S3 的轻量实现建议；**[D 未证实推断]** 不应视作官方 App 的实现事实。

## 1. 使用资料与阅读边界

| 资料 | 用途 | 证据等级 |
| --- | --- | --- |
| `ESP 资料/2450/HLK LD2450 1T2R运动目标检测追踪模组说明书 V1.02 .pdf` | 模组定位、1T2R、多目标能力、安装和工作范围 | A |
| `ESP 资料/2450/LD2450 串口通信协议 V1.03.pdf` | UART、上报格式、配置命令、单/多目标和区域过滤命令 | A |
| `ESP 资料/2450/HLK-LD2450使用教程V1.1.pdf` | 工具使用、坐标显示、模式与区域过滤操作 | A |
| `ESP 资料/2450/HLK-LD2450上报解析参考代码/HLK-LD2450--demo/Hardware/Serial.c` | 官方随附 STM32 上报帧状态机和字段解码 | A |
| `ESP 资料/2450/HLK-LD2450上报解析参考代码/HLK-LD2450--demo/User/main.c` | 官方随附距离、角度计算和无效槽判断 | A |
| `ESP 资料/2450/HLK-2450_TOOL(v1.4.2.0_20230515_1)/HLK-2450_TOOL.exe`、`HLKRadarTool APP下载链接.pdf` | 官方 Windows/App 工具存在与联调入口 | A（仅工具存在） |
| `ESP 资料/HLK-2450-mac/` | 本地 macOS 工具源码；可用于核对串口解析与界面行为，但不是已验证的海凌科官方 App 源码 | B（资料库源码） |

Windows 工具是二进制文件，官方移动 App 只有下载入口，本次不能从其内部代码证明其采用 EMA、Kalman 或任何私有跟踪器。下面关于“官方 App 稳定”的算法部分因此将官方可见配置能力与合理工程推断严格分开。

## 2. LD2450 协议与目标语义

### 2.1 工作原理与能力

**[A 官方明确]** LD2450 是 24 GHz 1T2R 毫米波运动目标检测与跟踪模组，经 UART 持续输出最多 3 个目标的二维位置、速度和距离分辨率。模组内部已经完成雷达回波检测与目标输出；ESP32 的职责是可靠收帧、定义安装坐标、跨帧稳定显示，并将目标级结果推导为“有人/运动/区域”等业务语义。它不是摄像头，也没有官方协议中的人体类别、骨架或置信度字段。

**重要边界：**“检测到目标”不等于“确认是人体”，也不等于“正在运动”；后两者是上位机的时序和业务判定。

### 2.2 UART 与实时上报帧

**[A 官方明确]** 默认串口为 `256000, 8N1`。实时上报帧是固定 **30 字节**，没有 length、命令字或 CRC 字段：

```text
Byte  0..3   AA FF 03 00                上报帧头
Byte  4..11  target slot 1 (8 bytes)    目标槽 1
Byte 12..19  target slot 2 (8 bytes)    目标槽 2
Byte 20..27  target slot 3 (8 bytes)    目标槽 3
Byte 28..29  55 CC                      上报帧尾
```

每个槽的 8 字节均为小端序：

| 槽内偏移 | 长度 | 字段 | 单位与含义 |
| --- | ---: | --- | --- |
| `0..1` | 2 | `X` | mm，方向编码的有符号横向坐标 |
| `2..3` | 2 | `Y` | mm，方向编码的有符号纵向坐标 |
| `4..5` | 2 | `Speed` | cm/s，方向编码的有符号速度 |
| `6..7` | 2 | `Distance resolution` | mm，官方示例以全零槽识别无目标；它不是由 ESP 计算的欧氏距离 |

官方示例的方向解码是：若高字节 bit7 为 1，则 `value = raw - 32768`；否则 `value = -raw`。对 Y 使用 `raw - 32768`。因此应以实际原始帧和工具画面标定轴向，不能凭名称假定 X/Y 正负方向。

**Target ID：** 协议未提供持久、跨帧的目标 ID。官方示例把三个位置称为“目标一/二/三”，本质是 `slot 1..3`。槽位排序是否稳定、两人交叉时是否重排，协议没有保证。UI 可以显示槽号，但业务“人物 ID”必须由 ESP 自己关联。

**派生量：** 官方示例计算 `distance_mm = sqrt(X^2 + Y^2)`，并使用 `angle_deg = -atan2(X, Y) * 180/pi`。角度零向、正方向必须与绘图坐标系一起定义；不要混用 `atan2(y, x)` 的数学坐标约定。

### 2.3 配置/应答帧与校验边界

配置命令不是 30 字节上报帧，格式为：

```text
FD FC FB FA | length(u16 LE) | command(u16 LE) | data(length-2) | 04 03 02 01
```

应答使用相同包头/包尾，负载中包含命令及结果码。**[A 官方明确]** 命令帧有 length 和 command；**实时上报帧没有这些字段，也没有文档所示 CRC/checksum**。因此本工程的完整性边界应是固定长度、帧头、帧尾和合理字段范围；不能虚构 CRC 校验。

协议还提供单目标/多目标模式和最多 3 个矩形的区域过滤（关闭、仅区域内、排除区域内）。区域过滤优先应在模组端完成，用于屏蔽门外、风扇等固定干扰；但仍需上位机滤波，因为边界、丢失和 ID 关联属于跨帧问题。

## 3. 官方工具/App“稳定显示”的可证实部分

### 3.1 已证实能力

- **[A]** 模组支持单目标/多目标模式；多目标模式最多 3 个目标。
- **[A]** 模组支持最多 3 个矩形区域的 include/exclude 过滤，配置可持久化。
- **[A]** 官方 STM32 示例只在完整帧尾通过后置位新帧标志；全零目标槽不显示；距离和角度是由当前槽坐标直接计算。
- **[B]** 资料库中的 macOS 工具同样只发布最近完整帧，使用槽位 `1..3` 作为显示 ID，并保存有限历史轨迹供画布绘制；其协议源码没有 EMA、滑动平均、Kalman、最近邻匹配或目标生命周期实现。

### 3.2 不能证实的结论

不能仅凭“官方 App 看起来稳定”断言它使用滑动平均、EMA、Kalman 或私有目标关联。更可信的解释优先级是：

1. **[A]** 模组固件本身已经进行目标检测/跟踪，且其单/多目标和区域过滤配置可能不同；
2. **[A]** App 只显示完整帧、丢弃全零槽，并以当前帧替换界面；
3. **[D]** 二进制工具可能还有显示节流、丢失保持或平滑，但本次没有源码证据；
4. **[D]** 即使工具有平滑，也不能据此推导其参数或把槽位当永久 ID。

因此后续优化应该用可回放的原始 UART 帧和同一安装环境对齐，而不是“模仿 App 的猜测算法”。

## 4. 当前 ESP 雷达链路审查

```text
LD2450 UART1 (256000, 8N1)
  -> radar_service / radar_rx_task
  -> ld2450_parser (固定 30 字节、逐字节重同步)
  -> radar_frame_t (3 个 raw target slots)
  -> radar_local_adapter (100 ms)
  -> coordinate transform + 距离/房间准入
  -> target tracker (最近邻关联 + EMA)
  -> spatial state (sensor / occupancy / motion)
  -> registry / diagnostics
```

### 4.1 Parser 与原始目标

**[B]** `ESPS3/components/radar_ld2450/ld2450_parser.c` 缓冲 30 字节，识别 `AA FF 03 00`、验证 `55 CC`，对坏头/坏尾逐字节滑动重同步；支持半帧和粘包。每帧遍历三个槽，以“8 字节全零”为无目标；解出 X/Y/Speed/Resolution，另计算整数欧氏距离。它不生成跨帧 Target ID。

`radar_service` 由一个 RX task 独占正常数据读取，完整帧深拷贝为 latest frame；恢复后须连续 3 帧有效才进入 `VALID`。当前 parser 对长期不增长的半帧在 2 秒后才强制清空，不会把一次 100 ms UART read timeout 直接当成坏半帧。

### 4.2 坐标、过滤、跟踪与人体状态

**[B]** `radar_coordinate_transform` 的顺序是镜像、旋转、平移，随后按最大距离与房间矩形准入。默认值是 `flip_x=0`、`flip_y=0`、旋转 0、偏移 0、最大距离 6000 mm，保留 LD2450 原始轴，符合“未经真机标定不擅自翻转”的原则。

**[B]** `radar_spatial_state` 只把通过空间准入的目标放入 `accepted_targets`；若“仅一个接受目标且仅一个可见 track”发生大于 1500 mm 的突跳，会丢弃本帧该目标。当前 `raw_target_count` 与 `accepted_target_count` 是刻意分离的：前者用于诊断，后者用于空间/业务语义。

**[B]** `radar_target_tracker` 维护至多 3 条 track，以当前过滤位置与新目标的欧氏距离做全局贪心最近邻匹配，门限 900 mm；命中时：

```text
filtered = filtered + (measurement - filtered) / 2
```

即每轴 `EMA alpha=0.5`。新目标立即建轨（初始 confidence 40）；未命中每帧扣 20，超过 1500 ms 释放。它保留 `raw_x/y` 和 `filtered_x/y`，但没有速度预测、全局最优分配或轨迹确认期。

**[B]** `radar_spatial_state` 将传感器健康、占用和运动拆开：速度绝对值至少 15 cm/s 或单帧位移至少 120 mm 判为运动候选，连续 3 帧进入 MOVING、连续 8 帧退出；可见轨迹为 PRESENT，短暂消失但轨迹未过期为 HOLD，完全消失为 VACANT_INFERRED。这是比“任一目标=移动”更正确的业务层模型。

## 5. 与官方资料/工具可能不一致的根因

### 5.1 目标数量不一致

| 可能原因 | 当前证据 | 优先验证 |
| --- | --- | --- |
| 工具与设备的单/多目标、区域过滤配置不同 | A：协议支持这些持久配置；B：工程未从设备读取并对齐当前模式 | 查询 `0x0091` 和区域过滤，记录二者设置 |
| 两边输入帧不同或串口有丢失/错位 | B：parser 有 `bad_header/bad_tail/skipped/resync` 诊断 | 同时抓原始 UART hex，按 30 字节帧离线比对 |
| ESP 计数的是空间接受目标，工具显示的是原始非零槽 | B：ESP 有 raw 与 accepted 两种计数；官方示例只跳过全零槽 | 对同一帧同时记录 `raw/accepted/visible/active` 四个数量 |
| 目标槽不是持久 ID | A：协议无 ID；B：tracker 自建 track ID | 人员交叉时记录 slot -> track 的映射 |
| 丢帧/恢复期导致短暂 STALE | B：重连需连续 3 帧有效 | 对照 valid fps、recovery state 和时间戳 |

当前源码中，写入 registry 的数组和 `current_target_count` 都来自 `accepted_targets`，就这一适配边界而言没有“过滤后 count 配原始数组”的当前不一致；但外部比对必须先明确对方显示的是 raw slot、accepted target 还是 track。

### 5.2 位置与官方工具不同

- **坐标约定：** X/Y 的方向编码、`atan2(X,Y)` 与画布 Y 轴方向必须全链路一致。当前默认不翻转是保守正确的；只有用实物点位验证后才能设置镜像、旋转和 origin offset。
- **比较层级不同：** 官方示例/本地 macOS 工具的显示是原始当前槽；ESP 对外可看到 raw、accepted、filtered track 三层。拿其中任一层与另一个工具的点直接比较，都会制造“位置不一致”。
- **空间准入不同：** ESP 默认舍弃距离超过 6 m 或房间边界外的点；工具可能仍画出原始槽。
- **安装误差：** 模组偏航、安装高度、中心位置、天线罩/金属反射均可造成系统性偏差，不能用算法把未标定的安装误差“滤掉”。

### 5.3 静止人体漂移

原始坐标噪声、槽位重排和多径反射都会造成点漂移。当前 EMA 能削弱高频抖动，但 `alpha=0.5` 仍偏灵敏；并且位移阈值使用“上一 filtered 点到当前 raw measurement”的距离，异常测量仍可能触发运动。当前单目标突跳保护只覆盖 `1 accepted / 1 visible`，多人时不覆盖。

### 5.4 多人不稳定

当前最近邻有合理的轻量基础，但存在以下天然限制：

- 不使用上一速度或帧间 `dt` 预测，快速行走/交叉时 900 mm 固定门限可能误配或断轨；
- 贪心匹配不是全局最优，两个目标接近时先找到的最小 pair 会影响后续配对；
- 新检测立即建轨，没有两帧确认，短暂杂波会造成计数跳变；
- 只在单目标场景做突跳拒绝，多目标分裂/合并仍会改变轨迹数；
- 槽号并非持久 ID，不能把颜色、轨迹或业务绑定到 slot。

## 6. 推荐 ESP32-S3 处理架构

```text
LD2450 input
  -> Frame Parser
  -> Raw Target Filter
  -> Coordinate Calibration
  -> Target Association
  -> Target Tracker
  -> Spatial State
  -> Occupancy / Motion
```

| 模块 | 输入/输出 | 职责与约束 |
| --- | --- | --- |
| Frame Parser | UART bytes -> `raw_frame` | 只处理帧边界、固定长度、尾部、重同步和统计；不做人体语义。 |
| Raw Target Filter | raw slots -> candidate list | 丢弃全零、保留原始值、检查物理范围/重复帧；输出拒绝原因和 raw count。 |
| Coordinate Calibration | candidate -> calibrated point | 唯一坐标契约：镜像、旋转、平移、安装标定；原始值不得覆盖。 |
| Target Association | tracks + candidates -> assignments | 使用预测位置、距离门限、速度/分区约束，输出未匹配/新生/丢失。 |
| Target Tracker | assignments -> stable tracks | 分别保存 measurement、filtered state、velocity、age、miss 和 confidence；track ID 只在此生成。 |
| Spatial State | stable tracks -> zone/presence | 房间/ignore/active 区域、边界滞回、位置与区域停留。 |
| Occupancy/Motion | spatial tracks -> business state | 基于确认帧、丢失保持、速度和位移滞回输出；不得把 UART 离线误判为空闲。 |

要求所有调试输出同时带上 `frame_seq`、slot、track_id、raw/accepted/visible/active count、拒绝原因和 timestamp。这样才能区分“雷达没报”“空间过滤掉”“关联失败”“业务层保持”四类问题。

## 7. ESP32-S3 轻量算法建议

### 7.1 推荐组合

| 场景 | 推荐 | 初始参数（需真机标定） |
| --- | --- | --- |
| 位置抖动 | 自适应 EMA（一阶低通） | 静止 `alpha=0.20~0.35`，移动 `alpha=0.45~0.65`；按 `dt` 或速度切换 |
| 单帧离群点 | 速度/距离门限 + 3 帧中值 | 若相对预测点 > `max(0.8 m, v*dt + 0.4 m)`，暂不更新 track；连续 2 帧再接受 |
| 多目标关联 | 预测后最近邻 + gate | 以 `pred = pos + velocity*dt`，gate 初始 `0.6~0.9 m`；同区域优先，超过 gate 不匹配 |
| 新生目标 | 确认期 | 连续 2 帧命中后才对 occupancy/UI 输出；候选期可保留 300 ms |
| 目标丢失 | 软丢失 + 超时释放 | 可见丢失 300~500 ms 进入 HOLD；`1.5~2.0 s` 释放；保持期不增长位置 |
| 静止人体 | 慢 EMA + 静止确认 | 3~5 帧连续可见；不要用 `speed==0` 作为唯一条件 |
| 运动 | 速度与位移双条件 + 滞回 | 入动 2~3 帧，出动 5~8 帧；速度阈值先从 `15 cm/s`，位移从 `100~150 mm` 复测 |

这些都是 O(3^2) 的定长运算，3 目标/3 track 下无需动态内存、深度学习、云端或通用 Kalman 库。现有实现已经具备最近邻、EMA、hold 和运动滞回，优先按下列顺序增量强化，而不是重写链路。

### 7.2 建议的后续优化顺序

1. **先做数据对齐，不先调参。** 同一模组、同一配置、同一原始 UART 流，同时得到官方工具画面、raw slot、accepted target、track 四层记录。
2. **冻结坐标标定。** 用多个已知点确认 X/Y 符号、前向、角度零向、安装偏航与原点；在获得实机证据前维持当前原始坐标默认值。
3. **冻结“计数”语义。** UI 明确显示 raw / accepted / visible track / active track；业务 occupancy 以确认 track 为准。
4. **将单目标突跳规则扩展为预测残差规则。** 适用于多人，但必须防止同时拒绝真正的快速移动。
5. **增加 track 确认期与速度预测。** 先实现常速度模型的整数/定点预测，再决定是否需要简化 2D alpha-beta/Kalman；一般不需要完整矩阵 Kalman。
6. **最后再调 EMA、gate、hold。** 按静止、单人走动、两人交叉、进出门、门外干扰五类录制数据集回放评估，记录 ID switch、false track、漏检、延迟和位置 RMSE。

## 8. 结论与验证清单

- LD2450 实时上报是固定 30 字节、3 个无持久 ID 的目标槽；上位机不应把槽号当人体 ID。
- 当前 ESPS3 已实现可靠重组、坐标变换、空间过滤、最近邻关联、EMA、丢失保持和运动滞回；主要风险在于跨帧身份/多目标交叉、突跳规则覆盖范围，以及 raw/accepted/track 三种计数的比较口径。
- 官方资料明确提供多目标和区域过滤，但没有公开 App 滤波/跟踪算法；任何“官方使用 Kalman/EMA”的说法都必须标为推断。
- 后续优化必须以原始帧回放和实物坐标标定为前置验证，不能只凭 App 的视觉稳定性调整坐标或参数。

### 本轮未验证项

- 未 flash、monitor、连接实机或运行官方 Windows/App 工具；没有硬件验收结论。
- 未反编译官方二进制工具；无法确认其内部滤波、丢失保持或 ID 关联。
- 未修改 `ESPS3`、其他代码树、配置或工程结构；仅生成本文档。

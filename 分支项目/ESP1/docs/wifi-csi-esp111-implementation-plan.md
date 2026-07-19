# ESP-111 WiFi CSI 实施方案

更新日期：2026-06-11

## 1. 范围与结论

本文档只整理 ESP-111 后续 WiFi CSI 的实施方案，不代表当前已经接入真实 CSI 采集或算法。当前 CSI 是预留/独立模块：默认不接入 C5 app_main/orchestrator/WiFi callback，不让 S3 定时 ping C5，不把 CSI result 接入 `sensor_aggregator` 或 Server 上报。

当前项目结构按四个角色划分：

| 目录 | 角色 | CSI 相关定位 |
| --- | --- | --- |
| `ESPC51` | ESP32-C5 房间节点 1 | 未来负责本机 CSI 采集、轻量特征计算和结果上报。 |
| `ESPC52` | ESP32-C5 房间节点 2 | 与 `ESPC51` 使用同一套 CSI 模块和接口，只通过设备身份区分。 |
| `ESPS3` | ESP32-S3 本地网关 | 当前只保留 `/local/v1/csi/result` 和 trigger 接口预留；未来经确认后才定时触发 C5、转换为 Server v1 JSON 并上传。 |
| `ESP-server` | 后端服务器与 Dashboard 数据源 | 未来接收 S3 标准化 CSI 结果，保存/聚合并向 Dashboard 暴露轻量 occupancy 字段。 |

第一阶段目标只做：

- 有人/无人：`occupancy.state = "occupied" | "vacant" | "unknown"`。
- 运动强度：`motion_score`，范围建议为 `0.0-1.0`。
- 辅助诊断字段：`variance`、`rssi`、`sample_count`、`updated_at`。

第一阶段明确不做：

- 动作识别。
- 人数识别。
- 呼吸率、心率、步态等生理或身份推断。
- 深度学习模型。
- 原始 CSI 长期上传、Server 端 raw CSI 训练或 Dashboard raw CSI 可视化。

## 2. 硬边界

后续真正开始实现 CSI 时，必须先走“独立功能文件与接口定义”路线，不能直接接入主运行链路。

阶段 A 的硬边界：

- 可修改范围仅限 `ESPC51`/`ESPC52` 的 CSI 独立模块、测试入口和本文档/相关文档。
- S3、Server、Dashboard 在阶段 A 只写接口草案和字段草案，不修改运行代码。
- 只新增独立 CSI 功能文件、接口定义、数据结构、离线单元测试或日志验证入口。
- 不接入 `app_orchestrator`。
- 不在 C5 启动流程中启动 CSI 任务。
- 不修改 Wi-Fi 主流程、voice、BME690、server_comm 的启动逻辑或默认运行路径。
- 不让 S3 定时触发真实 C5 CSI。
- 不让 S3 上传真实 CSI 到 Server。
- 不让 Dashboard 展示真实 CSI 结果。
- 不影响 C5/S3/Server 现有 register、heartbeat、BME690、voice、command 主链路。
- 不上传原始 CSI。
- 不引入重模型或大依赖。

阶段 B 的接入前提：

- 先完成 S3 + 单 C5 基础联调。
- 再完成双 C5 基础联调。
- 最后完成 CSI 独立测试。
- 其他功能测试稳定。
- 阶段 A 的接口、日志、单元测试、内存占用和失败策略通过验收。
- 由用户明确确认可以接入完整链路后，才接入 C5 启动流程、S3 触发流程、Server 上传和 Dashboard 展示。

## 3. C5 资源限制与第一阶段策略

ESP32-C5 房间节点同时承担 Wi-Fi、BME690、Mic/VAD、wake、speaker、command、runtime 等职责，CSI 不能挤占主链路资源。

第一阶段策略：

- 只在 C5 本地做轻量统计，不上传 raw CSI。
- CSI callback 内只做最小拷贝或计数，不做复杂计算、不分配大内存、不发 HTTP。
- 特征计算放在独立处理函数中，使用固定大小环形窗口。
- 窗口长度、子载波数量、采样周期必须可配置，并有上限。
- `motion_score`、`variance` 和 `occupancy.state` 是唯一业务输出。
- 网络上报只发送小 JSON，不发送大数组、复数矩阵、相位序列或 CSI 原始帧。
- 阶段 A 不创建长驻任务；如需验证，用显式测试入口或离线样本驱动函数。

建议初始资源预算：

| 项 | 建议上限 | 说明 |
| --- | ---: | --- |
| 子载波选择数量 | 8-16 个 | 只选稳定且受人体扰动明显的子载波。 |
| 滑动窗口样本 | 32-128 帧 | 先保证内存和实时性，再调精度。 |
| 输出频率 | 1-5 秒一次 | C5 只报摘要结果。 |
| 本地 JSON body | 小于 512 bytes | 远低于现有 S3 local JSON 上限。 |
| CPU 策略 | 低优先级、可暂停 | 语音、命令、BME 主链路优先。 |

## 4. 阶段 A：独立文件、接口、结构与验证

阶段 A 只建立 CSI 能力的“可测试孤岛”，不接运行链路。

### 4.1 C5 模块建议

建议在 `ESPC51` 和 `ESPC52` 的 C5 侧保持同名模块结构，具体文件名后续实现时再确认。推荐职责拆分：

| 模块 | 作用 | 输入 | 输出 |
| --- | --- | --- | --- |
| `csi_capture` | 封装 CSI 配置、callback、采样开关。阶段 A 只定义接口和可注入样本入口。 | S3 ping 触发后的 CSI 帧或离线测试帧。 | 归一化后的单帧 amplitude 数据。 |
| `csi_feature` | 振幅提取、子载波选择、Hampel/中值滤波、滑动窗口统计。 | 单帧 amplitude、RSSI、时间戳。 | `variance`、CV、有效样本数、质量标志。 |
| `csi_presence` | 阈值判断和状态机。 | 窗口统计值、RSSI、样本质量。 | `occupancy.state`、`motion_score`、稳定/待确认标志。 |
| `csi_result_codec` | 构造 C5 -> S3 轻量结果结构。阶段 A 只生成字符串或结构体，不发 HTTP。 | presence 结果。 | 可日志打印的 local result。 |

阶段 A 不修改现有 `csi_placeholder` 行为。现有 placeholder 可以继续表示“接口预留、真实 CSI 未接入”。

### 4.2 C5 数据结构草案

阶段 A 建议先定义内部结构，不要求马上改共享协议头：

| 结构 | 字段 | 含义 |
| --- | --- | --- |
| `csi_frame_sample` | `timestamp_ms` | C5 本地时间。 |
|  | `rssi` | 当前 CSI 帧 RSSI，单位 dBm。 |
|  | `amplitude[]` | 已筛选子载波的振幅。 |
|  | `subcarrier_count` | 本帧有效子载波数量。 |
| `csi_window_stats` | `variance` | 窗口振幅方差或归一化方差。 |
|  | `cv` | 变异系数，用于跨环境归一化。 |
|  | `sample_count` | 窗口有效样本数。 |
|  | `quality` | 样本质量，后续可映射为 good/weak/invalid。 |
| `csi_presence_result` | `state` | `occupied/vacant/unknown`。 |
|  | `motion_score` | `0.0-1.0`，越高表示近期扰动越强。 |
|  | `variance` | 参与判断的统计值。 |
|  | `rssi` | 最近或窗口平均 RSSI。 |
|  | `sample_count` | 本次结果使用的样本数。 |
|  | `updated_at_ms` | C5 本地更新时间。 |

### 4.3 C5 处理流水线

阶段 B 的真实运行链路目标如下，但阶段 A 只实现可测试函数和日志验证：

```text
S3 ping
  -> C5 CSI callback
  -> 振幅提取
  -> 子载波选择
  -> Hampel/中值滤波
  -> 滑动窗口方差/CV
  -> 阈值判断
  -> 上报 S3
```

各步骤说明：

| 步骤 | 作用 | 第一阶段要求 |
| --- | --- | --- |
| S3 ping | 让 C5 在可控 Wi-Fi 交互中产生 CSI 样本。 | 阶段 A 只保留触发接口定义，不实际定时 ping。 |
| C5 CSI callback | 接收 CSI 帧。 | callback 内只做轻量转存，不做复杂计算。 |
| 振幅提取 | 对 I/Q 计算 amplitude。 | 只保留振幅，不保留 raw I/Q。 |
| 子载波选择 | 过滤空值、噪声大、边缘不稳定子载波。 | 先固定白名单或按稳定性选择少量子载波。 |
| Hampel/中值滤波 | 去除尖峰和偶发异常。 | 使用小窗口，避免大内存。 |
| 滑动窗口方差/CV | 计算近期扰动强度。 | 输出 `variance` 与 CV，作为 motion_score 基础。 |
| 阈值判断 | 把统计值转换为有人/无人。 | 只做静态阈值加滞回，不做分类模型。 |
| 上报 S3 | 发送轻量结果。 | 阶段 A 不发 HTTP；阶段 B 才接 `POST /local/v1/csi/result`。 |

### 4.4 阈值与状态机

第一阶段使用可解释阈值，不使用训练模型。

建议状态：

| 状态 | 含义 | 进入条件 |
| --- | --- | --- |
| `unknown` | 样本不足或质量不足。 | `sample_count` 低于窗口下限，或 RSSI/子载波质量过差。 |
| `vacant` | 近期扰动低，判断无人。 | `motion_score` 持续低于无人阈值。 |
| `occupied` | 近期扰动高，判断有人或有活动。 | `motion_score` 持续高于有人阈值。 |

建议加入滞回和稳定计数：

- `occupied_threshold` 高于 `vacant_threshold`，减少来回抖动。
- 连续 N 个窗口满足条件才切换状态。
- Wi-Fi RSSI 过低或样本质量不足时输出 `unknown`，不强行判断。

### 4.5 阶段 A 测试入口命名

阶段 A 的验证入口必须使用明确的独立测试命名，避免被误认为运行链路入口：

| 测试入口 | 验证范围 | 限制 |
| --- | --- | --- |
| `csi_feature_test` | 离线样本驱动振幅提取、子载波选择、滤波、窗口统计。 | 不启动 CSI callback，不接 Wi-Fi 主流程。 |
| `csi_presence_test` | 验证 `unknown/vacant/occupied` 阈值、滞回和连续窗口稳定计数。 | 不依赖 S3 触发，不上报网络。 |
| `csi_result_codec_test` | 验证轻量结果结构或 JSON 摘要的字段、单位、空值规则。 | 只生成结构体/字符串和日志，不发 HTTP。 |

## 5. 阶段 A 验收标准

阶段 A 完成后，只能证明“独立 CSI 算法文件可编译、接口清楚、离线样本或日志可验证”，不能声称 CSI 已接入运行链路。

验收项：

| 模块 | 验收标准 |
| --- | --- |
| C5 接口 | 能用离线样本或测试入口驱动振幅、滤波、窗口统计、阈值判断函数。 |
| C5 数据结构 | `occupancy.state`、`motion_score`、`variance`、`rssi`、`sample_count`、`updated_at` 字段语义明确。 |
| C5 资源 | 不上传 raw CSI，不创建默认运行任务，不引入重模型。 |
| S3 接口草案 | 明确未来如何触发 C5、如何接收 `/local/v1/csi/result`、如何转 Server v1 JSON。 |
| Server/Dashboard 草案 | 字段名、空值规则、兼容策略写清楚，但不改后端代码。 |
| 日志验证 | 可通过日志看到每个处理阶段的输入输出摘要，不打印 raw CSI 大数组。 |
| 主链路隔离 | register、heartbeat、BME690、voice、command 代码路径不受影响。 |

阶段 A 构建通过后，最终报告必须逐项列出“未接入项”，不能只写笼统结论。

阶段 A 完成报告必须写明：

- 没有接入 `app_orchestrator`。
- 没有启动 CSI 任务。
- 没有让 S3 真实触发 C5 CSI。
- 没有上传 CSI 结果到 Server。
- 没有修改 Dashboard 展示。
- 没有上传原始 CSI。
- 没有修改 Wi-Fi 主流程、voice、BME690、server_comm 的启动逻辑。

## 6. 阶段 B：确认后接入完整链路

阶段 B 只有在用户确认后才能开始。阶段 B 的目标是把阶段 A 的独立能力接入 C5 -> S3 -> ESP-server -> Dashboard。

### 6.1 C5 接入职责

C5 只负责本地轻量处理和结果上报：

| 职责 | 输入 | 输出 |
| --- | --- | --- |
| 配置 CSI | Wi-Fi 已连接 S3 SoftAP、CSI 参数。 | CSI callback 可接收帧。 |
| 接收 S3 ping 产生的 CSI 帧 | CSI callback 数据。 | 少量振幅特征进入窗口。 |
| 计算 occupancy | 滑动窗口统计。 | `state/motion_score/variance/rssi/sample_count/updated_at`。 |
| 上报 S3 | presence result。 | `POST /local/v1/csi/result` 轻量 JSON。 |

C5 不负责：

- 构造 Server 完整 envelope。
- 构造 Dashboard JSON。
- 保存 raw CSI。
- 训练或运行 SVM/RF/CNN/LSTM/Transformer 等模型。
- 根据 CSI 直接控制设备。

### 6.2 C5 -> S3 本地结果格式

推荐沿用当前本地轻量协议边界，结果示例：

```json
{
  "p": 2,
  "id": 1,
  "t": 5,
  "u": 123456,
  "q": 88,
  "v": [1, 0.73, 0.0182, -58, 96, 1781100000000]
}
```

`v[]` 建议顺序：

| 位置 | 字段 | 类型 | 含义 |
| ---: | --- | --- | --- |
| `v[0]` | `occupancy_state` | number | `0=unknown`、`1=occupied`、`2=vacant`。 |
| `v[1]` | `motion_score` | number | `0.0-1.0`。 |
| `v[2]` | `variance` | number | 当前窗口方差或归一化方差。 |
| `v[3]` | `rssi` | number | dBm。 |
| `v[4]` | `sample_count` | number | 本次结果使用的 CSI 帧数。 |
| `v[5]` | `updated_at` | number | ms 时间戳；没有绝对时间时可由 S3 补。 |

如果继续兼容字符串类型，也可以在迁移期接受：

```json
{"id":1,"t":"csi","u":123456,"v":[1,0.73,0.0182,-58,96,1781100000000]}
```

### 6.3 S3 职责

S3 是未来 CSI 链路的编排与转换层；当前默认不开启 trigger，也不把 result 写入 `sensor_aggregator` 或 Server：

| 职责 | 输入 | 输出 |
| --- | --- | --- |
| 定时触发 C5 | C5 registry、在线状态、触发周期配置。 | 默认关闭；未来显式打开后才对指定 C5 发起 ping 或轻量触发请求。 |
| 接收 C5 结果 | `POST /local/v1/csi/result`。 | 默认只本地 ACK/记录预留；未来显式打开 result ingest 后才进入 S3 状态表。 |
| 转换为 Server v1 JSON | C5 短字段、registry 中的 `device_id/room/name`。 | 默认不执行；未来输出 `payload_type="csi.motion"` 或 dashboard snapshot 中的 occupancy 字段。 |
| 上传 ESP-server | Server base URL、标准 JSON。 | 默认不上传 CSI；未来 Server 成功/失败状态进入 offline policy。 |
| 降级处理 | Server 不可达、C5 结果缺失。 | 本地仍可 ACK C5；Dashboard 状态保持旧值或 `unknown`。 |

S3 不负责：

- 解析 raw CSI。
- 运行动作识别或人数识别。
- 在本地训练模型。
- 把 CSI 失败当成 C5 整机离线。

### 6.4 S3 -> Server v1 JSON

推荐把 CSI 结果作为轻量行为摘要，而不是 raw 数据。

独立 ingest 草案：

```json
{
  "schema_version": 2,
  "payload_type": "csi.motion",
  "source": "s3_gateway",
  "device_id": "sensair_shuttle_01",
  "gateway_id": "sensair_s3_gateway_01",
  "room_id": "unassigned",
  "timestamp_ms": 1781100000000,
  "uptime_ms": 123456,
  "payload": {
    "occupancy": {
      "state": "occupied"
    },
    "motion_score": 0.73,
    "variance": 0.0182,
    "rssi": -58,
    "sample_count": 96,
    "updated_at": 1781100000000
  }
}
```

Dashboard snapshot 扩展草案：

```json
{
  "device_id": "sensair_shuttle_01",
  "local_id": 1,
  "name": "SensaiShuttle",
  "room_name": "living_room",
  "online": true,
  "wifi_rssi": -58,
  "timestamp": 1781100000000,
  "occupancy": {
    "state": "occupied",
    "motion_score": 0.73,
    "variance": 0.0182,
    "rssi": -58,
    "sample_count": 96,
    "updated_at": 1781100000000
  },
  "sensors": {
    "temperature": 29.57,
    "humidity": 48.2,
    "pressure": 1008.6,
    "gas_resistance": 35164,
    "air_quality_score": 72,
    "air_quality_level": "moderate",
    "air_quality_source": "s3_mapped"
  }
}
```

## 7. Server/Dashboard 字段设计

第一阶段 Dashboard 只需要显示或提供轻量 occupancy 数据，不展示算法细节。

建议字段：

| 字段 | 类型 | 单位 | 含义 | 空值规则 |
| --- | --- | --- | --- | --- |
| `occupancy.state` | string | 无 | `occupied`、`vacant`、`unknown`。 | 无结果、样本不足、质量不足时为 `unknown`。 |
| `motion_score` | number/null | 0-1 | 当前窗口运动强度。 | 无有效窗口时为 `null`。 |
| `variance` | number/null | 无 | 当前窗口振幅方差或归一化方差。 | 无有效窗口时为 `null`。 |
| `rssi` | number/null | dBm | 结果对应的 RSSI。 | 未上报时为 `null`。 |
| `sample_count` | number | 帧 | 本次结果使用的 CSI 样本数。 | 无结果时为 `0`。 |
| `updated_at` | number/null | ms | 结果更新时间。 | C5 未提供时由 S3 或 Server 接收时间补齐。 |

Dashboard 展示建议：

- 主状态只显示“有人 / 无人 / 未知”。
- `motion_score` 可作为条形强度或调试数值。
- `variance/rssi/sample_count/updated_at` 默认作为调试字段，不作为用户判断文案核心。
- 当 CSI 不可用时显示 `unknown` 或 unavailable，不让界面声称已经检测到人体。
- CSI 不上传不影响 BME、voice、command 和整机在线状态。

Server 验收重点：

- 接收 `csi.motion` 或 dashboard snapshot 扩展时不破坏 legacy BME ingest。
- `GET /api/dashboard/v1/overview` 中的 `devices[].occupancy` 字段可为空或 `unknown`。
- Server 重启或 S3 未上传 CSI 时，Dashboard 有明确 fallback。
- `ESP-server/docs/api.md` 在阶段 B 同步记录字段、错误码和兼容策略。

## 8. 未来增强，仅作预留

以下方法只作为未来增强方向，不进入第一阶段：

| 方法 | 未来用途 | 第一阶段状态 |
| --- | --- | --- |
| 相位校准 | 改善相位稳定性和定位能力。 | 不做。 |
| FFT/STFT/DWT | 分析频域、时频域和多尺度扰动。 | 不做。 |
| PCA | 降维、去噪、增强主成分。 | 不做。 |
| SVM/RF | 传统分类器，可用于动作类别。 | 不做。 |
| CNN/LSTM/Transformer | 深度学习动作识别、人数识别或复杂时序建模。 | 不做。 |

未来升级路径必须逐步推进：

1. 稳定有人/无人与 motion_score。
2. 增加环境自校准和阈值自适应。
3. 增加多 C5 结果融合。
4. 如确有必要，再评估轻量模型；优先在 Server 侧离线评估，不直接塞进 C5。
5. raw CSI 如需研究，另行设计采样、压缩、脱敏、分片、存储和开关策略，默认不进生产链路。

## 9. 分阶段任务清单

### 阶段 A：独立功能文件与验证

| 任务 | 模块 | 输出 | 验收标准 |
| --- | --- | --- | --- |
| A1 | C5 CSI 数据结构 | `csi_frame_sample`、`csi_window_stats`、`csi_presence_result` 草案。 | 字段语义、单位、空值规则清楚。 |
| A2 | C5 振幅与子载波选择 | 可注入样本的纯函数。 | 离线样本可输出固定子载波 amplitude。 |
| A3 | C5 Hampel/中值滤波 | 小窗口滤波函数。 | 异常尖峰被抑制，正常波动保留。 |
| A4 | C5 窗口方差/CV | 固定大小环形窗口统计。 | 输出 `variance/cv/sample_count`，内存上限明确。 |
| A5 | C5 阈值状态机 | `unknown/vacant/occupied` 判断。 | 支持滞回和连续窗口稳定计数。 |
| A6 | C5 result codec | 轻量结果结构或 JSON 草案。 | 不发 HTTP，只日志打印摘要。 |
| A7 | S3 接口草案 | trigger、result handler、Server mapping 设计。 | 不启动定时器，不上传 Server。 |
| A8 | Server/Dashboard 字段草案 | `occupancy.state/motion_score/variance/rssi/sample_count/updated_at`。 | 与现有 Dashboard snapshot 兼容。 |
| A9 | 验证 | `csi_feature_test`、`csi_presence_test`、`csi_result_codec_test` 或等价离线日志验证。 | 不影响现有主链路，不修改运行启动路径。 |

### 阶段 B：确认后接入链路

| 任务 | 模块 | 输出 | 验收标准 |
| --- | --- | --- | --- |
| B1 | C5 启动流程 | 经确认后接入 CSI init/start。 | C5 能启动 CSI，但 BME/voice/command 不回退。 |
| B2 | S3 触发流程 | 定时 ping 或触发 C5。 | 可按设备在线状态控制触发。 |
| B3 | C5 上报 | `POST /local/v1/csi/result`。 | S3 能 ACK，body 小于设计上限。 |
| B4 | S3 状态表 | 保存每个 C5 最新 occupancy。 | C51/C52 结果互不覆盖。 |
| B5 | S3 -> Server | 上传 `csi.motion` 或扩展 dashboard snapshot。 | Server 不可达时本地链路降级清楚。 |
| B6 | Server 接收与聚合 | 保存或内存聚合 occupancy。 | legacy API 和 BME ingest 不破坏。 |
| B7 | Dashboard 展示 | 展示 `occupancy.state` 和 `motion_score`。 | CSI 不可用时显示 unknown/unavailable。 |
| B8 | 文档同步 | 更新 `ESP-server/docs/api.md` 和联调验收文档。 | 字段、错误码、fallback、验收命令完整。 |

## 10. 回滚与风险控制

风险：

- CSI callback 过重影响 Wi-Fi、voice 或 BME。
- raw CSI 或大 JSON 导致内存压力和本地 HTTP 延迟。
- 阈值不适配环境，导致有人/无人频繁误判。
- Dashboard 把 `unknown` 误展示成确定状态。
- CSI 不可用被误判为设备离线。

控制策略：

- 阶段 A 默认不接主链路。
- 阶段 B 所有运行开关必须可关闭。
- C5 只上传摘要，不上传 raw CSI。
- S3 和 Server 必须把 CSI 视为独立模块状态，不影响整机在线。
- Dashboard 必须保留 `unknown/unavailable`。
- 接入顺序必须先完成 S3 + 单 C5 基础联调，再完成双 C5 基础联调，之后才做 CSI 独立测试；真正接入主链路时也要先单 C5 验证，再双 C5 验证。

回滚标准：

- 关闭 C5 CSI start 后，register、heartbeat、BME690、voice、command 恢复原行为。
- 关闭 S3 CSI trigger 后，`/local/v1/csi/result` 不再产生新结果，但其他 `/local/v1` 路由正常。
- 关闭 Server CSI 接收或 Dashboard 展示后，BME 和 gateway snapshot 旧字段仍可用。

## 11. 最终验收口径

阶段 A 完成时，只能说：

- 已新增 CSI 独立功能文件或接口草案。
- 已完成离线/日志级验证。
- 未启动 CSI 任务。
- 未让 S3 真实触发 C5 CSI。
- 未上传 CSI 结果到 Server。
- 未修改 Dashboard。
- 未上传 raw CSI。
- 未修改 Wi-Fi 主流程、voice、BME690、server_comm 启动逻辑。
- 未引入重模型。

阶段 B 完成时，才可以说：

- C5 已能基于 CSI 输出有人/无人和 `motion_score`。
- S3 已能触发、接收、转换并上传轻量 CSI 结果。
- Server/Dashboard 已能读取 `occupancy.state`、`motion_score`、`variance`、`rssi`、`sample_count`、`updated_at`。
- 动作识别、人数识别、呼吸率和深度学习仍未实现，仍属于后续增强。

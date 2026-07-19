# ESP 雷达全链路专项只读审计报告

- 日期：2026-07-17；范围：`ESPC51`、`ESPC52`、`ESPS3`，及仅为边界确认读取的 ESP-server / Dashboard。
- 方法：静态源码和现有文档。未改运行代码或配置；本报告是唯一写入文件。未 build、flash、monitor、启动 ESP-server 或连接硬件。
- 标记：**A 源码确认**；**B 静态推断**；**C 需硬件验证**。

## 一、当前雷达整体架构

```text
LD2450（BLE 广播/GATT/Notify 未经真机证实）
  -> C51/C52 NimBLE Central
  -> 512 B fixed byte stream
  -> LD2450 30-byte parser
  -> C5 v2 JSON POST /local/v1/radar/result
  -> S3 local_http_server：JSON / identity / sequence 校验
  -> 每 C5 独立 spatial_state
  -> coordinate transform -> zone map -> target tracker -> spatial state
  -> radar_registry
  -> X 未接入 S3 sensor_aggregator / server_client
  -> X 未进入 ESP-server / Dashboard 雷达模型

S3_LOCAL UART frame -> radar_local_adapter -> 同一 radar_registry
```

当前源码的真实 C5-S3 endpoint 是 `/local/v1/radar/result` v2；旧文档中出现的 `/local/v1/radar/state` 不是本审计所见的活跃 handler。**A**

|阶段|芯片与模块|输入|输出|责任边界|
|---|---|---|---|---|
|BLE|C51/C52 `radar_ble_transport`|广播、GATT、Notify|Notify bytes|绑定、连接、订阅、重试；不解析帧。|
|流缓冲|C51/C52 `radar_ble_stream`|Notify bytes|512 B 顺序流|回调仅拷贝；满时丢旧字节并重同步。|
|parser|C51/C52 `ld2450_parser`|任意分段字节流|最多 3 个原始目标|定长解帧、字段解码；无空间语义。|
|v2 codec|C51/C52 `radar_state_codec/client`|latest sample|JSON `p,id,t,u,q,v,targets`|只传基础解析字段和链路状态。|
|S3 ingest|`local_http_server/radar_gateway_ingest`|C5 JSON、device ID|每源 spatial state / registry|校验并适配为 S3 算法输入。|
|空间算法|S3 radar_domain|每源 frame|坐标、track、occupancy、motion|集中算法层。|
|Server/Dashboard|S3/ESP-server|registry output|无|当前没有上传或展示消费方。|

S3 registry 固定映射 C51 到 `living_room`、C52 到 `bedroom`，且各有 slot；但两个远端 slot 以相同默认空间配置初始化，因此这只是来源隔离，并非已标定的两个房间模型。`radar_registry.c:33-49`，`radar_gateway_ingest.c:488-494`。**A/B**

## 二、C5 端雷达审计

### 1. BLE 连接逻辑

|检查项|结果|
|---|---|
|严格绑定|启用分支同时要求 MAC、地址类型、Service UUID、Notify UUID 非空；否则 `unavailable`。`radar_ble_transport.c:238-249`。|
|误连接其他 LD2450|默认不连接；启用后 `peer_matches()` 比较地址类型和 6-byte MAC，非匹配广播不连接。`radar_ble_transport.c:74-87,165-170`。|
|UUID 校验|启用后先按 service UUID 发现服务，再按 notify UUID 且 notify 属性选择特征。`radar_ble_transport.c:133-158`。|
|C51/C52 独立|C51 为 local_id=1 / `sensair_shuttle_01`，C52 为 2 / `sensair_shuttle_02`；公共 BLE/runtime/parser 文件经 `cmp` 为一致，身份头与 MAC 不同。|
|当前默认状态|`RADAR_BLE_BINDING_ENABLED=0`，地址类型未指定，Service/Notify UUID 为空；启动直接 unavailable。`radar_ble_binding_config.h:6-19`，`radar_ble_transport.c:231-236`。|
|重连|1 s 指数退避，最大 30 s；连接、发现、订阅失败或断开均重试。`radar_ble_transport.c:61-72,191-200`。|

MAC 常量不能证明现场雷达的地址类型、UUID 或 Notify 行为；需以扫描和 GATT 发现确认。**C**

### 2. parser 审计

帧为 `AA FF 03 00` 头 + 3 个 8-byte target + `55 CC` 尾，共 30 B；目标结构为 `slot,x_mm,y_mm,speed_cm_s,resolution_mm,distance_mm`，最多 3 个。`ld2450_types.h:11-31`。X/speed 为方向位解码，Y 为 16-bit 原始值减 32768，实际符号和单位需对照真实帧。**A/C**

|项|结论|
|---|---|
|帧头/长度|滑动缓冲逐字节找 4-byte header，累计固定 30 B 后解帧。|
|尾校验|校验 `55 CC`；失败丢一个字节重新同步。|
|CRC/checksum|无实现，仅头/长度/尾。不能静态断言供应商协议是否另有 CRC。|
|异常帧|有 bad header/length/tail、skipped bytes、resync 等诊断。|
|半包/粘包|支持；不足 30 B 保留，多帧逐次回调。|
|丢帧恢复|2 s 未变化的半帧强制 reset；512 B 流满时丢旧数据。能恢复同步，不能恢复这段丢失数据。|

### 3. v2 协议审计

```json
{"p":2,"id":1,"t":"radar","u":123,"q":10,"v":{"link_state":5,"sample_valid":1,"frame_seq":234,"frame_uptime_ms":123,"target_count":1,"targets":[{"slot":0,"x_mm":1200,"y_mm":800,"speed_cm_s":15,"resolution_mm":320,"distance_mm":1442}]}}
```

编码器拒绝非法 local_id、零 sequence、无效样本带目标、无效/重复 slot。`radar_state_codec.c:27-86`。上报为 latest-only：pending 会被最新样本覆盖，最短成功间隔 100 ms、心跳 1 s、HTTP timeout 3 s。`radar_state_client.c:86-173`。payload 没有 zone、track ID、presence、motion、occupancy 或 confidence，故 C5 没有承担空间判断。**A**

### 4. C5 资源分析

|资源|静态事实|评价|
|---|---|---|
|BLE stream|512 B + 静态 mutex；回调拿不到锁即丢通知。`radar_ble_stream.c:15-42`|小且有界，过载时丢数据。|
|parser/sample|30 B parser frame buffer，最新 sample，无帧队列|内存有界。|
|BLE task|`radar_ble_rx`，4096 B，priority 2，20 ms 周期、最多取 128 B。`radar_ble_runtime.c:29-61,75`|适合低频输入；持续高于消费能力会丢数据。|
|report task|`radar_report`，4096 B，priority 3，100 ms poll，单 HTTP 最多 3 s。`radar_state_client.c:17-23,142-148`|不阻塞 parser，但会延迟上报。|
|heap/stack/CPU|无 watermark、最小 heap、BLE controller 内存或 CPU 遥测|不能证明长期运行余量。|

`app_runtime_non_voice_is_paused()` 会暂停上报但不暂停 parser；恢复后仅发最新状态。此设计防止堆积，但实时状态会在语音期间滞后。`radar_state_client.c:122-126`。**A/B**

结论：资源模型适合 latest-only 起点，但默认连接未配置、无资源实测，不能下“适合长期运行”的硬件结论。**A/C**

## 三、S3 雷达算法审计

### 1. 坐标转换算法

输入 LD2450 原始 x/y，依序执行可选 X/Y 镜像、二维旋转、原点 offset；之后重算 distance/angle，并以最大距离和 room rectangle 过滤。实现是：

```text
x1/y1 = 可选镜像(raw x/raw y)
x2 = x1*cos(a) - y1*sin(a) + offset_x
y2 = x1*sin(a) + y1*cos(a) + offset_y
accept = 最大距离检查 && room_bounds 检查
```

见 `radar_coordinate_transform.c:21-60`。默认不镜像、不旋转、offset=0，最大 6000 mm，room bounds 为 `[-6000,6000] x [-6000,6000]`。`radar_config.h:86-107`。算法形式支持真实部署，但默认只是雷达原点为中心的方框，没有每 C5 的朝向、实际墙体或现场标定；是可标定基础，不是现成房间模型。**A/B/C**

### 2. 区域判断算法

`radar_zone_map` 支持最多 8 个轴对齐矩形，不支持多边形或网格。先检查 room，排除 ignore rectangle，再使用上次 zone 的 hysteresis，否则按配置顺序命中第一个 zone。`radar_zone_map.c:28-77`。默认只有一个覆盖整间默认方框的 ACTIVE zone，250 mm hysteresis。`radar_config.h:98-107`。故当前仅有粗粒度空间感，不能区分床、门、沙发等功能区。**A/B**

重要缺口：`radar_spatial_state_on_frame()` 调 tracker 时传入 `NULL` zone map，`radar_spatial_state.c:247-249`；gateway output 重新 resolve zone 时又固定 previous zone 为 0，`radar_gateway_ingest.c:376-399`。所以 track 的 zone、zone_changed、zone_left、dwell 及 hysteresis 没有形成生命周期级空间语义。**A**

### 3. 目标跟踪算法

每源最多 3 tracks。算法搜索一对一分配，目标是在动态 gate 内最大化匹配数、最小化总距离；gate 默认 800--1200 mm，预测位置使用当前速度和方向。`radar_target_tracker.c:28-118`，`radar_config.h:111-116`。新目标给递增临时 track_id，confidence=40，tentative 连续 2 帧后 confirmed；已匹配坐标以 1/2 smoothing 更新、confidence 每帧 +20。`radar_target_tracker.c:170-225`。未匹配 confirmed track 在 500 ms 内 HOLD，confidence 每缺帧 -20，超过 1800 ms 删除。`radar_target_tracker.c:140-150,274-287`。**A**

这具备人体移动的基础关联、短期 ID、短暂丢失与 smoothing；没有卡尔曼滤波、速度低通、外观/跨长期重识别、遮挡策略，且最多 3 人。多人交叉和真实精度仍须回放和硬件验证。**A/C**

### 4. spatial state 算法

|状态|进入条件|退出/窗口|
|---|---|---|
|unknown|UART offline/backoff、帧超过 3000 ms、recovery 非 valid|恢复 valid。|
|occupied=present|至少一个 visible confirmed track|目标不可见而 confirmed 尚在，转 hold。|
|motion=moving|速度绝对值 >=15 cm/s 或位移 >=120 mm，连续 3 帧|连续 8 帧不满足后退出。|
|motion=still_candidate|visible track 连续 >=3 帧，且非 moving|仅为候选，不是微动/呼吸证明。|
|occupied=hold|无 visible，仍有 confirmed/HOLD track|1500 ms confidence decay；1800 ms track timeout。|
|vacant_inferred|没有 active track|是推断，不是绝对无人。|

实现：`radar_spatial_state.c:101-188,206-275`；阈值：`radar_config.h:110-127`。静止目标持续输出时可保持 `PRESENT + STILL_CANDIDATE`。若雷达停止输出静止目标，系统没有呼吸微动、能量/SNR 或床位策略，只能短 hold 后清空，不能声称可靠静止人体感知。**A/C**

## 四、C5 与 S3 职责边界检查

|检查|结论|
|---|---|
|C5 BLE/帧解析/基础整理|符合。C5 只保留 parser sample，v2 不带空间结论。|
|S3 坐标/区域/跟踪/人体状态|符合。远端 C5 ingress 和 S3 local adapter 都进入 `radar_spatial_state`。|
|算法重复|无 C5/S3 空间算法重复。两端 parser 分别服务 BLE 边界和 S3 本地 UART，是合理重复。|
|资源浪费|latest-only HTTP、512 B stream 和 3-track 均有界；代价是语音期间时效降低。|
|接口污染|C5-S3 边界干净；问题是 registry 没有通往 Server/Dashboard 的产品边界。|

`radar_remote_ingest.c` 是 registry-only 的旧路径；当前 `/local/v1/radar/result` handler 走 `radar_gateway_ingest_process_json()`，它实际调用完整 spatial state。并存增加理解成本，但当前 HTTP handler 没有绕开 S3 算法。**A**

## 五、当前算法能力评价

|维度|当前能力|问题|建议优化|
|---|---|---|---|
|有人无人|confirmed track 后有 present/vacant inferred|默认 C5 BLE 不连；空闲是推断|先完成 BLE 配置和现场 timeout 标定。|
|静止人体|连续稳定目标可 still_candidate|无微动/质量/长 hold|引入 confidence decay、静止窗口和场景策略。|
|移动人体|速度或位移 + 3/8 帧滞回|速度未滤波，参数未标定|速度中值/低通、跳变门控、现场标定。|
|多目标|最多 3 个短期 track|交叉/遮挡/重识别未证实|关联质量、预测滤波、多人回放测试。|
|空间理解|变换、bounds、矩形 zone 基础|默认一整块 zone，track zone 未接入|每源 room profile，修复 track-zone，进阶 room map。|
|长期稳定|退避、重同步、latest-only、freshness|无 heap/stack/丢包/共存基线|只读遥测与 24--72 h soak 验收。|

## 六、发现的问题

### P0：必须修复

1. **C5 默认无法连接任何 BLE 雷达**
   - 位置：C51/C52 `radar_ble_binding_config.h:6-19`，`radar_ble_transport.c:231-249`。
   - 原因：binding=0，地址类型、Service UUID、Notify UUID 为空。
   - 影响：C5 BLE 采集链路默认不可运行，S3 只能得到未知/无效远端状态。
   - 建议：真机扫描确认每台设备的 MAC 地址类型和 GATT UUID 后，分别启用绑定；不得按名称猜测连接。

2. **S3 到 Server/Dashboard 的雷达产品链路不存在**
   - 位置：S3 `radar_domain`；`sensor_aggregator/server_client/protocol_adapter` 未引用 radar registry/gateway ingest；ESP-server Dashboard 无 radar 字段消费者。
   - 原因：registry 是终点，未定义 Server payload、持久化和展示契约。
   - 影响：即使 S3 算法正确，Server/Dashboard 也不能显示或存储人体感知。
   - 建议：另行冻结 S3->Server latest-state 契约，含 source/room、online、occupancy、motion、confidence、last_seen、诊断；不直接上传 C5 原始包。

### P1：影响准确性

1. **默认空间参数非现场模型，两个远端源共用它**
   - 位置：`radar_config.h:86-107`，`radar_gateway_ingest.c:488-494`。
   - 原因：flip/rotation/offset 均为 0，只有整室 zone，未按 C51/C52 分源配置。
   - 影响：坐标虽可计算，却不能可靠表达真实房间空间。
   - 建议：按 source 建 installation profile，并用实测安装姿态标定。

2. **zone map 未接入 track 生命周期**
   - 位置：`radar_spatial_state.c:247-249`，`radar_gateway_ingest.c:376-399`。
   - 原因：tracker 收到 NULL zone map，输出也不传 previous zone。
   - 影响：dwell、跨区、边界 hysteresis 不能可靠工作。
   - 建议：传 `&state->zone_map` 并以 track previous zone 输出；补跨区测试。

3. **静止存在是短候选，无法覆盖静止目标撤销**
   - 位置：`radar_spatial_state.c:154-188`，`radar_config.h:114-124`。
   - 原因：hold 1500 ms、track timeout 1800 ms，无质量或微动数据。
   - 影响：坐姿、睡眠、遮挡可能快速空闲。
   - 建议：先证实官方/实测可用字段，再按场景建立 confidence decay。

4. **语音期间暂停 C5 上报**
   - 位置：`radar_state_client.c:122-126`。
   - 影响：S3 远端状态可能延迟甚至超过 5 s freshness/offline 窗口。
   - 建议：明确语音期间实时性要求，再做资源隔离与实测。

### P2：优化项

1. BLE stream 锁竞争会丢 Notify，且无对外 drop/high-water 遥测。位置：`radar_ble_stream.c:15-42`；建议加入限频诊断并压测。
2. parser 只检头尾、无 CRC 证据。位置：`ld2450_parser.c`；建议先与官方定义和原始帧比对，勿臆造 CRC。
3. C5 缺 stack watermark、最小 heap、BLE/Wi-Fi 共存指标；建议后续增加只读遥测和长期基线。

## 七、下一阶段优化建议

1. 对照官方 LD2450 资料建立“BLE 原始十六进制 -> 官方工具显示 -> parser target”样本，确认帧型、符号、单位和校验。
2. 在 S3 增强 target smoothing、velocity filtering 和 confidence score；C5 继续只传原始基础字段。
3. 空间模型分三步：修复并配置独立 rectangles -> 每 source room map（姿态、墙体、entry/ignore）-> 区域级 occupancy map；不得把不同房间 C5 坐标直接合并。
4. 先修复 tracker-zone 接入，再启用现有 `zone_entered_ms/dwell_ms` 做停留时间、短暂停留和离开确认；阈值需现场标定。
5. 冻结 S3->Server snapshot 后再接 Dashboard，消费 S3 领域结果而不是 C5 原始 target。

## 八、最终结论

1. **架构合理性：部分合理。** C5 轻采集、S3 集中空间计算、latest-only 传输和来源隔离正确；默认 BLE 未启用且 S3->Server/Dashboard 断链，端到端产品链路未完成。
2. **C5+S3 分层：正确。** C5 未承担空间算法，S3 是坐标、跟踪和人体状态的唯一所有者。
3. **S3 空间智能基础：已经具备。** 它有矩阵变换、边界、矩形 zone、短期多目标跟踪、confidence/hold 和 moving/still candidate；但默认标定和 track-zone 接入不足以称为真实房间智能。
4. **距离产品级的关键步骤：** 真机 BLE 身份/GATT 验证；官方协议和原始帧比对；每源房间标定；修复 zone-track；静止/移动/离开现场标定；完成 S3->Server/Dashboard 契约；最后进行 BLE/Wi-Fi/语音并发的长期硬件 soak。

本报告没有把任何静态路径视为硬件成功证据。在线、精度、静止人体、误报/漏报和长期稳定性均须在真实 LD2450、实际安装环境与完整链路下验证。

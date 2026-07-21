# ESP Radar Migration Completeness Audit

审计日期：2026-07-20
源项目：`/Users/zhiqin/ESP 雷达`
目标项目：`/Users/zhiqin/ESP 部分开发`
方法：静态源码、活动 CMake 源清单、启动调用链与逐文件比较。未修改源码，未编译、烧录、启动服务或设备。

证据级别：A=当前源码与活动编译/启动接线确认；B=静态风险推断；C=只能通过真机或运行时确认。

## 1. 总结

**源项目已具备的雷达能力迁移完整度：100%（17/17 项，A 级静态确认）。**

没有发现“源项目存在而目标缺失”的正式雷达功能。目标项目的核心链未被阉割，并额外加入了 HOME snapshot、Habit Rule 接入、source-context 锁、PSRAM 准入、受控停止和多处边界保护。

这个百分比只回答“是否完整迁移源项目已有能力”，不等于真机验收完成。源、目标两边都没有专用 ESP-server radar API/持久化/网页雷达视图；这不是迁移漏项，而是源项目本来就没有的端到端能力。UART、BLE、zone 开关、多目标准确性和资源水位仍为 C 级待运行验证。

### Feature Matrix

| 功能名称 | ESP 雷达项目 | 主项目 | 状态 | 文件位置 | 风险 |
|---|---|---|---|---|---|
| LD2450 解析 | 有 | 有 | 完整迁移 | `ESPS3/components/radar_ld2450/ld2450_parser.c` | C：真机帧未验收 |
| UART 雷达 | 有（S3_LOCAL） | 有（S3_LOCAL） | 完整迁移 | `radar_ld2450/ld2450_uart.c`, `radar_service.c` | C：接线/波特率/恢复 |
| BLE 雷达 | 有（C51/C52 Central） | 有 | 完整迁移 | `ESPC5*/components/Middlewares/radar_ble` | C：GATT/notify/重连 |
| 多设备 | 固定三源 | 固定三源 | 完整迁移 | `radar_source_context.c:20-42` | B：非动态扩容 |
| 房间映射 | 有 | 有 | 完整迁移 | `radar_registry.c`, `radar_ingest.c` | B：编译期绑定 |
| 坐标转换 | 有 | 有 | 完整迁移 | `radar_coordinate_transform.c` | C：安装校准 |
| zone | 有 | 有 | 完整迁移 | `radar_zone_map.c`, `radar_spatial_state.c` | B/C：开关与实测 |
| target tracking | 有 | 有 | 完整迁移 | `radar_target_tracker.c` | C：多人稳定性 |
| person tracking | 有 | 有 | 完整迁移 | `radar_person_continuity.c` | C：长期连续性 |
| reassociation | 有 | 有 | 完整迁移 | `radar_person_continuity.c:232-295` | C：现场误关联 |
| dormant | 有 | 有 | 完整迁移 | `radar_person_continuity.c:344-423` | C：阈值适配 |
| hold | 有 | 有 | 完整迁移 | `radar_target_tracker.c:151-170` | C：遮挡场景 |
| motion state | 有 | 有 | 完整迁移 | `radar_spatial_state.c`, tracker/person state | C：语义准确性 |
| radar 日志 | 有 | 有 | 完整迁移 | `radar_log_manager.c`, `radar_diagnostics.c` | C：现场吞吐 |
| debug | 有 | 有 | 完整迁移 | `local_http_server/radar_local_handler.c` | B：4 KiB 响应上限 |
| 本地工具兼容 | 有 | 有 | 完整迁移 | `RADAR_RX_FRAME`/source-room-home log contract | C：实际渲染 |
| home aggregation | 有 | 有且增强 | 完整迁移 | `radar_registry.c`, 目标 `radar_home_snapshot.c` | C：多源现场 |
| 事件上报 | 无雷达专用链 | 有（目标扩展） | 非迁移项 | `habit_event_reporter.c` | B：非 server radar API |
| rule 触发 | 无雷达专用链 | 有（目标扩展） | 非迁移项 | `habit_rule_adapter.c` | C：规则现场验证 |

## 2. 已完整迁移功能

| 功能 | 源项目实现 | 目标项目实现 | 验证依据 |
|---|---|---|---|
| S3 LD2450 UART | `ESPS3/components/radar_ld2450` | 同路径 | A：30-byte `AA FF 03 00 ... 55 CC` 解析器、256000 8N1、UART1、1024 B RX ring、20-entry event queue 均保留；目标解析器见 `ld2450_parser.c:7,256,305`，驱动见 `ld2450_uart.c:33,71,143`。 |
| UART RX/异常恢复 | `radar_service.c` | 同路径 | A：RX task、双 1024 B 暂存、128 B read、溢出 flush 和恢复状态机仍在；目标 `radar_service.c:47,173,368,405`。 |
| C51/C52 BLE Central | `radar_ble` | 同路径 | A：CMake 编入 `radar_ble_transport/runtime`，编排器实际启动；目标 C51/C52 `Middlewares/CMakeLists.txt:52`、`app_orchestrator.c:480`。 |
| BLE MAC/GATT/Notify/retry | `radar_ble_transport.c` | 同路径 | A：精确 MAC 绑定、服务/特征发现、CCCD notify、通知转交、扫描/连接失败指数退避均存在；目标 C51 `radar_ble_transport.c:119,229,374,533`，C52 同构。 |
| C5 采集与上送 | `radar_worker.c` | 同路径 | A：BLE bytes -> parser sample -> edge filter -> latest/upload，C5 不执行 transform/zone/tracker/person；目标 C51 `radar_worker.c:214-250`，C52 同构。 |
| 远端 ingest | `radar_ingest` + local HTTP | 同路径 | A：`/local/v1/radar/result` handler 进入正式 ingest worker，按 local_id 分槽、校验 identity/sequence；目标 `radar_local_handler.c:111-114`、`radar_ingest.c:42-55,205-224,569-598`。 |
| 数据处理链 | `radar_spatial_state` 等 | 同路径 | A：raw frame -> filter -> transform -> room bounds -> zone -> tracker -> person -> source context -> registry -> HOME；生产源已列入目标 `Middlewares/CMakeLists.txt:26-43`。 |
| 坐标/房间/zone | `radar_coordinate_transform.c`, `radar_zone_map.c` | 同路径且算法文件一致 | A：空间状态在目标 `radar_spatial_state.c:284-338` 处理；配置关闭时不会把 zone map 传入 tracker。 |
| target tracking | `radar_target_tracker.c` | 字节一致 | A：TENTATIVE -> CONFIRMED、速度门控、全局关联、HOLD、超时 RELEASE、HOLD recover 均在 `radar_target_tracker.c:151-170,215-273,277-362`。 |
| person tracking | `radar_person_continuity.c` | 字节一致 | A：VISIBLE/STILL_HOLD/DORMANT/RELEASED、预测重关联和 zone 兼容仍在 `radar_person_continuity.c:232-295,344-423,578-611`。 |
| 多人上限 | `radar_spatial_types.h` | 同路径 | A：最多 3 个 target/track/person；目标 `radar_spatial_types.h:16-20`。 |
| 三源隔离 | `radar_source_context/registry` | 同路径且增强 | A：固定 `S3_LOCAL/C51/C52`，每源 device_id、room_id、transport、spatial/tracker/person context 独立；目标 `radar_source_context.c:20-42,97-102,129-187`。 |
| device/room 绑定 | `radar_ingest.c` | 同路径 | A：C51/C52 local_id 限制为 1..2，device_id 与 room_id 均强校验；目标 `radar_ingest.c:183-223`。 |
| HOME aggregation | `radar_registry.c` | 同路径且增强 | A：按有效在线 source 重建 room/HOME；目标 `radar_registry.c:74-104,214-225`。 |
| radar 日志与 diagnostics | `radar_log_manager/diagnostics` | 同路径 | A：生产 CMake 已纳入，并由启动链调用 diagnostics；目标 `gateway_orchestrator.c:129-131`。 |
| local debug/latest/history | `radar_local_handler`, `radar_ingest` | 同路径 | A：正式 debug handler 输出 sources、tracks、parser/recovery 和 history stats；目标 `radar_local_handler.c:136-327`。 |
| 启动接入 | `gateway_orchestrator.c` | 同路径 | A：registry -> remote ingest -> S3 local adapter -> diagnostics；目标 `gateway_orchestrator.c:99-103,129-131`。 |
| 规则触发/事件上报 | 源项目无雷达到规则专用链 | 目标新增 | A：`radar_home_snapshot.c` 被 CMake 编入，并由 `habit_rule_adapter.c:7-22` 消费，再启动 event reporter；目标 `gateway_orchestrator.c:106-111`。这是目标扩展，不纳入源迁移分母。 |

### 数据流差异图

```text
源项目与目标的核心生产链相同

S3_LOCAL UART
LD2450 frame -> parser -> radar_local_adapter -> spatial_state
              -> filter -> transform/room -> zone -> tracker -> person
              -> SourceContext[S3_LOCAL] -> Registry -> HOME

C51/C52 BLE
notify -> C5 parser/filter/latest upload -> POST /local/v1/radar/result
       -> S3 radar_ingest worker -> gateway_ingest -> spatial_state
       -> filter -> transform/room -> zone -> tracker -> person
       -> SourceContext[C51|C52] -> Registry -> HOME

目标新增：Registry -> radar_home_snapshot -> habit_rule_adapter -> habit_event_reporter
```

## 3. 未迁移功能

**相对源项目：无。** 未发现源项目的正式 LD2450、BLE、空间处理、多源、日志、debug、history 或 HOME 能力在目标缺失。

以下项目必须明确为“源项目本来没有”，不应误报为迁移失败：

| 能力 | 源项目 | 目标项目 | 影响与建议 |
|---|---|---|---|
| ESP-server radar ingest/API | 无。ESP-server 搜索不到正式 `radar` 路由/服务 | 无。仅有 `csi.motion` | 端到端雷达数据无法进入 server。若产品需求需要云端雷达，P0 新增独立 radar contract，不能伪装为已迁移。 |
| Server radar storage/latest/history | 无 | 无 | dashboard 无 server-side radar history；S3 local history 不等于服务端持久化。 |
| Web 雷达显示/debug | 无专用页面/API | 无专用页面/API | S3 local debug 仍可用；浏览器端雷达可视化需另立需求。 |
| 动态设备注册/房间配置 | 固定三源编译期映射 | 固定三源编译期映射 | 新设备需要改源码并重新构建，不能自动加入。 |

## 4. 部分迁移功能

没有相对源项目的部分迁移。但以下已有能力的运行完成度不能由静态代码证明：

| 功能 | 已有代码 | 缺少部分 |
|---|---|---|
| UART 采集与恢复 | 完整 parser、UART event、RX task 与恢复状态机 | C：GPIO/波特率、持续溢出恢复及真实 LD2450 数据未验证。 |
| BLE 链路 | Central、MAC、GATT、CCCD、notify 和退避完整 | C：真实 MAC、地址类型、服务 UUID、CCCD/notify、重连时序未验证。 |
| Zone | 生产代码完整 | B/C：`CONFIG_S3_RADAR_ZONE_ACTIVE` 是否在实际 sdkconfig 打开未知。 |
| 多人关联/状态机 | 算法与参数已迁移 | C：安装位置、空房/单人/多人/静止/遮挡下的误差和状态转移未验证。 |
| Server 链路 | 两项目均只有 CSI server 链路 | 不是迁移遗漏，但没有 radar server 接入、持久化或 UI。 |

## 5. 架构风险

| 优先级 | 风险 | 依据与影响 |
|---|---|---|
| P1 | C5 出现轻量空间语义重复 | 目标新增 LCD 路径从 raw target count/speed 推导 `presence/motion/room_occupied`（C51/C52 `app_orchestrator.c:189-205`）。它不回写 S3/Server，且不做 zone/tracker/person；但若“C5只采集、S3唯一空间解释”为硬边界，这会造成语义漂移。 |
| P1 | zone 可能运行时关闭 | 仅在 `CONFIG_S3_RADAR_ZONE_ACTIVE` 为真时，目标 `radar_spatial_state.c:321-331` 才把 zone map 交给 tracker；静态审计不能证明实际开关。 |
| P1 | BLE stop 未停止 NimBLE host task | `radar_ble_transport_stop()` 清状态与回调，但不停止/反初始化 host task；为源、目标共有设计约束。 |
| P1 | 一段无主调用的兼容 ingest | `radar_domain/radar_remote_ingest.c` 虽编入，但未找到生产 caller；真正远端入口是 `radar_ingest` worker。不得将其当运行证据，建议下轮清理或加显式路由。 |
| P2 | 独立 BLE stream 未接入 | `radar_ble_stream.c` 在两边均有，但未列入 Middlewares 编译源、也未被正式引用；不应计为运行缓冲链路。 |
| P2 | 资源压力需实测 | 每个 C5：radar worker 4 KiB + upload 3 KiB PSRAM stack，加 raw/upload ring；S3：remote ingest 4 KiB、local adapter workspace/stack、log task、static diagnostics task。目标的 PSRAM admission 和停止清理较源增强，但不能替代水位实测。 |

目标工程中未发现生产 CMake 纳入的重复 spatial/tracker/person 实现，也未把测试代码当成正式能力。`radar_domain/tests` 未列入生产 `Middlewares/CMakeLists.txt`，本报告没有以测试作为接入依据。

## 6. 建议优先级

### P0：必须补齐

1. 若产品要求“雷达数据必须上云/可网页查看”，新增完整且独立的 radar server contract：S3 upload queue/retry、server ingest、latest/history storage、dashboard/debug API。该项为新能力，不是本次源项目迁移缺失。
2. 在目标硬件上执行 UART 与 BLE 基线验收：S3 UART、C51/C52 三块雷达的 MAC/GATT/notify、断线重连、空房/单人/多人/进出/静止/干扰场景。

### P1：建议完善

1. 明确并验证 `CONFIG_S3_RADAR_ZONE_ACTIVE` 的发布配置；否则 zone 代码可存在但运行时不生效。
2. 收紧 C5 职责：LCD 只消费 S3 的空间结果，或至少将本地推断显式标为 UI 估计，避免双重 presence/motion 语义。
3. 审核 `radar_remote_ingest.c` 未接入兼容路径，删除、接线或标记弃用；避免维护者误判其为活跃数据链。
4. 定义 NimBLE host 生命周期，确保 radar stop/restart 后没有残留 host task 与回调路径。

### P2：优化项

1. 对 64 B notify 临时拷贝加入显式截断计数/告警；当前足够 30 B LD2450 帧，但不适合更大厂商通知。
2. 真机采集各 radar task 的 stack high-water、PSRAM free/largest block、队列丢弃与 BLE retry 指标。
3. 若未来扩容，替换编译期 `S3_LOCAL/C51/C52` 映射为可配置的 device/room registry，并保留严格 identity gate。

## 审计边界

本报告没有执行构建、测试脚本、烧录、串口监控、启动 ESP-server 或访问真实数据库。所有“A”结论仅说明当前源码被生产构建清单和启动/handler 调用链接入；所有真实传输、运行时配置、内存与算法质量结论均保留为 C 级验收。

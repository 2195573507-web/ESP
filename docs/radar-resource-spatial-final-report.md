# 雷达资源与多源空间感知最终报告

日期：2026-07-18  
依据：[ESP雷达_C5资源自适应与S3多源空间感知总设计_2026-07-18.md](ESP雷达_C5资源自适应与S3多源空间感知总设计_2026-07-18.md)

## 结论

PASS（源码、host test、Mac 解析门禁和 C51/C52/S3 构建）。本轮没有 flash、monitor 或真机持续运行，因此 BLE 连接质量、真实上传频率、语音期间资源水位和实际房间标定仍属于硬件验证项。

## C5 资源调度

- 新增 `radar_resource_adapter`，维护 `VACANT_STABLE`、`VACANT_RECENT`、`PRESENT_STILL`、`PRESENT_MOVING`、`RADAR_DEGRADED`、`VOICE_ACTIVE`。
- BLE Notify、30-byte parser 和 edge filter 继续连续工作；新适配器只消费 filter 输出，不改变 BLE FFF0/FFF1 流程、LD2450 slot 语义、p=3 JSON 字段/单位或 `/local/v1/radar/result`。
- 默认上传上限为 vacant stable 5 s、recent 1 s、still 500 ms、moving 100 ms、degraded 5 s。状态、health、presence 和 moving 变化强制触发一次最新快照。
- 雷达上传改为单一在途请求和一个覆盖式 latest snapshot。失败采用 100/200/400/800/1000 ms 有界退避；不重试过期坐标，不堆积 32 个历史帧。
- `C5_RESOURCE_STATE_*` 与 `app_runtime_non_voice_is_paused()` 是 voice 的权威输入。voice active 时 radar 保持轻量接收/解析和 latest，HTTP 被既有 non-voice admission gate 延后；voice 结束后 2 s 冷却，再补发最新 radar 与 BME。这是比 1 Hz 上限更保守的实现，不会和 voice 争用 HTTP。
- BME 不重启 driver、不改变基线计算。仅在上传 client 限频：无人 10 s、recent 20 s、still 30 s、moving 60 s；voice active 不主动上传。调度器为 voice 恢复窗口提供 2 s 的提前 tick。
- 新增开关：`CONFIG_C5_RADAR_ADAPTIVE_UPLOAD`、`CONFIG_C5_BME_ADAPTIVE_REPORT`、`CONFIG_C5_VOICE_TELEMETRY_THROTTLE`，默认 `1`；关闭 adaptive upload 可恢复原逐帧队列路径。

## S3 三源空间状态

- `S3_LOCAL`、`C51`、`C52` 保持各自的 `radar_spatial_state_t`、tracker、健康/occupancy/motion、latest 输入、rate manager 与日志时间窗。C51/C52 的 source slot 不与 S3_LOCAL 共用 tracker ID 或节流计时。
- `radar_log_manager` 改为按 source 的 latest-only 日志槽。一个 source 的 high-rate moving 只会合并该 source 的日志，不能压制其他 source。
- 原有 `RADAR_STATE`、`RADAR_TRACK`、`RADAR_TRACK_COMPAT`、`RADAR_TRACK_COMPAT_METERS`、`RADAR_TRACKER`、`RADAR_LOG_DROP` 标识、原字段顺序和单位不变；只在行尾追加 `source=`、`room=` 等字段。`RADAR_HOME` 是新增独立摘要行。
- tracker 算法没有重写。空间状态机现在将已有 `state->zone_map` 传给原 tracker 更新函数，使 full-room active zone 和后续按 source 校准的 zone 配置进入实际路径。
- 阈值统一由 `radar_config.h` 提供，消除了空间状态默认函数中与配置不一致的 association、track、confidence、velocity、EMA 硬编码。
- 新增 home presence 快照：汇总每个 registry room 的 occupancy，输出 occupied room count、active room/source 和最近状态转换；不合并跨房间坐标，也不推断 person identity。
- 新增开关：`CONFIG_S3_RADAR_PER_SOURCE_LOG_SCHEDULER`、`CONFIG_S3_RADAR_ZONE_ACTIVE`，默认 `1`。关闭前者恢复为仅 S3_LOCAL 日志调度；关闭后者恢复 tracker 的空 zone-map 输入。

## 兼容性

- C5 p=3 schema、`device_id` identity 约束、BLE binding、parser 和 endpoint 未改。
- C51/C52 共享 radar worker/resource adapter 字节一致；BLE binding config 仍是唯一预期身份差异。
- Mac 工具已有显式 `source=` 识别与三源 store 隔离，因此不需要改 parser/store 源码。补充了 `RADAR_TRACK_COMPAT ... source=C51 room=living_room` 固定样本，并通过现有 parser checks 和 SwiftPM build。

## 修改范围

- C51/C52：`radar_worker`、新 `radar_resource_adapter`、Middlewares CMake、BME upload/service、C5 backpressure、app config。
- S3：`radar_log_manager`、`radar_gateway_ingest`、`radar_local_adapter`、`radar_spatial_state`、`radar_registry`、`radar_config`、S3 host-test script。
- Mac：`ESPS3-Radar-Debug/Samples/three-room-mixed.log`；没有修改 parser 逻辑。
- 工具：`tools/check_c5_radar_parity.sh` 扩展为检查新的共享 C5 radar/BME scheduler 文件。

## 验证

| 项目 | 结果 |
| --- | --- |
| `idf.py -C ESPC51 -B build-radar-c51 build` | PASS，`00_Learn.bin` 0x1abd40，67% 分区余量 |
| `idf.py -C ESPC52 -B build-radar-c52 build` | PASS，`00_Learn.bin` 0x1abd40，67% 分区余量 |
| `idf.py -C ESPS3 -B build-radar-s3 build` | PASS，`sensair_s3_gateway.bin` 0x11a460，84% 分区余量 |
| S3 radar domain host suite | PASS：protocol/registry/ingest、spatial/recovery、source isolation、latest worker |
| S3 radar core host suite | PASS |
| C5 parity + BLE stream host test | PASS |
| Mac parser checks | PASS：旧日志、三源交错、offline/recovery、追加 `source=` 字段 |
| `swift build`（ESPS3-Radar-Debug） | PASS |
| `git diff --check` 与 C51/C52 shared-byte comparison | PASS |

## 真机待验证

1. C51/C52 各自绑定的 LD2450 在移动、静止、离开、BLE reconnect 下的实际 mode/upload Hz。
2. voice active 时 voice latency、DMA/internal heap、radar/BME latest-only 恢复顺序。
3. C51/C52/S3_LOCAL 的 room bounds、axis、origin 与 zone 边界；当前默认值保持原始坐标，未擅自翻转。
4. S3 串口 interleaved 真实日志在 Mac 三个画布的长期表现。

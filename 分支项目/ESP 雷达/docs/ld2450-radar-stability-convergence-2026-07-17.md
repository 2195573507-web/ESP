# ESPS3 LD2450 雷达稳定性收敛说明

日期：2026-07-17  
范围：仅 `ESPS3` 本地 LD2450 UART、空间状态、`/radar` 只读快照。未修改 ESPC51、ESPC52、ESP-server、Dashboard、数据库或 UART 板级参数。

## 保持不变的契约

- UART 仍为 UART1、TX GPIO18、RX GPIO17、256000 baud、8N1。
- LD2450 数据帧仍为固定 30 字节：`AA FF 03 00`、3 个 8 字节目标槽、`55 CC`。
- 已验证的坐标解码、距离与角度公式未改变；默认不翻转坐标。

## 统计口径

雷达诊断快照现在独立维护以下累计字段：

| 字段 | 含义 |
| --- | --- |
| `bytes_received` | 交给字节流解析器的原始字节数。 |
| `valid_frames` | 头、固定长度与尾部均正确的完整帧数。 |
| `bad_header` | 缓冲区首部不匹配帧头时的跳过次数。 |
| `bad_length` | 不完整帧被超时/恢复强制丢弃，或固定帧缓存上限触发的次数。 |
| `bad_tail` | 固定长度候选帧的 `55 CC` 尾部不匹配次数。 |
| `skipped_bytes` | 为恢复同步而实际移除的字节数；不等同于 `resync_count`。 |
| `invalid_target_slots` | 全零槽与 `-32768` / `-32704` 哨兵槽的拒绝次数。 |
| `coordinate_outliers` | 空间层拒绝的距离或房间边界越界目标数；异常跳变另计为 `jump_outliers`。 |

`resync_count` 表示连续坏流进入重新同步的次数，而非被跳过字节数。`identity_mismatch_count` 继续只用于 C51/C52 远端身份、会话和对端 IP 校验，本地 UART 坏帧不会增加该计数。兼容用 aggregate `parse_error_count` 仍供 registry 摘要使用，但不再是根因判断依据。

UART 统计拆为 `read_timeout`、`read_zero`、`read_driver_error`、`fifo_overflow`、`queue_full`。有等待时间的 `uart_read_bytes()` 返回零计为正常 `read_timeout`，不会计入 `read_driver_error`。

## 目标接受与状态

- 全零、哨兵槽、最大探测距离外和房间边界外目标在 tracker 前被拒绝。
- 单可见 track 对应的单目标若相对上次平滑坐标跳变超过 1500 mm，被记为 `jump_outliers`，不会产生新轨迹或可视化点。
- tracker 为每条 accepted target 保留 `raw_x_mm/raw_y_mm` 与固定成本 EWMA `filtered_x_mm/filtered_y_mm`；滤波系数为 1/2，不增加动态内存。
- occupancy 只由可见 accepted track 驱动。motion 使用连续 3 帧运动证据进入 `moving`，连续 8 帧无运动证据退出；静止目标保持 `still_candidate`，避免与 `moving` 高频切换。
- zone 映射与配置代码保留，但本地接受、occupancy、motion、诊断摘要和 `/radar` 输出均不再依赖或暴露 zone。

## 只读快照与任务边界

`radar_local_adapter_get_readonly_snapshot()` 提供按值复制的 zone-free 快照，包含：

- 全局：`timestamp_ms`、`latest_frame_ms`、`frame_age_ms`、`sensor_state`、`occupancy_state`、`motion_state`。
- 每条 track：`track_id`、`raw_x_mm`、`raw_y_mm`、`filtered_x_mm`、`filtered_y_mm`、`distance_mm`、`angle_deg`、`speed_cm_s`、`confidence`、`visible`、`timestamp_ms`。

`/radar/data` 只读取该快照，返回 `occupancy`、`motion` 和 visible tracks；不发送 Server 数据，不修改 Dashboard。`radar_rx` 只读取 UART、更新 parser/latest frame；`radar_diag` 只复制 snapshot 并限频记录日志，增加 `RADAR_DIAG_STACK_MONITOR free_words` 栈余量日志。两者不承担 HTTP 或 SSE 发送。

时间年龄计算对零时间戳、未来时间和时钟回拨返回 0，避免无符号下溢导致巨大 `valid_age_ms`；64 位年龄日志使用 `PRIu64`。

## 验证

- `sh ESPS3/components/radar_ld2450/tests/run_host_tests.sh`：PASS。
- `sh ESPS3/components/Middlewares/radar_domain/tests/run_host_tests.sh`：PASS。
- `idf.py -C ESPS3 build`：PASS，产物 `ESPS3/build/sensair_s3_gateway.bin`，大小 `0x1199c0`，最小 app 分区剩余 84%。

未执行 flash、monitor、真实 UART 连续采样或硬件验收。真实设备仍需验证坐标范围、1500 mm 跳变阈值和 3/8 帧运动滞回是否适配现场。

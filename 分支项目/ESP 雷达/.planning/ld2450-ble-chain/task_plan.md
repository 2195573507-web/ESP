# LD2450 C5-S3 BLE 雷达链路计划

## 目标

在 C51、C52 和 S3 间完成固定绑定的 LD2450 BLE 雷达数据链路，保持 C5 仅做采集和基础预处理，S3 独占空间计算；不修改 ESP-server、Dashboard、BME690、voice 或 command 链路。

## 阶段

| 阶段 | 状态 | 内容 |
|---|---|---|
| 0 | complete | 读取现有 C5 BLE、C5-S3 协议和 S3 radar_domain 基线。 |
| 1 | in_progress | 等待 C51/C52 与 LD2450 真机，取得地址类型、Service UUID、Notify UUID 和可选 Write UUID。 |
| 2 | complete | 固定 MAC 的 NimBLE discovery/subscription 代码、日志、C5 解析/过滤和 v3 codec 已实现。 |
| 3 | complete | S3 v3 adapter、APP CPU latest-frame worker、PSRAM history 与 debug 路由已接入，既有空间算法保持不变。 |
| 4 | complete | C51/C52/S3 host tests 与三块固件 ESP-IDF build 已通过。 |
| 5 | complete | 已生成 radar-chain-audit-report.md，并列出硬件未验证项。 |

## 不变量

- C51 固定对应 `sensair_shuttle_01` / `ld2450_01`；C52 固定对应 `sensair_shuttle_02` / `ld2450_02`。
- 不自动连接附近设备；地址类型、MAC、Service UUID 与 Notify UUID 必须同时匹配。
- 不改写现有 S3 coordinate transform、zone map、tracker 或 spatial state 算法；接口差异通过 adapter 解决。
- 只维护最新雷达状态，不传输 raw BLE 帧，也不引入大缓冲或阻塞 Wi-Fi/HTTP 的处理路径。

## 当前阻塞

两端固定 MAC 已启用，Service/Notify UUID 仍为空，地址类型尚未由实物确认。现有主机只检测到 ESP32-S3，未检测到可刷写/监视的 C51/C52 或绑定 LD2450；需要实机 GATT discovery 与 Notify capture 才能完成最终验收。

## 错误记录

| 错误 | 次数 | 处理 |
|---|---:|---|
| 读取不存在的 `Middlewares/radar_ble/CMakeLists.txt` | 1 | radar_ble 由上层 Middlewares 组件收集，而非独立组件。 |

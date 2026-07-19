# C5 资源自适应与 S3 多源空间感知实施

## 目标

依据 `docs/ESP雷达_C5资源自适应与S3多源空间感知总设计_2026-07-18.md`，在不重写 BLE/parser/filter/p=3/S3 tracker 的前提下完成 C5 资源调度、S3 三源独立日志与空间适配，并完成三目标构建、host tests、parity、diff check 和最终报告。

## 阶段

- [x] C5 入口审计与共享自适应适配器
- [x] C5 latest-only 上传、voice/BME 协作和摘要
- [x] S3 三源独立日志调度与空间配置接入
- [x] host/parity/Mac 兼容验证
- [x] C51/C52/S3 构建、diff check、最终报告

## 约束

- C51/C52 共享实现字节一致，仅 board identity/MAC/room 配置允许差异。
- C5 保留 BLE、parser、filter、p=3 JSON schema 和 `/local/v1/radar/result`。
- S3 保留既有 tracker/空间算法和日志标签；日志仅允许行尾追加字段。
- 不修改 ESP-server、Dashboard 或无关用户变更。
- stale/offline 不得伪装成 vacant；voice 始终最高优先级。

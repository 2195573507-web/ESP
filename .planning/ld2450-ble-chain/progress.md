# LD2450 BLE 链路进度

## 2026-07-18

- 完成工作区与现有雷达模块基线核验；未改任何运行代码或配置。
- 识别到实现真实固定 BLE 绑定的唯一外部前提：每台雷达的地址类型、Service UUID、Notify UUID（以及需要时的 Write UUID）。
- 执行 host 基线：C51/C52 均因 codec 未输出 S3 v2 协议头而失败；S3 parser、ingest、spatial 和 recovery host tests 均通过。
- 只读串口识别确认两条 USB 端口对应同一 ESP32-S3，尚无连接的 C5 可作 BLE 真机验证。
- 已实现 C51/C52 fixed-MAC NimBLE transport、CCCD notify subscribe、required logs、edge filter 与 v3 codec；公共 C5 雷达文件 parity PASS。
- 已实现 S3 `radar_ingest` v3 adapter、APP CPU latest-frame worker、64-entry PSRAM history 和 `/local/v1/radar/debug`；未改写既有 spatial modules。
- 2026-07-18 验证 PASS：C51 build、C52 build、S3 build；所有 C5/S3 radar host tests；`git diff --check`。
- 最终验收阻塞于外部 C5/LD2450 硬件及尚未采集的 GATT UUID；报告 `radar-chain-audit-report.md` 已记录边界。

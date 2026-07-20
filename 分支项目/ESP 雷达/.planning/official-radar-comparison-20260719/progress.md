# 研究进度

## 2026-07-19

- 确认用户目标为只读研究 + 新增对比文档。
- 确认资料实际路径是 `/Users/zhiqin/Documents/ESP 资料`，不是 `/Users/zhiqin/ESP 资料`。
- 建立独立规划目录，避免污染工作区已有多轮计划。
- 已盘点官方资料类型和项目雷达模块入口。
- 读取官方 STM32 解析工程、Android LD2450 命令/BLE/区域处理源码、Windows 工具文件清单，并核对现行 S3 parser/service/spatial/tracker/zone/codec/remote ingest/macOS debugger。
- 确认官方可读示例是当前帧三槽解码与派生距离/角度，未证明主控跨帧人物 ID；确认 Android 同时混合多种 HLK 雷达型号，不能将所有命令/能力归给 LD2450。
- 新增 `docs/HLK-LD2450官方工具与ESP雷达项目全方位对比-2026-07-19.md`：608 行，覆盖协议、数据处理、轨迹、人数、存在/运动、区域、BLE、容错、可视化、验证矩阵、风险和下一步。
- 完成文档路径和资料路径存在性检查，完成 `git diff --check`，未改运行代码。

## 下一步

本研究交付已完成；后续若接入真实 LD2450，可按文档第 14 节执行同源硬件对照，不应把当前 host/build 证据升级为真机精度结论。

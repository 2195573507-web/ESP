# ESPS3 启动阶段内存安全审计与修复

## 目标
定位并修复 ESPS3 启动阶段的 heap 越界、元数据破坏、use-after-free、任务栈和队列契约问题，保留完整功能、heap integrity、stack monitor、diagnostics，并完成可复现的构建验证。

## 阶段
- [x] 启动链路审计：`app_main` 到各模块 init/task/worker
- [x] 全局内存、任务、日志、队列/ringbuffer 审计
- [x] 根因修复与分段 heap 检查增强
- [x] 配置和主机级验证
- [x] 生成 `s3_complete_memory_audit.md`

## 当前状态
ESPS3 主机测试与 ESP-IDF 5.5.4 构建通过。物理板卡 30 次冷启动仍需串口验收。

## 本轮目标：S3 ingress / internal-DMA 审计修复（2026-07-22）
- [x] 汇总 S3 sensor ingress 内存所有权链与阶段诊断
- [x] 收敛 sensor body / parser / ingress 的重复拷贝，保持 C5/C52 既有语音与 HTTP backoff 契约
- [x] 完成 S3 常驻 internal allocation、event bus drop 与 UART radar lifecycle 审计
- [x] 完成 C5 HTTP reset 恢复路径审计，不重构 C5 网络架构
- [x] 仅构建实际修改的 ESPS3、ESPC51、ESPC52 工程并记录 warning
- [x] 生成本轮开发日志、项目索引与最终报告

### 记录
- 任务约束来自 `/Users/zhiqin/.codex/attachments/649993ac-9e15-44e2-ac89-e73e9945b905/pasted-text-1.txt`。
- 活动源码显示 sensor handler 的 body、ingress allocation 使用 PSRAM；cJSON 校验/worker 仍存在多次解析，需结合 agent 审计结果决定是否窄化。
- 本轮验证边界：不 flash、不 monitor、不宣称硬件验收。
- 完整记录：`docs/development_logs/task_s3_sensor_ingress_internal_dma_repair_2026-07-22.md`。
# 2026-07-23 C5 startup chain refactor

- [x] Audit current C51/C52 LCD, Wi-Fi, BME, Radar, scheduler, and Voice order
- [x] Move local sensors, EventBus/Queue/Dispatcher, and local MIC/VAD before Wi-Fi
- [x] Bound initial Wi-Fi wait and add OFFLINE_MODE continuation
- [x] Keep Radar BLE enabled and preserve network upload gates
- [x] Complete integrated C51/C52 isolated build evidence

### Integrated build evidence (2026-07-23)
- C51: `/tmp/codex-final-c51-build`, full `1848/1848`, build complete; app
  binary `0x22b920` and 57% partition free.
- C52: `/tmp/codex-final-c52-real-build`, full `1850/1850`, build complete; app
  binary `0x23f020` and 55% partition free.
- BME/system runtime workers now start before `NETWORK_START`; deferred task is
  retained only as a retry if local worker allocation fails.
- Validation boundary: build only; no flash, monitor, hardware, or runtime
  acceptance.

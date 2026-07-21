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

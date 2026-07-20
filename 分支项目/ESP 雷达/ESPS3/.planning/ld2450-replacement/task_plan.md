# LD2450 替换 WiFi CSI 执行计划

## 目标

严格执行 `docs/ESP雷达_LD2450替换WiFi_CSI_最终实施计划.md`，仅修改 `ESPC51/`、`ESPC52/`、`ESPS3/`，完成全部软件阶段并如实隔离硬件验收项。

## 阶段

- [complete] C5-P0 / S3-P0：live-source、active CSI、dirty worktree、构建和资源基线
- [complete] C5-P1：移除 C51/C52 active CSI 采集、算法、worker、接口、配置和 CMake
- [complete] C5-P2：加入同构 LD2450 parser、UART、配置事务和 presence 状态机
- [complete] C5-P3：接入 C51/C52 runtime，雷达失败保持非关键、语音仅 gate HTTP
- [complete] C5-P4：加入 latest-only `POST /local/v1/radar/state` client/codec
- [complete] C5-P5：C51/C52 公共雷达源码 parity
- [complete] S3-P1：移除 trigger、ingest、fusion、queue/cache、upload 和 endpoint
- [complete] S3-P2：加入 radar payload parser 与 C51/C52/S3_LOCAL 三源 registry
- [complete] S3-P3：加入受身份/session/sequence gate 的 `/local/v1/radar/state`
- [complete] S3-P4：接入 S3 本地 LD2450，不经过 HTTP
- [complete] S3-P5：加入三源受限诊断与独立 freshness/UNKNOWN
- [complete] TEST：parser、presence、codec、ingest host/offline 测试
- [complete] VERIFY：三固件 build、parity、CSI/API/公网负向扫描、资源和范围审计
- [complete] DELIVER：执行日志、资源报告、修改/删除清单与 HW-P1..P4 blocked 验收表

## 硬约束

- 不修改 `ESP-server/`、Dashboard、数据库、`managed_components/`、原 `ESP-111` 或雷达内部固件。
- 不猜测 UART GPIO；三块板仅引用各自独立板级配置。
- 不执行 flash、monitor、erase-flash，不启动 Server。
- 保留 BME、WiFi、注册、心跳、状态、命令、语音、WakeNet、Mic、Speaker、LCD/LVGL 与 C5 只访问 S3 `/local/v1/*` 的合同。
- 现有工作树改动属于用户；不得回退或覆盖。

## 错误记录

| 错误 | 次数 | 处理 |
|---|---:|---|
| 新建 `/goal` 时发现任务已有 active goal | 1 | 读取并沿用现有目标 |
| 三套现有 `build/` 均绑定 `/Users/zhiqin/ESP-111/...`，基线 build 被 CMake cache 拒绝 | 1 | 不执行被禁止的 fullclean；改用独立 `build-radar-baseline/` |
| 相对 `-B build-radar-baseline` 从仓库根解析，三套并发构建共享目录并互相污染 | 1 | 隔离错误目录到 /tmp，恢复 lockfile，后续使用各工程绝对 build 路径 |
| parser resync 测试把伪帧头产生的坏候选数错误限定为 1 | 1 | 改为至少 1，继续验证只发布一个恢复后的完整帧 |
| S3 ingest host test runner 的 LD2450 include 路径少了 `components/` | 1 | 修正为工程根下 `components/radar_ld2450/include` 后重跑 |
| 三套既有 core test runner 没有 executable bit，直接调用返回 permission denied | 1 | 保持元数据不变，按现有权限使用 `sh run_host_tests.sh`，三套均通过 |
| 首轮最终 CSI 扫描的 include glob 后置，重新包含了 tests，并命中负向测试与显式禁用行 | 1 | 将 exclude glob 后置，并把 active `=y` 与允许的 `not set`/拒绝测试分开验证 |
| zsh 中使用只读变量名 `status` 保存 `rg` 退出码 | 1 | 改用普通变量 `code` 后重跑，C51/C52 启用值零命中 |
| `check-complete.sh` 不识别本计划的 `[complete]` 状态格式并输出 `11/0` | 1 | 以阶段列表中零条未完成状态的直接扫描作为完成判定 |

# LD2450 替换执行日志

## 2026-07-16

- 已读取强制流程技能并沿用当前 active `/goal`。
- 已确认允许和禁止修改边界、独立仓库根目录及 dirty worktree。
- 已完整阅读最终实施计划全部 1288 行，并将 C5-P0..P5、S3-P0..P5、TEST、VERIFY、DELIVER 写入执行清单。
- 已决定迁移报告写入 `ESPS3/docs/`，避免修改未获授权的顶层 `docs/`。
- 已完成第一轮 active CSI 文件/CMake/sdkconfig/启动入口扫描，并记录共享文件与 CSI-only 文件边界。
- 三套默认 `build/` 因仍绑定原 `ESP-111` 路径而拒绝构建；保留现场并切换到独立 baseline build 目录。
- 已隔离误用的根级共享 build 目录并恢复由该错误配置改写的 C51/C52 `dependencies.lock`；后续仅使用工程内绝对 build 路径。
- 已完成 S3 trigger/ingest/fusion/upload 调用链和 child/session 身份 API 基线阅读。
- 未执行任何 flash、monitor、erase-flash 或 Server 启动操作。
- 隔离 baseline build 全部通过：C51/C52 bin 1376976 bytes、HP SRAM 196880 bytes；S3 bin 1132640 bytes、DIRAM 175375 bytes。
- 已在 C51/C52/S3 加入独立 `radar_ld2450` parser、presence 状态机、UART/config transaction、snapshot service 与固定上限 JSON codec；UART GPIO 未猜测并保持板级禁用。
- 已完成 C51/C52 runtime 与 latest-only `/local/v1/radar/state` 接入，三套 parser/state/codec host tests 通过。
- 已完整移除 C51/C52 active CSI 文件、CMake、启动、worker、event/resource/protocol 链路；S3 CSI 拆除已进入 network worker/scheduler 收尾。
- 已完成 S3 active CSI 的 network worker、scheduler、local HTTP、stream gateway、gateway config 收尾，并通过拆除后的隔离 S3 build。
- 已新增 S3 radar protocol、三源 registry、remote identity/session admission、本地 adapter、低频 diagnostics 与 `/local/v1/radar/state` handler；开始执行 ingest host tests。
- 三套 parser/presence/codec host tests 与 S3 protocol/registry/ingest host tests 全部通过。
- C51/C52 公共雷达源码和 client parity 通过；CSI active 源码词表扫描与通用扫描均为零命中。
- C51/C52 当前及默认 Kconfig 已显式关闭 WiFi channel-state capture；相关采集/算法/链路源码已物理删除。
- 已重新完整核对最终实施计划 1288 行和三份交付报告；报告数据与当前 build 产物一致。
- 最终 `git diff --check -- ESPC51 ESPC52 ESPS3` 通过；修正排除顺序后的 required/general active CSI 扫描均为零命中，C51/C52 `CONFIG_ESP_WIFI_CSI_ENABLED=y` 为零命中。
- C5 `/api/` 为零命中；完整 URL 仅为 S3 本地网关 URL 构造/拒绝外部完整 URL 的 guard，以及未改动的依赖 registry 元数据。
- 已重新执行 C51、C52、S3 radar core host tests 和 S3 protocol/registry/ingest host tests，四组全部 PASS；随后用 `unlink` 删除四个临时测试二进制。
- C51/C52 radar core/client parity 再次通过；三套 final build dry-run 未调度 app 源码编译或链接，现有最终 bin 与当前源码同步。
- `managed_components`、dependency lock、`ESP-server/public`、`ESP-server/db` 状态检查均无本任务差异；ESP-server 保留原有 26 条 dirty 状态，未被本任务修改或启动。
- VERIFY 与 DELIVER 已完成。软件实施结论为 COMPLETE；UART 引脚和 HW-P1..P4 真机验收继续如实标记 BLOCKED。

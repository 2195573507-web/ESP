# C5/S3 Code Smell Audit Report - 2026-07-09

## Executive Summary

结论：不是“全项目都是屎山”，但 S3 网关侧已经出现明显的结构性堆叠风险；C5 侧主要问题不是单文件混乱，而是 C51/C52 镜像复制导致长期维护容易漂移。

当前最需要盯住的是 S3 的 `network_worker.c`、`s3_scheduler.c`、`protocol_adapter.c`。这些文件把网络状态、Server 上云、队列调度、CSI latest cache、命令拉取/ACK、协议兼容和诊断日志挤在少数 C 文件里。代码里有不少防护，但复杂度已经高到后续改动容易引入隐藏时序 bug。

## Audit Scope

- 扫描对象：`ESPC51`、`ESPC52`、`ESPS3`。
- 排除：`build/`、`managed_components/`、`.vscode/`、`.devcontainer/`。
- 本次只读审计；除本文档、notes、task-plan 外未改固件源码。
- 未执行 build、flash 或硬件闭环。

## Baseline Findings

- 自有 C/H 代码约 56,346 行，254 个项目自有 C/H/CMake/config 文件。
- 当前工作区并不干净：C5/S3 有 66 个 tracked 文件已修改，统计为 4,749 insertions / 1,062 deletions；另有 16 个未跟踪 C5/S3 运行时相关文件，合计 3,107 行。
- 所以下面的问题应理解为“当前工作区状态的维护风险”，不是干净 main 分支的最终结论。

## P0 - 暂无必须立刻阻断的硬错误

这次扫描没有发现 C5 直接构造 `/api/...` Server 路由，也没有发现 C5 绕过 S3 直连公网 Server 的明显路径。`ESPC51/components/Middlewares/server_comm/server_comm_config.c:79` 开始的 `server_comm_build_url()` 会拒绝 `http://`、`https://`，并强制 endpoint 属于 `/local/v1`。

CSI raw/subcarrier 泄漏也有显式拦截。比如 `ESPS3/components/Middlewares/protocol_adapter/protocol_adapter.c:667` 附近会拒绝 `raw_csi`、`subcarrier_data`、`selected_subcarriers`、`iq`、`phase` 等字段。

## P1 - S3 运行时职责堆叠已经偏重

`ESPS3/components/Middlewares/network_worker/network_worker.c` 约 2,036 行。文件注释说它负责网络状态 worker 和 Server 上云 gate，但实际还同时包含：

- STA 连接/重连和稳定窗口。
- Server health/failure/recover 计数。
- upload worker、command worker、event worker 三套队列。
- CSI latest JSON 缓存、节流、离线 dirty 标记、上云重试。
- BME cache in-flight 管理。
- snapshot、command pull、command ack、smart-home poll。

证据：该文件开头的全局状态从 `s_event_queue` 到 `s_low_priority_coalesce_count` 集中在 `network_worker.c:168` 到 `network_worker.c:218`；`network_worker_init()` 在 `network_worker.c:1815` 同时创建三条任务和三类队列；`enqueue_csi_latest_upload_if_needed()` 在 `network_worker.c:885` 负责 CSI latest 复制、节流、backlog drop 和入队。

风险：后续任何一个小需求，例如“CSI 上云更快一点”或“命令 ACK 不要丢”，都可能牵动同一个文件里的队列压力策略、Server gate、内存所有权和日志节流。代码不是不可读，但已经进入高耦合维护区。

建议：把 `network_worker.c` 拆成窄边界模块：`network_link_state`、`server_upload_queue`、`csi_upload_cache`、`command_sync_worker`。先不重写行为，只搬迁静态函数和状态，保持 public API 不变。

## P1 - `s3_scheduler.c` 仍是“大总管”

`ESPS3/components/Middlewares/runtime/s3_scheduler.c` 约 2,094 行。它目前既是 scheduler facade，又管 protocol worker、stream worker、CSI fusion worker、runtime latest state、周期 tick、事件优先级和网络/voice gate。

证据：`s3_scheduler.c:165` 到 `s3_scheduler.c:201` 集中保存 protocol/CSI/stream task handle、device/sensor/event state、网络状态、voice busy、各种 cadence 和 drop/coalesce 计数；`csi_fusion_worker_task()` 在 `s3_scheduler.c:1042`，`protocol_worker_task()` 在 `s3_scheduler.c:1094`，`stream_worker_task()` 在 `s3_scheduler.c:1169`，三类 worker 都在同一文件。

风险：当前 worker 拆分已经有了，但实现仍集中在同一个 `.c`，所以“架构上分层、代码上不分层”。后面要查卡顿、丢包、CSI fusion 延迟或 voice busy gate，会在一个文件里横跳。

建议：保留 `s3_scheduler.c` 作为 facade，把三个 worker 的队列和 task loop 分到 `s3_protocol_worker.c`、`s3_stream_worker.c`、`s3_csi_fusion_worker.c`，只把调度策略留在 scheduler。

## P1 - C51/C52 镜像复制维护成本高

C5 侧不是混乱，而是复制。扫描到 ESPC51/ESPC52 有 99 个成对项目路径，其中 97 个字节级相同，只有 `server_comm_config.h` 和 `terminal_config.h` 因设备身份不同而差异。

这说明 C51/C52 的公共逻辑仍在两个工程里复制。短期好处是 parity 很直观；长期风险是任何单边修复都可能悄悄漂移，尤其是 CSI、BME、voice、server_comm 这类跨工程路径。

建议：保留两个工程入口和身份配置，但把字节级相同的 `Middlewares` 公共逻辑逐步下沉到 `shared_components` 或生成式同步机制。至少先加一个 parity check 脚本，把“除身份头外必须一致”变成 CI/本地检查。

## P2 - S3 `protocol_adapter.c` 协议兼容逻辑偏厚

`ESPS3/components/Middlewares/protocol_adapter/protocol_adapter.c` 约 1,431 行，内部同时处理本地短字段解析、BME payload、CSI v2 payload、compact CSI 兼容、Server ingest JSON、CanonicalEvent v2 JSON 和 link 映射。

证据：`protocol_adapter_fill_csi_v2_payload()` 和 `protocol_adapter_fill_compact_csi_result_payload()` 都超过 100 行；文件顶部还硬编码了 local child 映射和 CSI link 映射。

风险：协议适配器一旦继续承担“兼容旧格式 + 生成新格式 + 校验 CanonicalEvent”，很容易变成所有协议变更的汇合点。好处是边界集中，坏处是会越来越难测试。

建议：拆出 `protocol_adapter_csi.c` 和 `protocol_adapter_bme.c`，保留主文件只做 envelope dispatch 和公共工具。拆分前先补 JSON golden tests，避免协议字段漂移。

## P2 - 固件默认配置仍有明文和环境绑定

默认 SoftAP SSID/password 仍在共享协议头中：`ESPS3/components/esp111_protocol_common/include/esp111_protocol_common.h:39` 到 `:40`。S3 默认 Server URL 在 `ESPS3/components/Middlewares/gateway_config/gateway_config.h:73` 到 `:74`，指向公网 IP。`GATEWAY_CONFIG_AUTH_TOKEN` 默认空字符串。

这不一定是当前阶段的上线漏洞，因为可能靠编译覆盖或 NVS 配置，但从代码卫生角度属于“容易误烧正式固件”的风险。

建议：把公网 Server URL、SoftAP password、auth token 改成构建配置或 NVS provisioning 的必填项；开发默认值保留在 example 文件，不放进生产默认头。

## P2 - Placeholder/deprecated 仍多，需设置清理线

代码里仍有 `display_placeholder`、`csi_placeholder_gateway`、`deprecated stream CSI rejected`、`mock` 相关注释和兼容拒绝路径。部分是刻意保留的边界保护，不是错误；但如果长期不清理，会让后续读代码的人分不清“真实能力”“兼容壳”“已废弃路径”。

建议：给 placeholder/deprecated 路径分类：

- 必须长期保留的接口门面：写进架构文档。
- 只为兼容旧数据的拒绝路径：加删除日期或 issue。
- 真实未实现能力：不要在 capabilities 里表现得像已实现。

## Good Signs

- C5 没有直接扫到 Server `/api/...` 路由构造，边界基本守住。
- S3 对 raw/subcarrier CSI 有显式拒绝逻辑。
- C51/C52 当前 parity 很高，说明镜像维护至少还没有明显失控。
- 队列压力、drop、coalesce、diagnostic log 已经有可观测点，后续重构有抓手。

## Recommended Next Steps

1. 先给 C51/C52 加 parity check，保护现有镜像一致性。
2. 把 S3 `network_worker.c` 的 CSI latest cache 和 upload queue 拆出，不改行为。
3. 把 `s3_scheduler.c` 的三个 worker task loop 拆文件，scheduler 只保留事件策略。
4. 给 `protocol_adapter` 增加 JSON golden tests，再拆 CSI/BME 子适配器。
5. 把固件默认公网 URL、SoftAP password、auth token 迁移到 build/NVS/provisioning 配置。

## Verification Commands Used

```bash
find ESPC51 ESPC52 ESPS3 \( -path '*/build/*' -o -path '*/managed_components/*' -o -path '*/.vscode/*' -o -path '*/.devcontainer/*' \) -prune -o -type f \( -name '*.c' -o -name '*.h' -o -name 'CMakeLists.txt' -o -name 'sdkconfig.defaults' \) -print
find ESPC51 ESPC52 ESPS3 \( -path '*/build/*' -o -path '*/managed_components/*' -o -path '*/.vscode/*' -o -path '*/.devcontainer/*' \) -prune -o -type f \( -name '*.c' -o -name '*.h' \) -print0 | xargs -0 wc -l
git diff --stat -- ESPC51 ESPC52 ESPS3
git diff --name-status -- ESPC51 ESPC52 ESPS3
git ls-files --others --exclude-standard -- ESPC51 ESPC52 ESPS3
diff -qr ESPC51/components/Middlewares ESPC52/components/Middlewares
rg -n "ESP111_PROTOCOL_SERVER_ROUTE|/api/|/kernel/|http://|https://" ESPC51 ESPC52 -g '*.{c,h,defaults}' -g '!**/build/**' -g '!**/managed_components/**'
rg -n "raw_csi|subcarrier_data|selected_subcarriers|iq|phase|deprecated|placeholder" ESPC51 ESPC52 ESPS3 -g '*.{c,h,defaults}' -g '!**/build/**' -g '!**/managed_components/**'
```

## Limits

- 没有编译，所以不能证明当前脏工作区可构建。
- 没有硬件运行，所以不能证明队列压力策略在真实 WiFi/CSI/Server 波动下稳定。
- 函数长度统计是轻量 brace-count 辅助，不作为编译器级精确 AST 结果。

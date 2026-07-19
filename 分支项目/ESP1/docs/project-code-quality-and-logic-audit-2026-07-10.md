# ESP-111 代码质量与逻辑优化审计报告

审计日期：2026-07-10  
审计对象：`ESPC51`、`ESPC52`、`ESPS3`、`ESP-server`、正式链路相关本地工具  
执行边界：只读源码审计与无副作用静态检查；未修改业务源码、前端、数据库或运行配置，未 build、flash、monitor，也未启动真实服务。

## 1. 结论先说

**有局部“屎山”，但不是整个项目都是。**

最符合“结构性屎山”定义的是 S3 的三个模块：

| 模块 | 当前行数 | 为什么不是单纯“大文件” |
|---|---:|---|
| `ESPS3/components/Middlewares/network_worker/network_worker.c` | 2473 | 同时拥有链路状态、Server health、上传/命令队列、CSI latest、BME 上传、ACK、资源释放和多个 task 的状态 |
| `ESPS3/components/Middlewares/runtime/s3_scheduler.c` | 2167 | 同时承担 ingress、event policy、protocol/stream/CSI worker、周期调度和大量 runtime latest state |
| `ESPS3/components/Middlewares/protocol_adapter/protocol_adapter.c` | 1574 | 同时解析 BME、两代 CSI、本地短协议、校验 CanonicalEvent 并构造 Server JSON |

它们的问题不是命名或行数，而是**一个小需求会穿过多个共享静态状态、队列 owner 和协议分支**。当前已经出现与这种耦合直接相关的竞态、无限等待、状态误释放和错误语义丢失。

其他部分的判断：

- **C51/C52 不是乱，而是复制。** 两边 `Middlewares` 各 79 个 C/H 文件，目前只有两个身份配置头不同，说明当前 parity 很高；风险是未来单边修复静默漂移。
- **ESP-server 有多个单体热点，但尚可分域治理。** `dashboardService.js` 1367 行、`agent/stateStore.js` 1021 行、`smoke-regression.js` 3749 行；核心问题分别是职责聚合、跨领域状态和不可筛选的串行测试。
- **前端与 CSI 调试工具属于低优先级维护债务。** 它们不是当前系统可靠性的第一矛盾。

优先级总览：

| 优先级 | 结论 | 处理原则 |
|---|---|---|
| 条件性 P0 | 2 组 Server 安全边界；仅当端口可被非受信客户端访问时升为 P0 | 上线/公网暴露前必须 fail-closed |
| P1 | 12 组确定逻辑或并发缺口 | 先补不变量、幂等和隔离，再拆结构 |
| P2 | 7 组结构与工程治理债务 | 建合同测试后渐进拆分，不做大重写 |
| P3 | 2 组局部卫生问题 | 随功能迭代顺手收敛 |

## 2. 审计口径与现场基线

### 2.1 什么才算问题

- **确认缺陷**：当前源码能给出明确触发条件和错误结果。
- **结构性债务**：不一定马上报错，但已显著放大修改、排障和验证成本。
- **逻辑优化**：不改变外部合同即可减少队列压力、写放大、重复副作用或故障窗口。
- **运行假设**：必须用临时服务、故障注入或真机验证，报告不把它冒充成已复现故障。

本报告不按行数定罪，也不把 placeholder、兼容拒绝路径或必要状态机机械判成坏代码。

### 2.2 当前工作区

- 顶层仓库是 `main`，嵌套 `ESP-server` 是独立 `api` 仓库，两者都有既有未提交状态。
- 顶层当前约 90 个相关文件变化，约 `+9991/-3479`；S3 还有多组已进入 active CMake tree 的未跟踪模块。
- 顶层 Git 追踪 95 个 `ESP-server` 文件，嵌套 Git 追踪 94 个；嵌套仓库当前只显示真实 `db/database.db` 变化，而后端源码变化出现在顶层视图。
- 因此本报告描述的是 **2026-07-10 live snapshot**，不把既有改动归因于本轮，也不声称等同于干净 `main/api`。

## 3. 条件性 P0：部署暴露时必须阻断

### P0-1 Gateway 鉴权默认 fail-open，凭据也未绑定 gateway

证据：

- `ESP-server/src/services/gatewayAuthService.js:82-120`：未配置 token 时，任意非空 `gateway_id` 都返回 `ok:true, auth_required:false`。
- 同文件 `132-149`：每次请求会把 `gateway_auth.enabled` 写回 `1`，数据库禁用状态不能作为可靠 revoke。
- `ESPS3/components/Middlewares/gateway_config/gateway_config.h:73-79`：默认 Server 为明文 HTTP，auth token 为空。
- `ESP-server/.env.example:1-42`：没有列 gateway/admin 安全配置或安全 bind 要求。

触发条件：生产环境未注入 gateway secret，且 Server 可被非受信客户端访问。

影响：攻击者只声明 gateway ID 即可伪造 BME、CSI、snapshot、event 和 command ACK；共享 token 也无法证明 token A 属于 gateway A。

改进：

1. production profile 缺凭据时拒绝启动。
2. 每 gateway 独立 token hash/enabled 或 mTLS；token 必须绑定 gateway ID。
3. 仅显式 `ALLOW_INSECURE_DEV_GATEWAY=1` 且 loopback/隔离网允许无认证。
4. `recordGatewaySeen()` 只更新时间，不得自动恢复 `enabled`。

验收：无 secret 的 production 启动失败；错 token 401；disabled gateway 403；A token 冒充 B 被拒绝；拒绝请求不刷新任何 last_seen。

### P0-2 控制、删除、隐私和计费端点缺统一 RBAC，legacy 写入口绕过真源

证据示例：

- command create：`ESP-server/src/routes/commandRoutes.js:162-175`
- smart-home control：`ESP-server/src/routes/smartHomeRoutes.js:66-80`
- log cleanup/delete/SSE：`ESP-server/src/routes/eventRoutes.js:150-192`
- Server 统一挂载但无全局主体/RBAC：`ESP-server/server.js:95-140`
- legacy sensor/time/voice identity 路径仍以 body/header/IP 识别调用者；例如 `ESP-server/src/routes/sensorRoutes.js:116-178`、`ESP-server/src/routes/recordRoutes.js:25-64`。

触发条件：这些路由能被非受信客户端直接访问，且外部反向代理没有强制认证。

影响：匿名调用者可下发物理动作、删除事故证据、污染状态/记忆，或消耗 LLM/voice 资源；legacy 写还可绕过 v1 gateway binding 覆盖 Server truth。

改进：中央 route policy matrix，主体至少分 `gateway/operator/admin/read_only/internal_job`；所有写路由显式进入 allowlist；legacy 写仅允许 loopback/迁移代理并设下线日期；删除操作改为有 actor 的 tombstone/retention。

验收：路由清单自动扫描；匿名只允许静态资源和最小 liveness；所有写操作都有 identity、role、request_id、审计事件和限流。

## 4. P1：已确认的逻辑与并发缺口

### P1-1 S3 本地 C5 身份只“声明后学习”，未绑定真实 peer

证据：

- `ESPS3/components/Middlewares/local_http_server/local_http_server.c:506-565` 从 body 取 local/device ID，再读取 peer IP，未在 handler 边界验证二者既有绑定。
- pending GET `:821-852` 只按 query `id=1/2` 返回对应命令。
- ACK 校验 `:637-687,855-915` 只验证 body ID 属于允许集合，没有把 socket peer、target device 和 command entry 原子绑定。
- `ESPS3/components/Middlewares/voice_proxy/voice_proxy.c:269-303` 只校验 `X-Device-Id` 在 allowlist，即可占用全局 voice session。
- `ESPS3/components/Middlewares/child_registry/child_registry.c:279-318,342-393` 允许声明的 device 覆盖 peer IP/MAC 映射。

触发条件：进入共享 SoftAP 的客户端伪造另一台 C5 的 ID。

影响：可读取另一设备命令、伪造 ACK、抢占 peer mapping 或 voice_busy；固定共享 PSK 只能证明“在网内”，不能区分 C51/C52。

改进：每台 C5 provision 独立 secret/certificate；local envelope 使用 nonce/seq + HMAC；所有 register/ingress/pending/ACK/voice 强制绑定 `authenticated device -> peer -> session generation`。MAC 可作辅助信号，不能单独当认证。

验收：C52 peer 伪造 C51 的 register/pending/ACK/voice 全部 403，且 registry/resource/command state 不变化。

### P1-2 命令链是 at-least-once dispatch + best-effort ACK，存在重复执行窗口

证据：

- `ESPS3/components/Middlewares/command_router/command_router.c:533-540` 在 Server ACK 入队前先把本地 entry 标为 `ACKED`。
- 同文件 `:107-116` 允许新命令立即复用 ACKED/TIMEOUT 槽，短期去重记录随之消失。
- 同文件 `:589-620` 把解析出的 `error/message` 用 `snprintf` 重拼 JSON，且 enqueue 失败仍返回 `ESP_OK`。
- `ESPS3/components/Middlewares/network_worker/network_worker.c:2438-2463` 用 `portMAX_DELAY` 入 ACK queue。
- 同文件 `:1889-1913` 对非 2xx 只记录；多数普通网络错误不会重排。
- Server `ESP-server/src/commands/queue.js:320-500` 会重领超时 dispatched 命令；Server 终态 ACK 幂等无法覆盖“C5 已执行、ACK 丢失”的窗口。

影响：C5 已执行 -> S3 ACK 丢失 -> Server 重发 -> C5 再执行；非幂等物理动作可能重复。队列满时，本地 HTTP control handler 还可能无限阻塞。

改进：C5 建 `command_id` 执行 journal；S3 ACK 先写可恢复 outbox 后才向 C5 成功；有界 queue wait；仅 Server 幂等终态确认后清 journal；用 cJSON 构造 ACK；统一 lease/deadline/max_attempt/dead-letter。

验收：注入 queue full、断网、timeout、500、S3/C5 重启和响应丢失，同一 `command_id` 业务执行次数始终为 1，最终状态可收敛并保留 attempt。

### P1-3 S3 resource lifecycle 可被迟到 ingress 和未知 station 断开破坏

证据：

- `ESPS3/components/Middlewares/resource_manager/resource_manager.c:316-360` 在检查 `observed_at_ms < restore_not_before_ms` 之前，先调用 `child_registry_update_peer_ip()`。
- `ESPS3/components/Middlewares/network_worker/network_worker.c:1347-1382`：单个 AP station disconnect 无法映射时直接 `resource_manager_release_all()`。

触发条件：断联前的旧 ingress 延迟到达，或任意未注册/映射暂失的 station 断开。

影响：旧请求即使随后被判 stale，也已改写 peer mapping；一个未知 station 可释放两台合法 C5 的 command/sensor/CSI 资源。

改进：先在 manager lock 下验证 generation/timestamp，再提交身份映射；单 station mapping miss 只记录并等待对应 peer timeout，只有 SoftAP stop/global reset 才允许 release-all。

验收：注入迟到 ingress 与未知 station connect/disconnect，合法 session generation、peer mapping 和 resource state 均不变化。

### P1-4 S3 Server health 存在双套无锁状态和语义折叠

证据：

- `ESPS3/components/Middlewares/network_worker/network_worker.c:180-225,509-553` 的 ready/counter/error/pending 普通全局由 probe、upload、command 等路径更新。
- `ESPS3/components/Middlewares/offline_policy/offline_policy.c:18-87` 另存一套无锁 `available/last_error/failure_count`。
- 同文件 `:31-51` 把 409/429/普通 4xx 映射为业务错误，但 `record_server_result()` 仍把所有非 2xx 写成 unavailable。

影响：双核任务出现 lost update/撕裂快照；一个业务 4xx 可覆盖 transport readiness；pending flag 竞态可能重复或漏调度。`/local/v1/health` 只展示 last-result，不能代表完整 readiness。

改进：network supervisor 单 owner 消费 typed result event；拆分 transport、Server readiness、business rejection 和 per-endpoint health；其他 worker 不直接写共享状态。

验收：并发注入 probe 200、command 409、upload timeout，状态稳定且各域指标互不覆盖。

### P1-5 S3 voice 与 control plane 共用同步 HTTP task

证据：

- `ESPS3/components/Middlewares/local_http_server/local_http_server.c:924-949` 把 health/register/sensor/CSI/command/voice 挂在同一 ESP-IDF HTTP server。
- `ESPS3/components/Middlewares/voice_proxy/voice_proxy.c:263-401` 在同步 handler 内完成整段 PCM 接收、上游 turn 和流式回传。

触发条件：任一 voice turn 慢、超时或上游持续流式返回。

影响：另一台 C5 的 health/register/command/sensor 请求无法及时服务；后台 voice gate 不能解决 HTTP handler 的 head-of-line blocking。

改进：使用异步 handler 或独立 voice worker/连接，把长请求移出 control HTTP task；为 control plane 保留独立并发和延迟预算。

验收：C51 90 秒 voice 时，C52 health/register/command p99 仍在预算内。

### P1-6 C5 CSI latest-only 数据与 FIFO 事件语义冲突

证据：

- `ESPC51/components/Middlewares/runtime/c5_event_bus.h:21-32`：CSI、BME、heartbeat、status、command 共用 24 深度 FIFO；C52 同构。
- `ESPC51/components/Middlewares/sensor_domain/csi_placeholder/csi_service.c:170-179`：CSI callback 每帧都投一个 event，raw sample 本身却只有 latest slot；C52 同构。
- `ESPC51/components/Middlewares/runtime/c5_runtime_workers.c:70-123`：worker queue 满直接 drop；CSI compute 与最长 5 秒的 HTTP report 在同一 worker 串行；C52 同构。

影响：高频 callback 或慢 HTTP 会先填满共享 bus，再挤压 BME/control；大量事件可能对应同一个 latest sample，造成空转和修改时序放大。

改进：CSI ingress 用 pending bit/task notification/长度 1 overwrite queue；control plane 独立可靠容量；compute 与 egress 拆 worker，egress 只发送 latest feature。

验收：500 Hz callback + CSI endpoint 5 秒延迟时 control drop=0、command latency 有界、内存恒定、CSI 只增加 coalesce。

### P1-7 C5 WiFi 短抖动可保留旧 `LINK_READY`

证据：

- `ESPC51/components/Middlewares/server_comm/gateway_link.c:243-261` 的 WiFi down 只改 WiFi 字段并请求 reconnect，没有立即清 link state；C52 同构。
- reconnect `:544-556` 只有 down 达稳定窗口才置 `LINK_DOWN`；重新稳定后若仍是 `LINK_READY` 会直接 continue。

触发条件：不足 down-stable 窗口的短断连后重新拿 IP，同时 S3 重启或清空 registry。

影响：C5 跳过 health/register，继续放行业务但 S3 没有有效 session。

改进：每次 DISCONNECTED/GOT_IP 增加 connection generation 并立即撤销 READY；每个新 generation 必须完成 health + register 才能 READY。

验收：小于 1 秒 WiFi bounce + S3 registry reset 后，业务请求只在重新 register 完成后恢复。

### P1-8 C5 voice pause 没有完成全链路 quiescence/cancellation

证据：

- `ESPC51/components/Middlewares/runtime/app_runtime.c:45-79` 关闭 gate 后只等待 BME paused；C52 同构。
- `ESPC51/components/Middlewares/server_comm/server_comm_http.c:275-315` 只在请求开始时检查 voice gate，已经进入 perform 的请求不会中止；C52 同构。
- `ESPC51/components/Middlewares/server_voice/server_voice_client.c:666-678,721-752`：fixed PCM open/write 完成前 stream handle 尚未发布，断联 abort 无法覆盖早期阶段；C52 同构。

影响：system/CSI HTTP 可与 voice 竞争 socket/heap/S3 handler；上传中断可能卡到长 timeout；CSI callback 还会继续积累 event。

改进：公共 non-voice in-flight lease/counter；voice 前有界 quiescence；open/write/read 全阶段使用 owner-controlled cancel token 与总 deadline；voice 期间 CSI 只覆盖 latest、不投 event。

验收：voice 与在途 CSI/system HTTP 竞争、PCM 上传中拔 S3，资源在预算内释放且 control queue 不增长。

### P1-9 BME 无效 gas 会污染 baseline，运行期也缺恢复状态机

证据：

- `ESPC51/components/Middlewares/sensor_domain/bme690/service/bme_sensor_service.c:182-219` 读取后无条件进入 AQ update 和上传；C52 同构。
- `ESPC51/components/Middlewares/sensor_domain/bme690/service/bme_air_quality.c:81-128` 只检查 gas 电阻为正，随后推进 sample_count、EMA 和 baseline；没有使用 driver 已提供的 `new_data/gas_valid/heat_stable`；C52 同构。
- `ESPC51/components/Middlewares/sensor_domain/bme690/driver/bme690.c:963-1024` 只在初始化路径 soft reset；服务运行期 read error 仅日志并退出本 tick。

影响：heater 未稳定或 gas invalid 的样本也可在累计后形成 high-confidence AQ；连续 I2C/NACK/brownout 后只能永久重试失败，不能自愈。

改进：gas baseline 只接受 `new_data && gas_valid && heat_stable`；温湿压独立标 validity。连续错误进入 `DEGRADED -> RECOVERING`，执行 reset/reconfigure/bus recovery + 指数退避。

验收：invalid gas 不增加 warmup/baseline；连续 NACK 后自动恢复并暴露 recovery_count/last_error。

### P1-10 BME/CSI ingest 缺稳定幂等键和事务边界

证据：

- BME `ESP-server/src/services/sensorBme690Service.js:218-268` 每次无条件 INSERT，`ESP-server/src/db/sensorRecords.js:5-81` 对 `request_seq` 没有唯一约束。
- CSI `ESP-server/src/services/csiMotionService.js:147-194` 读取 `trace_id/tick_id`，但 persisted fact 不保存它们；`ESP-server/src/db/csiMotion.js:5-69` 没有 event identity 唯一键。
- 两条链都按“事实 -> 状态 -> event -> 内存/SSE”多步 auto-commit，`ESP-server/src/db/sqlite.js:15-45` 没有事务 helper。

触发条件：Server 已提交但响应丢失，S3 对同一 record 重放；或后续状态/event 步骤失败后 route 返回 5xx。

影响：重复历史行、重复告警/SSE、DB 与内存 current 分叉；重试会扩大部分提交。

改进：所有可重试写带稳定 `event_id + boot_id + seq`；Server 建唯一键，在单事务内完成主事实/状态/event outbox，commit 后再更新内存和 SSE。先做幂等，再讨论 S3 NVS outbox。

验收：同一事件重复 100 次只保留一条事实、一条状态 transition 和一次 SSE；任一步故障回滚后可安全重试。

### P1-11 SQLite migration、共享事务与 SSE shutdown 缺不变量

证据：

- `ESP-server/src/db/migrations.js:70-88` 遇历史重复只 warning 并返回 false，caller 继续启动。
- `ESP-server/src/services/userDataService.js:412-509` 在全站共享 connection 上跨多个 `await` 运行 `BEGIN IMMEDIATE ... COMMIT`，没有事务 owner/互斥。
- `ESP-server/src/services/eventStreamService.js:33-83` 忽略 `res.write()` backpressure，无连接/积压上限，也没有 `closeAll()`。
- `ESP-server/server.js:196-223` 先无限等待 `httpServer.close()`，错误只打印且最终 exit 0。

影响：唯一约束可永久缺失；无关请求可能进入别人的 transaction；慢 SSE 推高内存并让关停不收敛。

改进：versioned migration 在无法恢复不变量时让 readiness 失败；事务使用专用 connection/串行 owner；SSE 有认证/配额/backpressure/close-all；关停加 deadline 和非零失败退出。

验收：重复数据 migration 明确失败并给修复报告；并发事务回滚不影响旁路写；慢消费者内存有上限；活跃 SSE 下关停在预算内完成。

### P1-12 Server 状态真源和三类 command 生命周期不统一

证据：

- `ESP-server/src/services/deviceStatusService.js:154-169,719-745` 永不按时间降级 S3-authored child，并在 timeout scan 中排除 S3 来源。
- `ESP-server/src/services/smartHomeService.js:342-367` 的 claim 只做 `queued -> dispatched`，没有 lease/attempt/deadline；同文件 `:390-451` 的 ACK 先 SELECT 再无条件 UPDATE。
- generic command、smart-home、natural-language command 各自拥有不同状态机，natural-language 目前只有 queued/list。

影响：S3 失联后 child 可长期显示 online；smart-home 可永久 dispatched；冲突 ACK 后写覆盖；不同命令域无法共享重试、dead-letter 和审计语义。

改进：后台 reconciler 将失联 gateway 的 child 转 `unknown/stale`；UPDATE 带 `last_seen <= cutoff`；统一 command contract：lease token、deadline、max_attempt、terminal CAS、dead-letter、execution idempotency token。

验收：S3 失联后 child 在阈值内转 stale；新 heartbeat 不被旧 scan 覆盖；三类命令都能从 claim/timeout/duplicate ACK/终态冲突收敛。

## 5. P2：结构与工程治理债务

### P2-1 S3 三个上帝模块应在行为门禁后拆分

推荐边界：

- `network_worker.c` -> `link_state`、`server_health`、`upload_outbox`、`csi_uploader`、`command_transport`
- `s3_scheduler.c` -> facade + `protocol_worker`、`stream_worker`、`csi_fusion_worker`
- `protocol_adapter.c` -> envelope dispatch + `bme_adapter`、`csi_adapter`、JSON common

先补 queue ownership、state transition、JSON golden 和 fault-injection tests，再搬迁静态状态与函数；禁止拆文件时同时改协议。

### P2-2 C51/C52 镜像复制与 C52 假测试

- Live parity：79 对 C/H 文件中，仅 `server_comm_config.h`、`terminal_config.h` 是预期身份差异。
- `ESPC52/components/Middlewares/sensor_domain/csi_phase_a/csi_phase_a_tests.c:97-190` 仍硬编码 `C51`、`S3_TO_C51`、local ID `1`。
- 真实 C52 client 在 `ESPC52/components/Middlewares/sensor_domain/csi_placeholder/csi_server_client.c:34-42` 动态选择 C52/2，因此该测试无法发现 C52 identity/link 回归。

优化：公共 C5 component 只维护一份，板级工程注入 identity/config；过渡期至少加 parity allowlist。CSI contract test 参数化 `{device_id, local_id, link_id}` 并在两 target 执行。

### P2-3 后端 service/test/frontend 单体

- `dashboardService.js` 1367 行：snapshot normalization、cache/restore、CSI/BME merge、query adapter、overview 聚合。
- `agent/stateStore.js` 1021 行：environment、experience、relation、reminder、emergency、CSI behavior、LCD 多域。
- `smoke-regression.js` 3749 行：`npm test` 与 `test:smoke` 都进入同一个串行 `run()`；失败后后续域不执行，也不能按名称复现。
- `public/app.js` 2833 行、`public/pages/s3.js` 1139 行：API、normalizer、state、render、timer 和 command 交织。

优化顺序：先抽纯 normalizer/selector 和 DB/route contract test，再按业务域拆 service/store；保留一条端到端 smoke，其他转 Node test runner 可筛选用例；前端先测纯函数，再拆 DOM。

### P2-4 双重 Git 归属必须选一种模型

同一 `ESP-server` 目录同时被顶层普通文件追踪和嵌套 `.git` 管理，已经产生“源码只在顶层 dirty、数据库只在嵌套 dirty”的真实分裂。

应明确采用 submodule、subtree 或单仓库之一。迁移前先冻结发布并输出 ownership manifest，避免在错误仓库漏提或误提真实 DB。

### P2-5 API 文档与 live route 漂移，且无 CI gate

- live `ESP-server/src/routes/deviceRoutes.js:114-123,184` 只允许 BME 通用 ingest，CSI 使用 `/kernel/csi_event`。
- `ESP-server/docs/api.md:2145,2227,2244-2270,2729,3210` 仍宣称 `csi.motion` 走通用 ingest。
- 项目未发现统一 `.github/workflows`、C51/C52 parity、protocol semantic contract 或 route-doc diff gate。

优化：从 route/schema 生成或比对文档；快速 gate 覆盖 repo ownership、untracked active source、C5 `/api` 禁止、raw CSI negative、parity、JS syntax、temp-DB tests 和协议 golden。

### P2-6 观测、容量和写放大没有统一预算

- gateway auth/binding、device/module status、主记录/event log 每个 ingest 产生多次串行 SQLite 写。
- Server 缺独立 `/health/live` 与包含 DB/迁移/draining 的 `/health/ready`。
- CSI/event/snapshot 缺统一 retention、磁盘水位、backup/restore 和 RPO/RTO。
- S3 health/queue/heap 主要散在串口，C5 也缺每 worker depth/high-water/last-success。

优化应以可量化指标验收：每事件 SQL 数、p95 latency、SQLite busy rate、queue age/high-water、oldest outbox age、SSE buffered bytes、磁盘水位和恢复时间。

### P2-7 固件发布识别与回滚能力不足

C5 没有 OTA slot，S3 虽有 OTA 分区但 rollback/coredump 未形成经验证闭环；三套固件缺统一 git SHA/config hash，C51/C52 产物名也难区分。

先建立 artifact manifest 与签名发布，再做 OTA/rollback。secure boot、flash/NVS encryption 涉及 provisioning 和不可逆风险，不能在没有恢复演练时直接开启。

## 6. P3：局部卫生问题

### P3-1 本地 CSI 调试工具有两个未封顶维度

`tools/csi-debug-web/server.js:382-395` 对无换行串口持续拼接 buffer；`:240-260` 对每 device history 有上限，但 device key 总数没有上限。它固定监听 loopback、body 和单设备 history 已有限制，所以风险局部。

改进：最大 serial line bytes、device-key LRU/上限、丢弃计数即可，不需要按公网服务过度设计。

### P3-2 placeholder/deprecated 需要生命周期标记

`display_placeholder`、`csi_placeholder_gateway` 和 legacy rejection 有的是真实门面，有的是兼容壳。应分别标记“长期保留/迁移拒绝/尚未实现”及 owner、删除条件。尤其未接真实 LCD 时，不应把无副作用 placeholder 返回 `ESP_OK`解释为物理动作已完成。

## 7. 已有正确设计，应保留

- C5 URL builder 强制本地 `/local/v1`，没有发现 C5 绕过 S3 直连 Server。
- C5 raw CSI 只在 callback 内转低维 feature；正式序列化/客户端未发现 raw CSI、IQ、subcarrier 外发。
- S3 对 raw/legacy CSI 字段有显式拒绝名单，Server 不重新计算 CSI 算法。
- C5 event handler 已锁外执行，业务已有 worker 边界；问题是队列语义，不应退回 callback 做业务。
- S3 priority event bus、STATE coalescing、CSI latest 和 BME replay rate limit 是正确方向。
- `resource_manager` 的 generation、锁外 release/restore 和 partial-restore cleanup 值得保留，只需补门禁时序。
- Server 使用参数化 SQL；generic command claim 有条件 UPDATE，终态 ACK 有 ownership 和幂等保护。
- event log 先持久化再 SSE broadcast；smoke 使用临时 DB/端口/mock 外部依赖。
- Dashboard 抽查的动态 HTML 已做 escaping，timer 有 cleanup；不应误报成当前 XSS/泄漏。

因此不建议“推倒重写”。当前系统已经有可利用的边界和诊断点，正确做法是补齐可靠性合同后渐进拆分。

## 8. 与旧审计相比

| 主题 | 2026-06-16 / 2026-07-09 | 当前结论 |
|---|---|---|
| Gateway/设备身份 | 基本信任 body/header，写面大量无边界 | 已有 gateway middleware/binding，但默认 fail-open；S3 本地 peer 身份仍未闭合 |
| Generic command | claim/ACK ownership 不足 | Server claim/终态 ACK 已改善；C5/S3 journal/outbox 仍缺，重复执行窗口仍在 |
| CSI 架构 | 默认/文档漂移，旧 ingest | 已迁到 C5 feature -> S3 fusion -> canonical route；API 文档仍漂移 |
| S3 复杂度 | 三大模块已偏重 | 文件继续增长，并出现 resource/health/ACK 的具体时序缺陷 |
| C51/C52 | 镜像复制风险 | 当前 parity 仍高，但 C52 测试已出现 C51 身份假覆盖 |
| C5->Server / raw CSI | 边界需持续验证 | 当前静态检查仍通过，应作为永久 negative gate |

## 9. 建议实施顺序

### 第一阶段：0-3 天，先止损

1. Server production auth fail-closed，控制/删除/计费路由加统一 guard。
2. unknown AP disconnect 禁止 `release_all`；stale ingress 校验先于 peer mapping。
3. ACK 入队改有界等待，enqueue 失败不能返回成功，JSON 改结构化构造。
4. 明确当前部署是否外网可达；若可达，在上述完成前收紧防火墙/反向代理。

### 第二阶段：1-2 周，补可靠性合同

1. 定义 `event_id/boot_id/seq/trace_id/attempt`，先让 Server ingest 幂等和事务化。
2. C5 command journal + S3 durable ACK outbox + Server 统一 lease/dead-letter。
3. local C5 register/ingress/pending/ACK/voice 改为 peer/session generation 绑定认证。
4. CSI event coalesce、compute/egress 分离、control queue 隔离。
5. WiFi connection generation、voice quiescence/cancel、BME validity/recovery。
6. SSE backpressure/close-all、Server readiness、child stale reconciler。

### 第三阶段：1-2 个月，治理结构

1. 建 CI 快速 gate 与故障注入矩阵。
2. 在合同测试保护下拆 S3 三大模块和后端 service/test 单体。
3. 抽共享 C5 component，保留板级 identity/config 注入。
4. 解决 `ESP-server` 双 Git 所有权，建立 release/artifact manifest。
5. 补 retention、backup/restore、OTA/rollback 演练。

## 10. 不建议做的事

- 不做 S3/Server/C5 的一次性大重写。
- 不把 CSI 算法迁回 Server，也不上传 raw CSI/IQ/subcarrier。
- 不在 Server 还不幂等时先引入更强重放/NVS outbox；那只会更稳定地制造重复数据。
- 不用“增加队列长度/超时”替代 owner、backpressure 和状态机修复。
- 不用统一全局 mutex 粗暴包住所有 S3 状态；应明确单 owner 和消息边界。
- 不在没有 provisioning、坏镜像和回滚演练时直接开启 secure boot/flash encryption。

## 11. 验证记录与限制

本轮已通过：

- 顶层相关范围与嵌套 `ESP-server`：`git diff --check`
- `ESP-server` 与 `tools/csi-debug-web` 全量项目 JS：`node --check`
- C51/C52 `Middlewares` parity：仅两个预期身份头不同
- C5 Server-route negative scan：只发现本地 S3 HTTP 基址和绝对 URL 拒绝逻辑
- raw CSI negative scan：正式 C5 序列化/客户端未发现外发；S3 命中项均为拒绝名单

本轮没有执行：

- ESP-IDF build
- Server smoke/runtime 测试
- 真机 WiFi/CSI/BME/voice 故障注入
- 真实数据库读取或修改

因此本报告能证明当前源码中的控制流、状态和结构问题，但不能证明当前脏工作区可构建，也不能用静态结果替代 24/72 小时真机 soak。P1 修复完成后的最低硬件矩阵应覆盖：500 Hz CSI + 慢 endpoint、短 WiFi bounce、ACK 丢失/重复、voice 与在途 HTTP、PCM 上传中断、BME invalid/NACK、未知 station disconnect、S3/Server 重启和 SQLite/SSE 慢端。

## 12. 最终判断

项目已经不是早期“所有逻辑都挤在一起”的状态：三层边界、worker、event bus、Server binding、CSI canonical contract 都已经搭起来。真正危险的是**新架构的可靠性不变量还没全部落地，而代码继续快速增长**。

最值得优先投入的不是格式化或搬文件，而是：

1. 认证默认闭合；
2. 命令与 ingest 的端到端幂等；
3. peer/session generation 身份和资源状态机；
4. 数据面与控制面的队列/HTTP 隔离；
5. 事务、SSE、stale reconciliation；
6. 最后才是模块拆分与去复制。

按这个顺序治理，可以在不破坏 C5 -> S3 -> Server 现有合同的前提下，先消除重复副作用和恢复盲区，再降低长期维护成本。

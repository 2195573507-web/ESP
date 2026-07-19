# ESPC51 / ESPC52 / ESPS3 链路兼容性只读审计

日期：2026-07-10

范围：ESPC51、ESPC52、ESPS3 的 C5 -> S3 本地链路；不包含 ESP-server 行为。

方法：仅静态源码比对与调用链追踪。未修改固件或配置，未编译、烧录、启动服务或连接设备。因此，以下结论是源码证据，不是硬件闭环复现结论。

## 结论摘要

1. 当前源码没有证据表明 C52 相对 C51 缺少 register、heartbeat、status、BME 或 CSI 的关键发送事件。
2. `sensair_shuttle_01 <-> id=1 <-> S3_TO_C51` 与 `sensair_shuttle_02 <-> id=2 <-> S3_TO_C52` 在 C5 与 S3 的映射一致，没有发现 C52 身份反转或 `device_id/local_id/link_id` 不匹配。
3. 观察到 `CSI_RX` 后出现 runtime bus `ESP_ERR_INVALID_STATE` 时，首要怀疑应是 S3 会话生命周期/异步时序，而不是 C52 payload 差异：S3 会在异步处理阶段拒绝已经不再 `live` 的会话，或拒绝断链截止时间之前的旧 ingress。
4. 建议优先只修改 S3 的状态机诊断与会话转换可观测性；当前不建议为此同步修改 C51/C52。只有硬件日志证明 C52 发出的身份/时间戳与静态契约不符时，才考虑 C5 定点修正。

## 1. C51/C52 差异表

| 审计项 | C51 | C52 | 结论 |
| --- | --- | --- | --- |
| gateway_link / Wi-Fi reconnect | `gateway_link.c/h`、`wifi_manager.c` 字节一致 | 字节一致 | 一致。均为 Wi-Fi 稳定 -> health probe -> register -> `LINK_READY`。 |
| 启动与注册顺序 | 启动 Wi-Fi，等待稳定及 `gateway_link_wait_ready()`，再初始化 system/CSI/BME 服务 | 字节一致 | 一致；register 成功前，C5 不启动这些周期性业务上报。 |
| register | `/local/v1/register`；health frame `p,id,t,pt,h,u,q,ts,rid,r,v` | 字节一致 | 一致。另 `gateway_link` 重连注册与 `system_server_client_init()` 注册都使用同一 payload 契约。 |
| heartbeat | `/local/v1/heartbeat`；同一 health schema，`pt=heartbeat` | 字节一致 | 一致，默认 `5000 ms`。 |
| status | `/local/v1/status`；同一 health schema，`pt=status` | 字节一致 | 一致，默认 `15000 ms`。 |
| BME690 | `/local/v1/sensor`；同一 BME v2 payload、`u` uptime、`q` seq、`ts` 时间同步标记 | 字节一致 | 一致。 |
| CSI 发送实现 | 同一 `envelope_builder`、同一字段和 `100 ms` cadence | 仅 `CSI_LOCAL_LINK_ID` / `CSI_LOCAL_REPORT_ID` 不同 | 预期身份差异，非协议漂移。 |
| CSI payload | `id="1"`，`device_id=sensair_shuttle_01`，`lid=S3_TO_C51`，`timestamp_ms`，`state/motion_score/confidence`，metrics | `id="2"`，`device_id=sensair_shuttle_02`，`lid=S3_TO_C52`，其余字段同构 | 一致，且可被 S3 的显式身份一致性检查接受。 |
| device_id / local_id | `sensair_shuttle_01` / `1` | `sensair_shuttle_02` / `2` | 一致、无反转。 |
| alias / room | `SensaiShuttle` / `living_room` | `SensaiShuttle02` / `bedroom` | S3 使用相同映射。 |
| HTTP endpoint | register、heartbeat、status、sensor、CSI 均取共享 `esp111_protocol_common.h` 常量 | 同一共享头，字节一致 | 一致。 |

### 1.1 身份和链路映射

| 层 | C51 | C52 |
| --- | --- | --- |
| C5 终端配置 | `sensair_shuttle_01`, local ID `1` | `sensair_shuttle_02`, local ID `2` |
| C5 CSI 常量 | `S3_TO_C51`, report ID `"1"` | `S3_TO_C52`, report ID `"2"` |
| S3 `protocol_adapter` | `1 -> sensair_shuttle_01 -> SensaiShuttle -> living_room` | `2 -> sensair_shuttle_02 -> SensaiShuttle02 -> bedroom` |
| S3 合法 CSI link | `S3_TO_C51` | `S3_TO_C52` |

S3 对紧凑 CSI payload 的规则是：先将 `id` 映射为 canonical `device_id`，若 payload 另带 `device_id` 则必须完全相等；不一致时返回 `ESP_ERR_NOT_ALLOWED`，而不是 `ESP_ERR_INVALID_STATE`。因此当前映射问题不会伪装成题述的 INVALID_STATE。

### 1.2 关键源码证据

- C5 共享 endpoint、`id=1/2` 常量：`ESPC51/components/esp111_protocol_common/include/esp111_protocol_common.h:64-83,153-157`；该头与 ESPC52 字节一致。
- C51/C52 默认身份：`ESPC51/components/Middlewares/terminal_config/terminal_config.h:27-31`、`ESPC52/components/Middlewares/terminal_config/terminal_config.h:27-31`。
- C5 启动顺序与 `LINK_READY` 栅栏：`ESPC51/components/Middlewares/app_orchestrator/app_orchestrator.c:52-112`；C52 同文件内容一致。
- C5 注册 payload 与重连次序：`ESPC51/components/Middlewares/server_comm/gateway_link.c:466-580`；C52 文件字节一致。
- register/heartbeat/status 统一 health payload：`ESPC51/components/Middlewares/command_domain/system_command/system_server_client.c:552-598,750-855`；C52 文件字节一致。
- 5s heartbeat / 15s status：`ESPC51/components/Middlewares/command_domain/system_command/system_service.h:19-28`；C52 同。
- C5 BME payload：`ESPC51/components/Middlewares/sensor_domain/bme690/server_client/bme_server_client.c:115-231`；C52 同。
- CSI 唯一差异：两份 `csi_server_client.c:29-30`，其余同构 payload 由 `:47-74` 生成。

## 2. S3 状态机调用链

```text
C5 POST /local/v1/{register|heartbeat|status|sensor|csi/result}
  -> local_http_server: validate_local_body()
  -> local_http_server: enqueue_body_buffer()
  -> s3_scheduler_enqueue_ingress_owned()
  -> s3_event_bus_push_owned()
  -> protocol_worker / process_ingress()
  -> protocol_adapter_parse_local_envelope() + validate_local_envelope()
  -> 按消息类型分支
     register / heartbeat / sensor / CSI
       -> resource_manager_confirm_peer_at_us()
       -> child_registry_confirm_identity()/touch()/register_or_update()
       -> restore_live_resources(): command + sensor + CSI + network worker
       -> RESOURCE_MANAGER_SESSION_ACTIVE
     status
       -> child_registry_note_activity()  [只记录活动，不恢复会话]
  -> sensor_aggregator 或 csi_placeholder_gateway
  -> network_worker / server upload / stream worker
```

### 2.1 会话状态和允许信号

| 状态 | 含义 | 允许恢复为 live 的消息 | 会导致拒绝的典型条件 |
| --- | --- | --- | --- |
| `RELEASED` | 初始状态，或断链/心跳超时后已释放资源 | register、heartbeat、sensor、CSI | 旧 ingress 早于 `restore_not_before_us`；资源恢复失败。 |
| `GRACE` | AP 断开后的宽限状态 | 同上 | 同上；宽限到期转 `RELEASED`。 |
| `ACTIVE` | command、sensor、CSI、队列资源可用 | 新身份事件只更新最后活动时间 | 断链、心跳超时或 release generation 被更新。 |
| `RESTORING` | 枚举仍保留，但当前确认路径直接提交 `ACTIVE` | 当前路径没有显式进入该状态 | CSI warmup 完成时调用 `complete_restore()` 可能得到已容忍的 INVALID_STATE。 |

重要细节：资源管理器初始化时两个 allowlisted C5 都从 `RELEASED` 开始。register、heartbeat、sensor 和 CSI 都足以使其恢复；status 明确只做 passive activity，不能把 `GRACE/RELEASED` 变为 `ACTIVE`。

### 2.2 各消息类型的 S3 处理

| 消息 | 入口/优先级 | resource_manager 信号 | child_registry 动作 | 后续 |
| --- | --- | --- | --- | --- |
| register | `register_handler`，HIGH | `REGISTER` | `register_or_update()` | 注册成功并重置 stream timestamp baseline。 |
| heartbeat | `heartbeat_handler`，HIGH | `HEARTBEAT` | `touch()` | 续租、恢复会话。 |
| status | `status_or_sensor_handler`，NORMAL | 无 | `note_activity()` | 只更新状态摘要；不能恢复会话。 |
| BME sensor | `status_or_sensor_handler`，NORMAL | `SENSOR` | `touch()` | 更新聚合；仅 active peer 才投递实时上传，否则缓存/标记 deferred。 |
| CSI | `csi_result_handler`，REALTIME | `CSI` | 成功后 `touch()` | CSI gateway 校验 link/feature，要求 `s_peer_active && resource_manager_is_live()`；融合后才可上传。 |

关键入口证据：

- HTTP handler 完成解析和入队后立即响应，`local_http_server.c:620-719`；异步 protocol worker 随后才处理真实状态。
- S3 消息分支：`runtime/s3_scheduler.c:808-922,933-1009`。
- 初始 `RELEASED`：`resource_manager.c:242-290`。
- restore 到 `ACTIVE`：`resource_manager.c:512-659`。
- child registry 的 register/touch/confirm/status 语义：`child_registry.c:163-300`。

## 3. ESP_ERR_INVALID_STATE 根因定位

### 3.1 与本次链路直接相关的返回点

| 层/函数 | 上游调用者 | 消息类型 | 当前状态/条件 | 拒绝原因 |
| --- | --- | --- | --- | --- |
| `s3_event_bus_push_owned()` | `s3_scheduler_enqueue_ingress_owned()` <- local HTTP handler | 任意 register/heartbeat/status/sensor/CSI | `s_lock` 或 `s_signal` 未初始化 | runtime event bus 尚未就绪，HTTP 返回 503。正常启动顺序会先 init/start scheduler，再开放 local HTTP，因此不是 C52 差异。 |
| `protocol_worker_enqueue_ingress()` | scheduler dispatch | 任意入站 | `s_protocol_queue == NULL` | protocol worker queue 未初始化。启动序列本应排除。 |
| `csi_fusion_worker_enqueue()` | CSI scheduler dispatch | CSI | fusion queue 或其 lock 为 NULL | CSI worker 初始化前调用。启动序列本应排除。 |
| `resource_manager_confirm_peer_at_us()` | register/heartbeat/sensor 分支，及 CSI gateway | register、heartbeat、sensor、CSI | `observed_at_us < restore_not_before_us` | 断链后延迟到达的旧帧，被时间截止线拒绝（`stale_ingress`）。这是强时序保护。 |
| `resource_manager_confirm_peer_at_us()` | 同上 | register、heartbeat、sensor、CSI | 未传 peer IP 且 registry 无旧 IP | 没有可确认的网络身份。HTTP ingress 正常会携带 peer IP；直接 feature API 才可能走该分支。 |
| `resource_manager_confirm_peer_at_us()` | 同上 | register、heartbeat、sensor、CSI | restore 期间 generation/state 改变 | 断链/新 release 打断恢复，返回 INVALID_STATE。 |
| `csi_placeholder_gateway_handle_feature_internal()` | `handle_csi_ingress()` | CSI | CSI worker 未运行、fusion lock 未初始化 | CSI 子系统生命周期未就绪。 |
| `csi_placeholder_gateway_handle_feature_internal()` | 同上 | CSI | `!s_peer_active[peer]` 或 `!resource_manager_is_live(device)` | 资源已释放/尚未恢复，拒绝融合；此处最符合“CSI_RX 后 runtime INVALID_STATE”的状态机现象。 |
| `sensor_aggregator_handle_envelope()` | scheduler sensor 分支 | BME | peer inactive 或 realtime upload gate 未开 | BME 本地数据已接受并缓存，但 realtime forward 被延后；函数本身保持返回 `ESP_OK`，结果内 `server_ret=INVALID_STATE`。 |
| `network_worker_submit_*_for_peer()` / `perform_server_json()` | BME/传感器上传 | BME/peer ingest | peer inactive 或 `!resource_manager_is_live()` | 已入队/待上传项目在执行前被断链取消。不是 C5 JSON 校验失败。 |
| `stream_worker_task()` | S3 CSI trigger 发送 | S3 -> C5 CSI trigger | peer IP 无映射或 session not live | S3 下行 trigger 被主动丢弃，日志为 `reason=session_not_live`。 |
| `s3_scheduler_enqueue_command_pull()` | scheduler 周期逻辑 | command pull | 没有 live session | 正常空闲保护，与 C5 CSI 无关。 |
| `resource_manager_release_child_by_identity()` 内 `release_peer_to()` | Wi-Fi disconnect path | 非 C5 入站 | expected generation 不等于当前 generation | 丢弃过期断链事件，保护已经恢复的新会话。 |
| `resource_manager_complete_restore()` | CSI warmup 完成后 | CSI | state 不是 `RESTORING` | 当前恢复路径已直接转 `ACTIVE`；CSI gateway 对 ACTIVE 场景显式容忍此返回，故它是状态模型遗留语义，不是致命拒绝。 |

### 3.2 最高概率根因

静态源码下，题述现象最吻合以下时序：

```text
1. C5 CSI HTTP 请求通过 local_http_server 校验并入队，产生 CSI_RX/HTTP 成功证据。
2. 在 protocol worker、CSI fusion worker 或 stream worker 实际消费前，S3 收到 AP disconnect、
   heartbeat timeout，或检测到该帧的 received_at_us 早于 disconnect cutoff。
3. resource_manager 将会话置为 GRACE/RELEASED，暂停 sensor/CSI/network resources。
4. 异步 CSI 路径检查 !resource_manager_is_live() 或 stale_ingress，返回 ESP_ERR_INVALID_STATE。
```

这说明 `CSI_RX` 是“入口收到并通过轻量协议校验”的证据，不是“该 CSI 事件完成融合、会话仍 ACTIVE、或已上云”的证据。

### 3.3 非根因或次要 INVALID_STATE

- `child_registry` 的 INVALID_STATE 基本都是 `s_lock == NULL`，即模块没有初始化；`gateway_orchestrator` 已在开放 local HTTP 前调用 `child_registry_init()`、`resource_manager_init()`、`s3_scheduler_init()`、`network_worker_init()`、`s3_scheduler_start()`，所以正常启动中不应由 C52 触发。
- `network_worker` 对 server not ready、低优先级合并、snapshot rate limit/queue pressure 返回 INVALID_STATE，表示可丢弃或延迟的上行工作被门控；它不表示 C5 身份不合法。
- C5 本身在 voice/backpressure gate 下也可能让 heartbeat/status tick 返回 INVALID_STATE，但 C51/C52 的该代码一致，且不能解释 S3 单独针对 C52 的差异。
- 若 `id/device_id` 或 `lid` 有误，S3 通常返回 `ESP_ERR_NOT_ALLOWED`、`ESP_ERR_NOT_SUPPORTED` 或 `ESP_ERR_INVALID_ARG`，不是本次目标错误。

## 4. 对问题 A-D 的判断

### A. C52 是否比 C51 少发送某个关键事件？

否，当前源码证据不支持。

`gateway_link`、Wi-Fi reconnect、system service、BME service、CSI service、`c5_backpressure_controller`、`c5_runtime_workers` 及共享协议头在两端均字节一致。register、heartbeat、status、sensor 均由同一实现发出；CSI 只有 `id/link_id` 的预期配对差异。

### B. C52 是否因为 device_id/local_id 映射导致 S3 没有进入 ACTIVE？

否，当前映射正确且 S3 双向映射一致。

`id=2 -> sensair_shuttle_02 -> S3_TO_C52` 是完整闭环；C52 的 CSI payload 同时带 `id=2` 与 `device_id=sensair_shuttle_02`，S3 会强制这两者一致。若实际设备日志显示不同，才需调查运行时刷入的固件/配置是否不是本审计源码。

### C. CSI_RX 成功但 runtime bus INVALID_STATE 是否属于 S3 状态机问题？

是，静态代码显示这是 S3 状态机/异步队列时序问题的高概率表现。

具体保护条件是 `resource_manager_is_live(device_id)` 和 `s_peer_active[peer]`。CSI 入口完成、日志写入或 HTTP 200 都发生在后续 fusion/stream worker 之前；在此间隙发生断链、心跳超时或 stale cutoff，S3 会有意拒绝后续 CSI 工作。

### D. 是否需要修改？

当前建议：**只改 S3（P0/P1），不改 C5；不需要 C51/C52 同步修改。**

前提是硬件日志仍与当前源码一致。若日志表明 C52 实际发出的 `id`、`device_id`、`lid`、`timestamp_ms` 或 endpoint 偏离本报告的静态契约，再对 C52 做单点修正，并同步补充 C51/C52 parity 检查，而不是先修改两个 C5。

## 5. 修改建议（不直接修改）

### P0：S3 增强会话拒绝诊断

在 CSI 的每个 `ESP_ERR_INVALID_STATE` 返回点输出结构化字段：

- `device_id`、`link_id`、`peer_ip`
- resource `state`、`generation`、`restore_not_before_us`
- ingress `rx_time_us`、`last_identity_observed_us`
- `s_peer_active`、child registry `registered/status/last_seen_ms`
- 拒绝枚举：`stale_ingress`、`session_not_live`、`peer_inactive`、`worker_not_ready`、`restore_interrupted`

目标是把当前泛化的 `runtime bus ... INVALID_STATE` 直接归因到一个可测试条件。

### P0：统一 CSI 接收成功的日志语义

将日志分成至少三个阶段，避免 `CSI_RX` 被误读为全链路成功：

1. `CSI_HTTP_ACCEPTED`：HTTP body 已校验并已入 event bus。
2. `CSI_SESSION_CONFIRMED`：`resource_manager_confirm_peer_at_us()` 成功且会话 live。
3. `CSI_FUSION_ACCEPTED` / `CSI_UPLOAD_DEFERRED`：融合与上云门控的最终结果。

### P1：显式收敛 RESTORING 状态语义

当前 `resource_manager_confirm_peer_at_us()` 在 restore 成功后直接进入 `ACTIVE`，但 CSI warmup 路径仍调用只接受 `RESTORING` 的 `resource_manager_complete_restore()`，并将 ACTIVE 时的 INVALID_STATE 作为容忍分支。二选一即可：

- 若 CSI warmup 不再是恢复门槛，移除该过渡 API/调用，明确首次有效身份帧即 ACTIVE；或
- 若仍应以 warmup 完成作为 ACTIVE 门槛，restore 成功时先转 `RESTORING`，完成后再由 CSI warmup 转 ACTIVE。

这不是当前 C52 兼容性根因，但会降低 INVALID_STATE 噪声与状态机歧义。

### P1：按本次静态契约补充 S3 断链竞争测试

建议新增纯单元/集成测试，覆盖：

- `AP_DISCONNECTED(t2)` 与 `CSI(rx_time=t1<t2)` 的 stale ingress 拒绝；
- register/heartbeat/sensor/CSI 分别从 RELEASED 恢复为 ACTIVE；
- status 不能从 RELEASED 恢复；
- C51 `(1, sensair_shuttle_01, S3_TO_C51)` 和 C52 `(2, sensair_shuttle_02, S3_TO_C52)` 均可恢复；
- 已经 ACTIVE 的 CSI warmup completion 不产生面向用户的错误日志。

### P2：维持 C5 parity 防线

不修改当前 C5 行为；在后续任何 C5 改动中，对以下文件执行 C51/C52 结构化 parity 检查，仅允许身份常量差异：

- `gateway_link.*`
- `system_server_client.*` / `system_service.*`
- `bme_server_client.*` / `bme_sensor_service.*`
- `csi_server_client.c`
- `terminal_config.*` / `server_comm_config.*`
- 共享 `esp111_protocol_common.h`

## 6. 修改优先级

| 优先级 | 所属 | 建议 | 目的 |
| --- | --- | --- | --- |
| P0 | ESPS3 | CSI/session INVALID_STATE 结构化日志与阶段化成功日志 | 先确认真实拒绝点和状态，不凭 C5 名称猜测。 |
| P0 | ESPS3 | 确认 disconnect cutoff、session generation 和 worker 消费之间的可观测性 | 验证“CSI_RX 后被 stale/session_not_live 拒绝”。 |
| P1 | ESPS3 | 收敛 RESTORING/ACTIVE 语义 | 消除被容忍的 INVALID_STATE 与状态机歧义。 |
| P1 | ESPS3 | 断链竞争及双设备映射测试 | 防止回归并证明两个 C5 同等可恢复。 |
| P2 | ESPC51/ESPC52 | 仅做 parity 测试/日志采样，不改行为 | 只有运行时证据推翻当前静态契约才修改 C5。 |

## 7. 审计限制与现场取证建议

本次按限制未运行设备。因此不能证明某块实际刷入的 C52 与当前目录源码相同，也不能量化 Wi-Fi 断链时序。现场若复现，应按同一 `device_id` 收集下列顺序日志：

1. C5 `gateway_link` 的 `LINK_*` transition、register/heartbeat HTTP 结果；
2. S3 `SESSION_STATE_CHANGE`、`RESOURCE_RESTORE`、`RESOURCE_RELEASE_REASON`；
3. S3 CSI 的 HTTP accepted、session confirmed、fusion accepted/rejected 日志；
4. 该帧的 `rx_time_us` 与 `restore_not_before_us`；
5. `child_registry` 的 peer IP、registered/status、last_seen。

只有在同一 C52 会话中确认 register/heartbeat 已成功、状态为 ACTIVE，且随后没有 disconnect/timeout/stale cutoff 的情况下仍发生 INVALID_STATE，才应将问题升级为 S3 实现缺陷；如果从一开始 `id/device_id/lid` 校验失败，错误类别应不同，应先核查实际固件版本或烧录产物。

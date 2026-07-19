# ESP-111 C5 / S3 / ESP-server 稳定性与问题处理流程审计

审计日期：2026-07-10

审计范围：`ESPC51`、`ESPC52`、`ESPS3`、`ESP-server` 后端，以及 C5 -> S3 -> Server 的 BME、CSI、状态、命令和语音链路。

执行方式：只读源码审计、静态检查和合同核对；未修改业务源码、前端、数据库或运行配置，未 build、flash、monitor，也未启动 Server。

## 1. 结论摘要

当前系统不是“没有稳定性设计”。相反，三层已经有一批正确基础：

- C5 只访问 S3 `/local/v1/*`，没有绕过 S3 直连 Server。
- C5 有 WiFi 重连、`health -> register -> READY` 链路状态机、HTTP 超时和语音中止入口。
- S3 有优先级 event bus、worker 分流、CSI latest/coalesce、BME cache/replay、Server ready 去抖、Task WDT 和资源压力日志。
- Server 有参数化 SQL、输入校验、gateway/device binding、命令 claim/ACK 校验、临时 DB smoke 基础、事件持久化后再 SSE 广播。
- CSI 主链维持 C5 特征、S3 融合、Server 事实存储的分层，未发现 raw CSI/IQ/subcarrier 外发。

真正的问题集中在五个方面：

1. **控制面还不能默认信任。** Server 未配置 gateway token 时会 fail-open，多组控制、删除和外部计费接口也没有统一 RBAC。公网部署下可直接污染系统真源和事故证据。
2. **端到端只做到“能重试”，没有做到“可证明不重复副作用”。** 命令 ACK、BME/CSI ingest、S3 RAM cache 都存在请求已部分成功但响应丢失后重复执行/重复入库的窗口。
3. **部分状态机的恢复条件不完整。** C5 短 WiFi 抖动可能保留假 READY，BME 运行期 I2C 故障没有恢复状态机，CSI 长 gap/换信道后不重新校准，Server 对 S3 权威 child 状态没有 stale 语义。
4. **观测信息存在，但没有统一成事故证据链。** CSI 有 `trace_id/tick_id`，其他链路仍依赖不同的 seq/request/command ID；S3 queue/heap 主要在串口，Server 没有真正的 liveness/readiness/metrics。
5. **问题处理依赖个人经验和旧文档。** 当前没有一份版本化 runbook；部分部署/联调文档已经与 live source 冲突，甚至包含不应在事故中直接执行的破坏性步骤。

优先级结论：

| 优先级 | 数量/范围 | 处理原则 |
|---|---:|---|
| P0 | 3 组 Server 控制面问题 | 上线或公网暴露前先处理；不能靠“部署时记得配环境变量”兜底 |
| P1 | 端到端幂等、状态恢复、队列隔离、DB/SSE 生命周期、运行真源 | 先加门禁和观测，再做最小行为修复，最后做结构拆分 |
| P2 | 发布治理、日志降噪、架构拆分、长期安全与 OTA | 纳入中期治理，不应打断 P0/P1 闭环 |

## 2. 审计边界与证据等级

### 2.1 证据等级

| 等级 | 含义 | 本报告使用方式 |
|---|---|---|
| A：源码确认 | 当前 live source 可直接证明 | 可作为当前缺陷或现有保护 |
| B：静态检查确认 | parity、语法、negative scan、diff check | 可证明合同/结构一致，不证明运行时序 |
| C：需运行验证 | 需要临时 DB、服务、网络故障注入 | 作为验收计划，不宣称已通过 |
| D：需真机闭环 | 需要 C51/C52/S3 硬件、WiFi、传感器、语音 | 作为上线门禁，不用源码结果替代 |

### 2.2 本轮限制

- 顶层仓库和嵌套 `ESP-server` 仓库都有既有未提交改动。
- 审计期间 S3 源码仍在并发演进；报告只保留最终复核后仍存在的问题。
- 项目规则禁止本轮修改固件/后端/前端/真实 DB，也禁止 flash、monitor、fullclean、erase。
- 因此“源码已确认”和“真机已稳定”在本文中始终分开。

### 2.3 术语表

| 术语 | 本报告中的含义 |
|---|---|
| outbox | 先持久化待发送事实，再异步发送并在幂等确认后删除的可靠发送队列 |
| bulkhead | 把 voice、control、background 等资源隔离，单一路径阻塞时不拖垮其他路径 |
| singleflight | 同一 key 的并发刷新只允许一个真实上游请求，其余复用结果 |
| generation | 一次 boot/link/session 世代；旧世代事件不得修改新世代状态 |
| effectively-once | transport 可至少一次，但通过执行 journal、幂等键和条件更新让业务副作用对外表现为一次 |
| RPO / RTO | 可接受的数据丢失窗口 / 故障后恢复时间目标 |
| HWM | high-water mark，队列、栈或资源使用的历史峰值/最低余量 |
| RSS | Server 进程驻留内存大小 |
| SLO | 可量化的服务目标；本报告初始值见 9.1，真机基线后再固化 |

## 3. 当前正确设计，应保留

### 3.1 边界与隔离

- C5 URL builder 限制相对 `/local/v1` 路径；C5 不持有 Server `/api` 所有权。
- S3 是唯一协议融合和 Server-facing 网关；Server 不重新计算 CSI 信号特征。
- voice raw PCM 路由在 JSON parser 前挂载，避免 body parser 破坏流式请求。
- smart-home 没有真实执行器时返回失败，不伪造成功。

### 3.2 运行时保护

- C5 callback/producer 与 worker 已分离，链路状态读写使用临界区保护。
- S3 event bus 已区分 `CRITICAL > REALTIME > STATE > BACKGROUND`，STATE 可合并，后台任务可在压力下让路。
- S3 CSI 使用 latest/coalesce，不在恢复后回放高频历史。
- BME 有独立 replay worker 和速率限制，Server ready 有连续失败/成功去抖。
- S3 关键 scheduler/network/stream worker 已接 Task WDT。

### 3.3 Server 保护

- command claim 使用条件 UPDATE，终态 ACK 有 ownership 和幂等判断。
- event log 先写 SQLite 再广播 SSE，事实先于实时通知。
- smoke 使用临时 SQLite 和 mock 外部依赖，不需要污染真实数据库。

这些保护不应在后续重构中被“简化”掉。改进目标是补齐可靠性合同，不是重写全部架构。

## 4. P0：必须先处理

### P0-1 Gateway 认证在缺少密钥时 fail-open

证据：`ESP-server/src/services/gatewayAuthService.js:82-100,132-149`。

触发条件：部署环境未配置 gateway token，Server 端口可被非受信客户端访问。

影响：任意请求只要声明 `gateway_id` 就可通过 gateway middleware，进而伪造 BME、CSI、snapshot、command ACK 和 event log。`gateway_auth.token_hash/enabled` 未参与认证决策，seen bookkeeping 还会把该 gateway 的 `enabled` 写回 1。

改进：

- production profile 缺凭据时拒绝启动，不允许静默降级为 declared identity。
- 每个 gateway 使用独立、可轮换、可禁用的凭据；校验数据库 `enabled` 和 token hash，或采用 mTLS。
- 仅在显式 `ALLOW_INSECURE_DEV_GATEWAY=1` 的本地开发 profile 允许无 token，并在日志/health 明确标红。
- gateway A 的凭据不能声明 gateway B，device 绑定也必须验证 owner。

验收：无 token 生产启动失败；错 token 返回 401；disabled gateway 返回 403；A token 冒充 B 被拒绝；拒绝请求不刷新任何 device/gateway last_seen。

### P0-2 控制、删除、隐私和外部计费接口缺统一 RBAC

证据示例：

- command create：`ESP-server/src/routes/commandRoutes.js:162`
- smart-home control：`ESP-server/src/routes/smartHomeRoutes.js:66`
- log cleanup/delete：`ESP-server/src/routes/eventRoutes.js:150`、`:167`
- LLM：`ESP-server/src/routes/llmTextRoutes.js:22`
- voice prompt config：`ESP-server/src/routes/voiceRoutes.js:596-600`
- memory jobs：`ESP-server/src/routes/memoryRoutes.js:296-303`

影响：匿名或低权限调用者可下发设备动作、删除事故证据、修改 prompt、消耗 LLM/TTS 资源或读写个人记忆。

改进：建立中央 route policy matrix，角色至少分为 `gateway`、`operator`、`admin`、`read_only`；所有显式路由必须进入 allowlist。删除类操作采用审计化 retention/tombstone，不能让匿名请求物理删除事故记录。

验收：路由清单自动扫描；匿名只允许静态资源和最小 `/live`；所有写操作有 identity、role、request_id、审计事件和限流。逐路由匿名/错角色测试均返回 401/403，且 DB、外部调用计数和审计真源无未授权副作用。

### P0-3 Legacy 写入口可绕过 gateway 真源

证据：`ESP-server/src/routes/sensorRoutes.js:116-178`、`recordRoutes.js:25-64`、`server-time-sync/timeSync.js:105-123`、`src/voice/http.js:44-60`。

影响：旧 `/sensor`、`/asr`、`/llm` 等路径可用 body/header/IP 伪造设备身份，污染历史表和在线状态。事故中会出现“正式链路正常，但 Dashboard/状态被 legacy 写覆盖”的假象。

改进：legacy 直写默认关闭。迁移期只能由具备 mTLS 或独立 service credential 的迁移代理调用，并透传已认证的 gateway/device identity；loopback 地址本身不构成认证。其他入口统一接入 gateway auth + binding，禁止用 `req.ip` 生成业务 device identity；为 legacy 请求单独计数并设下线日期。

验收：直接访问 legacy 写路由返回 401/403；伪造 body/header/IP 不刷新 device truth；仅受控迁移代理可写，且每次写入都可按 service identity、gateway/device、request_id 追踪；到期后 legacy write 计数必须为 0。

## 5. P1：直接影响稳定性和恢复

### P1-1 命令链没有可证明的幂等执行

当前路径：Server lease/dispatch -> S3 RAM queue -> C5 执行 -> S3 本地 ACK -> Server ACK。

证据：S3 `command_router.c:96-117,460-620`、`network_worker.c:1889-1913,2438-2463`、`local_http_server.c:855-915`；C5 `system_server_client.c:602-669`；Server `src/services/smartHomeService.js:341-451`。

风险窗口：

- S3 local ACK route 返回 200 只表示 ingress 已进入 event bus，不表示 ACK 已持久或送达 Server。
- S3 在 Server ACK 确认前把本地 entry 标为 ACKED。
- ACKED slot 可被新命令复用，旧 command_id 去重记录随之消失。
- S3 ACK queue 使用无限等待；队列满时会卡住处理 ingress 的 `protocol_worker`，后续 register/heartbeat/sensor/ACK 都无法推进。
- HTTP 非 2xx 和大多数网络失败只记录后释放 ACK work，没有可靠重试。
- 8 槽本地队列把命令改为 DISPATCHED 后没有 TTL sweep；8 条未 ACK 命令可永久耗尽槽位。
- C5 已计算错误文本转义，但最终 ACK payload 不含 error text 和 `ack_seq`；S3 再手工重建 JSON，故障上下文和 attempt 关联丢失。
- Server 没收到 ACK 会按 timeout 重发；C5 没有执行 journal，会再次执行。
- generic、smart-home、natural-language 三套命令状态机的 lease、max attempt、expiry 和终态条件又不一致。

改进：

- C5 建带磨损预算的持久 journal：以 `command_id + seq` 记录 `RECEIVED -> APPLIED -> ACK_PENDING -> ACKED`，保留时间覆盖 Server 最大 retry/dead-letter 窗口；重复投递和掉电恢复只返回旧结果，不再次执行副作用。
- S3 建 durable ACK outbox；只有 Server 返回幂等终态确认后才清除。
- S3 local ACK 只有在 durable outbox 接管后才返回 accepted；协议 worker 不允许无限等待，DISPATCHED 必须按 TTL 转入可审计的不确定终态。
- Server 统一 lease/deadline/max_attempt/dead-letter；超过次数进入 `delivery_uncertain`，禁止无限静默重发。
- 所有终态 UPDATE 带旧状态条件，禁止并发 ACK 相互覆盖。
- ACK 使用结构化 JSON builder，保留稳定 `ack_id/ack_seq/error_code` 和有界错误摘要。

验收：丢首次 ACK、C5 掉电重启、S3 重启、Server 响应丢失、重复投递同一 command_id，业务执行计数始终为 1，最终状态可收敛且保留每次 attempt；journal 容量、保留期和 flash 写放大满足 9.1 初始门槛。

### P1-2 C5 CSI 事件可击穿共享 event bus

证据：`ESPC51/components/Middlewares/sensor_domain/csi_placeholder/csi_service.c:178`、`runtime/c5_event_bus.c:92-157`、`runtime/c5_runtime_workers.c:70-87`、`runtime/c5_backpressure_controller.c:397-406`；C52 同构。

虽然 raw sample 使用 latest slot，每个 callback 仍投递一个事件。CSI worker queue 满时 route handler 返回 timeout，而 dispatcher 的排空循环只在 `ESP_OK` 时继续；一个拥塞的 CSI event 会中止本轮主 bus 排空。CSI HTTP 阻塞或 callback 高频时，后续 heartbeat/status/command/BME 因而发生头阻塞，最终随共享 24 深度 FIFO 一起丢弃；三个 worker 的隔离发生得太晚。

改进：CSI ingress 使用 pending bit、task notification 或长度 1 overwrite queue；control plane 使用独立可靠队列/预留容量；dispatcher 不因单个下游 queue 拥塞停止其他 route；HTTP report 只消费 latest feature。按 event type/source 记录 coalesce/drop 和 progress age。

验收：500 Hz callback + CSI endpoint 5 秒延迟时，control event drop=0，command latency 有界，内存恒定，CSI 只增加 coalesce。

### P1-3 C5 短 WiFi 抖动可能保留假 READY

证据：`ESPC51/components/Middlewares/server_comm/gateway_link.c:243,254,544`、`wifi/wifi_manager.c:167`；C52 同构。

不足 1 秒的断连可能没有被判定为 down-stable；重新拿 IP 后 reconnect task 仍看到旧 `LINK_READY`，跳过 health/register。如果 S3 同时重启并清空 registry，C5 会继续放行业务但没有有效注册。

改进：每次 DISCONNECTED/GOT_IP 更新 connection generation 并立即清 READY；每个新 generation 必须完成 health + register 才能 READY。

### P1-4 Voice 独占没有完成全链路 quiescence/cancellation/bulkhead

证据：C5 `runtime/app_runtime.c:45,60`、`server_comm/server_comm_http.c:275`、`server_voice/server_voice_client.c:666,674,735`；S3 `local_http_server.c:918-930`、`voice_proxy.c:218-231,263-401`、`child_registry.c:496-510`。

- pause 只等待 BME，不等待或取消在途 system/CSI HTTP。
- CSI callback 在整轮语音期间继续向共享 bus 投事件。
- fixed PCM upload 完成 write 后才发布可取消 handle，断联时无法及时 abort，可能卡到长 timeout。
- S3 在与 register/heartbeat/commands/ACK 相同的 HTTP server task 内同步读 PCM、等待 Server 并流回响应；长 turn 会占用整个本地 HTTP 控制面。
- 若 AP disconnect 已把 resource 释放并标记 link_lost，voice 清理仍无条件清 link_lost、刷新 last_seen 并写 ONLINE，造成 `registry=online`、`resource=released` 分叉。

改进：公共 HTTP 增加 non-voice active lease；voice 前等待有界 quiescence；CSI 语音期只覆盖 latest、不投事件；open/write/read 全阶段使用 owner-controlled cancellation token 和总 deadline。S3 voice 使用独立 listener/task/bulkhead；清 busy 必须按 session generation 条件更新，不能把已 link_lost/released 的设备写回 online。

### P1-5 BME 有效性和运行期恢复不完整

证据：`ESPC51/components/Middlewares/sensor_domain/bme690/service/bme_sensor_service.c:182,204,219`、`bme_air_quality.c:94,101`；C52 同构。

- `gas_valid=false` 或 heater 未稳定时仍更新 AQ baseline/sample_count 并上传，可能污染后续 high-confidence 输出。
- 初始化后的 I2C/NACK/brownout 只重复失败，没有 soft reset、重新读取校准或 bus recovery。

改进：气体 baseline 只接受 `new_data && gas_valid && heat_stable`；温湿压与 gas validity 分开。连续错误进入 `DEGRADED -> RECOVERING`，执行 sensor reset/reconfigure/bus recovery + 指数退避，导出 recovery count/last error。

### P1-6 CSI 在长 gap/换信道后不重新校准

证据：`ESPC51/components/Middlewares/runtime/c5_backpressure_controller.c:194,210`、`csi_placeholder/csi_service.c:224,334`、`csi_phase_a/csi_feature.c:504`；C52 同构。

S3/WiFi 断联期间 feature processing 停止，恢复后仍沿用旧 baseline。换信道、设备移动或长 gap 后可能立即产生假 MOTION/IDLE。

改进：link generation、sample gap、selected-subcarrier attrition、持续弱 RSSI 触发 `CALIBRATING`；达到有效样本与收敛门槛前只上报 `stale/calibrating`。

### P1-7 BME/CSI ingest 缺幂等键和事务边界

证据：

- BME schema：`ESP-server/src/db/sensorRecords.js:5-38`
- BME insert：`ESP-server/src/services/sensorBme690Service.js:218-268`
- CSI schema/insert：`ESP-server/src/db/csiMotion.js:5-69`
- CSI 多步写：`ESP-server/src/services/csiMotionService.js:173-194`

`request_seq` 没有唯一约束；CSI 表不存可索引的 `trace_id/tick_id`。事实 INSERT、device status、event log、内存 latest 和 SSE 不是一个事务。中途失败或响应丢失后，S3 重试会产生重复 row、重复 event 或 DB/内存分叉。

改进：每条 BME/CSI 带稳定 `event_id + boot_id + seq`；Server 建唯一键并对重复返回原 accepted result。权威 DB 写入在单事务中完成，commit 后通过 outbox 更新内存/SSE。

### P1-8 S3 BME cache 是易失 RAM ring，满时直接覆盖

证据：`ESPS3/components/Middlewares/bme_cache_manager/bme_cache_manager.c:241-247,281-315`。

重启会清空离线数据；满容量时直接覆盖 oldest，只增加 `overwritten_count` 和串口日志。事故中既可能丢数据，也无法从 Server 端看到 cache age/overwrite。

改进：先让 Server ingest 幂等，再评估带寿命预算的 NVS/flash outbox；至少把 cache fill ratio、oldest age、replay lag、overwrite count 上报并告警。关键不是无限缓存，而是明确 RPO、容量和丢弃策略。

### P1-9 Server SQLite runtime、事务和 migration 缺门禁

证据：`ESP-server/src/db/sqlite.js:5-45`、`src/services/userDataService.js:412-510`、`src/db/migrations.js:79-81`。

- 未显式设置 WAL、busy timeout、foreign keys 和一致性策略。
- 所有路由共享一条 connection，跨多个 await 的事务可能夹带并发请求。
- migration 遇重复数据会静默跳过 unique index，但启动仍声称 ensured。

改进：集中 DB bootstrap/PRAGMA；事务使用互斥或专用 connection；版本化、可重入、事务化 migration。发现无法建立唯一约束时 readiness 必须失败，并输出隔离/修复报告。

### P1-10 Server 状态真源缺 stale/reconciler

证据：`ESP-server/src/services/deviceStatusService.js:159-168,724-745`。

Server 永久信任 S3 child 状态，timeout scan 排除 S3 来源，且 scan 只在 GET 状态时触发。S3 失联后 C5 可长期显示 online；旧 scan 与新 heartbeat 竞争时还可能产生假 offline。

改进：后台 reconciler 定时运行；gateway 超时后 child 先转 `unknown/stale`，而不是直接覆盖为普通 timeout。UPDATE 必须带 `last_seen <= cutoff` 条件，状态 transition 只记录一次。

### P1-11 SSE 和优雅关停缺容量与 deadline

证据：`ESP-server/src/services/eventStreamService.js:33-83`、`server.js:196-223`。

SSE 每客户端有 timer，但忽略 `res.write()` backpressure，没有最大积压、cursor/replay 或 `closeAll()`。慢端可推高内存，活跃 SSE 也可能让 `httpServer.close()` 无限等待。

改进：每客户端限制 buffered bytes/lag，监听 error/close，帧增加 event ID，支持 `Last-Event-ID` 从 event_logs 补洞。关停顺序：停止接入 -> closeAll SSE -> 限时 close HTTP -> close DB -> 超时非零退出。

### P1-12 Server 启动、恢复和容量治理不足

证据：`ESP-server/server.js:225-267`、`src/services/dashboardService.js:45-46,670-729,1263-1280`，以及对 `src/`、`scripts/`、部署文档的 retention/backup/restore 负面扫描。

- 启动成功事件在 listen 成功前写入，端口绑定失败可能留下假成功记录。
- Dashboard snapshot 存在内存先于 DB 更新，CSI current map 重启未从事实表恢复。
- CSI/event/snapshot 没有统一 retention、磁盘水位、backup/restore 与 RPO/RTO。

改进：listen/error/readiness 明确分阶段；DB commit 后再替换内存；启动从事实表恢复；按表定义 retention/rollup；磁盘阈值告警和定期 restore drill。

### P1-13 健康语义和可观测面分裂

证据：S3 `network_worker/network_worker.c:510-554`、`offline_policy/offline_policy.c:18-87`、`local_http_server/local_http_server.c:690-708`、`runtime/s3_scheduler.c:1453-1470`；C5 `server_comm/gateway_link.c:222-240`；Server route scan 与 `ESP-server/docs/deploy-branches.md:110-119`。

- S3 `server_available` 当前混合了 transport 可达、Server ready 和最后一个业务请求结果；4xx/409/429 也会造成全局不可用翻转。
- S3 queue/drop/coalesce/heap 指标主要在串口 heartbeat，local health 未完整暴露压力和 worker 进度。
- C5 只有总链路 READY/LOST，没有 per-endpoint/subsystem `HEALTHY/DEGRADED/RECOVERING` 和 last-success age。
- Server 只有 time status 被文档称为 health，没有 DB/readiness、disk、event loop、SSE、reject rate、queue age。

改进：三层统一拆分 `liveness`、`readiness`、`dependency health`、`business rejection`；健康状态由单 owner 更新，禁止多个 worker无锁 last-writer-wins。

### P1-14 S3 本地身份、注册和资源生命周期没有统一门禁

证据：`ESPS3/components/Middlewares/runtime/s3_scheduler.c:682-707,808-910`、`resource_manager/resource_manager.c:182-223,307-445`、`child_registry/child_registry.c:279-393,657-687`、`local_http_server/local_http_server.c:821-915`、`network_worker/network_worker.c:1285-1383`。

- 请求自报的 allowlisted `device_id` 会在 generation/freshness 检查前直接重映射 peer IP/MAC；pending、ACK、voice 也不验证 request peer 是否绑定到 target device。
- session 初始为 RELEASED，但 heartbeat/sensor/CSI 可在 register 前直接恢复 ACTIVE；未 registered 的 registry view 又被 timeout tick 跳过，可形成长期 `resource=ACTIVE + registry=unregistered`。
- SoftAP station disconnect 无法映射到 device 时会 `release_all`，单个未知/陈旧 station 事件可释放所有合法 C5。

改进：每个 boot/link generation 必须先通过 REGISTER 建立 `device_id <-> station MAC <-> session generation` 绑定；其他 signal 只能维持同一已注册绑定。peer mapping 只能在身份和 freshness 通过后提交。pending/ACK/voice 同时核对 request peer、target、command owner。unmapped disconnect 进入隔离/reconcile，不允许扩大为全局 release；registry/resource 不一致时 fail-closed 并报警。

验收：第三个 station 或 C51 声明 C52 无法重映射、拉取、ACK 或 voice；register 前 heartbeat/sensor/CSI 不激活资源；延迟旧 ingress 不改 peer；未知 disconnect 不影响两台合法 C5；真实 mapped disconnect 只释放对应 generation。

### P1-15 S3 critical path 的无限等待可把“零丢包”变成“无进展”

证据：`ESPS3/components/Middlewares/runtime/s3_scheduler.c:1323-1344,1513-1545`、`network_worker/network_worker.c:2438-2463`、`sdkconfig:1344-1348`。

CRITICAL/REALTIME bus 满时 producer 无限重试；CRITICAL 转 protocol/CSI/stream worker 又使用 `portMAX_DELAY`。命令 ACK 也无限等待 command queue。Task WDT 虽启用为 5 秒，但未启用 panic/reset。下游停止消费时，producer、scheduler 或 protocol worker 可永久阻塞；此时 drop 仍可能为 0，串口 heartbeat 也可能停止，表面指标会误导排障。

改进：为每层可靠队列定义最大等待、预留容量和 deadline；超时后进入可持久 retry/outbox 或显式 DEGRADED，不在共享 worker 内无限等待。增加 `last_progress_ms`、blocked duration、queue HWM 和 owner task 状态；先完成 coredump/reset reason 证据链，再为不可恢复 stall 设计受控 watchdog recovery。

验收：暂停任一下游 worker 并灌满队列，2 个正常调度周期内给出 DEGRADED/503，local control p99 小于 500 ms，且未出现永久卡死；恢复后 backlog 收敛且顺序/所有权正确；`drop=0` 必须同时满足 progress age 和 latency SLO。

## 6. P2：中期治理

### P2-1 C51/C52 parity 和多仓库发布门禁自动化

当前没有 CI workflow、parity 脚本或统一 release gate。应自动检查：

- 顶层与嵌套 Server repo ownership、dirty/untracked source。
- C51/C52 identity allowlist 之外的目录/文件 parity。
- 三份 active protocol header 的语义合同。
- C5 禁止 `/api`，raw CSI negative scan。
- JS syntax、temp-DB tests、固件 build、文档 route 生成校验。

### P2-2 固件版本、OTA、rollback 和 crash 证据

- C5 只有 factory app，无 OTA slot；S3 有双 OTA但 rollback 未开启。
- S3 coredump 分区存在，但配置为不生成 coredump；三块固件未统一记录 reset reason。
- 三套固件上报固定版本，缺 git SHA/config hash；C51/C52 仍使用同名工程产物。

先建立 artifact manifest 和签名发布，再做 OTA/rollback；secure boot/flash/NVS encryption 需在 provisioning 与恢复演练完成后分阶段启用，不能在生产事故中临时切换。

### P2-3 结构拆分应后置于行为门禁

S3 `network_worker.c`、`s3_scheduler.c`、`protocol_adapter.c` 职责较重。建议先用 golden tests、queue/fault tests 固化行为，再按 `link state`、`upload/outbox`、`command sync`、`CSI/BME adapter` 拆分。直接重写会增加时序回归风险。

### P2-4 日志降噪与结构化

C5 CSI 100ms 稳态成功日志过多，错误容易被淹没。稳态改为周期聚合，state transition/error 立即输出。所有日志统一字段：`timestamp, level, component, event, boot_id, trace_id, device_id, gateway_id, error_code, retryable, attempt, elapsed_ms`。

### P2-5 文档单一事实源

当前 API 边界、项目结构、部署健康检查和最小联调清单都有漂移：`ESP-server/docs/api.md:2145-2293` 仍称通用 ingest 接受已迁出的 CSI/occupancy；`ESP-server/docs/deploy-branches.md:14-40` 的后端部署清单漏掉运行所需的 `src/`，`:110-119` 又把 `/api/time/status` 当成 health；`docs/esp-111-minimal-integration-checklist.md:52,222-230` 在普通联调/排障步骤中包含 `erase-flash`。文档必须带 `适用版本/commit/schema/owner/最后验证日期`；route/schema 由代码生成或自动比对。旧清单移入 archive 并显式标记，不允许事故中直接执行破坏性操作。

## 7. 建议的端到端可靠性合同

### 7.1 统一消息身份

所有可重试写请求至少包含：

| 字段 | 作用 |
|---|---|
| `event_id` | 跨重试稳定不变，Server unique key |
| `trace_id` | 串联 C5、S3、Server、SSE/command |
| `boot_id` | 区分设备重启后的 seq 复用 |
| `seq` | 单 boot 单调递增 |
| `schema_version` | 合同版本 |
| `device_id/gateway_id` | 经认证后绑定的身份 |
| `created_at/received_at` | 区分事件时间与各层接收时间 |
| `attempt` | 传输尝试次数，不改变 event_id |

### 7.2 统一结果语义

错误响应统一为：

```json
{
  "ok": false,
  "code": "STABLE_ERROR_CODE",
  "message": "bounded public message",
  "retryable": true,
  "request_id": "...",
  "server_time_ms": 0
}
```

- 4xx validation/ownership 不重试。
- 409 idempotent duplicate 返回原 accepted result，不作为 Server unavailable。
- 429/503 可重试并返回 `Retry-After`。
- timeout/connection failure 由 outbox 重试，不能靠业务调用栈无限阻塞。

### 7.3 数据真源

- C5：传感采集、局部 validity、执行 journal、boot/reset reason。
- S3：register-first peer binding、child registry、协议融合、CSI state、可靠 outbox、资源和依赖状态。
- Server：持久事实、命令全局状态、审计、retention、SSE replay。
- Server 不推断 C5 原始 CSI；C5 不直连 Server；S3 不把 Server timeout 反写成 C5 传感事实。

## 8. 更优的问题处理流程

```mermaid
flowchart LR
    A["检测: 指标/告警/用户现象"] --> B["关联: incident_id + trace_id + boot_id"]
    B --> C["分层: C5 / S3 / Server / 外部依赖"]
    C --> D["止损: 降频/隔离/只读/暂停命令"]
    D --> E["恢复: 状态机重试/重校准/replay/reconcile"]
    E --> F["验证: SLO + 数据一致性 + 无重复副作用"]
    F --> G["复盘: 时间线/根因/门禁/owner/截止日期"]
```

### 8.1 七步闭环

1. **检测**：告警必须指向可执行状态，不以单条 console log 作为唯一信号。
2. **关联**：生成 incident_id；收集 device/gateway/server 的 boot_id、trace_id、版本、reset reason 和 last-success age。
3. **分层**：先判断 local C5-S3 是否健康，再判断 S3-Server transport/readiness，最后判断业务 validation/DB/外部依赖。
4. **止损**：保护 control plane；暂停新 command、降低 CSI/BME background cadence、隔离异常 gateway、Server 必要时进入受控只读。
5. **恢复**：只执行可逆动作；按状态机重试、重校准、replay 和 reconcile 恢复服务。
6. **验证**：检查队列清空、outbox 收敛、无重复 command、DB/内存一致、状态 transition 唯一，并达到 9.1 的 SLO。
7. **复盘**：保存不可变时间线，记录根因而非最后症状；每项改进必须有 owner、deadline、测试和自动门禁。

### 8.2 故障分诊表

| 现象 | 第一检查点 | 分层判断 | 止损 | 恢复完成判据 |
|---|---|---|---|---|
| 单个 C5 offline | S3 child registry、C5 boot/link generation | WiFi、身份/NVS、heartbeat、voice busy | 暂停该 device command | 新 generation register 一次，heartbeat 连续恢复，旧状态不反跳 |
| 两个 C5 同时 offline | S3 SoftAP/resource/heap、同步重连 | 优先判 S3，不先刷两块 C5 | 降低 background/CSI，保护 health/register | SoftAP stable，双设备带 jitter 恢复，critical drop=0 |
| BME 本地 accepted 但 Server 无数据 | S3 cache fill/oldest age、Server ingest reject/DB | local 成功与上云失败分开 | 禁止清 cache/重启；先保护 RPO | outbox 收敛、Server event_id 去重、overwrite=0 |
| CSI 卡住或假 MOTION | C5 calibration/link generation、S3 fusion freshness | 区分输入 stale、fusion、上云 | 标记 unknown/calibrating，不伪造 IDLE | 有效样本门槛完成，state transition 符合回放预期 |
| command 一直 dispatched | Server lease/attempt、S3 ACK outbox、C5 journal | 判断未执行、已执行未 ACK、终态冲突 | 暂停相同副作用命令 | 执行次数=1，Server 终态唯一，attempt/time line 完整 |
| voice 超时 | C5 phase、S3 busy/resource、Server stage timing | upload、ASR、LLM、TTS、download 分段 | 取消当前 turn，释放资源，不重启全系统 | 资源归零，Mic 回 LISTENING，下一 turn 成功 |
| Server 5xx/DB busy | `/ready`、DB busy/disk/migration | 进程活着不等于可写 | 停止写流量或只读，保留 outbox | DB write probe、migration、disk、latency 达标 |
| Dashboard 状态不一致 | DB fact、memory latest、SSE cursor | 区分事实错误与派生视图错误 | 暂停派生覆盖，不改原始事实 | 重启恢复一致，cursor 补洞，无重复 event |

### 8.3 事故关闭条件

不能以“日志不再报错”关闭。至少满足：

- 关键 queue/outbox 回到稳定区间，oldest age 不再增长。
- C5/S3/Server 的版本、boot_id、trace 可关联。
- command 无重复副作用，BME/CSI 重放无重复事实。
- `HEALTHY/DEGRADED/RECOVERING/STALE` 状态与实际故障一致。
- 观察至少一个完整业务周期；重大网络/资源事故需经过设定 soak 窗口。
- 复盘条目已进入自动测试、监控或发布门禁，不能只写“以后注意”。

## 9. 建议监控指标与初始阈值

以下是初始目标，必须用真机基线校准：

| 层 | 指标 | 初始目标/告警 |
|---|---|---|
| C5 | control event drop | 必须为 0；任何非零告警 |
| C5 | worker depth/high-water/inflight/last success | >80% 持续 10s warning；无进展超过 2 个正常周期 degraded |
| C5 | BME invalid/recovery | 连续 invalid/NACK 达阈值进入 recovering；恢复次数可查询 |
| C5 | CSI calibration age/gap | link generation 变化必须 recalibrate；未完成时不输出有效状态 |
| S3 | CRITICAL/REALTIME drop | 必须为 0 |
| S3 | producer/dispatcher/worker progress age | 任一关键 owner 超过 2 个正常周期无进展即 degraded；有队列深度但无进展为 critical |
| S3 | registry/resource/peer binding | 状态分叉、未注册 ACTIVE、peer bind reject、unmapped release-all 均需计数；分叉和 release-all 必须为 0 |
| S3 | BME cache fill/oldest age/overwrite | 70% warning；90% critical；overwrite >0 critical |
| S3 | command ACK outbox oldest age | 超过 command lease 的一半 warning；超过 deadline critical |
| S3 | heap/largest block/task HWM | 记录最低值和趋势，不只看当前 free heap |
| Server | 5xx/validation reject/auth reject | 按 route/gateway/version 分组，异常斜率告警 |
| Server | DB busy/write latency/disk | `SQLITE_BUSY` 非零告警；disk <20% warning、<10% critical |
| Server | SSE clients/buffer/lag | 单客户端和总 buffer 有硬上限；超限断开并可 cursor 重放 |
| Server | command attempts/dead-letter | attempt 超阈值进入 delivery_uncertain/dead-letter |
| 全链 | trace completeness | 每条关键 command/CSI/BME 可查到接收、持久化、派生和最终状态 |

### 9.1 故障测试初始通过参数

这些值用于让验收可判定，不是永久产品常量；首轮真机基线后必须版本化到 test profile。

| 参数 | 初始通过门槛 |
|---|---|
| local control p99 | voice/queue 压力下 health/register/pending/ACK 延迟小于 500 ms |
| critical progress age | 不超过 2 个对应正常调度周期；超出即失败，不以 drop=0 代替 |
| voice cancel/release | 断联或取消后 2 s 内 active lease、busy、socket 和资源状态归零/收敛 |
| command journal | 至少 128 个 ring slot；终态保留 7 d 且不少于 Server 最大 retry/dead-letter 窗口；每命令最多 4 次 durable state write；按峰值命令率推算 flash 寿命不少于 5 年 |
| BME recover | 连续 3 次 invalid/NACK 进入 RECOVERING；传感器恢复后 30 s 内重新产出有效样本 |
| CSI static false motion | 完成校准后的静态场景连续 30 min，错误 MOTION transition 为 0 |
| authority stale | gateway/child 连续缺失 3 个预期 heartbeat/snapshot 周期且至少 30 s 后必须转 STALE/UNKNOWN |
| shutdown | SIGTERM 后 10 s 内停止接入、关闭 SSE/HTTP/DB 并退出 |
| memory drift | 1 h 压力测试预热后，C5/S3 free heap/largest block 与 Server RSS 漂移不超过 5%，24/72 h 不得单调恶化 |
| retention/disk | 初始 raw CSI/event/snapshot 保留 30 d、BME raw 保留 90 d、rollup 保留 1 y；按峰值写入外推后占用不得超过分配文件系统 70% |
| critical loss | control、CRITICAL、REALTIME drop=0；BME overwrite=0；所有 retry/outbox 最终收敛 |

## 10. 分阶段改进路线

### 0-3 天：先止血和建立门禁

1. Server production auth fail-closed；给控制/删除/LLM/voice config/memory write 加 RBAC。
2. 禁止 legacy 匿名写刷新 device truth；日志删除改成受控 admin 操作。
3. 把本报告的高风险负面检查加入快速脚本：auth config、C5 `/api`、raw CSI、parity、untracked source、route policy。
4. 为 command、BME、CSI 定义稳定 event_id/boot_id/seq 合同，不立即大改实现。
5. 新增真正的 `/live`、`/ready` 设计与指标清单；旧 time status 不再称为 health。
6. 冻结 S3 `release_all` fallback；为本地 pending/ACK/voice 加 peer-target 审计和拒绝计数。

### 1-2 周：修可靠性主链

1. C5 CSI ingress coalesce + control queue 保留容量。
2. C5 connection generation 强制重新注册；voice quiescence/cancellation。
3. C5 BME validity gate + I2C recovery；CSI gap/link-generation recalibration。
4. C5 command journal、S3 durable ACK outbox、Server 统一 lease/dead-letter。
5. BME/CSI Server unique idempotency + transaction/outbox。
6. Server background reconciler、SSE backpressure/closeAll、shutdown deadline。
7. S3 register-first peer binding、voice/resource 条件状态转换、critical queue bounded progress。

### 3-6 周：运行治理

1. DB PRAGMA、版本化 migration、retention、backup/restore、disk watermarks。
2. 三层结构化 metrics/trace、incident runbook、自动告警。
3. 故障注入：断网、重启、DB busy/disk full、ACK 丢失、SSE slow client、voice timeout、queue saturation。
4. C51/C52 parity、protocol contract、firmware build、Server tests 纳入 CI。

### 6 周以后：发布与架构治理

1. artifact manifest、签名发布、S3 OTA rollback；评估 C5 分区迁移。
2. reset reason/crash fingerprint/coredump 受控采集。
3. 在行为测试覆盖后拆分 S3 重文件，不改变 public API 和现有优先级语义。
4. 完成 provisioning 后再评估 secure boot、flash/NVS encryption。

## 11. 验收矩阵

| 场景 | 注入 | 必须证明 |
|---|---|---|
| P0 gateway auth | production 缺 token、错 token、disabled gateway、A 冒充 B | 启动 fail-closed 或请求 401/403；last_seen、binding、业务表均无副作用 |
| P0 route RBAC | 匿名/错角色遍历所有写、删除、隐私和外部计费路由 | 全部按 policy matrix 拒绝；无 DB/外部调用副作用；拒绝事件可审计 |
| P0 legacy write | 直接调用、伪造 IP/header/body、受控迁移代理调用 | 直接/伪造请求拒绝；仅认证代理成功并完整透传 identity；到期计数为 0 |
| C5 bus 压力 | 500 Hz CSI + 5s S3 delay | control drop=0，CSI 只 coalesce；1 h memory drift 符合 9.1 |
| 短 WiFi bounce | <1s 断连 + 清 S3 registry | C5 立即离开 READY，新 generation 只 register 一次 |
| Voice 抢占 | system/CSI HTTP 在途时 wake | local control p99 <500 ms；取消后 2 s 内连接和资源收敛 |
| S3 voice 断联 | voice turn 中断 SoftAP station | registry/resource 保持同一 generation 的 link_lost/released，不被清 busy 写回 online |
| S3 身份绑定 | C51 声明 C52、register 前发 sensor、未知 station 断开 | 冒用全部拒绝；未注册不 ACTIVE；未知断开不释放合法设备 |
| S3 reliable queue | 暂停 protocol/CSI/stream consumer 并灌满 critical queue | 2 个正常周期内明确 degraded，control 仍达 SLO，恢复后 backlog 收敛 |
| BME 故障 | invalid heater/gas、连续 NACK、拔插 | 第 3 次失败进入 recovering，恢复后 30 s 内有效；baseline 不污染 |
| CSI 环境变化 | S3 重启/换信道/移动 | 先 calibrating；静态 30 min 错误 MOTION=0，再输出有效 fact |
| Command ACK 丢失 | 丢 ACK、C5/S3 重启、重复 dispatch、填满 8 个本地槽 | effectively-once：设备动作一次、终态唯一、attempt 可追踪、槽位按 TTL 收敛 |
| Server duplicate ingest | 同 event_id 并发/重放 | 一份事实、一组 event/SSE、返回幂等结果 |
| DB 故障 | busy、migration duplicate、disk full | readiness fail、无部分提交、outbox 保留、恢复可重入 |
| SSE 慢端 | 慢/断客户端 + 高事件速率 | 1 h RSS 漂移 <=5%，慢端隔离、cursor 补洞、SIGTERM <=10 s |
| 权威状态失联 | 断 S3 但 Server 存活 | 3 个预期周期且至少 30 s 后 child 转 stale/unknown，恢复只产生一次 transition |
| 24/72h soak | 双 C5 + S3 + Server 波动 | 内存无单调恶化、critical drop/overwrite=0、outbox 收敛、DB 外推占用 <=70% |

## 12. 明确不建议的改法

- 不允许 C5 为“容灾”直连 Server；这会破坏唯一网关边界和认证模型。
- 不回放全部高频 CSI 历史；继续保持 latest/coalesce，事实只在 S3 融合后输出。
- 不把 CSI 算法重新放到 Server，也不上传 raw CSI/IQ/subcarrier。
- 不在没有 golden/fault tests 前直接重写 scheduler/network worker。
- 不用真实 `ESP-server/db/database.db` 做测试；所有验证使用临时 DB。
- 不在事故中直接 erase flash、开启 secure boot/encryption 或做不可逆 provisioning。
- 不把“build 通过”当作断网、资源、真机时序和数据一致性已经通过。

## 13. 本轮静态验证

已确认：

- C51/C52 runtime、sensor、voice、command 业务目录 parity 通过，身份配置是已知差异。
- C5/S3 已跟踪差异 `git diff --check` 无 whitespace error；该命令不覆盖未跟踪源码，未跟踪 runtime 通过本轮定向 parity/源码复核，但仍需纳入 P2 release gate。
- C5/S3 production serializer/client 未发现 raw CSI/IQ/subcarrier 外发字段；相关命中仅为 S3 protocol adapter 的显式拒绝清单、注释和无关的 HTTP `phase` 变量。
- ESP-server `server.js`、`src/`、`server-time-sync/`、`scripts/` JS 语法检查通过。
- ESP-server 嵌套 repo `git diff --check` 通过；本轮未修改 `public` 或真实 DB。

未确认：

- 当前持续变化的三套固件能否完整 build。
- 真机 WiFi bounce、S3 重启、CSI/BME/voice/command 闭环。
- Server 并发事务、DB busy/disk full、SSE slow consumer 和 shutdown deadline。
- 真实生产鉴权配置、网络暴露面、备份恢复和 24/72h soak。

## 14. 推荐执行顺序

不要从“大文件拆分”开始。建议严格按以下顺序：

1. P0 auth/RBAC/legacy write 边界。
2. 统一 event identity 和错误语义。
3. command effectively-once effect + BME/CSI idempotent ingest。
4. C5 queue/link/voice/BME/CSI 恢复缺口。
5. S3 outbox/health/resource 可观测性。
6. Server DB/reconciler/SSE/shutdown/retention。
7. 故障注入与 soak 门禁。
8. 最后才做 S3 结构拆分、OTA 和长期安全加固。

这样每一步都能用前一步建立的 trace、指标和故障测试验证，避免“为了稳定重构，却无法证明更稳定”。

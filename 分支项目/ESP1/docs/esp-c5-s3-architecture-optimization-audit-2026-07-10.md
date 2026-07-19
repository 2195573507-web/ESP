# ESP-C5/S3 架构优化审计报告

审计日期：2026-07-10  
审计范围：`ESPC51`、`ESPC52`、`ESPS3`  
审计方式：只读静态源码审计，未修改源码或配置，未执行编译、烧录、运行服务、git commit。

## 1. 当前架构总结

### 1.1 三层职责

当前源码体现的是较清晰的三层架构：

- `ESPC51` / `ESPC52`：ESP32-C5 终端，负责本地 WiFi/CSI/BME/语音链路、轻量边缘计算、C5 本地状态维护，并只访问 ESPS3 暴露的 `/local/v1` 本地接口。
- `ESPS3`：ESP32-S3 Gateway，负责 SoftAP/STA 双网络、C5 child/session 管理、协议适配、CSI 双链路融合、上传 ESP-server、语音代理和本地 HTTP server。
- `ESP-server`：本轮不审计后端源码，只作为 S3 上云目标和事实中心接口边界参考。

整体分工是合理的：C5 做采集和轻量边缘特征，S3 做网关、融合、状态管理和上云，Server 做事实存储。当前源码没有发现 C5 直接访问 `/api/...` Server 路径的行为；`server_comm_build_url()` 明确拒绝 `http://` / `https://` 绝对地址，并要求 endpoint 以 `/local/v1` 为前缀。

### 1.2 C51/C52 同构状态

`ESPC51/components/Middlewares` 与 `ESPC52/components/Middlewares` 的目录 diff 只发现 3 类预期差异：

- `sensor_domain/csi_placeholder/csi_server_client.c`：本地 link/report 身份不同。
- `server_comm/server_comm_config.h`：设备/网关配置不同。
- `terminal_config/terminal_config.h`：终端身份配置不同。

两侧共享协议头 `esp111_protocol_common.h` 当前一致。因此 C5 风险应按 `ESPC51/ESPC52 同构风险` 处理，除身份配置外不应拆成两套不同设计。

### 1.3 C5 当前运行模型

C5 侧默认启用 CSI：`MAIN_ENABLE_CSI_SERVICE=1`，CSI feature 输出周期为 `100ms`。运行时由一个 dispatcher 加三个 worker 组成：

- dispatcher：`C5_SCHEDULER_TASK_STACK=12288`，优先级 `4`。
- worker：CSI/BME/system 三类 worker，`C5_WORKER_TASK_STACK=8192`，优先级 `3`。
- event bus：单队列长度 `24`，事件类型包括 CSI、BME、heartbeat、status、command。

优点是 dispatcher 只做路由，业务 HTTP/JSON/传感器路径下沉到 worker；CSI/BME/system 分队列，降低 CSI 高频事件挤占 heartbeat/status/command 的概率。主要风险不是 task 数量过多，而是 CSI callback 与共享 event bus/worker 队列之间的背压策略仍然偏粗。

### 1.4 C5 CSI 当前链路

CSI callback 当前不做 HTTP、不做 heap 分配、不做复杂算法，只把最新 CSI 样本拷贝到固定 pending buffer，然后投递 `C5_EVENT_CSI_READY`。CSI worker 再执行 `csi_service_process_tick()` 和按周期执行 `csi_service_report_tick()`，上报只发轻量 feature，不上传 raw CSI、I/Q buffer 或子载波矩阵。

这是正确方向，但当前仍有两个结构性压力点：

- callback 每存到一次 pending sample 就投递一次事件，但 pending sample 本身只有 latest/overwrite 语义，事件数量可能大于有效数据数量。
- `app_orchestrator_start()` 在启动 CSI service 后才启动 C5 scheduler/worker，早期 callback 可能先于 dispatcher/worker 准备完成。

### 1.5 C5 网络与状态

C5 `gateway_link` 状态包括 `LINK_DOWN`、`LINK_WIFI_CONNECTED`、`LINK_REGISTERING`、`LINK_READY`、`LINK_LOST`。当前 WiFi down 会立即进入 `LINK_DOWN` 并触发重连；拿到 IP 后进入 `LINK_WIFI_CONNECTED`，随后 health probe、register 成功才进入 `LINK_READY`。普通非语音任务通过 `c5_should_run()` 被 voice/gateway/cpu/backpressure gate 限制。

这说明旧式“短 WiFi 抖动仍保留 READY”的问题在当前源码里没有复现。但资源关闭是 gate 型：CSI callback 本身仍可能处于 started 状态，真正停止处理/上报发生在 worker tick 的 `c5_should_run()`，并非所有资源都被主动 pause/stop。

### 1.6 S3 当前运行模型

S3 侧由 `network_worker`、`s3_scheduler`、`protocol_worker`、`stream_worker`、`csi_fusion_worker`、`upload_worker`、`command_worker` 等任务共同组成：

- `network_worker`：优先级 `5`，网络状态推进和本地/上云 gate。
- `s3_scheduler`、`upload_worker`、`command_worker`：优先级 `4`。
- `protocol_worker`、`stream_worker`、`csi_fusion_worker`：优先级 `3`。
- S3 priority event bus 有 `CRITICAL`、`REALTIME`、`STATE`、`BACKGROUND` 四层；STATE 事件 coalesce latest，CRITICAL/REALTIME 队列满时返回 timeout。

整体优先级设计合理：网络状态高于调度，上云/命令高于协议解析/stream/CSI worker，且 S3 关键 worker 已注册 task WDT。主要风险来自若干共享状态和长路径仍集中在单 task/单锁中。

### 1.7 S3 网络状态与 C5 session 管理

S3 scheduler 网络状态与用户指定检查项一致：`NET_NOT_READY`、`STA_CONNECTED`、`IP_READY`、`LINK_STABLE`。`network_worker` 的 `evaluate_state()` 只让本地 ingest 依赖 SoftAP，STA/IP/Server ready 只影响上云 gate，避免本地 C5 register/heartbeat/sensor/CSI 因 Server 不可用被暂停。

`resource_manager` 已经能在 C5 释放时暂停 command/sensor/http/CSI/queue，并能在身份确认后 restore；`child_registry` 有 `ONLINE`、`VOICE_BUSY`、`LINK_LOST`、`OFFLINE`。这一套生命周期比单纯在线标志更稳，但也带来状态来源复杂度：UI/显示用的 onlineish 状态不能替代资源是否 live 的判断。

### 1.8 S3 CSI 融合与上传

S3 CSI 融合算法当前是固定 link 数的轻量融合：按 quality/freshness/RSSI 和 energy/variance/cv/motion_score 加权，状态机为 IDLE/MOTION/HOLD。复杂度约为 `O(CSI_FUSION_LINK_COUNT)`，当前主要压力不在融合数学本身，而在 CSI ingress、融合锁、JSON/聚合/上传发布路径。

上传链路有 coalesce/drop/backpressure：低优先级在 Server not ready 时会 drop，snapshot 会 coalesce，BME cache 在 2xx 上传后删除，latest CSI 只在 server stable 且有 active sessions 时上传。这个策略对稳定性有利，但需要更清晰地区分“可丢状态”和“必须可靠送达状态”。

## 2. P0 必须修复

### P0-0：本轮未发现 C5/S3 固件范围内的立即阻断项

- 文件位置：全局审计结论，重点核查 `ESPC51`、`ESPC52`、`ESPS3`。
- 当前行为：未发现 C5 直连 Server `/api/...`、上传 raw CSI、未知 C5 station 断开导致全局释放、WiFi down 后仍保持 `LINK_READY` 这类会立即破坏系统边界的 P0 行为。
- 风险：当前没有源码级 P0；但下方 P1 项若叠加真实硬件高频 CSI、长语音 turn、弱网和命令副作用，可能演化成线上稳定性事故。
- 推荐方案：先按 P1 修改顺序收敛背压、长阻塞、命令幂等和状态源冲突。
- 是否需要修改 C5/S3/server：本项无需立即修改；后续修复主要涉及 C5 与 S3，命令幂等可能需要 Server 配合。

## 3. P1 强烈建议

### P1-1：C5 CSI callback 事件投递与 latest buffer 语义不匹配，存在事件风暴和早期丢事件风险

- 文件位置：
  - `ESPC51/components/Middlewares/sensor_domain/csi_placeholder/csi_service.c:72-103`
  - `ESPC51/components/Middlewares/sensor_domain/csi_placeholder/csi_service.c:170-179`
  - `ESPC51/components/Middlewares/sensor_domain/csi_placeholder/csi_service.c:328-348`
  - `ESPC51/components/Middlewares/app_orchestrator/app_orchestrator.c:98-148`
  - `ESPC51/components/Middlewares/runtime/c5_event_bus.c:92-124`
  - `ESPC51/components/Middlewares/runtime/c5_runtime_workers.c:70-81`
  - `ESPC52` 同名路径同构。
- 当前行为：CSI callback 将最新 sample 拷贝进单个 pending buffer，若已有 pending 会增加 overwrite 计数；随后每次成功 store 都执行 `c5_event_bus_enqueue(C5_EVENT_CSI_READY, C5_EVENT_SOURCE_CALLBACK)`。event bus 长度为 24，`xQueueSend(..., 0)` 非阻塞，满队列直接记 drop。worker queue 同样非阻塞入队。启动顺序上，`app_orchestrator` 先 `csi_service_start()` 配置 WiFi CSI callback，再 `c5_scheduler_start()` 启动 dispatcher/worker。
- 风险：
  - 高频 CSI callback 下，事件数量可能远高于有效 latest sample 数量，worker 处理到的可能是空事件或被 overwrite 后的最新值，浪费 CPU 和 event bus 容量。
  - scheduler/worker 启动前如果 CSI callback 已经触发，会先填充 event bus；队列未及时 drain 时会产生早期丢包。
  - WiFi/link 未 ready 时，处理/上报会被 `c5_should_run()` gate，但 callback 仍可能持续产生事件压力。
- 推荐方案：
  - 调整启动顺序：先初始化并启动 dispatcher/worker，再启用 WiFi CSI callback；或者在 scheduler ready 前不置 `s_csi_started=true`。
  - 将 CSI event 改成 coalescing/notify 语义：例如 pending bit、长度 1 overwrite queue、`xTaskNotifyGive`，只保证“有最新样本待处理”，不要每帧都入全局 event bus。
  - 分离 `pending_sample_overwrites`、event drop、worker queue drop 三类指标，并在诊断日志中分别输出。
- 是否需要修改 C5/S3/server：需要修改 C5；S3 和 Server 不需要改接口。

### P1-2：C5 voice exclusive pause 不能中断已在途 HTTP/stream，语音期间仍可能被旧请求占用 socket 和 CPU

- 文件位置：
  - `ESPC51/components/Middlewares/runtime/app_runtime.c:45-79`
  - `ESPC51/components/Middlewares/server_comm/server_comm_http.c:275-333`
  - `ESPC51/components/Middlewares/server_comm/server_comm_http.c:474-600`
  - `ESPC51/components/Middlewares/server_comm/server_comm_http.c:799-921`
  - `ESPC52` 同名路径同构。
- 当前行为：`app_runtime_pause_non_voice()` 设置 non-voice HTTP gate 并暂停 BME，等待 BME paused 后返回。`server_comm_check_ready()` 能阻止新发起的普通 HTTP，但已经进入 `esp_http_client_perform()` 的请求没有中途取消点；fixed raw stream 在 `server_comm_http_post_raw_fixed_stream_begin()` 中 open 并完整写入 body 后才把 stream handle 暴露给调用方，`server_comm_stream_write_all()` 循环写入期间也没有 abort/non-voice-paused 检查。
- 风险：语音 turn 进入后，旧的 BME/CSI/status/command HTTP 可能继续占用 socket、heap、CPU 或等待网络 timeout，导致语音延迟、播放卡顿、弱网下长时间阻塞，甚至影响 watchdog 余量。
- 推荐方案：
  - 为 non-voice HTTP 增加 in-flight registry 和 abort flag，voice pause 时标记取消；在 perform 前后、body write 循环、response read 循环都检查。
  - raw fixed stream 写入阶段暴露可取消上下文，或者改成可分片写入并定期检查 voice/non-voice gate。
  - 对 CSI/BME/status/command 使用更短的非语音 timeout，并在 voice busy 期间只保留必须本地状态，不做外发。
- 是否需要修改 C5/S3/server：需要修改 C5；S3/Server 接口不需要改。

### P1-3：C5 BME690 空气质量基线未使用 `gas_valid` / `heat_stable`，无效样本可能污染 IAQ

- 文件位置：
  - `ESPC51/components/Middlewares/sensor_domain/bme690/driver/bme690.h:344-358`
  - `ESPC51/components/Middlewares/sensor_domain/bme690/driver/bme690.c:1076-1099`
  - `ESPC51/components/Middlewares/sensor_domain/bme690/service/bme_air_quality.c:94-128`
  - `ESPC51/components/Middlewares/sensor_domain/bme690/service/bme_sensor_service.c:182-227`
  - `ESPC52` 同名路径同构。
- 当前行为：driver 已经在 `bme690_data_t` 中暴露 `new_data`、`gas_valid`、`heat_stable`，并在 `bme690_read()` 填充。但 `bme_air_quality_update()` 只检查 `gas_resistance_ohm > 0` 和 finite，就会递增 sample count、更新 EMA 和 baseline。BME service 对 read/calc/upload 失败主要是 log 后返回，没有 DEGRADED/RECOVERING 状态机。
- 风险：heater 未稳定或 gas invalid 时，baseline 仍可能被拉偏，后续空气质量评分长期错误；弱硬件/I2C 抖动时，系统只表现为周期性失败日志，缺少恢复、降级和健康状态输出。
- 推荐方案：
  - AQ 更新前必须检查 `new_data && gas_valid && heat_stable`；不满足时只输出 warmup/degraded，不更新 baseline。
  - 增加连续失败计数、恢复计数和 BME runtime 状态：`WARMUP`、`OK`、`DEGRADED`、`RECOVERING`。
  - 上传 payload 保持兼容：可新增内部质量字段，但旧字段语义不能变。
- 是否需要修改 C5/S3/server：需要修改 C5；若要把 degraded/warmup 暴露给 Server，可在 C5->S3 payload 和 S3 转发中做 additive 扩展；Server 可不改或只做可选字段接收。

### P1-4：C5 命令执行与 ACK 没有本地 journal，ACK 丢失后可能重复执行有副作用命令

- 文件位置：
  - `ESPC51/components/Middlewares/command_domain/system_command/system_service.c:75-82`
  - `ESPC51/components/Middlewares/command_domain/system_command/system_server_client.c:601-687`
  - `ESPC51/components/Middlewares/command_domain/system_command/system_server_client.c:858-927`
  - `ESPC52` 同名路径同构。
- 当前行为：system worker 周期拉取 pending command，`limit=1`，解析后立即执行并 POST ACK。若执行完成但 ACK 请求失败，C5 本地没有 durable journal 记录 command_id 的完成状态；下一轮 Server 仍可能返回同一命令。ACK 构造阶段会 escape error_code/error_message，但最终 JSON 只携带本地 error 数值，没有携带详细错误文本。
- 风险：对于重启、控制设备、状态切换等有副作用命令，ACK 丢失会导致 at-least-once 执行，可能重复触发动作；排障时 Server 端也拿不到 C5 详细错误信息。
- 推荐方案：
  - C5 维护 command journal：`command_id`、`seq`、`state`、`result`、`last_ack_attempt`，至少覆盖最近 N 条，可用 NVS 或小型环形持久区。
  - ACK 失败时重试 ACK，不重复执行已完成 command；收到重复 command_id 时直接重发上次 ACK。
  - Server 侧最好增加 command idempotency/lease/ack 状态约束，避免重复投递已完成 command。
  - ACK payload additive 增加 error_code/error_message 字段，保留当前本地 error 数值。
- 是否需要修改 C5/S3/server：需要修改 C5；强烈建议 Server 配合幂等；S3 若只透传本地命令可不改，若统一管理命令 ACK 则需要新增可靠 outbox。

### P1-5：S3 local HTTP control plane 与 voice turn 共用同一 httpd task，存在队头阻塞风险

- 文件位置：
  - `ESPS3/components/Middlewares/local_http_server/local_http_server.c:581-616`
  - `ESPS3/components/Middlewares/local_http_server/local_http_server.c:1121-1157`
  - `ESPS3/components/Middlewares/voice_proxy/voice_proxy.c:76-115`
  - `ESPS3/components/Middlewares/voice_proxy/voice_proxy.c:188-205`
  - `ESPS3/components/Middlewares/voice_proxy/voice_proxy.c:263-401`
- 当前行为：local HTTP server 使用一个 `httpd_start()`，stack `8192`，`max_uri_handlers=13`；health/register/heartbeat/status/sensor/CSI/device_stream/voice/command routes 都注册到同一 server。`voice_proxy_handle_turn()` 在 handler 中同步读取 PCM、调用 `server_client_post_voice_turn()` 上游请求，并通过 `httpd_resp_send_chunk()` 把响应流式回 C5。
- 风险：长语音 turn 或上游 Server 慢响应时，httpd 处理线程可能长时间被 voice handler 占用，影响 register/heartbeat/sensor/CSI/command ACK 等控制面请求；弱网下会放大 C5 `LINK_LOST`、heartbeat grace、CSI 丢包和语音卡顿。
- 推荐方案：
  - 拆分 voice route：本地 HTTP handler 只做鉴权、body 接收或入队，把上游 voice proxy/streaming 放到专用 voice worker 或单独 httpd server/task。
  - 若必须保持同步 HTTP 语义，至少调大 httpd 并发/stack，给 voice 单独 URI handler 工作队列和更强超时/取消策略。
  - 保持 `/local/v1/voice/turn` 外部接口不变，内部解耦 control plane 和长流式代理。
- 是否需要修改 C5/S3/server：主要修改 S3；C5/Server 接口可不变。

### P1-6：S3 CSI 融合锁覆盖发布路径，慢 JSON/聚合/上传会阻塞 CSI ingress

- 文件位置：
  - `ESPS3/components/Middlewares/csi_placeholder_gateway/csi_placeholder_gateway.c:716-753`
  - `ESPS3/components/Middlewares/csi_placeholder_gateway/csi_placeholder_gateway.c:998-1011`
  - `ESPS3/components/Middlewares/csi_placeholder_gateway/csi_placeholder_gateway.c:1132-1165`
  - `ESPS3/components/Middlewares/csi_fusion/csi_fusion.c:523-545`
  - `ESPS3/components/Middlewares/runtime/s3_scheduler.c:1198-1247`
- 当前行为：CSI ingress 进入 `csi_placeholder_gateway_handle_feature_internal()` 后持有 `s_fusion_lock`，执行 `csi_fusion_update()`，随后仍在锁内调用 `publish_fusion_outputs()`。flush 路径也在持锁期间调用 publish。`publish_fusion_outputs()` 会做日志 JSON 格式化，并调用 `sensor_aggregator_handle_csi_fact()`，后续可能触发聚合和上传队列。
- 风险：融合算法本身复杂度低，但锁持有范围包含 JSON/log/aggregator/queue 等慢路径，会阻塞后续 CSI ingress 或 flush；高频 CSI 与 Server 不稳定叠加时，S3 压力会集中到 fusion lock 上，导致最新数据延迟、丢弃或状态滞后。
- 推荐方案：
  - 锁内只更新 fusion state 并复制 `fact` / `telemetry` 快照；释放锁后再执行 `publish_fusion_outputs()`。
  - 将 telemetry log 的 last-state 节流锁与 fusion 状态锁拆开，避免日志节流状态扩大 fusion 临界区。
  - 为 CSI fusion worker 增加 lock wait / publish duration 指标，区分算法耗时和发布耗时。
- 是否需要修改 C5/S3/server：需要修改 S3；C5/Server 接口不需要改。

### P1-7：S3 Server ready 与 offline_policy 双状态源，弱网下可能出现 gate 与错误码视图不一致

- 文件位置：
  - `ESPS3/components/Middlewares/network_worker/network_worker.c:433-439`
  - `ESPS3/components/Middlewares/network_worker/network_worker.c:558-603`
  - `ESPS3/components/Middlewares/offline_policy/offline_policy.c:18-87`
  - `ESPS3/components/Middlewares/voice_proxy/voice_proxy.c:350-356`
- 当前行为：`network_worker` 内部维护 `s_server_ready`、success/failure counters 和 `server_link_stable()` gate；`offline_policy` 也维护自己的 `s_server_available`、last error 和 failure count。voice proxy、network upload 等路径会记录 offline_policy，而 scheduler/upload gate 主要看 `network_worker` 的 `s_server_ready`。
- 风险：同一次弱网波动下，control gate、错误码、日志、UI/诊断可能观察到不同 Server 状态；问题排查时难以判断到底是 STA/IP 不稳、Server 不可用，还是 voice upstream 失败。
- 推荐方案：
  - 收敛为单一 Server health owner：推荐 offline_policy 或新的 server_health 模块持有阈值、错误码、最后状态和锁。
  - `network_worker` 只消费统一 health snapshot，不再维护第二套 counters。
  - 明确 voice upstream 是否影响全局 Server ready；如果影响，要走同一 owner 和同一阈值策略。
- 是否需要修改 C5/S3/server：需要修改 S3；C5/Server 接口不需要改。

### P1-8：S3 command/ACK 工作队列仍是 RAM-only，重入队失败会丢弃工作

- 文件位置：
  - `ESPS3/components/Middlewares/network_worker/network_worker.c:1816-1854`
  - `ESPS3/components/Middlewares/network_worker/network_worker.c:1871-1900`
  - `ESPS3/components/Middlewares/network_worker/network_worker.c:2326-2363`
- 当前行为：command worker 只有 `LINK_STABLE` 后才处理命令队列，queue 入队/等待均为 RAM 队列。`requeue_or_drop_work()` 失败时直接释放 item，并在注释中说明 S3 不在 RAM 中堆积离线队列。
- 风险：对于 ACK、smart-home 命令或 Server 状态同步这类语义，纯 RAM 队列在重启、队列满、弱网抖动时可能丢失关键确认，造成 Server 事实状态和 S3/C5 实际执行状态分叉。
- 推荐方案：
  - 区分可丢和不可丢工作：snapshot/latest 可 coalesce/drop；command ACK、设备控制结果、关键告警需要 durable outbox。
  - 对 ACK 类 item 增加小型持久队列、指数退避、最大保存时间和 command_id 去重。
  - Server 端配合幂等 ACK，避免重放导致重复副作用。
- 是否需要修改 C5/S3/server：需要修改 S3；建议 Server 配合幂等 ACK；C5 不一定需要改。

## 4. P2 优化项

### P2-1：C51/C52 复制式 Middlewares 维护成本高，建议建立 parity 检查和共享化边界

- 文件位置：`ESPC51/components/Middlewares`、`ESPC52/components/Middlewares`。
- 当前行为：两侧绝大多数实现同构，仅身份/配置相关文件不同；共享协议头当前一致。
- 风险：后续修复 P1 时容易只改 C51 或 C52，造成 CSI JSON、BME payload、命令 ACK、backpressure 策略漂移。
- 推荐方案：把真正共享的 C5 runtime/sensor/command 代码抽到共享组件，或至少在 CI/脚本中固定 `diff -qr` allowlist，所有 C5 修改后强制 parity 检查。
- 是否需要修改 C5/S3/server：只涉及 C5 工程结构；S3/Server 不需要。

### P2-2：C5 dispatcher/worker 未见 task WDT 注册，长期阻塞只能靠栈/heap 日志侧面发现

- 文件位置：
  - `ESPC51/components/Middlewares/runtime/c5_backpressure_controller.c:390-438`
  - `ESPC51/components/Middlewares/runtime/c5_runtime_workers.c:210-270`
  - `ESPS3/components/Middlewares/network_worker/network_worker.c:2253-2363`
  - `ESPS3/components/Middlewares/runtime/s3_scheduler.c:1198-1815`
- 当前行为：C5 有 stack guard 编译检查、worker latency 统计和日志；本轮 `rg esp_task_wdt/task_wdt/watchdog` 未在 C5 middleware 中发现 task WDT 注册。S3 的 network/upload/command/scheduler/protocol/stream/CSI worker 已使用 `app_task_wdt_add_current()`。
- 风险：C5 某个 worker 在 HTTP、I2C 或 JSON 路径长期卡住时，系统可能不能及时定位到具体 task；只靠日志和高水位不足以覆盖死锁/阻塞类问题。
- 推荐方案：为 C5 dispatcher、CSI worker、BME worker、system worker 注册 task WDT，并在合法长等待处 reset/delay；同时把 worker latency 与 WDT reset 周期关联到诊断日志。
- 是否需要修改 C5/S3/server：需要修改 C5；S3 可保持现状。

### P2-3：S3 `network_worker.c`、`s3_scheduler.c`、`protocol_adapter.c` 体量过大，维护风险偏高

- 文件位置：
  - `ESPS3/components/Middlewares/network_worker/network_worker.c`：约 2827 行。
  - `ESPS3/components/Middlewares/runtime/s3_scheduler.c`：约 2347 行。
  - `ESPS3/components/Middlewares/protocol_adapter/protocol_adapter.c`：约 1574 行。
- 当前行为：单文件同时承担状态机、队列、上传、drop/coalesce、CSI latest、命令、日志、协议路由等多类职责。
- 风险：后续修改容易产生跨职责副作用；review 时很难只验证一个行为面，测试粒度也不清晰。
- 推荐方案：按职责拆分内部模块，但保持 public facade 不变：`network_state`、`upload_queue`、`command_queue`、`server_health`、`csi_upload_latest`、`scheduler_workers`、`protocol_parse_*`。
- 是否需要修改 C5/S3/server：只涉及 S3 内部结构；接口不需要变。

### P2-4：`child_registry_is_online()` 将 `LINK_LOST` 视为 onlineish，资源路径必须避免误用

- 文件位置：
  - `ESPS3/components/Middlewares/child_registry/child_registry.c:42-47`
  - `ESPS3/components/Middlewares/child_registry/child_registry.c:575-590`
  - `ESPS3/components/Middlewares/child_registry/child_registry.c:614-632`
  - `ESPS3/components/Middlewares/resource_manager/resource_manager.c:193-201`
- 当前行为：`child_registry_is_online()` 对 `ONLINE`、`VOICE_BUSY`、`LINK_LOST` 都返回 true。resource_manager 另有 `ACTIVE/RESTORING` live 判断，当前 upload/CSI 多处已经使用 `resource_manager_is_live()`。
- 风险：如果后续新代码把 `child_registry_is_online()` 当作“资源仍可用”的判断，会在 grace window 内继续触发无意义资源运行或上云尝试。
- 推荐方案：明确 API 命名或文档：`child_registry_is_onlineish_for_ui()` 与 `resource_manager_is_live_for_io()` 区分；在资源路径上禁止只用 child_registry online 判断。
- 是否需要修改 C5/S3/server：只涉及 S3；不影响接口。

### P2-5：S3 resource restore/CSI warmup 语义较复杂，需要测试和日志防止误判

- 文件位置：
  - `ESPS3/components/Middlewares/resource_manager/resource_manager.c:188-201`
  - `ESPS3/components/Middlewares/resource_manager/resource_manager.c:277-305`
  - `ESPS3/components/Middlewares/csi_fusion/csi_fusion.c:649-657`
- 当前行为：CSI 信号不能直接激活 session；restore 会让 command/sensor/CSI/queue 恢复，但 CSI 需要 warmup 后再 complete。`RESTORING` 且 `live_resources_ready` 时被视为 live。
- 风险：运维或后续代码可能把 CSI warmup 阶段的 `ESP_ERR_INVALID_STATE` 误判为异常；也可能在 RESTORING/ACTIVE 边界上引入重复恢复或过早上传。
- 推荐方案：补充 session state transition 测试和结构化日志，明确 register/heartbeat/sensor/CSI 哪些信号能 activate，哪些只能 warmup。
- 是否需要修改 C5/S3/server：只涉及 S3；不影响接口。

### P2-6：S3 上传 drop/coalesce 策略已存在，但缺少按数据类型的可靠性等级说明

- 文件位置：
  - `ESPS3/components/Middlewares/network_worker/network_worker.c:1656-1814`
  - `ESPS3/components/Middlewares/network_worker/network_worker.c:2082-2121`
  - `ESPS3/components/Middlewares/network_worker/network_worker.c:2284-2324`
- 当前行为：Server 未 ready 时 upload worker 清理低优先级 backlog；snapshot 可 coalesce/drop；latest CSI 只保留最新；BME cache 在 2xx 后删除。
- 风险：策略本身合理，但没有清晰表明哪些数据“可丢”、哪些“必须送达”、哪些“只保留 latest”。后续新增事件时容易误放到低优先级队列，造成事实丢失。
- 推荐方案：建立上传可靠性矩阵：`latest-only`、`coalesced`、`retriable-RAM`、`durable-outbox`；每类有 queue 深度、drop 计数、告警阈值和 Server unavailable 策略。
- 是否需要修改 C5/S3/server：主要修改 S3 文档/内部策略；若新增 durable 类型，Server 可能需要幂等字段。

### P2-7：默认设备/网络配置与运行时密钥治理仍需收敛

- 文件位置：`ESPC51/components/Middlewares/server_comm/server_comm_config.h`、`ESPC52/components/Middlewares/server_comm/server_comm_config.h`、`ESPS3/components/Middlewares/gateway_config` 相关配置。
- 当前行为：本轮未改配置；C5 已限制只访问 S3 local base，但默认 WiFi/URL/身份配置仍是固件配置面的一部分。
- 风险：量产或多设备部署时，如果默认身份、密码、Server URL 固化在源码/头文件，容易造成部署漂移和泄露风险。
- 推荐方案：迁移到 NVS provisioning 或构建期私有配置，源码只保留示例值；报告/日志中避免输出敏感值。
- 是否需要修改 C5/S3/server：C5/S3 配置治理；Server/前端接口不需要变。

### P2-8：资源关闭与内存策略偏“gate + 日志观测”，缺少统一 quiesce/PSRAM/串口降噪策略

- 文件位置：
  - `ESPC51/components/Middlewares/server_comm/gateway_link.c:243-263`
  - `ESPC51/components/Middlewares/runtime/c5_backpressure_controller.c:194-224`
  - `ESPC51/components/Middlewares/sensor_domain/csi_placeholder/csi_service.c:359-370`
  - `ESPC51/components/Middlewares/runtime/app_runtime.c:45-79`
  - `ESPC51/components/app_config/app_debug_config.h:130-137`
  - `ESPS3/components/Middlewares/gateway_config/gateway_config.c:111-134`
  - `ESPS3/components/Middlewares/voice_proxy/voice_proxy.c:123-125`
  - `ESPS3/components/Middlewares/runtime/s3_scheduler.c:1567`
- 当前行为：C5 WiFi down 会立即进入 `LINK_DOWN`，后续普通 CSI/BME/system 上传通过 `c5_should_run()` gate 掉；CSI 只有 pause/resume，没有 link-down 统一 stop/deinit；BME voice 场景有 pause/wait/resume，但 WiFi down 主要靠 link gate；UART/串口侧主要体现为 `ESP_LOG*`，调试配置降低了部分 WiFi tag 日志级别，但没有按 link-down/reconnect 阶段统一对 CSI/BME/HTTP 日志降噪。S3 侧会检查 PSRAM，voice PCM 优先分配 `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`，失败后回退 8bit heap，并在 scheduler 诊断里输出 SPIRAM free。
- 风险：弱网或 C5 重连期间，业务虽然被 gate，但 callback、timer、日志和部分待完成 HTTP 仍可能消耗 CPU/heap/UART 带宽；S3 voice 大 buffer 回退到内部 heap 时，可能与 local_http JSON body、scheduler event、server_client buffer 竞争。当前 stack/heap 日志能帮助定位，但不是统一资源 quiesce 策略。
- 推荐方案：
  - C5 定义统一 resource state：`RUNNING`、`QUIESCING`、`OFFLINE`、`RESTORING`，WiFi down 时显式 pause CSI/BME/report timers 和非必要串口高频日志，重连 READY 后按顺序恢复。
  - CSI 保持 callback 快速，但 link-down 时只保留必要计数，不持续投递 event bus。
  - S3 为大 buffer 建立分配等级：voice/device-stream 优先 PSRAM，失败时降级或拒绝，而不是无限制回退内部 heap。
  - heap/PSRAM/stack 诊断增加“按模块最近大块分配失败”和“link-down/reconnect 阶段日志预算”。
- 是否需要修改 C5/S3/server：需要修改 C5 和 S3 内部资源策略；Server/前端接口不需要变。

## 5. 重点检查项结论

### 5.1 C5 端

- task 数量/优先级：总体合理。dispatcher 优先级 4，worker 优先级 3，业务从 dispatcher 下沉到 worker，设计方向正确。
- 阻塞风险：主要在 C5 worker 中执行 HTTP/BME/command 路径，以及 voice pause 不能取消 in-flight HTTP。
- queue/event bus：单 event bus + CSI callback 高频投递是主要风险；建议 CSI 改 coalescing/notify。
- watchdog：S3 已较完整，C5 middleware 未见 task WDT 注册，建议补齐。
- CSI callback：无 heap、无 HTTP、固定拷贝，基本安全；但投递事件过频、启动顺序和 offline gate 需优化。
- 网络通信：C5->S3 local HTTP 边界清晰；重连状态机较稳；数据缓存/ACK journal 不足。
- 资源管理：WiFi/link down 后普通任务会被 gate，但不是所有 callback/定时源主动停止；建议区分 pause/stop/gate。
- 状态机：`LINK_*` 与 voice/backpressure 结合基本合理；主要竞争在 voice pause 与 in-flight 请求。
- 内存：C5 CSI callback 使用固定 buffer；HTTP URL/response 有 heap 分配，已有低 heap preflight；仍建议把高频路径 heap 分配和 worker stack 观测纳入 WDT/诊断。

### 5.2 S3 端

- 网络状态机：`NET_NOT_READY -> STA_CONNECTED -> IP_READY -> LINK_STABLE` 转换逻辑清晰，本地 ingest 与 Server upload gate 分离是正确设计。
- C5 管理：`child_registry` + `resource_manager` 生命周期较完整，C5 断开后能释放 command/sensor/http/CSI/queue；注意 `LINK_LOST` onlineish 不可作为资源 live。
- CSI 融合：算法复杂度低，压力主要来自锁内发布路径和队列/上传，而不是数学计算。
- HTTP Server：本地 route 集中，JSON body 有大小限制；voice turn 同步长路径与 control plane 共用 httpd 是主要风险。
- 上传服务器：已有 coalesce/drop/retry gate，BME cache 有 2xx 删除语义；但 ACK/命令类需要 durable outbox。
- 调度：优先级总体合理，没有发现明显 `s3_scheduler` / `network_worker` / `stream_worker` / `protocol_worker` 优先级倒挂；需继续减少单文件复杂度和共享状态。

### 5.3 整体架构

- C5 负责采集、边缘计算、状态维护：合理。
- S3 负责网关、融合、管理：合理。
- 重复计算：C5 输出 local motion/metrics，S3 做双链路融合，这是必要的分层，不属于无意义重复计算。
- 职责混乱：主要在 S3 `network_worker` 和 `s3_scheduler` 文件体量过大、Server health 状态双源。
- 状态来源冲突：重点是 `network_worker.s_server_ready` 与 `offline_policy.s_server_available`，以及 child_registry onlineish 与 resource_manager live。
- 数据重复上传：未发现 raw CSI 或明显重复上云；但 latest/coalesce/drop 策略需要可靠性矩阵约束。
- 不必要通信：C5 CSI callback 每帧入 event bus 是主要不必要内部通信；外部 C5->S3 边界总体合理。

## 6. 推荐修改顺序

1. C5 CSI 启动顺序与 coalescing：先解决高频 callback 对 event bus 的压力，收益最大且不影响外部接口。
2. C5/S3 voice 长路径解耦：C5 增加 in-flight HTTP cancel，S3 拆分 voice worker 或独立 server task，降低语音与控制面互相阻塞。
3. 命令/ACK 幂等与 durable outbox：C5 command journal 与 S3 ACK outbox 优先做，必要时 Server 增加 command_id 幂等约束。
4. S3 Server health 单一状态源：合并 `network_worker` 与 `offline_policy` 的 Server ready/available 视图。
5. S3 CSI fusion 锁缩小：锁内只做状态更新，锁外发布 fact/telemetry。
6. C5 BME 有效性与恢复状态：使用 `gas_valid/heat_stable`，增加 DEGRADED/RECOVERING。
7. C5 task WDT 与观测：给 dispatcher/worker 注册 WDT，增强 stack/heap/latency 指标。
8. S3 大文件拆分和 C51/C52 parity 自动检查：降低后续维护成本。

## 7. 修改影响范围

| 优先级 | 问题 | C5 | S3 | Server | 前端/API 影响 |
| --- | --- | --- | --- | --- | --- |
| P1 | CSI callback/event coalescing | 需要 | 不需要 | 不需要 | 不影响现有接口 |
| P1 | voice pause 与 in-flight HTTP cancel | 需要 | 可配合 | 不需要 | 不影响现有接口 |
| P1 | BME gas_valid/heat_stable | 需要 | 可选 additive | 可选 additive | 不影响旧字段 |
| P1 | C5 command journal/ACK 幂等 | 需要 | 可选 | 建议 | 可能新增可选 ACK 字段，不影响前端 |
| P1 | S3 local HTTP voice 解耦 | 不需要 | 需要 | 不需要 | `/local/v1/voice/turn` 可保持不变 |
| P1 | S3 CSI fusion 锁缩小 | 不需要 | 需要 | 不需要 | 不影响接口 |
| P1 | Server health 单状态源 | 不需要 | 需要 | 不需要 | 不影响接口 |
| P1 | S3 command/ACK durable outbox | 不需要 | 需要 | 建议 | 不影响前端，Server 可能需幂等字段 |
| P2 | C51/C52 parity | 需要 | 不需要 | 不需要 | 不影响接口 |
| P2 | C5 task WDT | 需要 | 不需要 | 不需要 | 不影响接口 |
| P2 | S3 大文件拆分 | 不需要 | 需要 | 不需要 | 不影响接口 |
| P2 | 资源 quiesce/PSRAM/串口降噪策略 | 需要 | 需要 | 不需要 | 不影响接口 |

## 8. 是否影响现有前端和服务器接口

- 前端：本轮建议均不要求修改前端。
- Server 接口：大多数建议不要求修改 Server 现有接口。
- 可能涉及 Server 的只有命令/ACK 幂等和 durable outbox：建议以 additive 方式增加 `command_id` 幂等、ACK 重试状态或可选错误字段；不应破坏现有前端调用。
- C5->S3 local API：CSI/BME/voice 的优化可以保持 `/local/v1` 路径和 JSON 兼容；如新增诊断字段，应全部 optional。

## 9. 审计限制

本报告只基于当前源码静态审计。未进行 ESP-IDF 编译、硬件闭环、WiFi 弱网注入、CSI 高频实测、语音长连接压测或 Server 运行验证。因此结论属于源码级稳定性/性能/可维护性风险判断，不等同于硬件实测结论。

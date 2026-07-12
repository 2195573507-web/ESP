# ESP-111 语音流式与音频稳定性实施计划

审计日期：2026-07-12（Asia/Shanghai）  
审计方式：仅对 live source 执行只读检查；未启动服务、未执行 build/test、未写数据库。  
工作区状态：发现顶层仓库及嵌套 `ESP-server` 中已有大量未提交改动和未跟踪文件；本计划不归因、不回退、不覆盖这些改动。  
本次唯一允许写入的文件：本文档。

# 1. 最终目标与非目标

目标是建立“稳定的 C5 流式播放资源模型 + Server 首块 PCM 立即输出”：C5 在一轮播放中不再临时申请关键 DMA/internal heap 或创建 writer task；Server 在 TTS 产生首个可播放 PCM chunk 后立即写给 HTTP response；S3 将该 chunk 直接转给 C5。

非目标：

- C5 上行实时录音 streaming 不是本轮目标。保留当前完整 PCM、固定 `Content-Length` 的 C5 -> S3 -> Server 上传合同。
- 不修改前端、`ESP-server/public/**`、Dashboard UI 或 Dashboard API 响应合同。
- 不修改 CSI、BME、register、heartbeat、status、device stream、command pending/ACK、wake prompt、snapshot、event、smart-home、认证或离线恢复合同；CSI 继续保持 summary-only，raw CSI/IQ/phase/amplitude/subcarrier 不离开 C5 特征层。
- 不做无关的 S3 大文件拆分，不把 C5 改为直连 Server 或构造 `/api/*` 请求，不迁移 I2S DMA 到 PSRAM。
- 不升级供应商 SDK、ESP-IDF、npm 或其他依赖，不调整 CMake、sdkconfig、分区表、生成物或数据库 schema。

# 2. 当前源码事实

## C5（ESPC51 与 ESPC52）

- 两端 `server_voice_client.c`、`speaker_player.c`、`iis.c` 均为逐字节一致文件（本次以 `diff -q` 核对）；`voice_chain.c` 和 `sdkconfig.defaults` 也一致。因此公共修复必须同一提交同时落在 C51/C52 对应路径。
- 上行是整段缓存，不是 request-side chunked：`server_voice_client_start_turn()` 先按 16 KiB 起始容量申请 upload buffer；`server_voice_client_append_pcm()` 按倍增拷贝 PCM；最大 320 KiB，见 `ESPC51/components/Middlewares/server_voice/server_voice_client.c:160-206,573-629`。`finish_turn()` 以该完整 buffer 调 `server_comm_http_post_raw_fixed_stream_begin()`，见 `:632-696`；该固定 body 的实现从调用者 body 直接写完 HTTP client，见 `ESPC51/components/Middlewares/server_comm/server_comm_http.c:884-965`。
- PSRAM 申请失败会退回 `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`，见 `server_voice_client.c:183-194`。这可把最大 320 KiB 的 upload buffer 压入 internal heap，是与 DMA/task 峰值竞争的明确风险。
- 下行已是逐块播放：`server_voice_response_task()` 在取到 headers 后把 `server_comm_http_read_response()` 的每个 read chunk 回调给 `server_voice_play_response_chunk()`，见 `server_voice_client.c:408-481`；该回调处理奇数尾字节，首次 chunk `audio_player_stream_open()`，随后立即 `audio_player_write_pcm_chunk()`，见 `:336-405`。C5 不聚合 Server response PCM。
- C5 HTTP reader 支持已知长度、chunked 与未知长度连接关闭：已知长度比较累计字节；chunked 用 `esp_http_client_is_complete_data_received()`；未知且非 chunked 只有已读数据后 `read_len==0` 或 `-ESP_ERR_HTTP_CONNECTION_CLOSED` 才视为 EOF，见 `server_comm_http.c:1151-1177,1196-1378`。这比旧“所有 `read_len==0` 都是 EOF”的描述更准确。
- 当前 response task 每轮动态创建：上传 buffer 释放后 `server_voice_client_finish_turn()` 调 `xTaskCreate(server_voice_response_task, "server_voice_rx", ...)`，见 `server_voice_client.c:721-756`；任务结束时 `vTaskDelete(NULL)`，见 `:486-529`。`playback_ctx` 也在该 task 内 `heap_caps_calloc()`/free，见 `:453-481`。
- 当前 speaker 的 writer task、ringbuffer、二元 semaphore、scratch item、I2S channel/DMA 都在每轮打开路径取得：`audio_player_stream_open()` 的 `iis_init()`、`xRingbufferCreateNoSplit()`、`xSemaphoreCreateBinary()`、scratch `heap_caps_calloc()`、`iis_start()`、`xTaskCreate()` 分别在 `speaker_player.c:679-751`；`audio_player_stream_finish()` 完成 EOS/drain 后删除这些对象并 `iis_deinit()`，见 `:799-845`。abort 也停止 IIS、等待或通知 writer，再释放，见 `:848-885`。
- I2S DMA 由 ESP-IDF driver 在 `iis_init()` 时随 `i2s_new_channel()`/`i2s_channel_init_pdm_tx_mode()` 建立，`iis_start()` 才 enable，`iis_deinit()` 删除 channel，见 `ESPC51/components/BSP/IIS/iis.c:215-349,353-386,438-467`。DMA 要继续由 driver 放在 internal/DMA-capable 内存；不能强行移到 PSRAM。当前 8 descriptors x 512 frames x 2 bytes，裸 data 公式为 `8*512*2=8192 bytes`，实际总量以 `i2s_chan_info_t.total_dma_buf_size` 为准（已在 `speaker_player.c:684-691` 读取）。
- 当前 ringbuffer 为 4 个固定 512-sample 条目，writer stack 为 6144 bytes，见 `speaker_player.h:53-63`。每条目大小为 `type+sequence+valid_samples+512*int16_t=1036 bytes`（未含 ringbuffer 管理开销）；最小分配应按运行时 `sizeof(audio_player_ring_item_t)` 记录，不能在计划里把 4144 bytes 当作精确总数。
- `CONFIG_SPIRAM=y`、always-internal threshold 16 KiB、internal reserve 32 KiB，见两端 `sdkconfig.defaults:12-19`。这不改变 DMA/internal 的硬约束。
- `voice_chain` 当前是半双工编排：首次 response PCM 到达时进入 `VOICE_PLAYING`、结束本地录音窗口、暂停 Mic 并清缓存，见 `voice_chain.c:197-214`；结束/错误后取消 voice、重启 Mic/VAD、恢复非语音/BME，见 `:228-339`；状态包括 `IDLE/LISTENING/WAKE_ACK/RECORDING/WAITING_RESPONSE/PLAYING/ERROR`，见 `voice_chain.h:31-40`。wake prompt 是独立路径，当前会先下载到 spool 文件再播放，不能在此计划中无意改变，见 `wake_prompt_cache.c:240-410`。

## S3

- S3 `voice_proxy_handle_turn()` 仍完整缓存 C5 request：以 `req->content_len` 在 PSRAM 分配，失败退到普通 8-bit heap，然后循环 `httpd_req_recv()`，见 `ESPS3/components/Middlewares/voice_proxy/voice_proxy.c:109-167`。这属于已明确保留的上行合同。
- S3 下行没有完整 response PCM heap：`server_client_post_voice_turn()` 的 `read_voice_response()` 使用栈上 1024-byte `buf`，每次 `esp_http_client_read()` 后立即调用 `on_data`，见 `ESPS3/components/Middlewares/server_client/server_client.c:1623-1726`；回调 `stream_to_httpd()` 立即 `httpd_resp_send_chunk()`，见 `voice_proxy.c:188-205`。
- 现有 S3 reader 对 `Content-Length >= 0` 用累计字节判定，未知长度用 `esp_http_client_is_complete_data_received()`，见 `server_client.c:1610-1621`；但 `read_len==0 && content_length<0` 被无条件当作完成（`:1692-1700`），没有保留“是否真实 complete / 是否连接关闭 / 未完成 timeout”的区分。这是 P3 的最小功能性修正点。
- S3 先完整写完 request，再 fetch headers，见 `server_client.c:1847-1912`。HTTP buffer 为 1024、TX 为 512，见 `:1773-1780`；没有证据表明 1 KiB 是瓶颈，P3 不调大它，除非基线显示 read/forward 后端吞吐是首包或 underrun 根因。
- S3 在首个 Server response chunk 前已设 `audio/L16` type（`voice_proxy.c:322-344`）；成功只发一次终止 chunk（`:356-375`）。下游发送失败时 callback 返回 error，当前上游 read loop 因 callback error 退出并清理 client（`server_client.c:1682-1686,1969-1979`），但没有明确的“child disconnect -> cancel upstream”日志/指标和统一 cancel 原因。
- `voice_busy` 是单会话互斥且在所有当前 handler exit 路径释放，见 `voice_proxy.c:218-260,286-401`。scheduler 在 voice busy 时跳过 CSI trigger、snapshot、command pull、smart-home poll，见 `s3_scheduler.c:1721-1768`；它不应被扩大为暂停 heartbeat/status/BME/ACK 或网络 worker。

## ESP-server

- `/api/voice/turn` 仍先用 `express.raw()` 将 request body 完整读入，见 `ESP-server/src/routes/voiceRoutes.js:141-152,375-421`，这与当前上行非目标一致。
- route 保留鉴权/metadata、device binding、每设备和全局并发限制（默认 1），见 `voiceRoutes.js:154-180,423-474`；request `aborted`/response `close` 已连接一个 `AbortController`，见 `:485-498`。
- 实时 TTS WebSocket 逐个解 base64 delta，却收集在 `audioChunks[]` 并 `Buffer.concat()`，见 `ESP-server/src/voice/chain.js:165-250`；HTTP TTS 直接 `await upstreamResponse.arrayBuffer()`，见 `:253-302`。因此两条 TTS 路径都在 Server 内完整聚合。
- `runVoiceTurnChain()` 等 ASR、LLM、完整 TTS result 后才返回 `pcm`，见 `chain.js:333-373`。route 先 `await logVoiceTurnRecord()`，然后 `sendVoiceTurnPcm()`，见 `voiceRoutes.js:504-545`；当前持久化在完整 PCM 与完整 TTS 后、响应前。
- `sendVoiceTurnPcm()` 固定 `Content-Length` 并 `res.end(pcmBuffer)`，见 `ESP-server/src/voice/http.js:150-165`。所以 Server 首块不能提前发送；老的 `docs/api-boundary-v1.md:89` “Gateway -> Server streaming”以及旧迁移文档中 C5 直连 `/api` 的描述已经与 live source 漂移。
- `ttsAudio.js` 的 WAV parser 依赖完整 RIFF buffer，JSON/base64 同样是整包解析；它们不能被伪装成天然 streaming，见 `ttsAudio.js:56-143`。现有 smoke voice test 用 `requestRaw` 完整读取 body，只证明最终 bytes，不证明首块时序，见 `ESP-server/scripts/smoke-regression.js:3770-3790`。
- 错误映射目前在 headers 未发送时 JSON `sendVoiceError()`，headers 已发送时仅 `res.end()`，见 `http.js:124-148`；turn 总 timeout 默认 60 s，最大并发默认 1，见 `turnConfig.js:5-29`。C5/S3 为 90 s 总预算（C5 `server_voice_protocol.h:47-56`、S3 `server_client.c:34-47`），存在 Server 先超时的既有预算差异，P0 必须记录但不顺带改值。

# 3. 架构不变量

1. C5 只访问 S3 `/local/v1/*`，永不直连 Server、构造 `/api/*` 或保存 Server URL。
2. S3 是唯一 Server-facing gateway；C51/C52 公共逻辑、协议、状态机和资源模型保持同步。
3. C5 response 每个有效 PCM chunk 到达即入播放器；S3 不缓存完整 Server response PCM。
4. 上行维持完整、固定长度 raw PCM，除非后续独立阶段以新合同审计批准。
5. voice 不得阻塞 heartbeat、status、BME、CSI fusion/report、command ACK、snapshot/event worker；现有仅对低优先级周期任务的 busy gate 不扩大。
6. raw CSI 永不离开 C5 特征层；smart-home 无真实设备继续返回 failed ACK，不伪造成功。
7. 不改 `ESP-server/public/**`、真实数据库、Dashboard API 或非 voice route/service；不新增依赖。
8. 兼容非流式 TTS provider：最终 PCM 字节序仍是 s16le mono 16 kHz，老客户端完整读取 body 仍成功。
9. client abort 必须停止上游 TTS，或至少立即停止向已断开的 response 写数据；每个失败路径只释放自己的资源一次。

# 4. 方案比较与最优方案选择

## C5 播放资源模型决策矩阵

|方案|常驻 internal RAM|播放峰值/最大连续块依赖|DMA 安全|时序与 abort|范围/回归|结论|
|---|---|---|---|---|---|---|
|A 每轮全部动态创建，仅缩 stack/DMA|最低|最高，仍在碎片化时申请 task、I2S/DMA|可保持|简单但无法消除根因|代码少、回归风险高|拒绝|
|B 常驻 writer，I2S/DMA 每轮创建|中等|关键 DMA 仍要求大连续 internal block|可保持|task 竞态减少，driver 失败仍在|中等|拒绝|
|C writer、ringbuffer、I2S channel、DMA 全常驻；每轮 reset/enable/drain/disable|中等且可测|播放阶段不再申请关键资源；只依赖启动时一次成功|保持 driver 的 internal/DMA 分配|需要明确状态机，abort 可收敛到 owner|最小地覆盖根因，C51/C52 可镜像|**选择**|
|D 静态 task/静态 buffer 全静态|最高且刚性|最低|DMA driver 仍可能动态分配且静态 ring 不等于静态 driver DMA|实现和维护复杂|范围大、与现有 FreeRTOS/I2S 接口不匹配|不作为最小路径|

选择 C 不是单纯追求代码最少，而是唯一同时消除 per-turn `xTaskCreate`、I2S/DMA 和 ringbuffer/scratch 的 internal heap 峰值，并保持现有播放器边界的方案。writer stack 仍应先用实测 HWM 证明是否需要 `xTaskCreateStatic`；默认不把“静态任务”作为前提。

## Server TTS 输出模型决策矩阵

|方案|背压/abort|provider 兼容|测试和耦合|结论|
|---|---|---|---|---|
|A 完整 Buffer|无首块、内存峰值最大|现有兼容|容易但不满足目标|拒绝|
|B callback `onPcmChunk`|容易把 HTTP 写入耦合进业务，背压难 await|可做|错误/测试边界弱|不选|
|C AsyncIterator/AsyncGenerator|route 可 `for await` 并 await drain；AbortSignal 可贯通|实时 WS/raw HTTP 直接 yield，WAV/JSON 有界 fallback|业务与 HTTP 分离、易 mock|**选择**|
|D Node `PassThrough`|天然背压|可做|生命周期、error listener、route 耦合复杂，现有代码无 stream 抽象|不作为最小路径|

统一接口建议：`async function* iterateVoiceTtsPcm(text, config, deviceId, { signal, limits })`；首次 yield 前返回/携带 metadata `{provider,sampleRate:16000,channels:1,format:'pcm_s16le_mono_16k'}`，每个 yield 为非空、偶数字节、受单 chunk 和累计上限限制的 `Buffer`。异常抛现有 voice stage error；`AbortSignal` 传入 WS、fetch、reader 和 fallback adapter。实时 WS raw PCM delta 直接 yield；HTTP raw PCM 用 `response.body.getReader()`；WAV/JSON/base64 保留**有界整包兼容 adapter**，不实现高风险的跨 chunk WAV/base64 解析器。

## S3 方案

|方案|结论|
|---|---|
|A 完全不修改|当前逐块转发已经成立，但未知长度 `read_len==0` 判定与下游断开可观测性不足；不可直接宣称无需修改。|
|B 只加未知长度 EOF、异常断流、下游写失败取消与指标|**选择**。不变更 request-side 固定长度、不引入 response malloc、不改路由。|
|C 重构 voice proxy|拒绝，扩大 single-session、busy、offline 和 local-http 回归面，不能比 B 更低风险。|

# 5. ESPC51 / ESPC52 修改任务

以下每个任务必须在 C51/C52 镜像文件同次落地并做 parity diff；本节不包含 S3/Server 修改。

## C5-1：建立播放资源所有权与常驻初始化

- 文件/函数：两端 `components/Middlewares/speaker/speaker_player.{c,h}` 的 `audio_player_init()`、`audio_player_stream_open()`、`audio_player_stream_finish()`、`audio_player_stream_abort()`；两端 `components/BSP/IIS/iis.{c,h}` 的 `iis_init/start/stop/deinit`。
- 当前/目标：当前 `stream_open` 在 `speaker_player.c:679-751` 动态取得 I2S、ring、semaphore、scratch、writer；目标改为 `audio_player_init()` 一次创建并拥有 writer、ringbuffer、done primitive、scratch、I2S channel/DMA，启动失败立即回滚到未初始化。每轮只 reset state/ring、enable I2S、唤醒 writer；完成 drain 后 disable I2S/PA；仅 app deinit（若项目有明确生命周期）才 delete channel/objects。
- 状态机：`UNINITIALIZED -> READY_DISABLED -> OPENING -> PLAYING -> DRAINING -> READY_DISABLED`；任何 `open` 失败回 `READY_DISABLED`，任何 abort 走 `ABORTING -> READY_DISABLED`。禁止 `PLAYING` 时二次 open；close/abort 必须幂等。尚无明确 app-level speaker deinit 调用，实施前须确认是否新增受控 `audio_player_deinit()`；若没有可靠生命周期，P1 不引入运行期 deinit。
- 锁/notification：`s_play_mutex` 只保护公开 API 与状态转换；writer 是唯一 `iis_write()` owner。用 task notification 或常驻 queue/ring event 唤醒 writer，完成/abort 用 generation/sequence，禁止旧 EOS 完成下一轮。非 owner abort 只置原子 abort+notify，不释放 ring/I2S/mutex；owner 执行复位，沿用当前注释中的所有权原则（`speaker_player.c:858-865`）。
- 内存：I2S DMA/descriptors 继续由 I2S driver 以 `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA` 约束分配，绝不 PSRAM；writer stack 先保持现值并记录 HWM，只有 HWM 证明足够且 FreeRTOS 静态 API 与当前 heap capability 可满足时再评估 static task；ring/scratch 可一次性 allocate，优先 internal 8-bit 并计入常驻预算。
- 正常/异常：finish 必须先入唯一 EOS，等 writer drain 所有已入队 PCM，再 disable；HTTP error、空 response、WiFi disconnect、playback timeout 都触发 abort，清 ring、停止/disable I2S，回 READY_DISABLED；重复 open/close 无二次 enable/disable；writer 与 enable/disable 的调用由状态和同一 mutex 串行。
- wake/Mic：C5 不改变 `voice_chain` 在首 chunk 回调暂停 Mic 的语义；旧 beep/prompt 和 Server response 都经同一 player，但不得与 response 交叉占用。
- 日志/验证/回滚：增加一次性 init 的 `dma_alloc_fail`，每轮 `writer_ready/i2s_enable_fail/stream_abort/underrun`，以及 internal/DMA free/largest、writer HWM；静态检查禁止播放路径的 `xTaskCreate`/`i2s_new_channel`/`i2s_channel_init_pdm_tx_mode`；分别 build C51/C52，真机连续轮次和断流通过后才能删除旧 per-turn cleanup。回滚是恢复现有 per-turn init/finish/deinit，不动协议。

## C5-2：将 server voice response task 变为可预测生命周期

- 文件/函数：两端 `components/Middlewares/server_voice/server_voice_client.c`：`server_voice_client_init()`、`server_voice_client_finish_turn()`、`server_voice_response_task()`、`server_voice_client_cancel_turn()`/`request_abort()`。
- 当前/目标：当前 `finish_turn()` 在 `:721-756` 释放 upload buffer 后再动态创建 `server_voice_rx`。目标是 init 时创建一个常驻 response worker（或证明其 stack 不与 DMA 创建重叠的另一低风险方案）；worker 等待 notification/queue 的 “stream ready” job，完成后返回 idle。不要在实际播放临界点动态创建 task。
- 所有权：client context 唯一拥有 `server_comm_raw_stream_t *`，response worker 只在已接管后 fetch/read/close；upload buffer 只属于录音阶段，转交 HTTP 后释放。`playback_ctx` 优先成为常驻 context 或 worker stack 结构；不得由 abort callback free worker 正在读的对象。
- 状态与竞态：保留外部 `IDLE/PREPARING/STREAMING/FINISHING` 合同，内部增加 `RX_WAIT/RX_ACTIVE/RX_ABORTING`。`stream_open` 重入必须返回 invalid-state；关闭按 generation id，只允许当前 generation cleanup。WiFi disconnect 先请求 abort/关闭 HTTP，再由 worker 完成播放器 abort；空 response 保持现有 204 成功、2xx 空 PCM 的明确验收规则（需 P1 前确认是否视成功）。
- upload 内存：PSRAM 失败后**禁止** 320 KiB internal fallback；返回 `ESP_ERR_NO_MEM` 并走既有 voice error/recovery。它不改变完整上行合同，只移除会挤压 DMA 的危险降级。若平台实测 PSRAM 不可用，语音 turn 应在开始前拒绝并记录原因，不能静默转 internal。
- 验证/回滚：日志加 `stream_open_success/fail`、response worker HWM、first_chunk_to_i2s_ms；构建两端、模拟 repeated open/close/abort 并双机真测。回滚分别恢复 dynamic worker 和 internal fallback，不能部分回滚 C51/C52。

## C5-3：与 voice_chain 的交接门禁

- 文件/函数：两端 `components/Middlewares/voice_domain/voice_chain.c` 的 `voice_chain_server_playback_start_sink()`、`voice_chain_finish_or_recover_to_listening()`、gateway abort callback；必要时仅补充 headers。
- 目标：播放首块前完成 Mic pause acknowledgement（不是仅设置 pause bit），确保 Mic cache 清理不与常驻 player reset 竞争；完成/abort 后遵循“player READY_DISABLED -> Mic standby -> non-voice resume -> LISTENING”。不得重排 wake prompt 合同。
- 失败：Mic pause timeout、I2S enable fail、HTTP error、WiFi disconnect、空 response、播放超时均只投递一次 error/recovery event；队列满必须记录并采取确定的 abort owner，而不能跨 task 释放。
- 验证：WakeNet/VAD 触发、prompt/beep、response、Mic resume 的串口时间线；检查 BME pause/resume 对称，C51/C52 diff；回滚仅移除新门禁，保留原 `voice_chain` 状态枚举和接口。

# 6. ESPS3 修改任务

## S3-1：保留转发，修正结束与取消语义

- 文件/函数：`ESPS3/components/Middlewares/server_client/server_client.c` 的 `response_complete()`、`read_voice_response()`、`server_client_post_voice_turn()`；`voice_proxy/voice_proxy.c` 的 `stream_to_httpd()`、`voice_proxy_handle_turn()`；按需要只改这两个 header。
- 不修改的证据：当前每个 1024-byte upstream read 都同步进入 `stream_to_httpd()`（`server_client.c:1630-1686`、`voice_proxy.c:188-205`），没有完整 response malloc；因此不重构 proxy、不调 request 固定 `Content-Length`、不调 1 KiB buffer。
- 修改：未知长度/chunked 情况以 `esp_http_client_is_complete_data_received()` 为主；`read_len==0` 只有 complete 或明确 connection-close EOF 才完成，否则计为 EAGAIN/timeout；有已知 `Content-Length` 的提前 0 必须失败。记录 `incomplete` 及 bytes。上游 headers 在首个 downstream chunk 前已确定，继续保留现有 type/format headers。
- 下游断开：`httpd_resp_send_chunk()` 失败后 callback 立即停止 read loop；在 `server_client_post_voice_turn` 清理前显式取消/close upstream request（或在单一 cleanup 中证明 close 立即中断），不得继续读取完整 TTS；只记录一次 disconnect；绝不再发终止 chunk。成功仅一次 `httpd_resp_send_chunk(req,NULL,0)`；部分 body 后上游失败同样只终止一次，不尝试 JSON error。
- busy/nonvoice：用一个 `goto cleanup` 或等价单出口保证 `voice_proxy_release_active_device()` 一次执行。不得让 `voice_busy` gate 扩大到 heartbeat/status/BME/ACK/network worker；维持 scheduler 的低优先级抑制边界。
- 指标：`upstream_headers_ms`、`first_upstream_chunk_ms`、`first_downstream_chunk_ms`、`response_chunks`、`response_bytes`、`client_disconnect`、`upstream_incomplete`、`stream_cancelled`、`voice_busy_duration_ms`；详细 chunk 日志只 debug。
- 验证/回滚：S3 build、mock chunked/known-length/connection-close/early-EOF/downstream-write-fail 测试；真机一轮与断开。回滚仅去除 EOF/cancel 分支，不触碰 S3 router/worker 结构。

# 7. ESP-server 修改任务

## SVR-1：TTS 统一为可取消的 PCM AsyncIterator

- 文件/函数：`ESP-server/src/voice/chain.js`（替代 `requestRealtimeVoiceTts`/`requestHttpVoiceTts` 的返回 shape，新增 `iterateVoiceTtsPcm`）、`ttsAudio.js`（有界 adapter/验证）、`mockTurn.js`（可控 chunk 边界）。
- 实时 WS raw PCM：收到 `event.isAudioDelta` 后 decode、校验非空偶数字节和累计限制，立即 `yield`；删除实时路径的 `audioChunks.push/Buffer.concat`（当前 `chain.js:165-227`）。`finally` 关闭 WS；abort 以原 signal 传递。
- HTTP raw PCM：fetch 成功后先判断 `Content-Type`；raw PCM 以 `response.body.getReader()` 逐块 yield，处理跨 chunk 的一个奇数字节并限制总量/时长。WAV 与 JSON/base64 使用最大上限下的完整 adapter，复用 `normalizeTtsPcmBuffer()`，明确它们不会获得提前首块；provider 不支持 streaming 时保持该兼容回退。
- metadata/异常：iterator 开始前协议元数据固定为当前 PCM 合同；最终 metrics 从 yield 累计。上游非 2xx 在 headers 尚未给 client 前抛现有 stage error；WAV header 跨 chunk、base64 quartet 跨 chunk 不在最小 streaming 路径实现，应走有界 fallback 或被明确拒绝。

## SVR-2：HTTP route 实施 chunked response、背压和持久化语义

- 文件/函数：`ESP-server/src/voice/http.js`（新增 `beginVoiceTurnPcm`/`writeVoiceTurnPcmChunk`，保留 `sendVoiceTurnPcm` 给旧内部调用）、`src/routes/voiceRoutes.js` 的 `handleVoiceTurn()`、必要时 voice test/smoke 脚本；不改 Express parser 顺序。
- 行为：ASR、LLM 仍等待最终文本。TTS iterator 首个实际 PCM 前设置 200、`Content-Type`、`X-Audio-Format`、`Cache-Control`、`nosniff`，不设置 `Content-Length`；每个 `res.write(chunk)` 若返回 false 必须 await 一次 `drain`，并监听 `req.aborted`、`res.close`、socket error。已销毁/writableEnded 时不写。
- 错误：headers 未发出仍 `sendVoiceError`；第一块后 provider error 不能改 HTTP status，只 abort iterator、记录 `partial_failed`、安全 `res.end()` 一次。client abort 不写、不 end 已销毁 response，立即 controller.abort。`finally` 释放 device/global concurrency 一次。
- 持久化：不等待完整 PCM 后再持久化。完整成功在 stream 完结后按实际 `bytes_written` 记录 success；client abort 记录 aborted（若现有枚举不接受则 record failed + 明确 reason，**不改 schema**）；上游失败在未写/部分写分别记录失败/partial；event 继续 best-effort。先审计 `voice_turns` 既有 status 约束，若无法表达 partial，不改 schema，只以现有列记录 reason/actual response bytes；schema 扩展仅列为未来可选阶段。
- 兼容：路径仍为 POST `/api/voice/turn`，header/PCM 格式相同；完整 body 客户端仍能读完。mock 必须可分 N 个可控 chunks、随机边界、延时首块，供测试而非生产行为。

# 8. 分阶段实施顺序

|阶段|前置条件|修改文件|验证命令（未来执行）|验收/停止条件|回滚点|
|---|---|---|---|---|---|
|P0 基线与保护|工作区可单独运行临时测试|仅测试/日志计划文件|见第 11 节的 rg、Node、临时 DB smoke|取得 C5 heap/largest/DMA/HWM 和 Server 首块/完整时延；未得到基线不得调 buffer/stack|无业务改动|
|P1 C5 生命周期|P0 显示动态峰值或现有 NO_MEM|两端 speaker/IIS/server_voice/必要 voice_chain|C51/C52 build、parity diff、真机循环|连续多轮无 DMA/writer `ESP_ERR_NO_MEM`；失败立即停止，不进入 Server 改造|独立 C5 commit|
|P2 Server chunk 输出|P0 mock 客户端可测首块|仅 `ESP-server/src/voice/**`、`voiceRoutes.js`、voice tests/smoke|`node --check`、临时 DB/mock provider tests|完整结束前已观测首 write；任何 bytes 不等价停止|独立 Server commit|
|P3 S3 小加固|P2 HTTP chunked 可用|`voice_proxy`/`server_client` 与定向测试|S3 build、EOF/abort 测试|下游断开即停止上游；未知长度不误报成功|独立 S3 commit|
|P4 端到端|P1-P3 单端均通过|不新增功能文件|三 build、临时 DB smoke、双 C5 真机|首 chunk 已经 C5 I2S；非 voice 回归全过|按 P3 -> P2 -> P1 逆序|

# 9. 不影响其他链路的回归矩阵

|链路|为何可能受影响|自动验证|真机通过标准|
|---|---|---|---|
|C51/C52 register|共享 C5 HTTP/runtime|现有 route smoke + 两端 parity rg|两端各注册一次成功|
|heartbeat/status/UDP stream/HTTP fallback|voice busy 与 network slot|定向 S3 unit/log contract|voice 中仍按既有频率或既有 gate 行为|
|BME 实时、离线恢复|voice_chain pause/resume|BME tests/route smoke|播放前后上传/恢复无丢失|
|CSI callback/feature/S3 fusion/Server ingest|scheduler busy 不得扩大边界|CSI tests、rg raw CSI guard|无 raw 数据上云，summary 链路正常|
|command pending/ACK|S3 scheduler/HTTP slot|command smoke、ACK contract|ACK 不被 voice 锁阻塞|
|smart-home failed ACK|不得伪造成功|smart-home smoke|无设备仍 failed ACK|
|wake prompt/prompt cache|共用 player、但路径独立|prompt cache smoke|wake prompt/beep 后仍可 voice|
|voice busy、正常、abort、provider error、S3 upstream down、C5 WiFi down、双 C5 串行|本轮直接影响|第 11 节脚本/故障注入|busy 必释放，第二台可在第一台完成/失败后运行|
|Dashboard overview/snapshot/event|route/持久化/调度并存|临时 DB smoke regression|API 200 与历史/事件不回归|
|Server auth/device binding|voice route headers 未改变|auth/device tests|拒绝/接受行为不变|
|Server 不可用 S3 本地链路|S3 cleanup/voice busy|S3 failure test|本地 register/status/命令恢复可用|

# 10. 内存预算

所有数值先用公式，不编造实测数字；P0/P1 在 C5 `heap_caps_get_free_size/get_largest_free_block` 和 `i2s_chan_info_t.total_dma_buf_size` 日志采集实际值。

|端|项目|当前|P1 后|公式/采集点|
|---|---|---|---| 
|C5|server_voice_rx stack|每轮动态|常驻或经验证不与 DMA 重叠|`SERVER_VOICE_RESPONSE_TASK_STACK` + HWM，`server_voice_client.c:726-756`|
|C5|speaker writer stack|每轮 6144 B|常驻 6144 B（先不缩）|`AUDIO_PLAYER_I2S_WRITER_TASK_STACK_SIZE` + HWM|
|C5|I2S DMA data/descriptors|每轮 driver 申请|常驻，空闲 disable|driver actual `total_dma_buf_size`；裸 data 8192 B|
|C5|ringbuffer|每轮|常驻|`sizeof(item)*max(4,2)+FreeRTOS overhead`|
|C5|scratch/playback context/read buffer|scratch 每轮；response ctx/read buffer动态|scratch/ctx 可常驻，read buffer按现状临时或预分配后实测|`sizeof` 与 `SERVER_COMM_HTTP_READ_CHUNK_BYTES`|
|C5|upload buffer|PSRAM 优先、可退 320 KiB internal|PSRAM-only；失败拒绝|`min(320KiB,next_power_of_2(required))`|
|C5|峰值/最小 largest|动态资源叠加，未实测|常驻资源从峰值移到启动，播放不再需大块|P0 记录开始、open、finish、abort；minimum largest 为启动后安全余量，数值待真机|
|S3|voice proxy request|完整 PSRAM/8-bit body|不变|`req->content_len`，不在本轮改上行|
|S3|upstream/downstream response|1024-byte栈 buffer，无完整 PCM heap|同|`uint8_t buf[1024]`；新增 cancel state 为常数级|
|Server|旧 PCM peak|delta 数组 + concat 或 arrayBuffer + normalized buffer|删除实时/raw HTTP 的完整 PCM 聚合|旧约 `sum(chunks)+concat`，新为 `max(chunk, socket high-water)`|
|Server|fallback|整包 WAV/JSON/base64|受 `maxBytes` 限制的整包|最大缓存 = 配置上限；并发总上限 = `maxConcurrent * maxFallbackBytes`|

# 11. 测试与验证命令

以下是后续阶段执行命令，**本次未执行**：

```sh
cd /Users/zhiqin/ESP-111
diff -u ESPC51/components/Middlewares/server_voice/server_voice_client.c ESPC52/components/Middlewares/server_voice/server_voice_client.c
diff -u ESPC51/components/Middlewares/speaker/speaker_player.c ESPC52/components/Middlewares/speaker/speaker_player.c
rg -n 'https?://|/api/' ESPC51/components ESPC52/components
rg -n 'Buffer\.concat|arrayBuffer\(\)|audioChunks\.push' ESP-server/src/voice ESP-server/src/routes/voiceRoutes.js
rg -n 'heap_caps_malloc\(.*response|malloc\(.*response|response.*malloc' ESPS3/components/Middlewares/voice_proxy ESPS3/components/Middlewares/server_client

cd ESPC51 && idf.py build
cd ../ESPC52 && idf.py build
cd ../ESPS3 && idf.py build
cd ../ESP-server && node --check src/voice/chain.js && node --check src/voice/http.js && node --check src/routes/voiceRoutes.js
cd ../ESP-server && ESP_SERVER_DB_PATH="$(mktemp -u /tmp/esp111-voice-XXXX.sqlite)" VOICE_TURN_MOCK=1 npm test -- --test-name-pattern=voice
cd ../ESP-server && ESP_SERVER_DB_PATH="$(mktemp -u /tmp/esp111-smoke-XXXX.sqlite)" npm run smoke
```

新增定向测试必须使用临时数据库与 mock provider：逐 chunk 到达时间的 Node client（ReadableStream reader，不得 `arrayBuffer()`）、随机 chunk 边界、慢 drain、client abort、TTS 中途失败、0 字节、奇数字节、超大 response、错误 `Content-Type`、已知长度提前 EOF、chunked EOF、连接关闭 EOF。真机串口应筛选：

```sh
idf.py -p <PORT> monitor | rg 'stream_open|writer_ready|dma_alloc_fail|first_chunk_to_i2s|VOICE_RX|voice_response|client_disconnect|upstream_incomplete|voice_busy_duration'
```

build 成功不等于解决：P4 必须完成双 C5 串行 voice turn、至少一次 C5 WiFi 断开、一次 Server/provider 中断，且同时观察非 voice 链路。

# 12. 观测与验收指标

- C5：`stream_open_success/fail`、`writer_ready`、`i2s_enable_fail`、`dma_alloc_fail`（P1 后必须为 0）、`stream_abort`、`underrun`、internal/DMA `free/largest`、writer/response HWM、`first_chunk_to_i2s_ms`。
- S3：`upstream_headers_ms`、`first_upstream_chunk_ms`、`first_downstream_chunk_ms`、`response_chunks/bytes`、`client_disconnect`、`upstream_incomplete`、`stream_cancelled`、`voice_busy_duration_ms`。
- Server：`asr_done_ms`、`llm_done_ms`、`tts_first_pcm_ms`、`response_first_write_ms`、`tts_complete_ms`、`bytes_generated/written`、`chunks_written`、`backpressure_wait_count`、`client_abort`、`provider_abort`。

按 turn 输出一条 info summary；每 chunk 仅 debug。验收以 P0 baseline 为比较：Server `response_first_write_ms < tts_complete_ms`，S3 first downstream 不晚于 first upstream 一个同步转发周期，C5 first I2S write 不晚于完整 response；不设未经真机证明的毫秒阈值。

# 13. 风险清单

|风险|预防/检测|回滚条件|
|---|---|---|
|常驻 I2S/DMA 增加静态 internal 占用|P0/P1 记录启动后 free/largest 与所有任务 HWM|启动后低于已定义安全余量或影响非 voice|
|I2S disable/enable 状态不一致|单一状态机、重复 API 幂等测试|任一重复轮无声/driver invalid-state|
|writer 与 abort 竞态/旧 ring 数据串轮|generation、owner cleanup、drain/clear test|跨轮 PCM 或 mutex 卡死|
|TTS chunk 非 PCM frame 对齐|Server/C5 保留一个字节拼接、偶数校验|字节等价失败或爆音|
|headers 后 provider error 无法改 status|partial 结束与持久化 reason 测试|重复 end/写已销毁 response|
|client disconnect 仍产生计费|AbortSignal 到 WS/fetch、S3 close/cancel 指标|abort 后仍有持续 upstream bytes|
|WAV header/base64 quartet 跨 chunk|只 raw PCM 直流，其他有界 fallback|adapter 无法证明正确即禁用 streaming|
|persistence partial 语义不一致|临时 DB 覆盖 success/abort/upstream failure|需 schema 才表达才进入后续可选阶段|
|S3 将 0 误判 EOF|known/chunked/connection-close fault tests|未知未完成被记成功|
|双 C5 busy 未释放|单出口 release、双机串行 test|第二台无法进入下一轮|
|C51/C52 单边漂移|每提交 diff gate|diff 非预期|
|smoke 仍只读完整 body|新增 reader 首块测试|无法测出首块早到|
|非 voice worker 被纳入 busy|回归矩阵逐项观测|heartbeat/status/BME/ACK 被停|

# 14. 文件级任务清单

## ESPC51 / ESPC52

- 预计修改：两端 `components/Middlewares/speaker/speaker_player.{c,h}`、`components/BSP/IIS/iis.{c,h}`、`components/Middlewares/server_voice/server_voice_client.{c,h}`。
- 可能修改：两端 `components/Middlewares/voice_domain/voice_chain.c`（仅交接/错误门禁）、对应 CMake 仅当新增文件不可避免；优先不新增文件。
- 明确不修改：两端 `sdkconfig*`、分区表、CMake（除非编译需要）、server_comm 对外合同、BME/CSI/command/status/register 业务源码、wake prompt 合同、任何 `/api/*` 目标。

## ESPS3

- 预计修改：`components/Middlewares/server_client/server_client.c`、`components/Middlewares/voice_proxy/voice_proxy.c`；相应 `.h` 仅当 cancel/metrics 需要对外声明。
- 可能修改：voice 定向测试或已有测试入口；不新增完整 response buffer。
- 明确不修改：`local_http_server` 路由合同、network worker/scheduler 行为边界、CSI 算法、BME cache/replay、command router、smart-home、snapshot/event 业务逻辑。

## ESP-server

- 预计修改：`src/voice/chain.js`、`src/voice/ttsAudio.js`、`src/voice/http.js`、`src/voice/mockTurn.js`、`src/routes/voiceRoutes.js`、已有 voice 测试/`scripts/smoke-regression.js` 的 voice 段。
- 可能修改：仅 voice 专用错误/测试 helper；优先不新增依赖。
- 明确不修改：`ESP-server/public/**`、`ESP-server/db/database.db`、Dashboard routes（除非 voice route 内部 helper 且外部合同不变）、sensor/CSI/command/smart-home/event/memory 业务源码、package dependencies、Express 非 voice parser 顺序。

# 15. 最终推荐实施路线

推荐路线是 P1 C5 常驻播放资源与 response worker 生命周期，P2 Server `AsyncIterator` raw PCM streaming + HTTP drain，P3 S3 EOF/cancel/metric 小加固，P4 端到端回归。它在现有源码约束下风险最低：C5 先修能单独消除 DMA/task 内存峰值，不等待 Server 改造；Server 是唯一完整 response 聚合与 `Content-Length + res.end` 的首块阻塞点；S3 已有逐块转发，故只补正确性和取消，不重构。

预计最小修改数量：C5 每端 3-5 个镜像文件（6-10 个路径计数），S3 2-4 个文件，Server 5-7 个文件。建议提交顺序：`P0 tests/baseline`、`P1 C5 parity`、`P2 server iterator/route/test`、`P3 S3 stream hardening`、`P4 no-code validation evidence`；每一阶段均可独立 revert。

Definition of Done：

1. C51/C52 连续多轮 voice response 不再出现 I2S DMA 或 writer task `ESP_ERR_NO_MEM`。
2. C5 播放资源没有重复创建、重复释放或跨轮残留。
3. Server 在完整 TTS 结束前已经发送首个 PCM chunk；S3 收到后立即转发；C5 完整响应结束前开始 I2S 播放。
4. C5/S3/Server client abort 贯通释放资源；最终 PCM 与旧整包模式字节等价，只允许 chunk 边界不同。
5. BME、CSI、heartbeat、status、commands、snapshot、event 回归通过；`ESP-server/public` 与真实数据库均无 diff。
6. C51/C52 公共源码保持 parity，三套固件 build、Server 临时 DB tests 通过，双 C5 真机串行 turn 与异常断流通过。

仍需真机验证：实际 I2S driver DMA total size、启动后可接受的 internal largest-free-block 下限、writer/response stack HWM、C5/I2S 首块播放时延、S3 1 KiB buffer 是否构成吞吐瓶颈，以及生产 TTS provider 是否真正输出 raw PCM delta。以上未测事实不得以推测替代。

## 本次实际读取的关键文件与只读命令

关键文件：`ESPC51/ESPC52/components/Middlewares/server_voice/server_voice_client.c`、`speaker/speaker_player.c`、`BSP/IIS/iis.c`、`server_comm/server_comm_http.c`、`voice_domain/voice_chain.c`、`sdkconfig.defaults`；`ESPS3/components/Middlewares/voice_proxy/voice_proxy.c`、`server_client/server_client.c`、`local_http_server/local_http_server.c`、`runtime/s3_scheduler.c`；`ESP-server/src/routes/voiceRoutes.js`、`src/voice/{chain,http,ttsAudio,turnConfig,mockTurn}.js`、`scripts/smoke-regression.js` 及既有 docs。

只读命令：`find`、`rg`、`sed`、`nl`、`wc`、`diff`、`git status`、`git log`、`stat`。未执行 build、服务、测试、flash、clean、依赖安装或数据库写入。

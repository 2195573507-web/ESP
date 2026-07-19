# ESP-111 语音流式与音频稳定性实施计划

审计日期：2026-07-12（Asia/Shanghai）
审计方法：只读核对当前 live source；本次未 build、test、启动服务、访问数据库、调用火山网关、修改源码或配置。
本次唯一可写文件：本文档。本文只规划后续独立源码提交，不实施业务变更。

## 1. 最终目标、范围和非目标

P2a 的目标链固定为：

```text
ASR final text -> LLM final reply -> one complete reply submitted to Volcengine TTS
  -> Volcengine incremental audio -> ESP-server first PCM write
  -> S3 immediate forward -> C5 receive-and-play
```

目标是让 C5 播放资源在启动期一次就绪，Server 在收到首个有效 PCM 后立即写 HTTP response，S3 将每个 Server PCM chunk 立即转给 C5，C5 在完整 TTS 完成前开始 I2S 播放。P2a 不改 LLM 主链，也不把 LLM token 直接提交 TTS。

范围只覆盖 TTS 下行。C5 上行保持完整 PCM、固定 `Content-Length` 上传；ASR 和 LLM 仍等待完整输入/文本。C5 只能访问 S3 `/local/v1/*`，S3 仍是唯一 Server-facing gateway，C5 不构造 `/api/*`。

非目标：前端、Dashboard UI/API、数据库/schema、`ESP-server/public/**`、`ESP-server/db/database.db`、CSI、BME、register、heartbeat、status、device stream、command/ACK、snapshot、event、smart-home、认证和离线恢复均不改。不得新增依赖、SDK、重采样器或编码器；不得把 I2S DMA/descriptors 放入 PSRAM、默认常驻 `server_voice_rx`、扩大 `voice_busy` gate 或把上行改为 chunked。每阶段必须独立提交、验证和回滚。

## 2. 当前源码事实与火山请求模式审计

### C5（C51/C52）

- 两端当前公共语音文件逐文件 `diff -q` 一致；后续改动必须保持 parity。`voice_chain_start()` 已在 Mic ADC/VAD 前调用 `audio_player_init()`、`server_voice_client_init()`，随后注册 stream ops、创建 voice task，最后才启动 Mic（`ESPC51/components/Middlewares/voice_domain/voice_chain.c:612-685`；C52 对应文件相同）。但 `audio_player_init()` 目前只创建 mutex（`speaker_player.c:600-609`）。
- 下行 response task 对每个 HTTP read chunk 调用 `server_voice_play_response_chunk()`；它处理一个 byte 的 leftover，首个有效偶数字节调用 `audio_player_stream_open()`，随后立即 `audio_player_write_pcm_chunk()`（`server_voice_client.c:336-405`）。
- `server_voice_client_finish_turn()` 完整 POST 后释放 upload buffer，再以 `SERVER_VOICE_RESPONSE_TASK_STACK` 创建每轮 `server_voice_rx` task（当前配置 8192 B；`server_voice_client.c:632-760`）。
- 当前 `audio_player_stream_open()` 每轮 `iis_init()`、创建 ringbuffer、semaphore、scratch、启动 IIS 并创建 `speaker_iis_writer`；writer 是唯一 `iis_write()` owner（`speaker_player.c:421-425,652-764`）。

### S3

- `read_pcm_body()` 依据 C5 request `content_len` 一次完整缓存；此上行合同保持不变（`ESPS3/components/Middlewares/voice_proxy/voice_proxy.c:102-167`）。
- 下行不聚合 PCM：`read_voice_response()` 使用 1024-byte 栈 buffer，每次 `esp_http_client_read()` 成功即调用 callback；`stream_to_httpd()` 立即 `httpd_resp_send_chunk()`（`server_client.c:1623-1689`；`voice_proxy.c:188-205`）。
- 当前未知长度 zero-read 可能被当成完成，且 downstream send 失败没有立即取消 upstream；这是 S3 P3 的收敛点（`server_client.c:1692-1701`；`voice_proxy.c:198-205,336-354`）。`voice_busy` 只维持现有边界，不扩大。

### ESP-server 当前火山请求

|审计项|live source 证据与当前行为|
|---|---|
|WebSocket URL 构造|`readVolcGatewayConfig()` 读 `VOLC_GATEWAY_WS_BASE_URL`、TTS path、model，再以 `buildGatewayTtsUrl()` 构造 `config.tts.url`（`ESP-server/src/voice/gatewayConfig.js:31-78,108-120`; `src/utils/env.js:92-109`）。实际 URL 随环境配置，不写死为文档事实。|
|鉴权 headers|`buildVolcGatewayHeaders(config, "tts")` 发送 `Authorization: Bearer <apiKey>`；仅在启用时附带 `X-Api-Resource-Id`（`src/voice/gatewayHeaders.js:1-14`）。|
|subprotocol|当前 `openRealtimeWebSocket()` 只设置标准 Upgrade、Connection、`Sec-WebSocket-Key`、`Sec-WebSocket-Version` 与传入 headers；未设置 `Sec-WebSocket-Protocol`（`src/voice/realtimeSocket.js:265-325`）。|
|基础 client|复用本地 `MinimalWebSocket`；其文本帧、close、error、waiter 语义在 `src/voice/realtimeSocket.js:60-263`。|
|session update|连接后发送 `tts_session.update`；session 含 voice、`output_audio_format:"pcm"`、`output_audio_sample_rate`、speed/pitch/volume 和 `text_to_speech.model`（`src/voice/chain.js:170-176`; `src/voice/realtimeEvents.js:156-170`）。|
|model / voice / format|model、voice、sample rate 和 format 从环境读取；默认目标为 16 kHz 与 `pcm_s16le_mono_16k`，且验证器当前拒绝非该格式或非 16000（`src/voice/gatewayConfig.js:17-29,47-82,202-216`）。session 当前请求 raw `pcm`，而不是 WAV 容器。|
|channels / PCM bit depth / endian|session 未显式发送 channels、bit depth 或 endian；当前 Server 下游合同固定 16 kHz mono PCM16 little-endian（`src/voice/http.js:3-5,150-165`）。是否由网关的 `pcm` 明确保证这些细节，必须由官方协议与 canary 证明。|
|`input_text.append`|在 session ack 后只调用一次，`delta` 为完整 LLM 回复（`src/voice/chain.js:186-194`）。|
|`input_text.done`|紧接唯一 append 后发送一次（`src/voice/chain.js:195-197`）。|
|`response.create/start`|当前代码未发送该消息；session ack 后直接 append/done（`src/voice/chain.js:176-199`）。不能以其他 Realtime API 推测其是否需要。|
|audio delta|parser 识别 `response.audio.delta` 或包含 `audio.delta` 的 event；audio 字段优先 `delta`，也兼容 `audio_base64`、`pcm_base64`、`audio`、`data`（`src/voice/realtimeEvents.js:116-137`）。decoded delta 在完成事件前的接收循环中被逐个 `audioChunks.push()`（`src/voice/chain.js:199-219`）。|
|complete / error / close / cancel|完成识别 `response.audio.done`、`audio.done` 或 `completed`；error 为 event name 含 `error` 或 `payload.error`（`src/voice/realtimeEvents.js:127-137`）。正常 return 后 finally 发送 WebSocket close（`src/voice/chain.js:224-249`）；当前没有 provider 级 cancel 消息。异常 close 会拒绝 waiter（`src/voice/realtimeSocket.js:85-108,220-261`）。|
|当前首包阻塞|audio delta 被本地收集，只有 `isAudioDone` 后执行 `Buffer.concat()` 与 `normalizeTtsPcmBuffer()`，route 随后设 `Content-Length` 再 `res.end()`（`src/voice/chain.js:216-227`; `src/routes/voiceRoutes.js:504-545`; `src/voice/http.js:161-165`）。这是 Server 本地聚合导致的首包延迟。|

**当前模式分类：B（完整文本输入、音频增量事件输出）的源码结构分类。** 一次完整文本被 append 后 done，消费循环可在 `completed` 前处理 audio delta；当前又因本地 `Buffer.concat()` 阻塞下游首包。源码并不能证明 provider 的首个 audio delta 在 completion 前实际到达，也不能证明 raw `pcm` 的完整 PCM16/16 kHz/mono 合同。因此 P2a 先以脱敏 fixture 和真实 canary 验证时序；在验证前，不得把“能合成”或“收到多个 WebSocket event”当成真正流式或格式正确的证明。

当前没有 live source、fixture 或 canary 证据表明必须修改 operation、session、format、sample rate、文本结束流程或 response start 才能增量输出。请求侧暂不进入 P2a 最小修改范围；若后续证据否定模式 B 的真实增量时序，才进入第 3 节批准的最小差异路径。

## 3. 火山生产兼容基线与字段决策

默认保持不变：当前火山产品和网关入口、有效鉴权体系和凭据读取方式、已验证 model/voice、基础 WebSocket client、错误码映射和兼容事件字段、用户文本及业务语义、安全脱敏规则。

“默认不改”不是绝对禁止。只有官方协议依据、脱敏 fixture 和真实 canary 共同表明 P2a 无法在 provider completion 前取得有效 audio 时，才可最小调整与音频流直接相关的 operation/mode、session streaming 输出、output format、sample rate、channels、PCM bit depth/endian、增量事件开关、input append/done 或等价结束、response start/create、provider cancel/close、streaming/idle timeout 和最大字节限制。不得修改与流式无关的已工作字段，也不得根据通用 OpenAI Realtime API 猜测火山字段。

|字段或消息|当前值/行为|目标值/行为|最小路径必改|修改依据|首包延迟影响|最终 PCM 合同|fixture 测试|canary 验收|flag 关闭回退|失败停止条件|
|---|---|---|---|---|---|---|---|---|---|---|
|operation/mode|live source 未显式发送 operation|保持不变，除非官方协议证明当前网关需流式 mode|否，待证据|当前源码无缺口证据|仅必要时改善|不得破坏 PCM16|现有/候选 mode 事件时序|首 delta 早于 completion|现有 buffered strategy|未知字段或兼容失败立即停 P2a|
|session update|`tts_session.update` 后等 ack|保持顺序；仅允许必要 streaming 输出字段|否，待证据|`chain.js:176-199`|可能|保持或显式获得目标 PCM|session ack + delta fixture|auth/session/delta 正常|现有 session|ack/音频失败停止|
|`output_audio_format`|`pcm`|当前满足目标则保持；否则按协议最小指定 raw PCM|否，待证据|`realtimeEvents.js:156-170`|可能|必须 PCM16 LE、无 WAV|raw PCM 和错误格式 fixture|格式可播放且无容器|当前 buffered parse|不能证明格式即停止|
|sample rate|配置验证为 16000|保持 16000；不能安全流式规范化才最小请求 16000|否，当前已满足|`gatewayConfig.js:202-216`|无或可能|16000 Hz|格式 fixture|16000 证据|当前配置|不匹配停止|
|channels|session 未显式设置|保持 provider 默认，除非协议要求显式 mono|否，待证据|live session source|无或可能|mono|format fixture|单声道证据|当前行为|不明停止|
|PCM bit depth/endian|session 未显式设置|保持或最小明确 PCM16 LE|否，待证据|C5 合同|无或可能|PCM16 LE|even length/known samples|可播放性与格式|现有 buffered normalize|不明停止|
|`input_text.append`|一次完整 LLM 回复|P2a 保持一次完整回复；P2b 才评估多次 append|否|`chain.js:191-194`|P2a 无|不变|单次 append fixture|文本/音频正确|当前流程|语义变化停止|
|`input_text.done`|append 后立即一次|保持；仅官方协议证明需要等价结束才调整|否，待证据|`chain.js:195-197`|可能|不变|done 时序 fixture|delta/complete 正常|当前流程|兼容失败停止|
|`response.create/start`|未发送|保持未发送；仅协议/canary 证明必需才加入|否，待证据|live source 未出现|可能|不变|有/无该消息对照|仅最小必需路径成功|当前无该消息|多余或失败停止|
|completed|`audio.done`/`completed` 结束|保持解析别名与 once guard|是，消费侧|已有 parser|决定结束，不制造首包|完成前不得留 odd byte|重复/早到 completion fixture|完整终态一次|buffered 同 parser|终态歧义停止|
|cancel/close|当前 finally WebSocket close，无 provider cancel|保留 close；仅协议证实后增加 provider cancel|否，待证据|client abort 需有界停止|减少浪费|不输出半 sample|abort/close fixture|abort 后有限时间停止|关闭 streaming 走当前 close|无法停止上游停止|
|timeout|全 turn timeout 60s|保留基线；仅添加有界 streaming/idle timeout|否，待证据|`turnConfig.js:5-21`|保护首包/卡死|不改变格式|slow/idle fixture|timeout 可回退|现有 timeout|误杀成功 turn 停止|
|maximum audio bytes|当前完整路径受其既有上限|保持或加入 byte-bounded queue/single-chunk limit|是，消费侧|防止无界内存|不应阻塞合法首包|偶数字节|overflow fixture|超限取消且无泄漏|buffered 上限|超限继续收包停止|

情况一：fixture 和 canary 证明当前模式 B 的首个 audio event 早于 provider completion 时，保持 URL、鉴权、model、voice、session 顺序、文本顺序、operation 与既有请求侧代码；P2a 仅改 audio delta 消费、PCM aligner、bounded queue、stream object、Express chunked response 与 client abort 后 close/cancel。

情况二：只有证据证明当前请求不能在 provider completion 前产生 audio，才可按表切换当前网关对应的 streaming operation/mode、必要 session/output、response start/create、input done/end、raw PCM、16 kHz mono PCM16 LE 与 provider cancel。改动必须为最小差异，基础 client 和 parser 不重写。

## 4. 火山 Provider Adapter 边界

新增或提炼仅限 `src/voice/**` 的 adapter，输出：

```js
{ type: "metadata" | "audio" | "completed" | "error", provider: "volcengine", payload: ... }
```

adapter 复用现有 parser 与所有已兼容 audio aliases；不直接写 Express、不持有 HTTP response、不强迫重写 parser、不改变 chunk 内容/顺序。`completed` 和 `error` 各只产生一次。normal close 仅在 completed 后允许成功；其他 close 转为一次失败；abort 关闭 WS、在协议证实后发最小 provider cancel，并唤醒 waiter。所有 listener 在终态移除。

streaming 与 buffered 必须共享配置读取、URL 构造、鉴权、基础 WebSocket client、model、voice、错误映射、event decode helper、安全日志和 provider metrics。经过 canary 验证，二者可在 operation/mode、session output、response start/create、text finish/end、cancel 与 audio consumer 上有明确且很小的 request strategy 差异；不得复制粘贴两套完整 provider client。

## 5. PCM 输出合同

目标仍是 signed PCM16 little-endian、16000 Hz、mono、无 WAV 容器，且 C5 可直接播放。`http.js` 现有 response headers 固定该合同（`ESP-server/src/voice/http.js:3-5,150-165`）。不新增重采样或转码依赖。

如果当前配置已满足合同则保持不变；如果完整 Buffer 路径经由规范化才满足，必须审计该规范化能否逐块运行。无法安全流式规范化时，优先按第 3 节请求火山直接输出目标格式。canary 不能证明格式正确时停止 P2a，不能静默下发。

raw PCM 是 streaming 主路径；WAV、JSON/base64 仍为受最大 bytes 限制的整包 fallback。frame aligner 共用于 WebSocket raw PCM 和 HTTP raw PCM：保留最多 1 byte carry，只输出偶数字节；正常完成仍留 1 byte 为 `VOICE_TTS_ODD_LENGTH`，error/abort 清 carry。必须测试 sample 跨 delta/chunk 与最终奇数字节。

## 6. Server Streaming 对象设计

```js
createVoiceTtsPcmStream(text, config, deviceId, { signal, limits }) => ({
  metadata: { provider, sampleRate: 16000, channels: 1,
              format: "pcm_s16le_mono_16k", streamingCapable },
  stream, // AsyncIterable<Buffer>; non-empty, even byte length, bounded
  completion, // resolves exactly once
  abort
})
```

`completion` 返回 `{ status, provider, bytesGenerated, chunksGenerated, failureReason }`。queue overflow、client abort、provider error 不得产生 unhandled rejection；`abort(reason)` 幂等中断 WS、HTTP reader、queue 与 provider lifecycle。

## 7. WebSocket 有界 PCM Queue

producer 只做 parse、base64 decode、align 与 queue push，绝不直接 `res.write()` 或等待 Express drain。queue 按偶数字节 `queuedBytes` 硬限流、限制 single chunk、记录 peak。overflow 必须原子失败为 `VOICE_TTS_QUEUE_OVERFLOW`，停止上游并唤醒 waiter；禁止静默丢 chunk。finish/fail/abort/WS close/WS error 使用 once guard；完成后禁止 push。

HTTP raw PCM 用 pull reader 并复用 aligner，禁止 `arrayBuffer()`；WAV/JSON fallback 继续有界整包。

## 8. ESPC51 / ESPC52 修改任务

### C5-1：播放资源常驻化（P1）

文件：两端 `components/Middlewares/speaker/speaker_player.{c,h}`、`components/BSP/IIS/iis.{c,h}`；必要时最小修改 `voice_domain/voice_chain.c`。不默认修改 `server_voice`。

`audio_player_init()` 成为唯一 resource owner：启动时创建 I2S channel/DMA、ringbuffer、同步对象、scratch 和 writer，进入 `READY_DISABLED`；首次 `stream_open` 不得再创建这些资源。每轮只 reset/enable/write/EOS/drain/disable。writer 是唯一 `iis_write()` owner；abort 只设 flag/generation/notify，由 owner 完成清理。状态机为 `UNINITIALIZED -> READY_DISABLED -> OPENING -> PLAYING -> DRAINING -> READY_DISABLED`，异常经 `ABORTING` 回到 `READY_DISABLED`。init fail 只让 voice fail-closed，不阻断非 voice。

记录 internal/DMA free/min/largest、I2S total DMA bytes、`sizeof(audio_player_ring_item_t)` 与 writer HWM。DMA data/descriptors 绝不迁 PSRAM。C51/C52 同提交，真机验证连续 voice、abort、Wi-Fi disconnect、prompt/beep、Mic/VAD 顺序、`first_chunk_to_i2s_ms` 与无 writer/I2S DMA `ESP_ERR_NO_MEM`。

### C5-2：上行 buffer 与条件性 P1b

`server_voice_rx` 保持每轮创建；仅当 P1 后 HWM 与 largest-free-block 证明风险时，才评估常驻 RX、static task 或非 DMA 内存 placement。upload buffer 后续独立改进必须 PSRAM-only，失败有界拒绝，不改 fixed `Content-Length` 上行合同。

## 9. ESPS3 修改任务（P3）

仅改 `components/Middlewares/server_client/server_client.c`、`components/Middlewares/voice_proxy/voice_proxy.c`，必要 header/voice 测试。保留完整 C5 request、S3 request cache、1024-byte read buffer、逐 read `httpd_resp_send_chunk()`、路由和 mutex 合同。

- known `Content-Length`：总 bytes 必须相等，提前 zero-read 是 incomplete。
- chunked/unknown：以 `esp_http_client_is_complete_data_received()` 为主；未完成 zero-read 继续现有 deadline/有界短等，记录 `repeated_zero_reads`，不忙循环。
- downstream send 失败：立即停止 upstream read，并经已有 peer cancel/close 语义单次取消；不再继续消费 TTS。
- success 只发一次 zero-length terminator，busy 只 release 一次；`voice_busy` 不扩大，非 voice ingress 不差于基线。

## 10. ESP-server HTTP Route 修改任务

预计：`src/voice/chain.js`、`ttsAudio.js`、`http.js`、`mockTurn.js`、`routes/voiceRoutes.js`；必要时 voice-only queue/aligner/helper/test 和 voice request strategy/config。不得重写基础 WebSocket client；保持 POST route、raw parser 顺序、鉴权、binding、concurrency、metadata、audio headers 和非 voice routes/services。

route 先取首个有效 PCM。首块前 provider fail 仍用 `sendVoiceError()`；completed 但无 PCM 为 `VOICE_TTS_EMPTY_AUDIO`。首块有效后保留 Content-Type/X-Audio-Format、`Cache-Control:no-store`、`nosniff`，不设 `Content-Length`，写首块；后续逐 `res.write()`。`res.write(false)` 等待 drain，但与 `req.aborted`、`res.close`、socket error、AbortSignal race；任一先到即 abort provider。partial 后不返回 JSON error，listeners 清理，concurrency release once；持久化使用真实 `bytesWritten`，不把 `bytesGenerated` 冒充已写出。

## 11. 火山脱敏协议 Fixture

P2a 前对当前可用请求建立脱敏快照：URL 仅保留结构、headers 只保留字段名、session/append/done 仅保留类型和假文本、音频使用微小人工 PCM/base64；绝不含 key、Authorization 值、token、secret、signature、真实设备身份、真实用户文本或真实音频。

fixture 必须证明 audio event 的到达序列，而非仅 parser 可解析：当前请求配 buffered consumer、当前请求配 streaming consumer、如决定表批准调整则调整请求配 streaming consumer。覆盖 session ack、`delta`/`audio_base64`/`pcm_base64`/`audio`/`data`、unknown、completed、error、complete 前 close、重复 completion、跨 delta sample、odd tail、overflow、abort、WS error、empty audio、wrong format 和 flag on/off。相同 fixture 供旧聚合参考与新 aligner+queue；最终 PCM 必须字节一致，允许 chunk 边界不同。

## 12. 真实火山网关 Canary

仅在 fixture/mock 通过、进入 S3/C5 前执行。使用既有合法环境变量和固定短文本、临时 DB，不写 `ESP-server/db/database.db`；限制次数和费用，不打印凭据，不改生产密钥。

分别验证：当前请求 + buffered consumer；当前请求 + streaming consumer；仅在必要时，批准字段调整后 + streaming consumer；client abort；provider error；empty audio；wrong format；streaming flag 回退。短文本只有一个 audio chunk 时，不得以 chunk 数认定非流式。

验收：当前 model/voice 继续工作；至少一个 audio event；首个 audio event 早于 provider completion；首个 response write 早于 provider completion；PCM 非空、偶数字节、符合 C5 格式；新 streaming 拼接与同请求 buffered 参考逐字节一致，或 provider 在 deterministic 条件下仍不一致时，以同请求证据、格式、长度和可播放性补充说明，不能无依据放宽；abort 后 provider 在有限时间停止；无凭据、listener 或 Promise rejection 泄漏。任何失败保持 `VOICE_TTS_STREAMING_ENABLED` 关闭，不进入 S3/C5 E2E；请求调整失败时回到当前已工作请求与 buffered consumer。

## 13. Streaming 与 Buffered 回退

规划 `VOICE_TTS_STREAMING_ENABLED`，无新依赖。关闭时用现有有上限 buffered path；开启时用 stream object/chunked response。两种策略共享第 4 节基础接入，经过 canary 才允许第 3 节的受控 request strategy 差异。关闭 Server flag 即可回退，无需回滚 C5/S3；两条路径都受最大 bytes 和 timeout 限制。

## 14. 测试计划

未来 Node fixture/mock 覆盖 first audio/first write timing、随机边界、aligner、odd tail、empty/wrong format、slow consumer、drain/abort、首块前/partial error、queue overflow、close/error once、listener cleanup、generated vs written bytes、flag on/off 和 byte equivalence。S3 定向验证 known/unknown EOF、downstream disconnect、terminator/busy once。C5 真机验证 init success/fail non-voice continuity、多轮、DMA/writer 不重复创建、abort/Wi-Fi、prompt/beep、Mic、双 C5、memory/HWM 与首块播放时延。

本次禁止执行 build、test、服务启动、真实 canary、数据库操作、依赖安装、flash、clean 或配置修改；未来命令与环境隔离应在实施提交中另行记录。

## 15. 观测指标

C5：player init、internal/DMA free/min/largest、I2S DMA、writer HWM、abort、underrun、`first_chunk_to_i2s_ms`。S3：headers、first upstream/downstream chunk、chunks/bytes、zero reads、disconnect、incomplete、cancel、busy duration。Server：connect/session/first audio/complete/abort、audio event/byte/type、first PCM/first write/complete、queue、generated/written、backpressure、client abort、listener cleanup、strategy。每 turn 一条脱敏 summary，每 chunk 仅 debug。

## 16. 分阶段实施顺序

|阶段|内容|停止条件与回滚|
|---|---|---|
|P0|记录 C5 内存/Server 时延基线；当前火山请求脱敏快照、fixture、mock|没有时序/格式证据，不调整请求、不进 P2a|
|P1|C51/C52 player/IIS 常驻化|NO_MEM、underrun、非 voice 回归或 parity diff 即回滚独立 C5 commit|
|P1b|仅经 P1 证据做 RX/PSRAM 非 DMA 优化|回归即回滚|
|P2a|先审计请求；满足 B 则只改消费/输出，否则只改批准字段；adapter、aligner、queue、route、flag|fixture 不通过或 canary 前不完整，不进入 S3/C5|
|P2a canary|真实火山时序、格式、abort、回退|任一失败 flag off；请求侧退回已工作路径|
|P3|S3 EOF/cancel/metrics 小修|不重构 scheduler/network worker|
|P4|Server -> S3 -> 双 C5 E2E 与非 voice 回归|`public`/database/非 voice diff 非零即停止|
|P2b（可选）|文本输入双向流式化|仅 P2a、canary、S3/C5 E2E 全部稳定后另行规划|

### P2b：文本输入双向流式化（独立可选阶段）

P2b 的目标是 LLM 文本增量按句子或语义边界送入 TTS，并持续接收音频。它不属于 P2a 最小必改路径，必须单独评估火山是否支持同 session append、是否需要 response create、append/done 合同、中文/英文断句、数字/金额/日期/时间/单位、URL/邮箱/缩写、Markdown/代码块、最小文本长度、最大等待、首句延迟、跨 chunk 韵律与上下文、LLM 后续更正、不可撤回文本、abort、session 复用/turn 隔离与 buffered fallback。

## 17. 文件级最小修改范围

**C51/C52：** `speaker_player.c/.h`、`iis.c/.h`；可能最小 `voice_chain.c`。不改 BME、CSI、command、register、heartbeat、status、device stream、terminal protocol、`/api`、sdkconfig、partition、CMake。

**S3：** `server_client.c`、`voice_proxy.c`，必要 header/voice test。不改 local route contract、scheduler/network worker、BME cache/replay、CSI、command、smart-home、snapshot、event、frontend。

**Server：** 第 10 节列出的 voice-only 文件及必要 request strategy/config；不改 `public/**`、真实 DB、Dashboard、sensor/CSI/command/smart-home/event/memory/user data、dependencies/schema、非 voice parser 或无关 provider 字段。

## 18. Definition of Done

1. C51/C52 在 `voice_chain_start` 早期完成 player init；多轮无 writer/I2S DMA `ESP_ERR_NO_MEM`，且 init fail 不阻断非 voice。
2. 已用准确文件、函数、行号明确当前火山请求的 A/B/C/D/E 分类；当前结论为 B 的源码结构分类，并用 fixture/canary 验证真实事件时序。
3. 已证明或否证首个 audio event 早于 provider completion；不得用“能合成”或多个 WebSocket event 代替证明。
4. 若当前请求满足 P2a，无无意义请求改动；若不满足，只改决策表和 canary 批准字段。
5. 当前鉴权、model、voice、事件别名继续工作；最终 PCM 为 C5 可播放的 PCM16 LE、16 kHz、mono、无 WAV。
6. provider completion 前写首块，S3 首 chunk 立即转发，C5 完整 response 前 I2S 播放；abort 有界停止上游，drain 不永久等待，listeners 清理。
7. queue 不越界、overflow 显式失败、空音频不返回 200；S3 EOF/terminator/busy 正确且不扩大 gate。
8. streaming flag 可回退当前工作路径；共享基础接入而不形成两套 client；P2a 不改 LLM 输出方式，P2b 独立可选。
9. `public/**`、database、Dashboard、非 voice contracts 无 diff；C51/C52 parity、后续独立 build/test/canary/E2E 均通过。

## 19. 风险和停止条件

|风险|预防与停止条件|
|---|---|
|把通用 Realtime 字段当火山字段|仅 live source、官方协议、fixture、canary 共同批准；未知字段或兼容失败即停 P2a|
|当前 delta 只在 completion 时到达|以 event/completion 时间戳判定；失败则不伪装 streaming，按最小字段表或 buffered 回退|
|格式仅表面为 PCM|canary 验证 raw PCM、16 kHz、mono、PCM16 LE；不明即停|
|queue/aligner/terminal race|byte limit、once guard、odd-tail、abort、listener tests；失败即回滚 P2a|
|abort 后继续计费|cancel/close fixture 与 canary；有限时间不能停止即不进 S3/C5|
|P2b 破坏语义与韵律|P2b 独立，P2a/E2E 稳定前不得实施|

## 20. 最终推荐路线

**C5：** P1 将 player、I2S/DMA、writer、ring、sync、必要 scratch 在 `audio_player_init()` 一次创建；每轮 reset/enable/drain/disable；`server_voice_rx` 保持 per-turn，P1b 只看证据。

**Server：** 先审计当前火山请求。若 fixture/canary 确认当前完整文本输入、增量 audio 输出，则保留请求侧，做 parser -> standard event -> aligner -> bounded queue -> stream object -> chunked HTTP。若否，只做表中批准的最小 request strategy/config 改动，基础 client 和 parser 保持共享。

**S3：** 保留逐块代理，只修 EOF、断流取消与指标，不重构。

路线是 `P0 snapshot/fixtures -> P1 C5 -> P2a Server mock/fixture -> P2a canary -> P3 S3 -> P4 dual-C5 E2E`；P2b 完全独立。真实火山 canary 失败时，关闭 Server streaming flag 并保留当前 buffered 路径，不能以 C5/S3 改动掩盖 provider 不兼容。

# API Reference

本文件记录当前后端、Dashboard 和 ESP 设备之间已经使用的 HTTP 接口。接口字段变更必须先更新本文档，再修改相关后端或调用方代码。当前统一设备协议 v1 实施不修改任何前端文件。

## 通用机器接口错误格式

`/api/*`、`/sensor`、`/sensor/*`、`/asr`、`/asr/*`、`/llm` 和 `/llm/*` 这类 ESP、Dashboard 或脚本消费的机器接口，在路由不存在时返回 JSON，而不是 Express 默认 HTML/文本：

```json
{
  "ok": false,
  "error": "Not found"
}
```

未预期的后端 route 异常会返回通用 JSON 500：

```json
{
  "ok": false,
  "error": "Internal server error"
}
```

这些兜底错误响应不改变已存在成功接口的响应结构，也不改变 `/dashboard` 或静态前端资源路由。

## ESP 上传数据接口

### `POST /api/llm/text`

Mic-getaway 将 ASR final 文本发送到服务器，服务器只代理文本到火山引擎 LLM Chat Completions 网关，并把模型回复文本返回给 ESP。服务器不接收、不转发、不处理任何 ASR/TTS 音频。
服务器在调用 LLM 前会统一通过 `llmPromptContextService` 注入 `deviceContextService` 读取到的设备、模块、环境、空气状态和数据新鲜度上下文；route 不直接散查 `sensor_records`。

请求体：

```json
{
  "text": "ASR最终文本",
  "device_id": "mic-getaway-001",
  "session_id": "可选"
}
```

字段说明：

- `text`: ASR final 文本，必填，去除首尾空白后不能为空，最大 `4000` 字符。
- `device_id`: 设备 ID，可为空，仅用于服务器日志脱敏定位；服务器会 trim，最多保留 `128` 个字符。
- `session_id`: 会话 ID，可为空，仅用于服务器日志脱敏定位；服务器会 trim，最多保留 `128` 个字符。

成功响应：

```json
{
  "ok": true,
  "text": "LLM回复文本",
  "id": 123,
  "model": "Doubao-Seed-1.6-flash",
  "server_time_ms": 1780000000000
}
```

失败响应：

```json
{
  "ok": false,
  "error": "LLM request failed"
}
```

非法请求会返回 HTTP `400`：

```json
{
  "ok": false,
  "error": "text is required"
}
```

服务器 `.env` 配置示例：

```dotenv
LLM_API_KEY=<火山网关API Key>
LLM_BASE_URL=https://ai-gateway.vei.volces.com
LLM_CHAT_PATH=/v1/chat/completions
LLM_MODEL=Doubao-Seed-1.6-flash
LLM_TIMEOUT_MS=30000
```

说明：

- `Authorization` 只由服务器使用 `.env` 中的 `LLM_API_KEY` 生成，不会返回给 ESP。
- 默认请求地址为火山引擎边缘大模型网关旧版控制台地址：`https://ai-gateway.vei.volces.com/v1/chat/completions`。
- `LLM_MODEL` 使用网关访问密钥绑定的平台预置模型名，例如 `Doubao-Seed-1.6-flash`。不要把新版方舟 Endpoint ID 或新版方舟地域网关地址填到这里。
- 当前服务器只调用平台预置 Chat Completions 模型，不发送 `X-Api-Resource-Id`。该请求头仅适用于自有三方渠道等需要 Resource ID 的场景。
- 调用成功后会写入现有 `llm_records` 表：`prompt=text`，`response=模型回复`，因此 `GET /llm/latest` 仍可显示最新回复。
- 该接口不新增 WebSocket，不处理 ASR 音频，不处理 TTS 音频，不代理 ASR/TTS。

### `POST /api/llm/structured`

将用户文本发送到 LLM，并要求模型返回稳定 JSON。服务器解析 `chat` 与 `commands` 分段：`chat.text` 用于自然语言回复，`commands` 进入命令白名单、设备能力校验和命令队列。该接口是新增能力，不改变 `POST /api/llm/text` 的旧响应字段。
该接口同样统一使用 `llmPromptContextService` 注入设备上下文，并在结构化命令提示中保留 JSON-only 指令。

请求体：

```json
{
  "text": "把音量调到 35",
  "device_id": "mic-getaway-001",
  "target_device_id": "esp32-c5-001",
  "session_id": "可选"
}
```

字段说明：

- `text`: 用户文本，必填，最大 `4000` 字符。
- `device_id`: 发起请求的设备 ID，可为空，仅用于日志和命令来源记录；服务器会 trim，最多保留 `128` 个字符。
- `target_device_id`: 命令目标设备 ID；如果为空，服务器使用 `device_id`。结构化 LLM 输出中的命令目标由 Server 使用该字段统一附加或覆盖，避免模型自由选择设备。
- `session_id`: 会话 ID，可为空，仅用于日志定位；服务器会 trim，最多保留 `128` 个字符。

成功响应：

```json
{
  "ok": true,
  "text": "好的，我会把音量调到 35。",
  "chat": {
    "text": "好的，我会把音量调到 35。"
  },
  "commands": [
    {
      "command_id": "uuid",
      "device_id": "esp32-c5-001",
      "name": "voice.set_volume",
      "payload": {
        "volume": 35
      },
      "status": "queued",
      "created_at": "2026-06-07T00:00:00.000Z"
    }
  ],
  "rejected_commands": [],
  "structured": {
    "parsed": true,
    "version": "agent-command-v1",
    "error": ""
  },
  "id": 123,
  "model": "Doubao-Seed-1.6-flash",
  "server_time_ms": 1780000000000
}
```

兼容降级：

- 如果 LLM 返回的内容不是合法 JSON，服务器不会让请求失败；会把原始文本作为纯聊天回复返回，并给出 `structured.parsed=false`。
- 如果 LLM 返回合法 JSON 但缺少 `chat.text`，`text` 和 `chat.text` 会保持为空字符串，不会把整段 JSON 当作自然语言回复。
- 如果命令不在白名单、目标设备未注册能力，或 payload 不合法，对应命令进入 `rejected_commands`，不会进入队列。
- `rejected_commands[]` 中每项包含 `name`、`target_device_id`、`code` 和 `error`，常见 `code` 包括 `COMMAND_NOT_WHITELISTED`、`COMMAND_TARGET_INVALID`、`DEVICE_CAPABILITIES_REQUIRED`、`DEVICE_COMMAND_UNSUPPORTED`、`COMMAND_PAYLOAD_INVALID`。
- 该接口只建立 Server 侧结构化协议和命令队列，不代表 ESP 固件已经实现命令执行。

### `POST /api/voice/turn`

ESP 设备上传一轮 PCM 音频，服务器校验格式后返回一轮语音音频。该接口默认不调用任何火山引擎语音地址，也不会把请求发到不存在的 `/v1/voice`。
该接口支持统一设备协议 v1 header metadata，并会刷新 `device_status` 与 `device_module_status(module_type="voice.turn")`。真实链路中的 ASR final 文本进入 LLM 前也统一通过 `llmPromptContextService` 注入设备上下文。

请求头：

```http
Content-Type: audio/L16; rate=16000; channels=1
X-Audio-Format: pcm_s16le_mono_16k
X-Device-Id: esp32-c5-001
X-Voice-Turn-Id: 可选请求追踪ID
X-Schema-Version: 1
X-Device-Type: esp32c5_env_voice_node
X-Firmware-Version: 0.1.0
X-Request-Seq: 123
X-Esp-Uptime-Ms: 12345678
X-Esp-Time-Ms: 1780732142207
X-Time-Synced: true
X-Payload-Type: voice.turn
```

字段说明：

- `Content-Type`: 必须声明 `audio/L16; rate=16000; channels=1`。
- `X-Audio-Format`: 必须声明 `pcm_s16le_mono_16k`，用于和服务端 PCM 校验契约对齐。
- `X-Device-Id`: 可选，用于并发控制和脱敏日志定位。
- `X-Voice-Turn-Id`: 可选，用于将 ESP 侧日志和 Server 侧 `voice_turns` 持久化记录关联；未传时 Server 自动生成。
- `X-Schema-Version`、`X-Device-Type`、`X-Firmware-Version`、`X-Request-Seq`、`X-Esp-Uptime-Ms`、`X-Esp-Time-Ms`、`X-Time-Synced`、`X-Payload-Type`: 统一设备协议 v1 metadata，raw PCM body 不改成 JSON。

服务器 `.env` 配置示例：

```dotenv
VOICE_TURN_MOCK=1
VOICE_TURN_TIMEOUT_MS=60000
VOICE_TURN_MAX_CONCURRENT=1
VOICE_TURN_MAX_BYTES=4194304
VOLC_GATEWAY_API_KEY=<火山网关API Key，仅 VOICE_TURN_MOCK=0 时需要>
VOLC_GATEWAY_WS_BASE_URL=wss://ai-gateway.vei.volces.com
VOLC_GATEWAY_HTTP_BASE_URL=https://ai-gateway.vei.volces.com
VOLC_GATEWAY_REALTIME_PATH=/v1/realtime
VOLC_GATEWAY_CHAT_PATH=/v1/chat/completions
VOLC_GATEWAY_ASR_MODEL=bigmodel
VOLC_GATEWAY_CHAT_MODEL=Doubao-Seed-1.6-flash
VOLC_GATEWAY_TTS_MODEL=<TTS 模型>
VOLC_GATEWAY_TTS_VOICE=<TTS 音色>
VOLC_GATEWAY_TTS_PATH=/v1/realtime
VOLC_GATEWAY_TTS_SAMPLE_RATE=16000
VOLC_GATEWAY_TTS_FORMAT=pcm_s16le_mono_16k
VOLC_GATEWAY_TTS_SPEED=1.0
VOLC_GATEWAY_TTS_PITCH=1.0
VOLC_GATEWAY_TTS_VOLUME=1.0
```

配置说明：

- 当前项目没有内置真实 ASR+LLM+TTS voice turn 上游时，请设置 `VOICE_TURN_MOCK=1`。服务器会稳定返回 `audio/L16; rate=16000; channels=1` 的 mock PCM 音频，用于验证 ESP32 和 Node 后端的 HTTP 音频链路。
- `VOICE_TURN_MOCK=1` 也会让 `GET /api/voice/prompt` 返回 1 秒非静音 mock PCM，用于验证 wake prompt cache 下载、保存和播放链路；该 mock PCM 只是测试音，不代表真实 wake prompt TTS 音色。
- 只有接入真实火山网关 ASR -> LLM -> TTS 链路时，才设置 `VOICE_TURN_MOCK=0` 并填写 `VOLC_GATEWAY_*` 配置。
- 火山网关当前已知可用的是文本 Chat Completions：`https://ai-gateway.vei.volces.com/v1/chat/completions`，以及 Realtime WebSocket：`wss://ai-gateway.vei.volces.com/v1/realtime?model=bigmodel`。这些都不是 `/v1/voice` HTTP turn 上游。
- 当前后端实现使用 `VOLC_GATEWAY_*` 配置完成 ASR -> LLM -> TTS 链式处理；`VOICE_TURN_MOCK=1` 时不调用外部 ASR/LLM/TTS。
- HTTP TTS 上游响应允许 raw PCM、WAV PCM s16le mono 16kHz，或 JSON 中的 base64 PCM 字段（如 `pcm_base64`、`audio_base64`、`pcm`、`audio`、`data`）。
- `VOICE_TURN_TIMEOUT_MS` 是 Server 侧单轮语音总超时，默认与 C5/S3 的 `VOICE_REQUEST_TIMEOUT_MS` 统一为 `60000ms`；环境变量仍可按部署需要覆盖。
- Server 会在 SQLite 中创建并维护 `voice_turns` 表，用于记录每轮 `/api/voice/turn` 的请求 ID、设备 ID、模式、状态、错误码、输入/响应字节、ASR/LLM/TTS 耗时和总耗时。该表是后端诊断日志，不改变当前 HTTP 响应格式。

失败响应会返回结构化错误码。缺少 `X-Audio-Format` 时返回 HTTP `415`：

```json
{
  "ok": false,
  "code": "VOICE_UNSUPPORTED_AUDIO_FORMAT",
  "error": "X-Audio-Format must be pcm_s16le_mono_16k"
}
```

`Content-Type` 不是 `audio/L16; rate=16000; channels=1` 时返回 HTTP `415`：

```json
{
  "ok": false,
  "code": "VOICE_UNSUPPORTED_CONTENT_TYPE",
  "error": "Content-Type must be audio/L16; rate=16000; channels=1"
}
```

PCM 请求体为空时返回 HTTP `400`：

```json
{
  "ok": false,
  "code": "VOICE_BODY_EMPTY",
  "error": "PCM request body must not be empty"
}
```

PCM 请求体不是 16-bit little-endian 对齐的偶数字节长度时返回 HTTP `400`：

```json
{
  "ok": false,
  "code": "VOICE_PCM_ALIGNMENT_INVALID",
  "error": "PCM s16le body length must be an even number of bytes"
}
```

PCM 请求体超过 `VOICE_TURN_MAX_BYTES` 时返回 HTTP `413`：

```json
{
  "ok": false,
  "code": "VOICE_BODY_TOO_LARGE",
  "error": "request entity too large"
}
```

未启用 mock 且未配置 ASR 网关参数时返回 HTTP `503`：

```json
{
  "ok": false,
  "code": "VOICE_ASR_NOT_CONFIGURED",
  "error": "VOLC_GATEWAY_API_KEY and VOLC_GATEWAY_ASR_MODEL must be configured when VOICE_TURN_MOCK is not 1"
}
```

未启用 mock 且未配置 TTS 参数时返回 HTTP `503`：

```json
{
  "ok": false,
  "code": "VOICE_TTS_NOT_CONFIGURED",
  "error": "VOLC_GATEWAY_TTS_MODEL, VOLC_GATEWAY_TTS_VOICE, and VOLC_GATEWAY_TTS_PATH must be configured when VOICE_TURN_MOCK is not 1"
}
```

如果 ASR、LLM 或 TTS 上游返回错误，服务器会保留上游状态码信息：

```json
{
  "ok": false,
  "code": "VOICE_LLM_FAILED",
  "error": "上游错误信息",
  "upstream_status": 415
}
```

### `GET /api/voice/prompt/config`

读取 wake prompt 当前配置。ESPS3 会先读取本接口，比较 `voice_config_hash` 与本地 metadata；hash 变化时重新拉取 PCM 缓存。C5 不调用本接口，也不解析提示词文本。

```json
{
  "ok": true,
  "config": {
    "wake_prompt_text": "我在，你说",
    "provider": "volc",
    "voice_id": "server_prompt_v1",
    "speaker_id": "",
    "speed": 1,
    "pitch": 1,
    "volume": 1,
    "sample_rate": 16000,
    "format": "s16le",
    "channels": 1,
    "prompt_version": "wake:...",
    "voice_config_hash": "...",
    "updated_at_ms": 1780732144669
  }
}
```

`voice_config_hash` 由 `wake_prompt_text`、`provider`、`voice_id`、`speaker_id`、`speed`、`pitch`、`volume`、`sample_rate`、`format`、`channels` 生成；任一字段变化都会改变 hash。

### `PUT /api/voice/prompt/config`

更新 wake prompt 文本或必要 TTS 参数。配置保存到服务器本地 JSON 文件，默认位于 `cache/voice_prompts/wake_prompt_config.json`；测试可用 `VOICE_PROMPT_CONFIG_PATH` 指向临时文件。修改后不需要重新烧录 C5/S3，下一次 S3 请求会按 hash 重新拉取。

```http
PUT /api/voice/prompt/config
Content-Type: application/json
```

```json
{
  "wake_prompt_text": "你好，我在",
  "voice_id": "server_prompt_v2",
  "speed": 1,
  "pitch": 1,
  "volume": 1,
  "sample_rate": 16000,
  "format": "s16le",
  "channels": 1
}
```

### `GET /api/voice/prompt-cache`

ESP 唤醒提示音服务器缓存接口。当前 ESP-111 主链路是 ESPS3 请求 Server 缓存/生成 wake prompt PCM，C5 再从 ESPS3 `/local/v1/audio/wake-prompt` 获取二进制流播放。

```http
GET /api/voice/prompt-cache?prompt_key=wake_ack_zh&device_id=esp32-c5-whole-001
```

请求可携带统一设备协议 v1 header metadata，`X-Payload-Type` 使用 `voice.prompt`。服务器会刷新 `device_status` 与 `device_module_status(module_type="voice.prompt")`。

成功响应为 raw PCM，不是 JSON：

```http
Content-Type: audio/L16; rate=16000; channels=1
X-Prompt-Key: wake_ack_zh
X-Prompt-Cache: hit
X-Audio-Format: pcm_s16le_mono_16k
X-Audio-Sample-Rate: 16000
X-Audio-Channels: 1
X-Audio-Version: wake:...
X-Voice-Config-Hash: ...
X-Sample-Rate: 16000
X-Channels: 1
X-Server-Time-Ms: 1780732144669
Cache-Control: public, max-age=86400
```

`X-Prompt-Cache` 取值：

- `hit`: 命中服务器缓存，直接返回文件，不请求 TTS、LLM 或网关。
- `miss`: 缓存缺失，本次调用 TTS 生成并写入缓存后返回。
- `stale`: 本次刷新失败，但存在旧缓存，返回旧缓存。

缓存文件默认位于 `cache/voice_prompts/`；测试可通过 `VOICE_PROMPT_CACHE_DIR` 指向临时目录，避免污染仓库。

### `GET /api/voice/prompt`

兼容唤醒提示音接口。旧请求 `GET /api/voice/prompt?wake=1&prompt_key=wake_ack_zh` 保留，并复用服务器 prompt cache 行为。缓存命中时同样不会请求 TTS、LLM 或网关。`POST /api/voice/turn` 主语音链路不受该缓存影响。

## 命令系统接口

### `GET /api/commands/whitelist`

读取服务器当前允许的命令白名单。LLM 或外部调用只能创建白名单内的命令。

```json
{
  "ok": true,
  "commands": [
    {
      "name": "voice.set_volume",
      "description": "Request a bounded playback volume change.",
      "payload": {
        "volume": {
          "type": "integer",
          "min": 0,
          "max": 100,
          "required": true
        }
      }
    }
  ]
}
```

当前白名单：

- `device.noop`
- `voice.set_volume`
- `sensor.set_upload_interval`
- `display.show_text`
- `alert.play_tone`

### `POST /api/devices/capabilities`

ESP 设备注册自己支持的命令能力。服务器只会向目标设备排队该设备已注册支持的白名单命令。`device_id` 会 trim 首尾空白，最大长度为 `128` 个字符。`protocol_version` 会 trim 首尾空白，最大长度为 `40` 个字符。`capabilities.commands` 中的命令名会 trim、去重，并且只保存当前白名单中的命令；未知或超长命令名会被忽略，不会进入设备能力快照。
该接口支持统一设备协议 v1 header metadata，并会刷新 `device_status` 与 `device_module_status(module_type="command.capabilities")`。

请求体：

```json
{
  "device_id": "esp32-c5-whole-001",
  "protocol_version": "agent-command-v1",
  "capabilities": {
    "commands": [
      "device.noop",
      "display.show_text"
    ]
  }
}
```

当前 `Whole-project` 固件会注册 `device.noop` 和 `display.show_text`。其中 `display.show_text` 只调用固件上层 `screen_service` / `ai_screen_bridge` 占位接口，不代表 LCD 底层驱动已经接入。

成功响应：

```json
{
  "ok": true,
  "device_id": "esp32-c5-whole-001",
  "protocol_version": "agent-command-v1",
  "capabilities": {
    "commands": [
      "device.noop",
      "display.show_text"
    ]
  },
  "server_time_ms": 1780000000000
}
```

错误响应：

- `400 DEVICE_ID_REQUIRED`: 缺少 `device_id`。
- `400 DEVICE_ID_INVALID`: `device_id` 超过 `128` 个字符。
- `400 PROTOCOL_VERSION_INVALID`: `protocol_version` 超过 `40` 个字符。

### `GET /api/devices/:device_id/capabilities`

读取某个设备最近注册的能力。路径中的 `device_id` 会 trim 首尾空白，最大长度为 `128` 个字符。未注册时返回 HTTP `404`。

错误响应：

- `400 DEVICE_ID_REQUIRED`: 缺少 `device_id`。
- `400 DEVICE_ID_INVALID`: `device_id` 超过 `128` 个字符。

### `POST /api/commands`

手动创建一条命令。服务器会先检查命令白名单，再检查目标设备是否注册支持该命令。`target_device_id` 会 trim 首尾空白，最大长度为 `128` 个字符；结构化 LLM 入口的 `target_device_id` 也遵循同一限制。`reason` 可选，最大 `240` 个字符。

请求体：

```json
{
  "name": "voice.set_volume",
  "target_device_id": "esp32-c5-001",
  "payload": {
    "volume": 35
  },
  "reason": "用户要求调低音量"
}
```

成功响应返回 HTTP `201`：

```json
{
  "ok": true,
  "command": {
    "command_id": "uuid",
    "device_id": "esp32-c5-001",
    "name": "voice.set_volume",
    "payload": {
      "volume": 35
    },
    "status": "queued",
    "created_at": "2026-06-07T00:00:00.000Z"
  }
}
```

失败示例：

```json
{
  "ok": false,
  "code": "COMMAND_NOT_WHITELISTED",
  "error": "command is not whitelisted",
  "name": "unknown.command"
}
```

目标设备 ID 过长时返回：

```json
{
  "ok": false,
  "code": "COMMAND_TARGET_INVALID",
  "error": "target_device_id must be <= 128 characters",
  "name": "display.show_text",
  "target_device_id": "被截断到128字符的预览"
}
```

`reason` 过长时返回：

```json
{
  "ok": false,
  "code": "COMMAND_REASON_INVALID",
  "error": "reason must be <= 240 characters"
}
```

### `GET /api/commands/pending`

ESP 设备轮询待执行命令。服务器会把返回的命令从 `queued` 标记为 `dispatched`。如果设备领取后没有成功回执，`COMMAND_DISPATCH_TIMEOUT_MS` 之后该命令会再次出现在轮询结果中，避免 ack 丢失后命令永久卡在 `dispatched`。
该接口支持统一设备协议 v1 header metadata，并会刷新 `device_status` 与 `device_module_status(module_type="command.poll")`。

查询参数：

- `device_id`: 必填，目标设备 ID。ESP 或其他客户端放入 URL query 前必须做 percent-encoding；例如 `esp 32+c5&测试` 应发送为 `esp%2032%2Bc5%26%E6%B5%8B%E8%AF%95`。
- `limit`: 可选，默认 `10`，最大 `50`。

错误响应：

- `400 DEVICE_ID_REQUIRED`: 缺少 `device_id`。
- `400 DEVICE_ID_INVALID`: `device_id` 超过 `128` 个字符。

配置：

```dotenv
COMMAND_DISPATCH_TIMEOUT_MS=60000
```

响应：

```json
{
  "ok": true,
  "commands": [
    {
      "command_id": "uuid",
      "device_id": "esp32-c5-001",
      "name": "voice.set_volume",
      "payload": {
        "volume": 35
      },
      "status": "dispatched",
      "created_at": "2026-06-07T00:00:00.000Z",
      "dispatched_at": "2026-06-07T00:00:01.000Z"
    }
  ],
  "server_time_ms": 1780000000000
}
```

### `POST /api/commands/:command_id/ack`

ESP 执行命令后回传结果。该接口只记录回执，不由 Server 判断风险或代替 ESP 执行动作。
`status` 只接受 `completed` 或 `failed`；其他值返回 HTTP `400`，不会把命令误标为完成。
该接口支持统一设备协议 v1 header metadata，并会刷新 `device_status` 与 `device_module_status(module_type="command.ack")`。

成功回执：

```json
{
  "status": "completed",
  "result": {
    "applied": true
  }
}
```

失败回执：

```json
{
  "status": "failed",
  "error_code": "DEVICE_REJECTED",
  "error_message": "volume output is disabled",
  "result": {
    "applied": false
  }
}
```

非法状态响应返回 HTTP `400`：

```json
{
  "ok": false,
  "code": "COMMAND_ACK_STATUS_INVALID",
  "error": "status must be completed or failed"
}
```

命令不存在或已经完成后再次回执会返回 HTTP `404`：

```json
{
  "ok": false,
  "code": "COMMAND_ACK_NOT_ACCEPTED",
  "error": "command not found or already completed",
  "command_id": "uuid"
}
```

### `GET /api/commands/history`

读取命令历史。该接口面向后端调试和未来 Dashboard API，当前不修改 Dashboard 前端。

查询参数：

- `device_id`: 可选，会 trim 首尾空白，最大 `128` 个字符。
- `limit`: 可选，默认 `50`，最大 `200`。

错误响应：

- `400 DEVICE_ID_INVALID`: `device_id` 超过 `128` 个字符。

### `POST /api/commands/v1/natural-language`

创建一条自然语言命令记录。该接口只保存用户自然语言意图，写入 `natural_language_commands` 并记录 `command_created` 事件；当前不会自动解析成设备控制命令，也不会直接下发到 ESP。

请求体：

```json
{
  "text": "把客厅灯打开",
  "source": "dashboard",
  "room_id": "living_room",
  "device_id": "sensair_shuttle_01"
}
```

成功响应返回 HTTP `202`，使用通用 API envelope：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "command": {
      "command_id": "nlcmd_uuid",
      "type": "natural_language",
      "text": "把客厅灯打开",
      "source": "dashboard",
      "room_id": "living_room",
      "device_id": "sensair_shuttle_01",
      "status": "queued",
      "parsed_intent": null,
      "created_at_ms": 1780000000000,
      "updated_at_ms": 1780000000000
    }
  },
  "error": null
}
```

错误响应：

- `400 NATURAL_LANGUAGE_TEXT_REQUIRED`: 缺少 `text` 或 trim 后为空。

### `GET /api/commands/v1/recent`

读取最近自然语言命令记录，按创建顺序倒序返回。

查询参数：

- `limit`: 可选，默认 `50`，最大 `200`。

成功响应：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "commands": [
      {
        "command_id": "nlcmd_uuid",
        "type": "natural_language",
        "text": "把客厅灯打开",
        "source": "dashboard",
        "room_id": "living_room",
        "device_id": "sensair_shuttle_01",
        "status": "queued",
        "parsed_intent": null,
        "created_at_ms": 1780000000000,
        "updated_at_ms": 1780000000000
      }
    ]
  },
  "error": null
}
```

## 智能家居接口

本节接口用于 Server 记录外部智能家居网关状态、排队控制命令，并供 S3 或其他网关拉取执行。当前后端只维护状态、命令队列和事件日志，不代表真实智能家居云平台已经接入。

本节接口使用通用 API envelope：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {},
  "error": null
}
```

失败时 `data=null`，`error.code` 是稳定错误码，`error.message` 是可读说明。

### `GET /api/smart-home/v1/status`

读取 Server 当前保存的智能家居设备状态。

成功响应：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "available": true,
    "configured": true,
    "provider": "s3_gateway",
    "last_update_ms": 1780000000000,
    "devices": [
      {
        "id": "light_001",
        "type": "light",
        "name": "客厅灯",
        "room_id": "living_room",
        "room_name": "客厅",
        "online": true,
        "state": {
          "power": true,
          "brightness": 80
        },
        "updated_at_ms": 1780000000000
      }
    ]
  },
  "error": null
}
```

`provider` 当前归一化为 `local`、`s3_gateway` 或 `none`；设备类型归一化为 `air_conditioner`、`light`、`fan`、`tv`、`curtain` 或 `unknown`。

### `POST /api/smart-home/v1/state`

S3 或其他网关批量上报智能家居设备状态。服务器会按 `device_id` upsert `smart_home_devices`，并写入 `smart_home_state_updated` 系统事件。

请求体：

```json
{
  "provider": "s3_gateway",
  "gateway_id": "sensair_s3_gateway_01",
  "devices": [
    {
      "id": "light_001",
      "type": "light",
      "name": "客厅灯",
      "room_id": "living_room",
      "room_name": "客厅",
      "online": true,
      "state": {
        "power": true,
        "brightness": 80
      }
    }
  ]
}
```

成功响应返回 HTTP `202`，`data.devices[]` 是实际归一化并保存的设备数组。

错误响应：

- `400 SMART_HOME_DEVICES_REQUIRED`: `devices` 不是非空数组。
- `400 SMART_HOME_DEVICE_ID_REQUIRED`: 某个设备缺少 `id` 或 `device_id`。

### `POST /api/smart-home/v1/control`

创建一条智能家居控制命令，等待网关通过 pending 接口拉取。该接口不直接执行设备动作。

请求体：

```json
{
  "target_id": "light_001",
  "gateway_id": "sensair_s3_gateway_01",
  "room_id": "living_room",
  "room_name": "客厅",
  "action": "set_power",
  "params": {
    "power": true
  },
  "source": "dashboard",
  "requested_by": "user"
}
```

成功响应返回 HTTP `202`：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "command": {
      "command_id": "shcmd_uuid",
      "target_id": "light_001",
      "gateway_id": "sensair_s3_gateway_01",
      "room_id": "living_room",
      "room_name": "客厅",
      "action": "set_power",
      "params": {
        "power": true
      },
      "source": "dashboard",
      "requested_by": "user",
      "status": "queued",
      "created_at_ms": 1780000000000,
      "updated_at_ms": 1780000000000
    },
    "message": "queued; waiting for gateway pull"
  },
  "error": null
}
```

错误响应：

- `400 SMART_HOME_TARGET_REQUIRED`: 缺少 `target_id` 或 `device_id`。
- `400 SMART_HOME_ACTION_REQUIRED`: 缺少 `action`。

### `GET /api/smart-home/v1/commands`

读取智能家居命令历史，默认按最新记录倒序返回。

查询参数：

- `limit`: 可选，默认 `50`，最大 `200`。

响应 `data.commands[]` 字段包括 `command_id`、`target_id`、`gateway_id`、`action`、`params`、`status`、`result`、`error_message`、`created_at_ms`、`updated_at_ms`、`acknowledged_at_ms` 和 `executed_at_ms`。

### `GET /api/smart-home/v1/commands/pending`

网关拉取待执行智能家居命令。返回前服务器会把命令从 `queued` 标记为 `dispatched`；如果命令原本没有 `gateway_id`，会绑定本次请求的 `gateway_id`。

查询参数：

- `gateway_id`: 可选，网关 ID；为空时只领取未绑定网关的 queued 命令。
- `limit`: 可选，默认 `20`，最大 `200`。

成功响应：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "commands": [
      {
        "command_id": "shcmd_uuid",
        "target_id": "light_001",
        "gateway_id": "sensair_s3_gateway_01",
        "action": "set_power",
        "params": {
          "power": true
        },
        "status": "dispatched",
        "created_at_ms": 1780000000000,
        "updated_at_ms": 1780000000100
      }
    ]
  },
  "error": null
}
```

### `POST /api/smart-home/v1/commands/:command_id/ack`

网关执行智能家居命令后回传结果。`status="completed"` 会兼容归一化为 `success`；最终只接受 `success` 或 `failed`。

请求体：

```json
{
  "status": "success",
  "executed_at_ms": 1780000000200,
  "result": {
    "applied": true
  },
  "error_message": ""
}
```

成功响应：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "command": {
      "command_id": "shcmd_uuid",
      "target_id": "light_001",
      "status": "success",
      "result": {
        "applied": true
      },
      "acknowledged_at_ms": 1780000000300,
      "executed_at_ms": 1780000000200
    }
  },
  "error": null
}
```

错误响应：

- `400 SMART_HOME_COMMAND_ID_REQUIRED`: 缺少 `command_id`。
- `400 SMART_HOME_COMMAND_ACK_STATUS_INVALID`: `status` 不是 `success`、`completed` 或 `failed`。
- `404 SMART_HOME_COMMAND_NOT_FOUND`: 命令不存在。

## 长期记忆接口

### `POST /api/conversation/turns`

保存一条对话 turn。该接口是长期记忆的原始输入记录，不代表该 turn 已经进入长期画像。

请求体：

```json
{
  "turn_id": "可选，客户端自带去重ID",
  "session_id": "session-001",
  "device_id": "esp32-c5-001",
  "role": "assistant",
  "input_text": "用户输入",
  "response_text": "助手回复",
  "structured": {
    "parsed": true
  },
  "command_ids": [
    "uuid"
  ],
  "memory_level": "episodic",
  "importance": 2,
  "source": "voice_turn"
}
```

字段说明：

- `turn_id`: 可选；如果不传，Server 自动生成。客户端指定的 `turn_id` 必须唯一，最大 `80` 个字符。
- `session_id`: 可选，会 trim 首尾空白，最大 `128` 个字符。
- `device_id`: 可选，会 trim 首尾空白，最大 `128` 个字符。
- `role`: 可选，默认 `user`，最大 `40` 个字符。
- `source`: 可选，默认 `api`，最大 `80` 个字符。
- `input_text` 和 `response_text` 至少一个不能为空。
- `memory_level`: 可为 `volatile`、`episodic`、`important`、`profile_candidate`、`archived`。
- `importance`: `0` 到 `5` 的整数。

错误响应：

- `400`: `input_text` 和 `response_text` 均为空。
- `400 CONVERSATION_TURN_ID_INVALID`: `turn_id` 超过 `80` 个字符。
- `400 CONVERSATION_ROLE_INVALID`: `role` 超过 `40` 个字符。
- `400 CONVERSATION_SOURCE_INVALID`: `source` 超过 `80` 个字符。
- `400 DEVICE_ID_INVALID`: `device_id` 超过 `128` 个字符。
- `400 SESSION_ID_INVALID`: `session_id` 超过 `128` 个字符。
- `409 CONVERSATION_TURN_ID_DUPLICATE`: 请求体指定的 `turn_id` 已存在。

### `GET /api/conversation/turns`

读取对话 turn。

查询参数：

- `session_id`: 可选，会 trim 首尾空白，最大 `128` 个字符。
- `device_id`: 可选，会 trim 首尾空白，最大 `128` 个字符。
- `limit`: 可选，默认 `50`，最大 `200`。

错误响应：

- `400 DEVICE_ID_INVALID`: `device_id` 超过 `128` 个字符。
- `400 SESSION_ID_INVALID`: `session_id` 超过 `128` 个字符。

### `POST /api/memory/daily`

写入每日摘要候选。摘要内容必须来自 LLM 输出、明确用户输入或人工确认；Server 不自行推理总结。

请求体：

```json
{
  "memory_date": "2026-06-07",
  "summary": "今天的交互摘要候选",
  "status": "candidate",
  "confidence": 0.6,
  "source": "llm_summary"
}
```

字段说明：

- `memory_date` 或 `date`: 必填，必须是有效日历日期，格式 `YYYY-MM-DD`。
- `status`: 可为 `candidate`、`active`、`rejected`、`archived`；非法值会按 `candidate` 写入。
- `confidence`: `0` 到 `1`；超出范围会被夹到边界。

错误响应：

- `400 DAILY_MEMORY_DATE_INVALID`: `memory_date` 或 `date` 不是有效的 `YYYY-MM-DD` 日历日期。

### `GET /api/memory/daily`

读取每日摘要候选。

查询参数：

- `memory_date` 或 `date`: 可选，必须是有效日历日期，格式 `YYYY-MM-DD`。
- `limit`: 可选，默认 `50`，最大 `200`。

错误响应：

- `400 DAILY_MEMORY_DATE_INVALID`: `memory_date` 或 `date` 不是有效的 `YYYY-MM-DD` 日历日期。

### `POST /api/memory/profile`

写入或更新长期画像候选。该接口只保存候选或已审核画像，不由 Server 自行生成画像结论。

请求体：

```json
{
  "profile_key": "user.prefers_quiet_volume",
  "profile_value": "用户偏好较低音量",
  "category": "user",
  "status": "candidate",
  "confidence": 0.5,
  "evidence": [
    {
      "turn_id": "turn_uuid"
    }
  ],
  "source": "weekly_profile"
}
```

字段说明：

- `profile_key`: 画像键，必填，最大 `120` 个字符。
- `category`: 可选，默认 `user`，最大 `80` 个字符。
- `status`: 可为 `candidate`、`active`、`rejected`、`archived`。
- `confidence`: `0` 到 `1`。

错误响应：

- `400 PROFILE_KEY_INVALID`: `profile_key` 超过 `120` 个字符。
- `400 PROFILE_CATEGORY_INVALID`: `category` 超过 `80` 个字符。

### `GET /api/memory/profile`

读取长期画像候选或有效画像。

查询参数：

- `status`: 可选。
- `category`: 可选，会 trim 首尾空白，最大 `80` 个字符。
- `limit`: 可选，默认 `50`，最大 `200`。

错误响应：

- `400 PROFILE_STATUS_INVALID`: `status` 不是 `candidate`、`active`、`rejected` 或 `archived`。
- `400 PROFILE_CATEGORY_INVALID`: `category` 超过 `80` 个字符。

### `POST /api/memory/corrections`

用户纠错入口。纠错会写入 `memory_corrections`；当 `target_type=profile` 时，会把目标画像重新标记为 `candidate` 并增加纠错次数。

请求体：

```json
{
  "target_type": "profile",
  "target_id": "user.prefers_quiet_volume",
  "correction_text": "我不是一直喜欢低音量，只是晚上需要低音量。",
  "corrected_value": "用户晚上偏好较低音量",
  "device_id": "esp32-c5-001",
  "session_id": "session-001"
}
```

字段说明：

- `target_type`: 目标类型，必填，最大 `40` 个字符；当前 `profile` 会触发画像候选回写。
- `target_id`: 目标记录 ID 或画像键，最大 `120` 个字符。
- `device_id`: 可选，会 trim 首尾空白，最大 `128` 个字符。
- `session_id`: 可选，会 trim 首尾空白，最大 `128` 个字符。
- `status`: 可为 `applied`、`pending`、`rejected`、`archived`；非法值会按 `applied` 写入。

错误响应：

- `400 MEMORY_CORRECTION_TARGET_TYPE_INVALID`: `target_type` 超过 `40` 个字符。
- `400 MEMORY_CORRECTION_TARGET_ID_INVALID`: `target_id` 超过 `120` 个字符。
- `400 DEVICE_ID_INVALID`: `device_id` 超过 `128` 个字符。
- `400 SESSION_ID_INVALID`: `session_id` 超过 `128` 个字符。

### `POST /api/jobs/daily-summary/run`

运行每日总结聚合。服务器会读取当天未 soft-deleted 的 `sensor_records`、`device_status`、`device_module_status`、`voice_turns`、`command_queue`、`conversation_turns` 和 `memory_corrections`，生成结构化统计、evidence 和候选摘要，默认写入 `daily_memory(memory_type="daily_summary")` 与 `memory_job_runs(status="completed")`。

请求体：

```json
{
  "date": "2026-06-07",
  "force": true,
  "dry_run": false,
  "summary": "可选人工摘要覆盖"
}
```

字段说明：

- `date` 或 `target_date`: 可选；不传时使用服务器当天日期。传入时必须是有效日历日期，格式 `YYYY-MM-DD`。
- `force`: 可选，默认 `false`；为 `false` 且当天已有未删除 daily summary 时返回 `skipped`，不重复写入。
- `dry_run`: 可选，默认 `false`；为 `true` 时只返回聚合结果，不写 `daily_memory` 或 `memory_job_runs`。
- `summary`: 可选；传入时使用该文本作为候选摘要，否则服务器根据统计结果生成摘要。

成功响应示例：

```json
{
  "ok": true,
  "status": "completed",
  "job_id": "job_uuid",
  "memory_id": 12,
  "memory_date": "2026-06-07",
  "memory_type": "daily_summary",
  "summary": "2026-06-07 daily summary: sensor samples 10; ...",
  "stats": {
    "sensor_records": {
      "count": 10,
      "avg_temperature": 26.4
    },
    "command_queue": {
      "count": 2,
      "status_counts": {
        "completed": 1,
        "failed": 1
      }
    }
  },
  "sample_count": 18,
  "server_time_ms": 1780000000000
}
```

错误响应：

- `400 DAILY_SUMMARY_DATE_INVALID`: `date` 或 `target_date` 不是有效的 `YYYY-MM-DD` 日历日期。

### `POST /api/jobs/weekly-profile/run`

运行每周总结与候选画像聚合。服务器会读取最近 7 天未删除的 `daily_memory(memory_type="daily_summary")` 与必要原始数据，写入 `daily_memory(memory_type="weekly_summary")`，并生成 `long_term_profile`、`environment_profile`、`experience_memory`、`relation_memory` 候选记录。候选记录默认 `candidate`，不会直接成为 `active`。

请求体：

```json
{
  "week_end": "2026-06-07",
  "force": true,
  "dry_run": false
}
```

字段说明：

- `week_end`、`date` 或 `target_date`: 可选；表示最近 7 天窗口的结束日期，不传时使用服务器当天日期。传入时必须是有效日历日期，格式 `YYYY-MM-DD`。
- `week_start`: 兼容旧字段；如果只传 `week_start`，服务器按 `week_start + 6 天` 计算 `week_end`。传入时必须是有效日历日期，格式 `YYYY-MM-DD`。
- `force`: 可选，默认 `false`；为 `false` 且该周已有未删除 weekly summary 时返回 `skipped`。
- `dry_run`: 可选，默认 `false`；为 `true` 时只返回统计和候选摘要，不写库。

成功响应包含：

- `memory_type="weekly_summary"` 的候选 summary。
- `stats.daily_memory.count`、`stats.sensor_records.count`、`stats.voice_turns.status_counts` 等聚合统计。
- `candidates.profile`、`candidates.environment`、`candidates.experience`、`candidates.relation` 的候选写入结果。

错误响应：

- `400 WEEKLY_PROFILE_DATE_INVALID`: 日期字段不是有效的 `YYYY-MM-DD` 日历日期。

### `GET /api/jobs/memory`

读取记忆相关任务记录。

查询参数：

- `job_name`: 可选，例如 `daily_summary` 或 `weekly_profile`，会 trim 首尾空白，最大 `80` 个字符。
- `target_date`: 可选，必须是有效日历日期，格式 `YYYY-MM-DD`。
- `limit`: 可选，默认 `50`，最大 `200`。

响应中的 `completed_at` 仅当任务状态为 `completed`、`failed` 或 `skipped` 时有值；`queued` 和 `running` 保持为空。

错误响应：

- `400 MEMORY_JOB_NAME_INVALID`: `job_name` 超过 `80` 个字符。
- `400 MEMORY_JOB_TARGET_DATE_INVALID`: `target_date` 不是有效的 `YYYY-MM-DD` 日历日期。

## 用户数据管理接口

本节接口用于正式的数据概览、删除预检查、执行删除和删除审计查询。接口不修改 Dashboard 前端，不删除 SQLite schema、索引、表结构、`sqlite_sequence`、`device_capabilities`、命令白名单或基础配置。

权限要求：

- 必须在服务器环境变量中配置 `USER_DATA_DELETE_TOKEN` 或 `ADMIN_TOKEN`。
- 请求必须携带 `X-Admin-Token`、`X-User-Data-Token`，或 `Authorization: Bearer <token>`。
- 未配置 token 时返回 `503 USER_DATA_ADMIN_TOKEN_NOT_CONFIGURED`；token 缺失或错误时返回 `401 USER_DATA_ADMIN_REQUIRED`。

支持的 `scope`：

- `summaries`: `daily_memory` 中 `daily_summary` 和 `weekly_summary`。
- `profiles`: `long_term_profile`、`environment_profile`、`experience_memory`、`relation_memory`。
- `memory`: `summaries`、`profiles`、`memory_corrections`。
- `conversations`: `conversation_turns`、`asr_records`、`llm_records`。
- `device_history`: `sensor_records`、`voice_turns`、`command_queue`、`device_status`、`device_module_status`、`csi_behavior_events`、`lcd_status`、`reminder_rules`、`reminder_records`、`emergency_events`。
- `jobs`: `memory_job_runs`。
- `all_user_data`: `memory`、`conversations`、`device_history`、`jobs`，不包含 `device_capabilities` 或系统结构。

删除模式：

- `soft_delete`: 对 scope 内所有尚未 soft-deleted 的匹配行设置 `deleted_at`、`delete_reason`、`updated_at`；不会改写 `voice_turns.status`、`command_queue.status`、记忆/画像 `status` 或其他业务状态字段。
- `hard_delete`: 执行 SQL `DELETE` 删除 scope 内匹配行，包括已 soft-deleted 的行；不会默认执行 `VACUUM`。

危险操作保护：

- `POST /api/user-data/delete` 必须传 `confirm="DELETE"`。
- preview 只统计并写一条 `data_deletion_runs(request_type="preview")` 审计记录，不修改任何业务数据。
- delete 会先统计 affected counts，再在事务内执行删除并写 `data_deletion_runs(request_type="delete")`。
- 删除失败会回滚业务变更，并保留一条 `status="failed"` 的删除审计记录。

### `GET /api/user-data/summary`

返回每个 scope 的当前未删除数据概览。

响应示例：

```json
{
  "ok": true,
  "scopes": [
    {
      "scope": "device_history",
      "display_name": "设备运行数据",
      "description": "传感器、语音 turn、命令队列、设备状态、提醒和预留事件记录",
      "count": 1200,
      "date_range": {
        "from": "2026-06-01T00:00:00.000Z",
        "to": "2026-06-09T23:59:59.999Z"
      },
      "last_updated_at": "2026-06-09T20:00:00.000Z",
      "last_deleted_at": "",
      "supports_soft_delete": true,
      "supports_hard_delete": true,
      "danger_level": "high",
      "tables": [
        {
          "table": "sensor_records",
          "count": 1000
        }
      ]
    }
  ]
}
```

### `POST /api/user-data/delete/preview`

统计删除将影响的表和行数。该接口只写 `data_deletion_runs` 预览审计，不修改任何业务数据。

请求体：

```json
{
  "scope": "device_history",
  "mode": "soft_delete",
  "reason": "user_request",
  "requested_by": "dashboard_admin",
  "include_audit_logs": false
}
```

响应示例：

```json
{
  "ok": true,
  "run_id": "deletion_uuid",
  "scope": "device_history",
  "mode": "soft_delete",
  "request_type": "preview",
  "include_audit_logs": false,
  "danger_level": "high",
  "required_confirm": "DELETE",
  "protected_tables": [
    "sqlite_sequence",
    "device_capabilities"
  ],
  "affected_counts": {
    "sensor_records": 1000,
    "command_queue": 20
  }
}
```

错误响应：

- `400 USER_DATA_SCOPE_INVALID`: `scope` 不在允许列表。
- `400 USER_DATA_DELETE_MODE_INVALID`: `mode` 不是 `soft_delete` 或 `hard_delete`。
- `500 USER_DATA_PREVIEW_AUDIT_FAILED`: preview 审计记录写入失败；业务数据不会被修改。

### `POST /api/user-data/delete`

执行删除。该接口是危险操作，必须带管理员 token 和 `confirm="DELETE"`。

请求体：

```json
{
  "scope": "all_user_data",
  "mode": "hard_delete",
  "confirm": "DELETE",
  "reason": "user_request",
  "requested_by": "dashboard_admin",
  "include_audit_logs": false
}
```

成功响应：

```json
{
  "ok": true,
  "run_id": "deletion_uuid",
  "scope": "all_user_data",
  "mode": "hard_delete",
  "status": "completed",
  "affected_counts": {
    "sensor_records": 1000,
    "daily_memory": 8
  },
  "protected_tables": [
    "sqlite_sequence",
    "device_capabilities"
  ],
  "server_time_ms": 1780000000000
}
```

错误响应：

- `409 USER_DATA_DELETE_CONFIRM_REQUIRED`: `confirm` 不是 `DELETE`。
- `500 USER_DATA_DELETE_FAILED`: 删除事务失败；业务变更已回滚，失败审计会记录在 `data_deletion_runs`。

### `GET /api/user-data/deletion-runs`

读取删除审计记录，默认按 `started_at DESC` 返回最近记录。

查询参数：

- `scope`: 可选，按删除 scope 过滤。
- `mode`: 可选，`soft_delete` 或 `hard_delete`。
- `request_type`: 可选，`preview` 或 `delete`。
- `status`: 可选，例如 `completed`、`failed`。
- `include_deleted`: 可选，`true`/`1` 时包含已被 soft-deleted 的审计记录；默认只返回 `deleted_at IS NULL` 的审计记录。
- `limit`: 可选，默认 `50`，最大 `200`。

响应示例：

```json
{
  "ok": true,
  "runs": [
    {
      "run_id": "deletion_uuid",
      "scope": "device_history",
      "mode": "soft_delete",
      "request_type": "delete",
      "reason": "user_request",
      "requested_by": "dashboard_admin",
      "include_audit_logs": false,
      "preview_counts": {
        "sensor_records": 1000
      },
      "affected_counts": {
        "sensor_records": 1000
      },
      "started_at": "2026-06-09T20:00:00.000Z",
      "completed_at": "2026-06-09T20:00:00.100Z",
      "created_at": "2026-06-09T20:00:00.000Z",
      "status": "completed",
      "error_message": "",
      "deleted_at": ""
    }
  ]
}
```

### `GET /api/user-data/export`

当前预留，返回：

```json
{
  "ok": false,
  "code": "USER_DATA_EXPORT_RESERVED",
  "error": "user data export is reserved for a future backend phase"
}
```

## 事件日志与实时事件接口

本节接口用于 Dashboard、调试工具和后端任务读取告警、系统日志、语音事件，以及通过 SSE 订阅实时事件。事件记录写入 `event_logs`；`event_type` 归一化为 `alarm`、`system`、`command`、`voice`、`device` 或 `csi`，未知类型按 `system` 保存。

### `GET /api/logs/v1/alarms`

读取告警事件，按最新记录倒序返回。

查询参数：

- `limit`: 可选，默认 `50`，最大 `200`。

成功响应：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "alarms": [
      {
        "id": "alarm_uuid",
        "level": "warning",
        "source": "server",
        "device_id": "sensair_shuttle_01",
        "room_id": "living_room",
        "room_name": "客厅",
        "title": "空气质量提醒",
        "message": "空气质量下降",
        "payload": {},
        "created_at_ms": 1780000000000,
        "acknowledged": false
      }
    ]
  },
  "error": null
}
```

### `POST /api/logs/v1/alarms`

创建一条告警事件，写入 `event_logs(event_type="alarm")`，并通过 SSE 广播。

请求体：

```json
{
  "device_id": "sensair_shuttle_01",
  "level": "warning",
  "title": "空气质量提醒",
  "message": "空气质量下降",
  "room_id": "living_room",
  "room_name": "客厅",
  "acknowledged": false,
  "payload": {
    "air_quality_score": 42
  },
  "source": "dashboard"
}
```

成功响应返回 HTTP `201`，`data.alarm` 为告警展示对象。

### `GET /api/logs/v1/system`

读取系统类日志，包含 `system`、`device`、`csi`、`command` 和 `voice` 事件。

查询参数：

- `limit`: 可选，默认 `50`，最大 `200`。

成功响应：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "logs": [
      {
        "id": "system_uuid",
        "level": "info",
        "source": "server_startup",
        "message": "server started and database migrations ensured",
        "payload": {},
        "created_at_ms": 1780000000000
      }
    ]
  },
  "error": null
}
```

### `POST /api/logs/v1/system`

创建一条系统日志事件，写入 `event_logs(event_type="system")`，并通过 SSE 广播。

请求体：

```json
{
  "device_id": "sensair_shuttle_01",
  "level": "info",
  "message": "device connected",
  "payload": {
    "rssi": -58
  },
  "source": "s3_gateway"
}
```

成功响应返回 HTTP `201`，`data.log` 为系统日志展示对象。

### `GET /api/voice/v1/events`

读取语音事件日志，按最新记录倒序返回。该接口只读 `event_logs(event_type="voice")`，不读取或修改 `voice_turns` 原始诊断表。

查询参数：

- `limit`: 可选，默认 `50`，最大 `200`。

成功响应：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "events": [
      {
        "id": "voice_uuid",
        "event": "voice_event_created",
        "device_id": "sensair_shuttle_01",
        "message": "voice_turn_completed",
        "payload": {},
        "created_at_ms": 1780000000000
      }
    ]
  },
  "error": null
}
```

### `POST /api/logs/v1/cleanup`

清理指定类型的历史事件。默认会拒绝小于 1 小时的清理窗口，除非显式传 `force=true`。`dry_run=true` 时只统计，不删除。

请求体：

```json
{
  "types": [
    "system",
    "device",
    "csi"
  ],
  "older_than_ms": 604800000,
  "dry_run": true,
  "force": false
}
```

字段说明：

- `type`: 可选，单个事件类型；传 `"all"` 表示全部类型。
- `types`: 可选，事件类型数组；可包含 `"all"`。
- `older_than_ms`: 必填，正整数，表示删除早于当前时间减该窗口的事件。
- `dry_run`: 可选，`true` 时只返回将删除的数量。
- `force`: 可选，`older_than_ms < 3600000` 时必须为 `true`。

成功响应：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "deleted": {
      "system": 12
    },
    "dry_run": true
  },
  "error": null
}
```

错误响应：

- `400 LOG_CLEANUP_OLDER_THAN_INVALID`: `older_than_ms` 不是正数。
- `400 LOG_CLEANUP_WINDOW_TOO_RECENT`: 清理窗口小于 1 小时且未传 `force=true`。

### `DELETE /api/logs/v1/events`

按 query 参数执行事件删除。该接口会调用同一清理逻辑，并强制 `dry_run=false`。

查询参数：

- `type`: 可选，默认 `system`；可传 `all` 或单个事件类型。
- `older_than_ms`: 必填，正整数。
- `force`: 可选，允许小于 1 小时的清理窗口。

成功响应同 `POST /api/logs/v1/cleanup`，其中 `dry_run=false`。

### `GET /api/events/v1/stream`

实时事件 Server-Sent Events 订阅接口。

响应头：

```http
Content-Type: text/event-stream
Cache-Control: no-cache, no-transform
Connection: keep-alive
X-Accel-Buffering: no
```

连接建立后立即发送：

```text
event: connected
data: {"server_time_ms":1780000000000}
```

连接存活期间每 `15000ms` 发送一次心跳：

```text
event: ping
data: {"server_time_ms":1780000015000}
```

后端调用 `recordEvent`、智能家居命令创建/回执、日志清理等路径会广播对应事件名，例如 `alarm_created`、`system_log_created`、`command_created`、`voice_event_created`、`command_acknowledged`、`logs_cleaned`。事件数据统一为：

```json
{
  "server_time_ms": 1780000000000,
  "event": "command_created",
  "data": {}
}
```

## Agent 状态与未来接入接口

本节接口属于 P3 Server 侧基础设施。当前实现只做协议、API、数据库记录和命令队列，不修改 Dashboard 前端，不接入 CSI 固件，不实现 LCD 固件驱动，也不让 Server 替代 LLM 或 ESP 做风险决策。

### `POST /api/environment/profile`

写入或更新环境画像候选。环境画像结论必须来自 LLM 输出、明确设备摘要或人工确认；Server 只做保存。

请求体：

```json
{
  "profile_key": "room.night_noise_level",
  "profile_value": "夜间环境通常较安静",
  "device_id": "esp32-c5-001",
  "status": "candidate",
  "confidence": 0.6,
  "evidence": [
    {
      "source": "daily_memory",
      "id": 1
    }
  ],
  "source": "llm_environment_profile"
}
```

字段说明：

- `profile_key`: 环境画像键，必填，最大 `120` 个字符。
- `device_id`: 可选，会 trim 首尾空白，最大 `128` 个字符。

错误响应：

- `400 ENVIRONMENT_PROFILE_KEY_INVALID`: `profile_key` 超过 `120` 个字符。
- `400 DEVICE_ID_INVALID`: `device_id` 超过 `128` 个字符。

### `GET /api/environment/profile`

读取环境画像候选。

查询参数：

- `device_id`: 可选，会 trim 首尾空白，最大 `128` 个字符。
- `status`: 可选。
- `limit`: 可选，默认 `50`，最大 `200`。

错误响应：

- `400 DEVICE_ID_INVALID`: `device_id` 超过 `128` 个字符。
- `400 PROFILE_STATUS_INVALID`: `status` 不是 `candidate`、`active`、`rejected` 或 `archived`。

### `POST /api/memory/experience`

写入经验记忆候选，用于记录某类情境下的处理经验。

请求体：

```json
{
  "title": "夜间降低音量",
  "situation": "用户在夜间要求降低音量",
  "action": "排队 voice.set_volume 命令",
  "outcome": "用户确认音量合适",
  "status": "candidate",
  "confidence": 0.5,
  "evidence": [
    {
      "turn_id": "turn_uuid"
    }
  ],
  "source": "llm_experience"
}
```

错误响应：

- `400`: 缺少 `title`。
- `400 EXPERIENCE_TITLE_INVALID`: `title` 超过 `200` 个字符。
- `400 EXPERIENCE_ID_INVALID`: `experience_id` 超过 `120` 个字符。
- `409 EXPERIENCE_ID_DUPLICATE`: 请求体指定的 `experience_id` 已存在。

### `GET /api/memory/experience`

读取经验记忆候选。

查询参数：

- `status`: 可选。
- `limit`: 可选，默认 `50`，最大 `200`。

错误响应：

- `400 MEMORY_STATUS_INVALID`: `status` 不是 `candidate`、`active`、`rejected` 或 `archived`。

### `POST /api/memory/relation`

写入关系记忆候选，用于记录用户、地点、设备或其他对象之间的关系。

请求体：

```json
{
  "subject": "用户",
  "predicate": "常使用",
  "object": "书房设备",
  "relation_type": "device",
  "status": "candidate",
  "confidence": 0.5,
  "evidence": [
    {
      "turn_id": "turn_uuid"
    }
  ],
  "source": "llm_relation"
}
```

错误响应：

- `400`: 缺少 `subject`、`predicate` 或 `object`。
- `400 RELATION_SUBJECT_INVALID`: `subject` 超过 `200` 个字符。
- `400 RELATION_PREDICATE_INVALID`: `predicate` 超过 `120` 个字符。
- `400 RELATION_OBJECT_INVALID`: `object` 超过 `200` 个字符。
- `400 RELATION_ID_INVALID`: `relation_id` 超过 `120` 个字符。
- `400 RELATION_TYPE_INVALID`: `relation_type` 超过 `80` 个字符。
- `409 RELATION_ID_DUPLICATE`: 请求体指定的 `relation_id` 已存在。

### `GET /api/memory/relation`

读取关系记忆候选。

查询参数：

- `status`: 可选。
- `relation_type`: 可选，会 trim 首尾空白，最大 `80` 个字符。
- `limit`: 可选，默认 `50`，最大 `200`。

错误响应：

- `400 MEMORY_STATUS_INVALID`: `status` 不是 `candidate`、`active`、`rejected` 或 `archived`。
- `400 RELATION_TYPE_INVALID`: `relation_type` 超过 `80` 个字符。

### `POST /api/reminders/rules`

创建提醒规则记录。该接口只保存规则，不代表提醒调度器或前端 UI 已接入。

请求体：

```json
{
  "title": "开窗提醒",
  "message": "二氧化碳偏高时提醒用户通风",
  "rule": {
    "type": "sensor_threshold",
    "field": "co2",
    "gt": 1200
  },
  "channel": "voice",
  "status": "active",
  "next_run_at": "2026-06-07T21:00:00.000Z",
  "source": "api"
}
```

字段说明：

- `title`: 必填，最大 `200` 个字符。
- `message`: 必填，最大 `1000` 个字符。
- `channel`: 可选，默认 `voice`，最大 `40` 个字符。
- `next_run_at`: 可选，最大 `80` 个字符。
- `suppress_until`: 可选，最大 `80` 个字符。

错误响应：

- `400`: 缺少 `title` 或 `message`。
- `400 REMINDER_ID_INVALID`: `reminder_id` 超过 `120` 个字符。
- `400 REMINDER_TITLE_INVALID`: `title` 超过 `200` 个字符。
- `400 REMINDER_MESSAGE_INVALID`: `message` 超过 `1000` 个字符。
- `400 REMINDER_CHANNEL_INVALID`: `channel` 超过 `40` 个字符。
- `400 REMINDER_NEXT_RUN_AT_INVALID`: `next_run_at` 超过 `80` 个字符。
- `400 REMINDER_SUPPRESS_UNTIL_INVALID`: `suppress_until` 超过 `80` 个字符。
- `409 REMINDER_ID_DUPLICATE`: 请求体指定的 `reminder_id` 已存在。

### `GET /api/reminders/rules`

读取提醒规则。

查询参数：

- `status`: 可选。
- `limit`: 可选，默认 `50`，最大 `200`。

错误响应：

- `400 REMINDER_RULE_STATUS_INVALID`: `status` 不是 `active`、`paused` 或 `archived`。

### `POST /api/reminders/events`

创建提醒事件记录，用于后端任务或未来设备端消费。

请求体：

```json
{
  "reminder_id": "reminder_uuid",
  "message": "该通风了",
  "status": "pending",
  "due_at": "2026-06-07T21:00:00.000Z",
  "action": {
    "command": "alert.play_tone"
  }
}
```

字段说明：

- `reminder_id`: 可选，关联的提醒规则 ID，最大 `120` 个字符。
- `message`: 必填，最大 `1000` 个字符。
- `due_at`: 可选，最大 `80` 个字符。
- `status`: 可为 `pending`、`triggered`、`confirmed`、`canceled`、`suppressed`；非法值会按 `pending` 写入。
- `completed_at`: 仅当状态为 `confirmed` 或 `canceled` 时写入；其他状态保持为空。

错误响应：

- `400`: 缺少 `message`。
- `400 REMINDER_ID_INVALID`: `reminder_id` 超过 `120` 个字符。
- `400 REMINDER_EVENT_MESSAGE_INVALID`: `message` 超过 `1000` 个字符。
- `400 REMINDER_DUE_AT_INVALID`: `due_at` 超过 `80` 个字符。
- `400 REMINDER_EVENT_ID_INVALID`: `reminder_event_id` 超过 `120` 个字符。
- `409 REMINDER_EVENT_ID_DUPLICATE`: 请求体指定的 `reminder_event_id` 已存在。

### `GET /api/reminders/events`

读取提醒事件。

查询参数：

- `status`: 可选。
- `limit`: 可选，默认 `50`，最大 `200`。

错误响应：

- `400 REMINDER_EVENT_STATUS_INVALID`: `status` 不是 `pending`、`triggered`、`confirmed`、`canceled` 或 `suppressed`。

### `POST /api/emergency/events`

ESP 上报紧急事件。Server 只校验、记录、转发所需数据，不做风险判断，不替代 ESP 本地快速动作，也不替代 LLM 深度决策。

请求体：

```json
{
  "device_id": "esp32-c5-001",
  "event_type": "fall_detected",
  "severity": "critical",
  "local_action": "played_local_alarm",
  "payload": {
    "source": "esp_local_rule"
  },
  "llm_decision": null,
  "status": "received"
}
```

字段说明：

- `event_id`: 可选；如果不传，Server 自动生成。客户端指定的 `event_id` 必须唯一，最大 `120` 个字符。
- `event_type`: 必填，最大 `120` 个字符。
- `local_action`: 可选，ESP 本地动作审计文本，最大 `500` 个字符。
- `severity`: 可为 `info`、`warning`、`critical`；非法值会按 `info` 写入。
- `status`: 可为 `received`、`llm_pending`、`forwarded`、`resolved`、`archived`；非法值会按 `received` 写入。
- `resolved_at`: 仅当状态为 `resolved` 时写入；未解决事件保持为空。

错误响应：

- `400`: 缺少 `event_type`。
- `400 DEVICE_ID_INVALID`: `device_id` 超过 `128` 个字符。
- `400 EMERGENCY_EVENT_TYPE_INVALID`: `event_type` 超过 `120` 个字符。
- `400 EMERGENCY_LOCAL_ACTION_INVALID`: `local_action` 超过 `500` 个字符。
- `400 EMERGENCY_EVENT_ID_INVALID`: `event_id` 超过 `120` 个字符。
- `409 EMERGENCY_EVENT_ID_DUPLICATE`: 请求体指定的 `event_id` 已存在。

### `GET /api/emergency/events`

读取紧急事件记录。

查询参数：

- `device_id`: 可选，会 trim 首尾空白，最大 `128` 个字符。
- `status`: 可选。
- `limit`: 可选，默认 `50`，最大 `200`。

错误响应：

- `400 DEVICE_ID_INVALID`: `device_id` 超过 `128` 个字符。
- `400 EMERGENCY_STATUS_INVALID`: `status` 不是 `received`、`llm_pending`、`forwarded`、`resolved` 或 `archived`。

### `POST /api/csi/behavior`

预留 CSI 行为事件上传协议。当前只接收行为特征和摘要，不接收原始 CSI 数据，不修改 CSI 固件。

请求体：

```json
{
  "device_id": "csi-node-001",
  "behavior_type": "presence",
  "confidence": 0.72,
  "features": {
    "motion_score": 0.82
  },
  "summary": "检测到有人在房间内活动",
  "occurred_at": "2026-06-07T21:00:00.000Z"
}
```

字段说明：

- `summary`: 可选，行为摘要，最大 `1000` 个字符。
- `occurred_at`: 可选，最大 `80` 个字符。

错误响应：

- `400`: 缺少 `behavior_type`。
- `400 DEVICE_ID_INVALID`: `device_id` 超过 `128` 个字符。
- `400 CSI_BEHAVIOR_TYPE_INVALID`: `behavior_type` 超过 `120` 个字符。
- `400 CSI_SUMMARY_INVALID`: `summary` 超过 `1000` 个字符。
- `400 CSI_OCCURRED_AT_INVALID`: `occurred_at` 超过 `80` 个字符。
- `400 CSI_EVENT_ID_INVALID`: `event_id` 超过 `120` 个字符。
- `409 CSI_EVENT_ID_DUPLICATE`: 请求体指定的 `event_id` 已存在。

### `GET /api/csi/behavior`

读取 CSI 行为事件。

查询参数：

- `device_id`: 可选，会 trim 首尾空白，最大 `128` 个字符。
- `behavior_type`: 可选，会 trim 首尾空白，最大 `120` 个字符。
- `limit`: 可选，默认 `50`，最大 `200`。

错误响应：

- `400 DEVICE_ID_INVALID`: `device_id` 超过 `128` 个字符。
- `400 CSI_BEHAVIOR_TYPE_INVALID`: `behavior_type` 超过 `120` 个字符。

### `POST /api/lcd/status`

预留 LCD 状态上报协议。当前只保存 Server 侧状态，不修改 LCD 固件，不修改 Dashboard。

请求体：

```json
{
  "device_id": "esp32-c5-001",
  "page": "idle",
  "state": {
    "voice": "idle",
    "sensor": "ok"
  },
  "last_command_id": "可选"
}
```

字段说明：

- `device_id`: 必填，会 trim 首尾空白，最大 `128` 个字符。
- `page`: 可选，默认 `idle`，最大 `80` 个字符。
- `last_command_id`: 可选，最大 `120` 个字符。

错误响应：

- `400`: 缺少 `device_id`。
- `400 DEVICE_ID_INVALID`: `device_id` 超过 `128` 个字符。
- `400 LCD_PAGE_INVALID`: `page` 超过 `80` 个字符。
- `400 LCD_LAST_COMMAND_ID_INVALID`: `last_command_id` 超过 `120` 个字符。

### `GET /api/lcd/status`

读取 LCD 状态记录。

查询参数：

- `device_id`: 可选，会 trim 首尾空白，最大 `128` 个字符。
- `limit`: 可选，默认 `50`，最大 `200`。

错误响应：

- `400 DEVICE_ID_INVALID`: `device_id` 超过 `128` 个字符。

### `POST /api/lcd/display`

创建一条 `display.show_text` 命令并更新 Server 侧 LCD 状态。目标设备必须先通过 `POST /api/devices/capabilities` 注册 `display.show_text` 能力。`ttl_ms` 可选，未提供时 Server 会按固件默认值归一为 `5000`。

请求体：

```json
{
  "device_id": "esp32-c5-001",
  "text": "你好",
  "ttl_ms": 5000
}
```

错误响应复用命令队列错误格式：

- `400 COMMAND_TARGET_REQUIRED`: 缺少 `device_id`。
- `400 DEVICE_CAPABILITIES_REQUIRED`: 目标设备尚未注册能力。
- `400 DEVICE_COMMAND_UNSUPPORTED`: 目标设备未声明支持 `display.show_text`。
- `400 COMMAND_PAYLOAD_INVALID`: 缺少 `text`、`text` 超过 `120` 个字符，或 `ttl_ms` 超出 `1000` 到 `60000`。

## 统一设备协议 v1 接口

### 通用 v1 envelope

新 ESP 主链路使用 JSON envelope v1。`device_id` 表示整机，`sensor_id` 或 `module_id` 放在 `payload` 内。`server_recv_ms`、`server_time_iso`、`upload_delay_ms` 只由服务器生成或计算，客户端上传的同名字段会被忽略。

```json
{
  "schema_version": 1,
  "device_id": "esp32-c5-whole-001",
  "device_type": "esp32c5_env_voice_node",
  "firmware_version": "0.1.0",
  "request_seq": 123,
  "esp_uptime_ms": 12345678,
  "esp_time_ms": 1780732142207,
  "time_synced": true,
  "payload_type": "sensor.bme690",
  "payload": {}
}
```

`time_synced=false` 时，`esp_time_ms` 应省略或为 `null`，不要用 `0` 伪装真实 Unix 时间。只有 `time_synced=true` 且 `esp_time_ms` 有效、延迟在 `0..60000ms` 的样本才进入 `latest_upload_delay_ms`、`avg_upload_delay_ms` 和 `delay_sample_count`。

### `POST /api/device/v1/ingest`

统一设备协议 v1 ingest 入口。当前已接入 `payload_type=sensor.bme690` 和 `payload_type=csi.motion`。BME690 会写入 `sensor_records`；CSI 只刷新设备/模块状态并在内存中聚合 Dashboard occupancy，不写入 raw CSI 或 BME 传感器表。

BME690 请求体：

```json
{
  "schema_version": 1,
  "device_id": "esp32-c5-whole-001",
  "device_type": "esp32c5_env_voice_node",
  "firmware_version": "0.1.0",
  "request_seq": 123,
  "esp_uptime_ms": 12345678,
  "esp_time_ms": 1780732142207,
  "time_synced": true,
  "payload_type": "sensor.bme690",
  "payload": {
    "sensor_id": "bme690_01",
    "temperature_c": 29.57,
    "humidity_percent": 30.29,
    "pressure_hpa": 986.26,
    "gas_resistance_ohm": 35164,
    "air_quality_score": 72,
    "air_quality_level": "moderate",
    "air_quality_confidence": "low",
    "air_quality_algo_version": "esp-bme690-relative-v1",
    "air_quality_source": "esp",
    "gas_baseline_ohm": 82000,
    "gas_ratio": 0.43,
    "gas_score": 43,
    "humidity_score": 87,
    "baseline_ready": false,
    "warmup_done": false,
    "sample_count": 12
  }
}
```

成功响应：

```json
{
  "ok": true,
  "server_recv_ms": 1780732144669,
  "server_time_iso": "2026-06-09T20:10:44.669Z",
  "request_id": "",
  "error": null,
  "data": {
    "id": 1,
    "device_id": "esp32-c5-whole-001",
    "sensor_id": "bme690_01",
    "payload_type": "sensor.bme690",
    "server_recv_ms": 1780732144669,
    "server_time_iso": "2026-06-09T20:10:44.669Z",
    "upload_delay_ms": 2462,
    "air_quality": {
      "air_quality_score": 72,
      "air_quality_level": "moderate",
      "air_quality_confidence": "low",
      "air_quality_source": "esp"
    }
  }
}
```

失败响应：

```json
{
  "ok": false,
  "server_recv_ms": 1780732144669,
  "server_time_iso": "2026-06-09T20:10:44.669Z",
  "request_id": "",
  "data": null,
  "error": {
    "code": "INVALID_PAYLOAD",
    "message": "temperature_c is required"
  }
}
```

错误码：

- `UNSUPPORTED_PAYLOAD_TYPE`: 当前只接受 `sensor.bme690` 或 `csi.motion`。
- `INVALID_PAYLOAD`: envelope 或 BME690 必填字段缺失/非法。
- `INVALID_CSI_OCCUPANCY_STATE`: CSI `occupancy.state` 不是 `unknown`、`vacant` 或 `occupied`。
- `DEVICE_INGEST_FAILED`: 服务器处理失败。

服务器会把 `payload.temperature_c`、`humidity_percent`、`pressure_hpa`、`gas_resistance_ohm` 映射到旧 `sensor_records.temperature`、`humidity`、`pressure`、`gas_resistance`，同时保存 `metadata_json`、`raw_json`、`air_quality_json` 和空气状态拆分列。

CSI motion 请求体：

```json
{
  "schema_version": 1,
  "device_id": "sensair_shuttle_01",
  "device_type": "esp32c5_env_voice_node",
  "firmware_version": "0.1.0",
  "request_seq": 456,
  "esp_uptime_ms": 12345678,
  "payload_type": "csi.motion",
  "room_id": "living_room",
  "payload": {
    "occupancy": {
      "state": "occupied"
    },
    "motion_score": 0.73,
    "variance": 0.0182,
    "rssi": -58,
    "sample_count": 96,
    "updated_at": 1781100000000
  }
}
```

CSI 成功响应：

```json
{
  "ok": true,
  "server_recv_ms": 1781100000100,
  "server_time_iso": "2026-06-10T10:00:00.100Z",
  "request_id": "",
  "error": null,
  "data": {
    "device_id": "sensair_shuttle_01",
    "payload_type": "csi.motion",
    "occupancy": {
      "state": "occupied"
    },
    "motion_score": 0.73,
    "variance": 0.0182,
    "rssi": -58,
    "sample_count": 96,
    "updated_at": 1781100000000,
    "server_recv_ms": 1781100000100,
    "server_time_iso": "2026-06-10T10:00:00.100Z",
    "dashboard_recorded": true
  }
}
```

CSI 字段规则：

- `occupancy.state`: 只能是 `unknown`、`vacant`、`occupied`；CSI 不可用、样本不足或质量不足时用 `unknown`。
- `motion_score`: `0.0..1.0`；Server 会裁剪越界值；无有效窗口时可为 `null`。
- `variance`、`rssi`、`sample_count`、`updated_at`: 仅作轻量诊断和排序；不是 raw CSI。
- `updated_at`: C5/S3 提供的毫秒时间戳；缺失时 Server 用接收时间补齐。
- Server 不保存 I/Q、相位、子载波数组或 CSI 原始帧，不做动作识别、人数识别、呼吸率、心率、步态或深度学习推理。
- 每个 `device_id` 单独聚合最新 occupancy；C51/C52 使用不同 `device_id` 时结果不会互相覆盖。

### `POST /api/device/v1/gateway-state`

S3 网关上传 Dashboard 标准快照的入口。该接口只接收 `payload_type="gateway.dashboard_snapshot"`，不会改变 `POST /api/device/v1/ingest` 对 `sensor.bme690` 的 legacy 成功响应形状。

请求体：

```json
{
  "schema_version": 2,
  "payload_type": "gateway.dashboard_snapshot",
  "source": "s3_gateway",
  "gateway": {
    "gateway_id": "sensair_s3_gateway_01",
    "online": true,
    "softap_ready": true,
    "sta_connected": true,
    "server_available": true,
    "voice_busy": false,
    "last_error": "",
    "timestamp": 1781100000000
  },
  "devices": [
    {
      "device_id": "sensair_shuttle_01",
      "local_id": 1,
      "name": "SensaiShuttle",
      "room_name": "living_room",
      "online": true,
      "wifi_rssi": -58,
      "timestamp": 1781100000000,
      "sensors": {
        "temperature": 29.57,
        "humidity": 48.2,
        "pressure": 1008.6,
        "gas_resistance": 35164,
        "air_quality_score": 72,
        "air_quality_level": "moderate",
        "air_quality_source": "s3_mapped"
      },
      "occupancy": {
        "state": "occupied",
        "available": true,
        "motion_score": 0.73,
        "variance": 0.0182,
        "rssi": -58,
        "sample_count": 96,
        "updated_at": 1781100000000
      },
      "appliances": {
        "air_conditioner": {"power": false, "mode": "cool", "target_temperature": 26, "source": "mock", "mock": true},
        "fan": {"power": false, "speed": 0, "source": "mock", "mock": true},
        "light": {"power": true, "brightness": 60, "source": "mock", "mock": true},
        "tv": {"power": false, "source": "mock", "mock": true},
        "curtain": {"open_percent": 70, "source": "mock", "mock": true}
      }
    }
  ],
  "home_summary": {
    "online_device_count": 1,
    "offline_device_count": 0,
    "avg_temperature": 29.57,
    "avg_humidity": 48.2,
    "avg_air_quality": 72
  },
  "history": [],
  "recent_voice_events": [],
  "recent_commands": []
}
```

成功响应：

```json
{
  "ok": true,
  "server_recv_ms": 1781100000100,
  "server_time_iso": "2026-06-10T10:00:00.100Z",
  "request_id": "",
  "error": null,
  "data": {
    "payload_type": "gateway.dashboard_snapshot",
    "gateway_id": "sensair_s3_gateway_01",
    "device_count": 1,
    "history_count": 0,
    "received_at_ms": 1781100000100
  }
}
```

约束：

- `schema_version` 必须为 `2`。
- `devices[].appliances.*` 目前允许 S3 或 Server 使用 mock/fake 数据，但每个 mock 对象必须包含 `source:"mock"` 和 `mock:true`。
- `devices[].occupancy` 可由 S3 snapshot 提供；缺失或 `available=false` 时 Server 归一化为 `state:"unknown"`、`motion_score:null`。
- 服务器会把 snapshot 写入 `dashboard_snapshots`，并保留最近一次 snapshot 的内存副本；`/api/dashboard/v1/overview` 优先读取该快照。Server 重启后会先尝试从数据库恢复最近快照，数据库无快照时才回退到传感器历史和设备状态聚合。

错误码：

- `UNSUPPORTED_PAYLOAD_TYPE`: `payload_type` 不是 `gateway.dashboard_snapshot`。
- `INVALID_SCHEMA_VERSION`: `schema_version` 不是 `2`。
- `INVALID_DASHBOARD_SNAPSHOT`: snapshot 结构非法。
- `GATEWAY_STATE_INGEST_FAILED`: 服务器处理失败。

空气状态字段是 ESP 本地基于 BME690 的相对空气状态估算，不是国标 AQI，不代表 PM2.5、PM10 或 CO2。服务器只接收、校验、入库、fallback 和用于 context，不做风险判断或紧急决策。

### `GET /api/device/v1/status`

读取整机状态。`device_id` 可选；为空时返回最近一个设备状态。响应包含通用 API envelope 的 `data.devices[]`，并额外保留顶层 `status` 兼容旧调试调用。

```json
{
  "ok": true,
  "server_time_ms": 1780732145869,
  "data": {
    "devices": [
      {
        "device_id": "esp32-c5-whole-001",
        "online": true,
        "device_online": true,
        "last_seen_ms": 1780732144669,
        "last_seen_iso": "2026-06-09T20:10:44.669Z",
        "last_seen_age_ms": 1200,
        "last_payload_type": "sensor.bme690",
        "last_module_type": "sensor.bme690",
        "time_synced": true,
        "latest_upload_delay_ms": 2462,
        "avg_upload_delay_ms": 1800,
        "delay_sample_count": 3,
        "reboot_count": 0
      }
    ]
  },
  "error": null,
  "status": {
    "device_id": "esp32-c5-whole-001",
    "online": true,
    "device_online": true,
    "last_seen_ms": 1780732144669,
    "last_seen_iso": "2026-06-09T20:10:44.669Z",
    "last_seen_age_ms": 1200,
    "last_payload_type": "sensor.bme690",
    "last_module_type": "sensor.bme690",
    "time_synced": true,
    "latest_upload_delay_ms": 2462,
    "avg_upload_delay_ms": 1800,
    "delay_sample_count": 3,
    "reboot_count": 0
  }
}
```

### `GET /api/device/v1/status/:device_id`

读取指定设备状态，响应形状同 `GET /api/device/v1/status?device_id=...`。路径参数会 trim，最多保留 `128` 个字符；当前实现没有为超长路径 ID 单独返回 400，而是按截断后的 ID 查询。

### `GET /api/device/v1/modules/status`

读取模块状态。`device_id` 可选；为空时返回已知模块状态。

```json
{
  "ok": true,
  "modules": [
    {
      "device_id": "esp32-c5-whole-001",
      "module_type": "sensor.bme690",
      "online": true,
      "module_online": true,
      "last_seen_age_ms": 1200,
      "latest_upload_delay_ms": 2462,
      "avg_upload_delay_ms": 1800,
      "delay_sample_count": 3
    }
  ],
  "server_time_ms": 1780732145869
}
```

BME 模块离线不等于整机离线；整机在线由 `device_status` 判断，模块在线由 `device_module_status` 判断。

### `GET /api/device/v1/context`

读取 `deviceContextService` 输出，供 LLM prompt、调试和后续前端迁移使用。该接口不调用 LLM。

```json
{
  "ok": true,
  "context": {
    "device": {
      "device_id": "esp32-c5-whole-001",
      "online": true,
      "avg_upload_delay_ms": 1800
    },
    "modules": {
      "sensor.bme690": {
        "available": true,
        "online": true
      },
      "csi.motion": {
        "available": false,
        "online": false
      },
      "lcd.status": {
        "available": false,
        "online": false
      }
    },
    "environment": {
      "available": true,
      "fresh": true,
      "temperature_c": 29.57,
      "humidity_percent": 30.29,
      "pressure_hpa": 986.26,
      "gas_resistance_ohm": 35164
    },
    "air_quality": {
      "available": true,
      "score": 72,
      "level": "moderate",
      "confidence": "low",
      "source": "esp",
      "note": "ESP local BME690 relative estimate, not national AQI, PM2.5, PM10, or CO2."
    }
  },
  "server_time_ms": 1780732145869
}
```

### `GET /api/device/v1/sensors/latest`

读取最新 BME690 传感器记录，返回 v1 字段、legacy 映射字段、metadata、raw_json 和空气状态。

```json
{
  "ok": true,
  "sensor": {
    "id": 1,
    "temperature": 29.57,
    "humidity": 30.29,
    "pressure": 986.26,
    "gas_resistance": 35164,
    "device_id": "esp32-c5-whole-001",
    "payload_type": "sensor.bme690",
    "sensor_id": "bme690_01",
    "upload_delay_ms": 2462,
    "metadata": {},
    "raw_json": {},
    "air_quality": {
      "air_quality_score": 72,
      "air_quality_level": "moderate",
      "air_quality_confidence": "low",
      "air_quality_source": "esp"
    }
  },
  "server_time_ms": 1780732145869
}
```

### v1 数据库迁移概要

启动时会可重复创建或补齐：

- `sensor_records`: legacy 传感器列继续保留，新增 `schema_version`、`device_type`、`firmware_version`、`request_seq`、`time_synced`、`payload_type`、`sensor_id`、`metadata_json`、`raw_json`、`air_quality_json`、`air_quality_score`、`air_quality_level`、`air_quality_confidence`、`air_quality_algo_version`、`air_quality_source`、`gas_baseline_ohm`、`gas_ratio`、`gas_score`、`humidity_score`。
- `device_status`: `device_id` 唯一，保存整机最后通信、最后 payload/module、ESP 时间、时间同步、重启计数、最新/平均延迟和样本数。
- `device_module_status`: `(device_id,module_type)` 唯一，保存模块最后通信、ESP 时间、时间同步、最新/平均延迟和样本数。
- 索引：`idx_sensor_records_device_recv_ms`、`idx_sensor_records_recv_ms`、`idx_sensor_records_payload_type_recv_ms`、`idx_device_status_device_id_unique`、`idx_device_module_status_device_module_unique`、`idx_device_status_last_seen`、`idx_device_module_status_last_seen`。

## Dashboard v1 前端统一读取接口

`/api/dashboard/v1/*` 是 Dashboard 前端的统一读取层，稳定输出页面展示模型。它可以在后端内部复用 `sensor_records`、`asr_records`、`llm_records`、时间同步状态、`device_status` 和 `device_module_status`，但不把 ESP ingest envelope 或 legacy 响应形状暴露为前端主契约。

职责边界：

- `/api/dashboard/v1/*`: 面向 Dashboard 前端读取，使用统一 envelope、统一空数据规则和页面字段命名。
- `/api/device/v1/*`: 面向 ESP 设备协议、设备状态、设备上下文和调试，不作为 Dashboard 前端唯一读取层。
- `/sensor/*`、`/asr/*`、`/llm/*`、`/api/time/status`: legacy 兼容接口，继续保留给旧 Dashboard、旧脚本和旧设备；前端迁移完成后不应再作为新 Dashboard 主读取入口。

后续任何后端接口新增、删除、字段变更、错误格式变更或空数据规则变更，都必须同步更新本文档，再修改实现和调用方。

### Dashboard v1 统一 envelope

成功响应：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {},
  "error": null
}
```

失败响应：

```json
{
  "ok": false,
  "server_time_ms": 1780000000000,
  "data": null,
  "error": {
    "code": "DASHBOARD_BAD_LIMIT",
    "message": "limit must be a positive integer"
  }
}
```

统一规则：

- `ok`: 布尔值，表示本次 Dashboard v1 读取是否成功。
- `server_time_ms`: Server 生成响应时的 Unix 毫秒时间戳；前端可用于延迟、数据新鲜度和本地时钟偏差展示。
- `data`: 成功时为接口数据；`latest` 类无记录返回 `null`；`history` 类无记录返回 `[]`；状态类返回状态对象。
- `error`: 成功时为 `null`；失败时包含稳定 `code` 和面向人类可读的 `message`。前端判断必须依赖 `error.code`，不要解析 `message`。
- Dashboard v1 失败时使用对应 HTTP 状态码；legacy 接口不会被改成这个 envelope。

### Dashboard v1 传感器字段表

`sensors/latest` 和 `sensors/history` 共享下列前端可读取字段。`gas_resistance` 是 BME690 气体阻值，单位 `Ω`，必须和空气质量字段分开展示；`air_quality_*` 是基于 BME690 的相对空气质量估算，不是国标 AQI，不代表 PM2.5、PM10 或 CO2。

| 字段名 | 类型 | 单位 | 前端显示名 | 含义 | 空值规则 |
| --- | --- | --- | --- | --- | --- |
| `id` | number | 无 | 记录 ID | `sensor_records.id`。 | 无记录时整条 `data=null`；历史数组中不应为空。 |
| `timestamp` | number/null | ms | 记录时间 | 传感器记录时间戳。 | 来源为空或非法时为 `null`。 |
| `temperature` | number/null | ℃ | 温度 | BME690 温度。 | 未上报或非法时为 `null`。 |
| `humidity` | number/null | % | 湿度 | BME690 相对湿度。 | 未上报或非法时为 `null`。 |
| `pressure` | number/null | hPa | 气压 | BME690 气压。 | 未上报或非法时为 `null`。 |
| `gas_resistance` | number/null | Ω | 气体阻值 | BME690 气体阻值，独立字段，不被 `air_quality_score` 或 `air_quality` 覆盖。 | 未上报或非法时为 `null`。 |
| `air_quality_score` | number/null | 分 | 空气质量分数 | ESP 或 Server fallback 生成的相对估算分数。 | 没有可用估算时为 `null`。 |
| `air_quality_level` | string/null | 无 | 空气质量等级 | `excellent/good/moderate/poor/bad/unknown` 等相对等级。 | 未知时为 `null`。 |
| `air_quality_confidence` | string/null | 无 | 置信度 | `none/low/medium/high`。 | 未知时为 `null`。 |
| `air_quality_source` | string/null | 无 | 空气质量来源 | 例如 `esp` 或 `server_fallback`。 | 未知时为 `null`。 |
| `air_quality` | object | 无 | 空气质量对象 | 包含 `air_quality_score/level/confidence/source` 的对象，供前端集中读取。 | 字段未知时对象内对应值为 `null`。 |
| `online` | boolean | 无 | 传感器在线 | Dashboard 聚合在线状态，当前为整机在线且 BME690 模块在线。 | 无状态记录时为 `false`。 |
| `device_online` | boolean | 无 | 整机在线 | 来自 `device_status` 的整机在线判断。 | 无整机状态时为 `false`。 |
| `sensor_online` | boolean | 无 | 传感器在线 | 来自 `device_module_status(module_type="sensor.bme690")`。 | 无模块状态时为 `false`。 |
| `latest_upload_delay_ms` | number/null | ms | 最近上传延迟 | 优先来自 BME690 模块状态，再回退整机状态或记录自身上传延迟。 | 无有效延迟样本时为 `null`。 |
| `avg_upload_delay_ms` | number/null | ms | 平均上传延迟 | 优先来自 BME690 模块状态，再回退整机状态。 | 无有效延迟样本时为 `null`。 |
| `delay_sample_count` | number | 个 | 延迟样本数 | 参与平均延迟计算的样本数。 | 无样本时为 `0`。 |
| `time_sync` | object/undefined | 无 | 时间同步状态 | `sensors/latest` 包含，与 `GET /api/dashboard/v1/time/status` 的 `time_sync` 同源。 | `history` 单条记录不返回该字段。 |

### `GET /api/dashboard/v1/overview`

Dashboard 首屏聚合读取接口。收到 S3 `gateway.dashboard_snapshot` 后，该接口优先返回 Server 保存的最新标准快照；快照会持久化到 `dashboard_snapshots`，服务重启后可从数据库恢复。尚未收到或恢复到快照时，后端会用现有 `sensor_records/device_status` 生成同形状兜底数据。某个子读取失败时，整体返回 `ok=false` 和 `DASHBOARD_OVERVIEW_READ_FAILED`。

请求参数：

- `device_id`: 可选，限制 `devices`、`history`、`recent_voice_events` 和 `recent_commands` 的设备 ID；服务器 trim，最多 `128` 个字符。

成功响应示例：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "gateway": {
      "gateway_id": "sensair_s3_gateway_01",
      "online": true,
      "softap_ready": true,
      "sta_connected": true,
      "server_available": true,
      "voice_busy": false,
      "last_error": "",
      "timestamp": 1780000000000
    },
    "devices": [
      {
        "device_id": "sensair_shuttle_01",
        "local_id": 1,
        "name": "SensaiShuttle",
        "room_name": "living_room",
        "online": true,
        "wifi_rssi": -58,
        "timestamp": 1780000000000,
        "sensors": {
          "temperature": 29.57,
          "humidity": 48.2,
          "pressure": 1008.6,
          "gas_resistance": 35164,
          "air_quality_score": 72,
          "air_quality_level": "moderate",
          "air_quality_source": "s3_mapped"
        },
        "occupancy": {
          "state": "occupied",
          "available": true,
          "motion_score": 0.73,
          "variance": 0.0182,
          "rssi": -58,
          "sample_count": 96,
          "updated_at": 1781100000000
        },
        "appliances": {
          "air_conditioner": {"power": false, "mode": "cool", "target_temperature": 26, "source": "mock", "mock": true},
          "fan": {"power": false, "speed": 0, "source": "mock", "mock": true},
          "light": {"power": true, "brightness": 60, "source": "mock", "mock": true},
          "tv": {"power": false, "source": "mock", "mock": true},
          "curtain": {"open_percent": 70, "source": "mock", "mock": true}
        }
      }
    ],
    "home_summary": {
      "online_device_count": 1,
      "offline_device_count": 0,
      "avg_temperature": 29.57,
      "avg_humidity": 48.2,
      "avg_air_quality": 72
    },
    "history": [],
    "recent_voice_events": [],
    "recent_commands": []
  },
  "error": null
}
```

前端可读取字段表：

| 字段名 | 类型 | 单位 | 前端显示名 | 含义 | 空值规则 |
| --- | --- | --- | --- | --- | --- |
| `gateway` | object | 无 | S3 网关状态 | S3 SoftAP、STA、Server 可用性、voice busy 和最近错误。 | 无快照兜底时返回默认 gateway 对象。 |
| `devices` | array | 无 | 设备列表 | 每个 C5 的标准 device model，包含 `sensors` 和 `appliances`。 | 无设备时 `[]`。 |
| `devices[].occupancy.state` | string | 无 | CSI 占用状态 | `unknown/vacant/occupied`，由 S3 snapshot 或 `csi.motion` ingest 聚合得到。 | 无 CSI、CSI 开关关闭、样本不足或设备不可用时为 `unknown`。 |
| `devices[].occupancy.motion_score` | number/null | 0-1 | 运动强度 | 轻量运动分数，不是动作分类结果。 | CSI 不可用时为 `null`。 |
| `devices[].occupancy.available` | boolean | 无 | CSI 可用性 | 当前设备是否已有可用 CSI 结果。 | 无结果、S3 标记不可用或开关关闭时为 `false`。 |
| `home_summary` | object | 无 | 家庭摘要 | 在线/离线设备数、平均温度/湿度/空气质量。 | 无传感器时平均值为 `null`。 |
| `history` | array | 无 | 传感器历史 | S3 快照中的 history，或后端从 BME690 历史兜底生成。 | 无历史时 `[]`。 |
| `recent_voice_events` | array | 无 | 最近语音事件 | S3 记录的 voice turn 事件。 | 无事件时 `[]`。 |
| `recent_commands` | array | 无 | 最近命令 | S3 记录的命令 ack 状态。 | 无命令时 `[]`。 |

`devices[].appliances.air_conditioner/fan/light/tv/curtain` 目前是 mock 控制状态，必须带 `source:"mock"` 和 `mock:true`。这些字段不代表真实电器控制模块已经接入。

CSI fallback 与兼容策略：

- C5/S3 CSI 运行开关关闭时，`devices[].occupancy` 必须保持 `state:"unknown"`、`available:false`、`motion_score:null`，不影响 BME、voice、command、heartbeat 或整机在线判断。
- `POST /api/device/v1/ingest payload_type="csi.motion"` 只更新 `device_status`、`device_module_status(module_type="csi.motion")` 和 Dashboard v1 内存聚合，不改变 legacy `/sensor`、`/sensor/latest`、`/sensor/history` 响应形状。
- `POST /api/device/v1/gateway-state` 的 snapshot 若带 `devices[].occupancy`，Server 会归一化同一字段模型；若同时存在较新的独立 `csi.motion` 结果，会按 `updated_at` 保留较新的 occupancy。
- Server 重启后会先尝试从 `dashboard_snapshots` 恢复最新快照；如果数据库没有可恢复快照，在下一次 S3 snapshot 或 `csi.motion` 上传前，Dashboard v1 才返回 `unknown/unavailable`。
- 前端展示层只应读取 `occupancy.state` 和 `motion_score`；`variance/rssi/sample_count/updated_at` 是诊断字段，不应被当作 raw CSI 或业务分类结果展示。

错误码：

- `500 DASHBOARD_OVERVIEW_READ_FAILED`

### `POST /api/dashboard/v1/snapshot`

手动写入 Dashboard 快照，行为和 `POST /api/device/v1/gateway-state` 一致，但返回 Dashboard v1 envelope。该接口用于后端测试、调试或需要直接写快照的管理流程。

请求体：同 `POST /api/device/v1/gateway-state`，并要求 `schema_version=2`、`payload_type="gateway.dashboard_snapshot"`。

成功响应返回 HTTP `202`：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "snapshot_id": "sensair_s3_gateway_01_1781100000100_uuid",
    "payload_type": "gateway.dashboard_snapshot",
    "gateway_id": "sensair_s3_gateway_01",
    "device_count": 1,
    "history_count": 0,
    "received_at_ms": 1781100000100
  },
  "error": null
}
```

错误码与 `POST /api/device/v1/gateway-state` 相同：

- `400 UNSUPPORTED_PAYLOAD_TYPE`
- `400 INVALID_SCHEMA_VERSION`
- `400 INVALID_DASHBOARD_SNAPSHOT`
- `500 DASHBOARD_SNAPSHOT_WRITE_FAILED`

### `GET /api/dashboard/v1/latest`

读取最近一次 Dashboard 标准快照。该接口只返回快照本身；如果内存中没有快照，Server 会从 `dashboard_snapshots` 读取最新记录。没有任何快照时返回 `ok=true, data=null, error=null`。

成功响应：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "gateway": {
      "gateway_id": "sensair_s3_gateway_01"
    },
    "devices": [],
    "home_summary": {
      "online_device_count": 0,
      "offline_device_count": 0,
      "avg_temperature": null,
      "avg_humidity": null,
      "avg_air_quality": null
    },
    "history": [],
    "recent_voice_events": [],
    "recent_commands": [],
    "received_at_ms": 1780000000000,
    "source": "s3_gateway"
  },
  "error": null
}
```

错误码：

- `500 DASHBOARD_LATEST_READ_FAILED`

### `GET /api/dashboard/v1/history`

读取 Dashboard 快照历史，按最新记录倒序返回。该接口读取 `dashboard_snapshots`，每项包含快照元信息和完整 `payload`。

查询参数：

- `limit`: 可选，默认 `50`，最大 `500`；必须是正整数。

成功响应：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "snapshots": [
      {
        "snapshot_id": "sensair_s3_gateway_01_1781100000100_uuid",
        "gateway_id": "sensair_s3_gateway_01",
        "server_recv_ms": 1781100000100,
        "schema_version": 2,
        "payload": {
          "gateway": {
            "gateway_id": "sensair_s3_gateway_01"
          },
          "devices": []
        },
        "created_at": "2026-06-10T10:00:00.100Z"
      }
    ]
  },
  "error": null
}
```

错误码：

- `400 DASHBOARD_BAD_LIMIT`: `limit` 不是正整数。
- `500 DASHBOARD_HISTORY_READ_FAILED`

### `GET /api/dashboard/v1/sensors/latest`

读取最新一条 Dashboard 传感器展示数据。无记录时返回 `ok=true, data=null, error=null`。

请求参数：

- `device_id`: 可选，限制设备 ID；服务器 trim，最多 `128` 个字符。

成功响应示例：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "id": 1,
    "timestamp": 1780000000000,
    "temperature": 29.57,
    "humidity": 30.29,
    "pressure": 986.26,
    "gas_resistance": 35164,
    "air_quality_score": 72,
    "air_quality_level": "moderate",
    "air_quality_confidence": "low",
    "air_quality_source": "esp",
    "online": true,
    "device_online": true,
    "sensor_online": true,
    "latest_upload_delay_ms": 120,
    "avg_upload_delay_ms": 118,
    "delay_sample_count": 3,
    "time_sync": {
      "ok": true,
      "server_time_ms": 1780000000000,
      "server_time_iso": "2026-06-09T12:00:00.000Z",
      "latest_ping": null
    }
  },
  "error": null
}
```

前端可读取字段：见 “Dashboard v1 传感器字段表”。

错误码：

- `500 DASHBOARD_SENSOR_LATEST_READ_FAILED`

### `GET /api/dashboard/v1/sensors/history`

读取 Dashboard 传感器历史数组，按时间从旧到新返回，便于图表直接消费。空历史返回 `ok=true, data=[], error=null`。

请求参数：

- `device_id`: 可选，限制设备 ID；服务器 trim，最多 `128` 个字符。
- `limit`: 可选，默认 `50`，最大 `500`；必须是正整数，超过上限时按 `500` 返回。

成功响应示例：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": [
    {
      "id": 1,
      "timestamp": 1780000000000,
      "temperature": 29.57,
      "humidity": 30.29,
      "pressure": 986.26,
      "gas_resistance": 35164,
      "air_quality_score": 72,
      "air_quality_level": "moderate",
      "air_quality_confidence": "low",
      "air_quality_source": "esp",
      "latest_upload_delay_ms": 120
    }
  ],
  "error": null
}
```

前端可读取字段：`data[]` 每项见 “Dashboard v1 传感器字段表”；历史单点不返回 `time_sync`。

错误码：

- `400 DASHBOARD_BAD_LIMIT`: `limit` 不是正整数。
- `500 DASHBOARD_SENSOR_HISTORY_READ_FAILED`

### `GET /api/dashboard/v1/devices/:device_id/history`

读取指定设备的 Dashboard 传感器历史数组。该接口等价于 `GET /api/dashboard/v1/sensors/history?device_id=...`，但设备 ID 来自路径参数，便于前端按设备页面读取。

路径参数：

- `device_id`: 必填，目标设备 ID。

查询参数：

- `limit`: 可选，默认 `50`，最大 `500`；必须是正整数。

成功响应：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": [
    {
      "id": 1,
      "timestamp": 1780000000000,
      "temperature": 29.57,
      "humidity": 30.29,
      "pressure": 986.26,
      "gas_resistance": 35164,
      "device_id": "sensair_shuttle_01",
      "payload_type": "sensor.bme690",
      "air_quality_score": 72,
      "air_quality_level": "moderate",
      "air_quality_source": "esp"
    }
  ],
  "error": null
}
```

错误码：

- `400 DASHBOARD_BAD_LIMIT`: `limit` 不是正整数。
- `500 DASHBOARD_DEVICE_HISTORY_READ_FAILED`

### `GET /api/dashboard/v1/asr/latest`

读取最新一条 ASR 文本。无记录时返回 `ok=true, data=null, error=null`。

请求参数：无。

成功响应示例：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "id": 1,
    "timestamp": 1780000000000,
    "text": "识别文本"
  },
  "error": null
}
```

前端可读取字段表：

| 字段名 | 类型 | 单位 | 前端显示名 | 含义 | 空值规则 |
| --- | --- | --- | --- | --- | --- |
| `id` | number | 无 | ASR 记录 ID | `asr_records.id`。 | 无记录时整条 `data=null`。 |
| `timestamp` | number/null | ms | 识别时间 | `asr_records.timestamp`。 | 来源为空或非法时为 `null`。 |
| `text` | string | 无 | 识别文本 | 最新 ASR 文本。 | 入库为空时返回空字符串。 |

错误码：

- `500 DASHBOARD_ASR_LATEST_READ_FAILED`

### `GET /api/dashboard/v1/llm/latest`

读取最新一条 LLM 文本。无记录时返回 `ok=true, data=null, error=null`。

请求参数：无。

成功响应示例：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "id": 1,
    "timestamp": 1780000000100,
    "prompt": "用户问题",
    "response": "模型回复"
  },
  "error": null
}
```

前端可读取字段表：

| 字段名 | 类型 | 单位 | 前端显示名 | 含义 | 空值规则 |
| --- | --- | --- | --- | --- | --- |
| `id` | number | 无 | LLM 记录 ID | `llm_records.id`。 | 无记录时整条 `data=null`。 |
| `timestamp` | number/null | ms | 回复时间 | `llm_records.timestamp`。 | 来源为空或非法时为 `null`。 |
| `prompt` | string | 无 | 用户问题 | 最新 LLM prompt。 | 入库为空时返回空字符串。 |
| `response` | string | 无 | 模型回复 | 最新 LLM response。 | 入库为空时返回空字符串。 |

错误码：

- `500 DASHBOARD_LLM_LATEST_READ_FAILED`

### `GET /api/dashboard/v1/time/status`

读取 Dashboard 时间同步状态。该接口不改变 legacy `GET /api/time/status` 的响应结构，只把同源数据包进 Dashboard v1 envelope。

请求参数：无。

成功响应示例：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "server_time_ms": 1780000000000,
    "server_time_iso": "2026-06-09T12:00:00.000Z",
    "latest_ping": null,
    "time_sync": {
      "ok": true,
      "server_time_ms": 1780000000000,
      "server_time_iso": "2026-06-09T12:00:00.000Z",
      "latest_ping": null
    }
  },
  "error": null
}
```

前端可读取字段表：

| 字段名 | 类型 | 单位 | 前端显示名 | 含义 | 空值规则 |
| --- | --- | --- | --- | --- | --- |
| `server_time_ms` | number | ms | 服务器时间 | Server 当前 Unix 毫秒时间。 | 总是返回。 |
| `server_time_iso` | string | ISO8601 | 服务器时间 | Server 当前 ISO 时间。 | 总是返回。 |
| `latest_ping` | object/null | 无 | 最近时间同步 | 最近一次 `POST /api/time/ping` 记录。 | 从未 ping 时为 `null`。 |
| `time_sync` | object | 无 | 时间同步对象 | 与 legacy `/api/time/status` 同源的完整对象。 | 总是返回对象。 |

错误码：

- `500 DASHBOARD_TIME_STATUS_READ_FAILED`

### `GET /api/dashboard/v1/device/status`

读取 Dashboard 整机状态。无设备状态时仍返回对象，核心状态字段使用 `null` 或 `false`，便于前端稳定渲染。

请求参数：

- `device_id`: 可选，限制设备 ID；服务器 trim，最多 `128` 个字符。为空时读取最近一个设备状态。

成功响应示例：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "device_id": "esp32-c5-whole-001",
    "online": true,
    "device_online": true,
    "last_seen_ms": 1780000000000,
    "last_seen_iso": "2026-06-09T12:00:00.000Z",
    "last_seen_age_ms": 1200,
    "time_synced": true,
    "latest_upload_delay_ms": 120,
    "avg_upload_delay_ms": 118,
    "delay_sample_count": 3
  },
  "error": null
}
```

前端可读取字段表：

| 字段名 | 类型 | 单位 | 前端显示名 | 含义 | 空值规则 |
| --- | --- | --- | --- | --- | --- |
| `device_id` | string/null | 无 | 设备 ID | 整机设备 ID。 | 未知时为 `null`；传入查询 ID 但无状态时回显该 ID。 |
| `online` | boolean | 无 | 整机在线 | Dashboard 整机在线状态。 | 无状态时为 `false`。 |
| `device_online` | boolean | 无 | 整机在线 | 与 `online` 同义，便于和模块状态区分。 | 无状态时为 `false`。 |
| `last_seen_ms` | number/null | ms | 最近上报时间 | 最近一次整机状态刷新时间。 | 未知时为 `null`。 |
| `last_seen_iso` | string/null | ISO8601 | 最近上报时间 | 最近一次整机状态刷新 ISO 时间。 | 未知时为 `null`。 |
| `last_seen_age_ms` | number/null | ms | 距最近上报 | Server 当前时间减 `last_seen_ms`。 | 未知时为 `null`。 |
| `time_synced` | boolean/null | 无 | 时间已同步 | ESP 上报 metadata 中的时间同步状态。 | 未知时为 `null`。 |
| `latest_upload_delay_ms` | number/null | ms | 最近上传延迟 | 最近有效上传延迟样本。 | 无有效样本时为 `null`。 |
| `avg_upload_delay_ms` | number/null | ms | 平均上传延迟 | 有效样本平均上传延迟。 | 无有效样本时为 `null`。 |
| `delay_sample_count` | number | 个 | 延迟样本数 | 参与平均延迟计算的样本数。 | 无样本时为 `0`。 |

错误码：

- `500 DASHBOARD_DEVICE_STATUS_READ_FAILED`

### `GET /api/dashboard/v1/modules/status`

读取 Dashboard 模块状态列表。BME690 模块离线不等于整机离线；前端应分别读取 `device_online` 和 `module_online`。

请求参数：

- `device_id`: 可选，限制设备 ID；服务器 trim，最多 `128` 个字符。为空时返回所有已知模块。

成功响应示例：

```json
{
  "ok": true,
  "server_time_ms": 1780000000000,
  "data": {
    "modules": [
      {
        "device_id": "esp32-c5-whole-001",
        "module_type": "sensor.bme690",
        "online": true,
        "module_online": true,
        "last_seen_ms": 1780000000000,
        "last_seen_iso": "2026-06-09T12:00:00.000Z",
        "last_seen_age_ms": 1200,
        "latest_upload_delay_ms": 120,
        "avg_upload_delay_ms": 118,
        "delay_sample_count": 3
      }
    ]
  },
  "error": null
}
```

前端可读取字段表：

| 字段名 | 类型 | 单位 | 前端显示名 | 含义 | 空值规则 |
| --- | --- | --- | --- | --- | --- |
| `modules` | array | 无 | 模块列表 | 已知模块状态数组。 | 无模块时为 `[]`。 |
| `modules[].device_id` | string/null | 无 | 设备 ID | 模块所属整机设备 ID。 | 未知时为 `null`。 |
| `modules[].module_type` | string/null | 无 | 模块类型 | 例如 `sensor.bme690`、`time.ping`。 | 未知时为 `null`。 |
| `modules[].online` | boolean | 无 | 模块在线 | 模块在线状态。 | 无状态时为 `false`。 |
| `modules[].module_online` | boolean | 无 | 模块在线 | 与 `online` 同义，便于和整机状态区分。 | 无状态时为 `false`。 |
| `modules[].last_seen_ms` | number/null | ms | 最近上报时间 | 最近一次模块状态刷新时间。 | 未知时为 `null`。 |
| `modules[].last_seen_iso` | string/null | ISO8601 | 最近上报时间 | 最近一次模块状态刷新 ISO 时间。 | 未知时为 `null`。 |
| `modules[].last_seen_age_ms` | number/null | ms | 距最近上报 | Server 当前时间减模块 `last_seen_ms`。 | 未知时为 `null`。 |
| `modules[].latest_upload_delay_ms` | number/null | ms | 最近上传延迟 | 最近有效上传延迟样本。 | 无有效样本时为 `null`。 |
| `modules[].avg_upload_delay_ms` | number/null | ms | 平均上传延迟 | 有效样本平均上传延迟。 | 无有效样本时为 `null`。 |
| `modules[].delay_sample_count` | number | 个 | 延迟样本数 | 参与平均延迟计算的样本数。 | 无样本时为 `0`。 |

错误码：

- `500 DASHBOARD_MODULE_STATUS_READ_FAILED`

### Legacy 保留策略

- `/sensor/*`、`/asr/*`、`/llm/*`、`/api/time/status` 和 `/api/device/v1/*` 保留，不删除、不重命名、不强制改成 Dashboard v1 envelope。
- `GET /sensor/latest` 无数据仍按旧约定返回 `{}`；`GET /sensor/history` 仍返回数组；`GET /asr/latest` 和 `GET /llm/latest` 无数据仍返回 `{}`；`GET /api/time/status` 仍直接返回时间状态对象。
- 新 Dashboard 前端应迁移到 `/api/dashboard/v1/*`；legacy 接口仅作为旧版本兼容和回滚路径。
- `/api/device/v1/*` 继续表达 ESP 设备协议、设备状态和调试视角；Dashboard v1 表达页面展示视角。后端可以复用同一批 service/mapper，但前端不要把 device v1 raw envelope 当成唯一读取契约。

### CSI 阶段 B 后端验收命令

从 `ESP-server` 目录执行：

```bash
node --check src/services/csiMotionService.js
node --check src/services/dashboardService.js
node --check src/routes/deviceRoutes.js
node --check scripts/smoke-regression.js
npm test
git diff -- public db/database.db
```

期望结果：

- `node --check` 无输出且退出码为 `0`。
- `npm test` 通过，覆盖 `sensor.bme690` legacy ingest、`csi.motion` ingest、双 C5 occupancy 不互相覆盖、Dashboard v1 `unknown/unavailable` fallback。
- `git diff -- public db/database.db` 无输出，确认本阶段没有修改前端文件或真实数据库。

## Legacy 传感器兼容接口

### `POST /sensor`

ESP 旧设备和旧脚本上传扁平传感器数据的 legacy 写入入口。新 `Whole-project` BME690 主链路已经改为 `POST /api/device/v1/ingest`，不要把新 BME 链路切回该接口。
服务器启动迁移会在干净 SQLite 库中自动创建 `sensor_records` 基础表，再补齐时间同步列；不会要求已有数据库先手工建表。

请求体：

```json
{
  "temperature": 25.6,
  "humidity": 58.3,
  "pressure": 1012.4,
  "gas_resistance": 132,
  "device_id": "esp32-001",
  "esp_time_ms": 1717300000000,
  "esp_uptime_ms": 123456
}
```

字段说明：

- `temperature`: 温度，数字。
- `humidity`: 湿度，数字。
- `pressure`: 气压，数字，可为空。
- `gas_resistance`: 气体阻值，数字，可为空。
- 传感器数值字段会转为有限数字；无法转换的值会按 `null` 保存。
- `device_id`: 设备 ID，可为空；服务器会 trim，最多保存 `128` 个字符。
- `esp_time_ms`: ESP 侧时间戳，毫秒，可为空。
- `esp_uptime_ms`: ESP 运行时长，毫秒，可为空。

成功响应：

```json
{
  "ok": true,
  "success": true,
  "id": 1,
  "device_id": "esp32-001",
  "esp_time_ms": 1717300000000,
  "esp_uptime_ms": 123456,
  "server_recv_ms": 1717300000100,
  "server_time_iso": "2026-06-02T00:00:00.100Z",
  "upload_delay_ms": 100
}
```

失败响应：

```json
{
  "ok": false,
  "success": false,
  "error": "错误信息"
}
```

## 前端读取最新数据接口

### `GET /sensor/latest`

前端兼容读取最新一条传感器数据。没有数据时返回空对象 `{}`。该接口保留旧字段，并追加 v1 状态、平均延迟和空气状态字段，供旧 Dashboard 不改代码继续运行。

响应字段来自 `sensor_records`，并额外包含 `time_sync` 状态：

```json
{
  "id": 1,
  "timestamp": 1717300000100,
  "temperature": 25.6,
  "humidity": 58.3,
  "pressure": 1012.4,
  "gas_resistance": 132,
  "device_id": "esp32-001",
  "esp_time_ms": 1717300000000,
  "esp_uptime_ms": 123456,
  "server_recv_ms": 1717300000100,
  "server_time_iso": "2026-06-02T00:00:00.100Z",
  "upload_delay_ms": 100,
  "online": true,
  "device_online": true,
  "sensor_online": true,
  "latest_upload_delay_ms": 100,
  "avg_upload_delay_ms": 96,
  "delay_sample_count": 5,
  "air_quality_score": 72,
  "air_quality_level": "moderate",
  "air_quality_confidence": "low",
  "air_quality_source": "esp",
  "air_quality": {
    "air_quality_score": 72,
    "air_quality_level": "moderate",
    "air_quality_confidence": "low",
    "air_quality_source": "esp"
  },
  "time_sync": {
    "ok": true,
    "server_time_ms": 1717300000100,
    "server_time_iso": "2026-06-02T00:00:00.100Z",
    "latest_ping": null
  }
}
```

### `GET /asr/latest`

读取最新一条 ASR 记录。没有数据时返回空对象 `{}`。

```json
{
  "id": 1,
  "timestamp": 1717300000100,
  "text": "识别文本"
}
```

### `GET /llm/latest`

读取最新一条 LLM 记录。没有数据时返回空对象 `{}`。

```json
{
  "id": 1,
  "timestamp": 1717300000100,
  "prompt": "用户问题",
  "response": "模型回复"
}
```

## 前端读取历史数据接口

### `GET /sensor/history`

前端读取传感器历史数据，按时间从旧到新返回数组。

查询参数：

- `limit`: 返回条数，默认 `50`，最大 `500`。

示例：

```bash
curl "http://localhost:3000/sensor/history?limit=10"
```

响应：

```json
[
  {
    "id": 1,
    "timestamp": 1717300000100,
    "temperature": 25.6,
    "humidity": 58.3,
    "pressure": 1012.4,
    "gas_resistance": 132,
    "device_id": "esp32-001",
    "esp_time_ms": 1717300000000,
    "esp_uptime_ms": 123456,
    "server_recv_ms": 1717300000100,
    "server_time_iso": "2026-06-02T00:00:00.100Z",
    "upload_delay_ms": 100
  }
]
```

## 状态/健康检查接口

### `GET /api/time/now`

返回服务器当前时间。

```json
{
  "ok": true,
  "server_time_ms": 1717300000100,
  "server_time_iso": "2026-06-02T00:00:00.100Z"
}
```

### `GET /api/time/status`

返回服务器当前时间和最近一次时间同步 ping 记录。前端当前使用该接口展示时间同步状态。

```json
{
  "ok": true,
  "server_time_ms": 1717300000100,
  "server_time_iso": "2026-06-02T00:00:00.100Z",
  "latest_ping": null
}
```

### `POST /api/time/ping`

ESP 设备可用该接口上报时间同步 ping。它属于状态/调试能力，不替代 `/sensor` 数据上传。
`device_id` 会 trim 首尾空白，最多保留 `128` 个字符；`esp_send_ms` 或 `esp_uptime_ms` 无法转换为有限数字时按 `null` 返回。

请求体：

```json
{
  "device_id": "esp32-001",
  "esp_send_ms": 1717300000000,
  "esp_uptime_ms": 123456
}
```

响应：

```json
{
  "ok": true,
  "device_id": "esp32-001",
  "esp_send_ms": 1717300000000,
  "esp_uptime_ms": 123456,
  "server_recv_ms": 1717300000100,
  "server_reply_ms": 1717300000101,
  "server_time_iso": "2026-06-02T00:00:00.101Z",
  "estimated_one_way_delay_ms": 100
}
```

## 其他现有写入接口

这些接口当前存在，但不属于 ESP 传感器上传协议。
服务器启动迁移会在干净 SQLite 库中自动创建 `asr_records` 与 `llm_records`，保证旧记录接口在空库环境下也能返回 `{}` 或正常写入。

### `POST /asr`

写入 ASR 文本。
`text` 会 trim 首尾空白，最多保存 `4000` 个字符。

```json
{
  "text": "识别文本"
}
```

成功响应保留旧 `success` 字段，并额外包含 `ok=true` 方便机器客户端统一判断：

```json
{
  "ok": true,
  "success": true,
  "id": 1
}
```

### `POST /llm`

写入 LLM 请求和响应。
`prompt` 和 `response` 会 trim 首尾空白，最多各保存 `4000` 个字符。

```json
{
  "prompt": "用户问题",
  "response": "模型回复"
}
```

成功响应保留旧 `success` 字段，并额外包含 `ok=true`：

```json
{
  "ok": true,
  "success": true,
  "id": 1
}
```

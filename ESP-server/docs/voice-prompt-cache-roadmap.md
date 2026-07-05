# Voice Prompt Cache Roadmap

本文定义并记录唤醒提示语音迁移到服务器缓存的方案。2026-06-09 本方案已在 `ESP-server` 后端和 `Whole-project` 固件落地，本文同时作为后续维护说明。

## 实施状态

- 后端已新增 `src/voice/promptCache.js`，实现 prompt key 清洗、PCM/metadata 读取校验、sha256 checksum、临时文件 + rename 写入、hit/miss/stale 返回。
- 后端已新增 `GET /api/voice/prompt-cache?prompt_key=wake_ack_zh`。
- 兼容旧 `GET /api/voice/prompt?wake=1&prompt_key=wake_ack_zh`，并复用 cache service。
- cache hit 直接返回服务器缓存 PCM，不请求 TTS、LLM 或网关。
- `voice.prompt` 请求会刷新 `device_status` 和 `device_module_status(module_type="voice.prompt")`。
- 固件 `wake_prompt_cache.c` 主路径已切到 `/api/voice/prompt-cache?prompt_key=wake_ack_zh&device_id=...`，并携带 v1 metadata headers。
- 固件唤醒失败 fallback 改为短 beep/静音/极小本地提示，不再依赖大体积内嵌固定语音作为主路径。
- `POST /api/voice/turn` raw PCM 主链路保持兼容。

## 1. 目标

- 唤醒后 ESP 请求服务器。
- 服务器返回缓存好的固定提示音。
- 缓存命中时不请求 TTS、LLM 或上游网关。
- 降低 ESP flash 占用。
- 降低唤醒后响应延迟。
- 减少网关请求次数和费用。
- 保持 `POST /api/voice/turn` 主链路不受影响。

## 2. 当前现状

- 固件 `wake_prompt_cache.c` 当前在 Wi-Fi stable 后异步请求 `GET /api/voice/prompt?wake=1&device_id=...`。
- ESP 会把返回音频保存为 `/spiffs/wake_prompt.pcm`，metadata 保存为 `/spiffs/wake_prompt.meta`。
- 当前 ESP 期望音频为 `audio/L16; rate=16000; channels=1`，`X-Audio-Format=pcm_s16le_mono_16k`，采样率 16000，mono，s16le。
- 唤醒时 `local_wake_word.c` 优先调用 `wake_prompt_cache_play()`，失败后回退到内嵌 `wake_ack_wozai_nishuo_16k.pcm`。
- 后端 `GET /api/voice/prompt` 当前 mock 模式返回 mock PCM；真实模式会调用 TTS 生成固定文本 `我在，你说`，没有服务器文件缓存层。

## 3. 推荐接口

新主接口：

```http
GET /api/voice/prompt-cache?prompt_key=wake_ack_zh
```

兼容接口：

```http
GET /api/voice/prompt?wake=1&prompt_key=wake_ack_zh
```

建议请求 headers：

```http
X-Schema-Version: 1
X-Device-Id: esp32-c5-whole-001
X-Device-Type: esp32c5_env_voice_node
X-Firmware-Version: 0.1.0
X-Request-Seq: 124
X-Esp-Uptime-Ms: 12349999
X-Esp-Time-Ms: 1780732142207
X-Time-Synced: true
X-Payload-Type: voice.prompt
```

## 4. 缓存目录

```text
ESP-server/cache/voice_prompts/
```

建议文件结构：

```text
cache/voice_prompts/
  wake_ack_zh_16000_s16le.pcm
  wake_ack_zh_16000_s16le.json
```

如果后续需要 WAV：

```text
cache/voice_prompts/
  wake_ack_zh_16000.wav
  wake_ack_zh_16000.json
```

## 5. 缓存文件格式

根据当前 ESP 播放链路，第一优先级：

- `wake_ack_zh_16000_s16le.pcm`
- raw PCM s16le
- mono
- 16000 Hz
- 对应 response `Content-Type: audio/L16; rate=16000; channels=1`
- 对应 response `X-Audio-Format: pcm_s16le_mono_16k`

候选兼容格式：

- `wake_ack_zh_16000.wav`
- WAV PCM s16le mono 16kHz
- 需要 ESP 端或服务器端确认是否剥离 WAV header。当前固件 `wake_prompt_cache_play()` 直接按 raw PCM 播放，因此第一阶段不建议返回 WAV 给当前 ESP。

## 6. metadata 字段

缓存 metadata 建议 JSON：

```json
{
  "prompt_key": "wake_ack_zh",
  "text": "我在，你说",
  "locale": "zh-CN",
  "audio_format": "pcm_s16le_mono_16k",
  "sample_rate": 16000,
  "channels": 1,
  "encoding": "s16le",
  "file_path": "cache/voice_prompts/wake_ack_zh_16000_s16le.pcm",
  "content_type": "audio/L16; rate=16000; channels=1",
  "byte_length": 32000,
  "tts_provider": "volc",
  "tts_voice": "server_prompt_v1",
  "version": "wake_ack_zh:v1",
  "checksum": "sha256:...",
  "created_at": "2026-06-09T00:00:00.000Z",
  "updated_at": "2026-06-09T00:00:00.000Z",
  "enabled": true
}
```

## 7. 返回 headers

```http
Content-Type: audio/L16; rate=16000; channels=1
X-Prompt-Key: wake_ack_zh
X-Prompt-Cache: hit
X-Audio-Format: pcm_s16le_mono_16k
X-Sample-Rate: 16000
X-Channels: 1
X-Server-Time-Ms: 1780732144669
Cache-Control: public, max-age=86400
```

`X-Prompt-Cache` 取值：

- `hit`: 命中可用缓存，未请求 TTS。
- `miss`: 原本缺失，本次生成并写入后返回。
- `stale`: 新生成失败，但存在旧缓存，返回旧缓存。

## 8. 服务器行为

- 启动预热可选：服务启动时检查默认 `wake_ack_zh` 是否存在，不存在可异步生成。
- 请求时先查 metadata 和音频文件。
- 命中且 enabled 时直接返回文件。
- 未命中时调用 TTS 生成并缓存。
- TTS 失败但有旧缓存时返回 `stale`。
- TTS 失败且无缓存时返回结构化错误，或返回极短 fallback 音。
- 不允许每次唤醒都请求网关。
- 生成缓存时必须校验音频非空、字节数偶数、采样率/声道/编码匹配当前 ESP。
- metadata 写入建议使用临时文件 + rename，避免半写状态。
- 上游 TTS key、voice、model 变化时应更新 `version` 或 checksum，避免旧音色误用。

## 9. ESP 行为

- 唤醒后或启动后请求服务器提示音。
- 播放服务器返回音频。
- 失败时 fallback 到短 beep、静音或极小本地提示。
- 不再依赖大体积本地固定语音文件作为主路径。
- 当前 ESP 可继续保存 `/spiffs/wake_prompt.pcm` 作为本地二级缓存，但服务器端缓存是上游 TTS 的主缓存。
- ESP 仍需校验 `Content-Type`、`X-Audio-Format`、字节数、非静音等基础条件。

## 10. 数据和状态接入

- `voice.prompt` 请求应刷新 `device_status`。
- `voice.prompt` 请求应刷新 `device_module_status(module_type="voice.prompt")`。
- prompt cache hit/miss/stale 可先只写日志；如后续要分析费用和命中率，可新增 `voice_prompt_cache_events`。
- LLM context 默认不需要包含 prompt cache 状态，除非进入调试模式。

## 11. 测试计划

- 已完成：miss 生成，缓存文件不存在时触发一次 TTS/mock TTS，写入音频和 metadata，返回 `X-Prompt-Cache: miss`。
- 已完成：hit 不请求 TTS，缓存存在且 metadata 合法时连续请求返回 `X-Prompt-Cache: hit`。
- 已完成：stale 返回，刷新失败但旧缓存存在时返回旧文件和 `X-Prompt-Cache: stale`。
- 已完成：格式匹配 ESP 播放链路，验证 `Content-Type`、`X-Audio-Format`、采样率、声道、字节长度为偶数且非静音。
- 已完成：不影响 voice turn，`POST /api/voice/turn` 的 raw PCM 校验和返回 PCM 行为不变。
- 已完成：兼容旧接口，`GET /api/voice/prompt?wake=1&prompt_key=wake_ack_zh` 复用 cache service。

## 12. 风险与回滚

- 风险：返回 WAV 给当前 ESP 会被当 raw PCM 播放，造成噪音；第一阶段推荐 raw PCM。
- 风险：缓存 metadata 和文件不一致；必须 checksum 或至少校验 byte length。
- 风险：TTS 生成慢；miss 可能仍慢，启动预热可缓解。
- 风险：缓存目录权限或磁盘空间不足；应返回明确错误并允许 ESP fallback。
- 回滚：保留旧 `GET /api/voice/prompt` TTS 逻辑，通过配置关闭 prompt cache。

# LLM Prompt Context Roadmap

本文定义并记录后端统一拼接设备上下文到 LLM prompt 的方案。2026-06-09 本方案已在 `ESP-server` 后端落地，本文同时作为后续维护说明。

## 实施状态

- 已新增 `src/services/deviceContextService.js`，集中读取整机状态、模块状态、最新 BME690 环境数据、空气状态和数据新鲜度。
- 已新增 `src/services/llmPromptContextService.js`，集中生成 LLM prompt context 文案和降级文案。
- `/api/llm/text` 已接入 `llmPromptContextService`。
- `/api/llm/structured` 已接入 `llmPromptContextService`，并保留结构化 JSON 指令。
- `/api/voice/turn` 的 ASR final 文本进入 LLM 前已接入同一 prompt context。
- route 不再各自散查 `sensor_records` 手写环境 prompt。
- `GET /api/device/v1/context` 已提供 `deviceContextService` 的结构化输出。
- CSI/LCD 当前仍按 unavailable/reserved 进入 context，不声称底层能力已接入。

## 1. 当前问题

- prompt 拼接分散。
- `/api/llm/text` 直接把用户 text 发给 LLM。
- `/api/llm/structured` 只调用 `buildStructuredPrompt()`，不注入设备上下文。
- `/api/voice/turn` 的 LLM 调用在 `src/voice/chain.js` 中，由 ASR 文本直接调用 `requestLlmText()`。
- 环境数据后续如果直接查旧 `sensor_records`，容易在不同 route 手写不同字段。
- 当前没有统一数据新鲜度判断。
- 当前没有统一 `device_online` / `module_online` 上下文。
- 空气质量来源需要说明为 ESP 本地估算。
- 空气质量不是国标 AQI。
- CSI/LCD 后续数据需要统一进入 prompt，但未接入时必须明确 unavailable。

## 2. 推荐服务

新增两个后端服务：

- `deviceContextService`
- `llmPromptContextService`

职责分工：

- `deviceContextService` 只负责读取和归一化设备上下文，不写 prompt 文案。
- `llmPromptContextService` 只负责把上下文压缩成稳定 prompt 片段，并处理降级文案。
- route 和 voice chain 不直接散查 `sensor_records`。

## 3. `deviceContextService` 输出结构

```json
{
  "device": {
    "device_id": "...",
    "online": true,
    "last_seen_age_ms": 2500,
    "firmware_version": "...",
    "time_synced": true,
    "latest_upload_delay_ms": 120,
    "avg_upload_delay_ms": 108,
    "delay_sample_count": 20
  },
  "modules": {
    "sensor.bme690": {
      "online": true,
      "last_seen_age_ms": 2500
    },
    "voice.turn": {
      "recent": true,
      "last_seen_age_ms": 8000
    },
    "csi.motion": {
      "available": false
    },
    "lcd.status": {
      "available": false
    }
  },
  "environment": {
    "available": true,
    "age_ms": 2500,
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
    "algo_version": "esp-bme690-relative-v1",
    "gas_baseline_ohm": 82000.0,
    "gas_ratio": 0.43,
    "note": "ESP local BME690 relative estimate, not national AQI."
  }
}
```

## 4. prompt 片段模板

建议中文模板：

```text
设备状态：
- 设备 {device_id} 当前{在线/离线}，最近通信约 {last_seen_age_ms} ms 前。
- 平均上传延迟约 {avg_upload_delay_ms} ms，最近一次有效延迟 {latest_upload_delay_ms} ms。
- 时间同步状态：{已同步/未同步}。

环境传感器：
- BME690 数据约 {age_ms} ms 前更新。
- 温度 {temperature_c}°C，湿度 {humidity_percent}%，气压 {pressure_hpa} hPa，气体电阻 {gas_resistance_ohm} Ω。
- ESP 本地空气状态估算：{level}，得分 {score}/100，置信度 {confidence}。
- 注意：该空气状态由 ESP 本地基于 BME690 气体电阻和湿度估算，不是国标 AQI，也不代表 PM2.5、PM10 或 CO2。

模块状态：
- sensor.bme690：{online/offline}
- voice.turn：{recent/not recent}
- csi.motion：{available/unavailable}
- lcd.status：{available/unavailable}
```

如果环境数据不可用：

```text
环境传感器：
- 当前没有可用的实时 BME690 环境数据。回答环境问题时必须说明无法确认实时环境。
```

如果数据过期：

```text
环境传感器：
- 最近 BME690 数据已过期，约 {age_ms} ms 前更新。不要把它当成实时环境，只能作为历史参考。
```

如果空气质量缺失：

```text
空气状态：
- 当前没有可靠的 ESP 本地空气状态估算。可以说明原始 BME690 数据，但不要给空气质量结论。
```

## 5. 降级策略

- `deviceContextService` 失败时，LLM 请求继续，但 prompt 明确说明设备上下文不可用。
- 数据过期时不提供实时环境结论。
- `confidence=low` 时回答保守。
- CSI/LCD 未接入时显示不可用，不假装存在。
- `air_quality_source` 缺失时，不把空气质量说成 ESP 本地计算。
- `time_synced=false` 时，延迟和绝对时间判断必须保守。
- 设备离线时，prompt 必须说明没有实时通信，不能假装知道当前环境。

## 6. 接入范围

所有调用 LLM 的路径必须统一使用 `llmPromptContextService`：

- `src/routes/llmTextRoutes.js`
- `src/routes/structuredLlmRoutes.js`
- `src/routes/voiceRoutes.js` 或 `src/voice/chain.js`

推荐调用方式：

1. route 或 voice chain 读取 `device_id`。
2. 调用 `deviceContextService.getContext(device_id)`。
3. 调用 `llmPromptContextService.buildPrompt(userText, context, mode)`。
4. 把最终 prompt 交给 `requestLlmText()`。
5. `llm_records.prompt` 可保存原始用户文本或压缩后的最终 prompt；建议保留 `raw_json` 或 metadata 记录上下文版本，避免审计困难。

## 7. 禁止事项

- 禁止 route 里直接散查 `sensor_records`。
- 禁止各 route 手写不同环境 prompt。
- 禁止把大量历史记录塞进 prompt。
- 禁止把 BME690 相对空气状态说成国标 AQI。
- 禁止在 CSI/LCD 未接入时让 LLM 说 "检测到屏幕/CSI 数据"。
- 禁止忽略数据新鲜度。

## 8. 上下文字段来源

- `device`: 来自 `device_status`。
- `modules`: 来自 `device_module_status`。
- `environment`: 来自最新有效 `sensor_records` 中 `payload_type=sensor.bme690` 或 legacy 映射。
- `air_quality`: 优先来自 ESP 上传字段；缺失时可来自 server fallback，但必须标注。
- `csi.motion`: 当前 unavailable；后续来自 `csi_behavior_events` 或 v1 ingest。
- `lcd.status`: 当前可从 `lcd_status` 表获取 server-side 状态，但固件屏幕硬件接入仍应按实际状态标注。

## 9. 数据新鲜度建议

- `sensor.bme690` 新鲜阈值：`30000ms`。
- `device_online` 阈值：`120000ms`。
- `voice.turn` recent 阈值：`60000ms`。
- 超过阈值时 prompt 必须用 "已过期/历史参考"。

## 10. 调试 API

为了调试 prompt 和前端迁移，已提供：

```http
GET /api/device/v1/context?device_id=esp32-c5-whole-001
```

返回 `deviceContextService` 的结构化 JSON。该接口只展示后端已知上下文，不调用 LLM。

## 11. 测试计划

- 已完成：`/api/llm/text` 发送请求时 mock LLM 收到包含设备状态、环境、空气状态、非 AQI 提示的 prompt。
- 已完成：`/api/llm/structured` 保持结构化 JSON 指令，同时包含设备上下文。
- 已完成：`/api/voice/turn` ASR 文本进入 LLM 前注入同一上下文。
- 已完成：设备离线或未知时 prompt 明确离线/未知。
- 已完成：BME 数据过期时 prompt 不说实时。
- 已完成：`air_quality_confidence=low` 时 prompt 保守。
- 已完成：CSI/LCD unavailable 时 prompt 不假装存在。

## 12. 风险与回滚

- 风险：prompt 变长导致 token 消耗增加；服务应限制上下文长度。
- 风险：把过期数据误当实时；必须集中做 age 判断。
- 风险：结构化命令 prompt 被上下文干扰；`llmPromptContextService` 应按 mode 控制上下文位置和长度。
- 回滚：通过配置关闭上下文注入，保留原始 `requestLlmText(userText)` 路径。

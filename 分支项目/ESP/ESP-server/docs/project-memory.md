# Project Memory

更新时间：2026-06-09

本文件记录 ESP `Whole-project` 与 `ESP-server` 当前统一设备协议 v1 实施状态，供后续 Codex 或人工开发继续实施前快速读取。

## 当前真实链路

1. 当前真实 BME690 上传链路是：
   `Whole-project/components/Middlewares/app_orchestrator/app_orchestrator.c`
   -> `Whole-project/components/Middlewares/sensor_domain/bme690/service/bme_sensor_service.c`
   -> `Whole-project/components/Middlewares/sensor_domain/bme690/server_client/bme_server_client.c`
   -> `POST /api/device/v1/ingest`。

2. `POST /sensor` 已保留为 legacy/前端兼容入口，不是新 ESP BME690 主链路。

3. `server_upload_bridge` 是 legacy duplicate path：仍作为 ESP-IDF component 参与构建，但当前 BME 服务真实调用链不走它。后续不要把 BME690 切回它。

4. `app_orchestrator_start()` 在 Wi-Fi stable 后调用 `app_time_sync_once(APP_TIME_SYNC_SERVER_URL)`，为 v1 metadata 和延迟计算提供时间同步基础。

## 统一设备协议 v1

5. 新主协议为 `POST /api/device/v1/ingest` + envelope v1。

6. envelope v1 中 `device_id` 表示整机；`sensor_id` / `module_id` 表示模块或传感器，放在 `payload` 内。

7. 统一 metadata 包括：`schema_version`、`device_id`、`device_type`、`firmware_version`、`request_seq`、`esp_uptime_ms`、`esp_time_ms`、`time_synced`、`payload_type`。

8. raw PCM 和 GET 请求不强制改 JSON body，使用 v1 header metadata：`X-Schema-Version`、`X-Device-Id`、`X-Device-Type`、`X-Firmware-Version`、`X-Request-Seq`、`X-Esp-Uptime-Ms`、`X-Esp-Time-Ms`、`X-Time-Synced`、`X-Payload-Type`。

9. `server_recv_ms`、`server_time_iso`、`upload_delay_ms` 由 server 生成或计算；ESP 上传的同名字段必须忽略。

10. `time_synced=false` 时不允许用 `0` 伪装真实 `esp_time_ms`；固件侧未同步时上传 `null`。

## BME690 与空气质量

11. BME690 v1 payload 使用 `sensor_id="bme690_01"`，整机 ID 使用 `SERVER_COMM_DEVICE_ID`。

12. 固件新增 `bme_air_quality.*`，ESP 本地基于 BME690 `gas_resistance_ohm` 和 `humidity_percent` 计算相对空气状态。

13. 空气状态字段命名为 `air_quality_score`、`air_quality_level`、`air_quality_confidence`、`air_quality_algo_version`、`air_quality_source`、`gas_baseline_ohm`、`gas_ratio`、`gas_score`、`humidity_score`、`baseline_ready`、`warmup_done`、`sample_count`。

14. 该指标不是 AQI，不代表 PM2.5、PM10 或 CO2，不适合跨设备或跨城市直接比较。

15. 服务器接收 ESP 空气状态并入库；缺失或非法时可 fallback 补算，并标记 `air_quality_source="server_fallback"`。服务器不能覆盖合法 ESP 上传结果。

## 后端存储与状态

16. `sensor_records` 继续保留 legacy 列，新增 v1 metadata、`metadata_json`、`raw_json`、`air_quality_json` 和空气状态拆分列。

17. 已新增 `device_status` 和 `device_module_status`，整机在线与模块在线分离。

18. 任意有效 ESP 请求可刷新 `device_status`；BME690 只刷新 `device_module_status(module_type="sensor.bme690")`。BME 停止上传不代表整机离线。

19. 延迟统计只使用 `time_synced=true` 且 `esp_time_ms` 有效、计算结果在 `0..60000ms` 的样本。

20. `latest_upload_delay_ms` 用于调试，`avg_upload_delay_ms` 优先用于 Dashboard/API/LLM context 展示，`delay_sample_count` 记录有效样本数。

## 唤醒提示音

21. 服务端新增 voice prompt cache：`GET /api/voice/prompt-cache?prompt_key=wake_ack_zh`。

22. 缓存命中时直接返回服务器缓存 PCM，不请求 TTS、LLM 或网关。

23. `GET /api/voice/prompt?wake=1&prompt_key=wake_ack_zh` 保持兼容并复用 cache service。

24. ESP wake prompt 主路径已改为请求 `/api/voice/prompt-cache?prompt_key=wake_ack_zh&device_id=...`，失败 fallback 为短 beep/静音/极小本地提示，不再依赖大体积内嵌固定语音作为主路径。

25. 当前 ESP 播放链路优先匹配 raw PCM：`audio/L16; rate=16000; channels=1`、`pcm_s16le_mono_16k`。

## LLM prompt

26. 已新增 `deviceContextService` 和 `llmPromptContextService`。

27. `/api/llm/text`、`/api/llm/structured`、`/api/voice/turn` 的 LLM 调用统一使用 `llmPromptContextService`。

28. route 不应散查 `sensor_records` 或手写环境 prompt。

29. prompt 会包含设备在线、模块状态、环境数据、空气状态、上传延迟、数据新鲜度和 CSI/LCD unavailable 状态。

30. 数据过期或设备离线时，prompt 必须明确说明，不能让 LLM 假装知道实时环境。

31. 空气状态进入 prompt 时必须说明：`ESP 本地 BME690 相对空气状态估算，不是国标 AQI，不代表 PM2.5、PM10 或 CO2`。

## API 与 legacy

32. 已新增：
   - `POST /api/device/v1/ingest`
   - `GET /api/device/v1/status`
   - `GET /api/device/v1/modules/status`
   - `GET /api/device/v1/context`
   - `GET /api/device/v1/sensors/latest`

33. `/sensor`、`/sensor/latest`、`/sensor/history` 保留 legacy/前端兼容，不删除，不破坏。

34. `/sensor/latest` 保留旧字段，并追加 `online`、`device_online`、`sensor_online`、`avg_upload_delay_ms`、`latest_upload_delay_ms`、`delay_sample_count` 和空气状态字段。

35. `voice.turn`、`voice.prompt`、`command.capabilities`、`command.poll`、`command.ack`、`time.ping` 已接入 metadata 解析和 device/module status 刷新。

## 边界与风险

36. 本轮没有修改 `ESP-server/public/`、Dashboard 前端、`managed_components/`、`node_modules/`、真实 `.env` 或真实 `ESP-server/db/database.db`。

37. 没有实现 CSI 底层采集或 LCD 底层驱动；它们在 v1 context 中仍按 unavailable/reserved 处理。

38. Server 不做风险判断，不做紧急决策，只做接收、校验、存储、prompt 构建和网关调用。

39. 固件构建通过只能证明编译和分区容量，不等于真实设备串口运行时已经端到端验证。

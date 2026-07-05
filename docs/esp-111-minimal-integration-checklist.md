# ESP-111 最小联调验收清单

更新日期：2026-06-11

## 范围

本清单用于当前三套固件的最小联调验收：

- `ESPS3` 作为本地网关，提供 SoftAP、`/local/v1` 本地 HTTP、子设备 registry、voice proxy、command router、S3 -> Server 转发。
- `ESPC51` 作为 C5 terminal，默认 `device_id=sensair_shuttle_01`。
- `ESPC52` 作为第二个 C5 terminal，默认 `device_id=sensair_shuttle_02`。

本清单只覆盖最小主链路。CSI 当前是预留/独立模块，默认不接入 C5 app_main/orchestrator/WiFi callback，不让 S3 定时 ping C5，也不把 CSI result 写入 `sensor_aggregator` 或上传 Server。默认宏保持 `MAIN_ENABLE_CSI_SERVICE=0`、`GATEWAY_CONFIG_ENABLE_CSI_TRIGGER=0`、`GATEWAY_CONFIG_ENABLE_CSI_RESULT_INGEST=0`；Dashboard 前端文件不在本清单范围内。

## 1. Build

先加载 ESP-IDF 环境，再分别构建三套固件：

```bash
source /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh
cd /Users/zhiqin/ESP-111/ESPC51
idf.py build
cd /Users/zhiqin/ESP-111/ESPC52
idf.py build
cd /Users/zhiqin/ESP-111/ESPS3
idf.py build
```

验收标准：

- 三个工程均 build 成功。
- build 输出中没有因为本轮改动新增的 compile error。
- C51/C52 协议实现一致；C52 只通过工程配置覆盖默认 `device_id`、alias 和本地短 `id=2`。

## 2. Flash 顺序

建议先刷 S3，再刷两块 C5：

```bash
cd /Users/zhiqin/ESP-111/ESPS3
idf.py -p <S3_PORT> flash monitor
cd /Users/zhiqin/ESP-111/ESPC51
idf.py -p <C51_PORT> flash monitor
cd /Users/zhiqin/ESP-111/ESPC52
idf.py -p <C52_PORT> flash monitor
```

如果 C52 曾经写入过旧 NVS，默认 `sensair_shuttle_02` 可能被 NVS 中的旧 `device_id` 覆盖。出现两个 C5 都注册成 `sensair_shuttle_01` 时，对 C52 先执行：

```bash
cd /Users/zhiqin/ESP-111/ESPC52
idf.py -p <C52_PORT> erase-flash flash monitor
```

## 3. S3 启动日志检查

S3 monitor 中应看到：

- `role=gateway gateway_id=sensair_s3_gateway_01`
- `registry initialized allowlist_count=2`
- `local command queue initialized`
- `voice proxy initialized single_session=true`
- `CSI trigger disabled by GATEWAY_CONFIG_ENABLE_CSI_TRIGGER`
- `CSI trigger enabled=0 result_ingest=0`
- `local HTTP server started port=80 base=/local/v1`

如果 `STA credentials are empty`，说明 S3 本地联调可继续，但 S3 -> Server 转发会进入 offline policy 降级。

## 4. C5 注册验收

C51 monitor 中应看到：

- `terminal config ... device_id=sensair_shuttle_01`
- `WiFi connected`
- `registered local terminal status=200`
- 默认关闭 CSI 时：`CSI service disabled by MAIN_ENABLE_CSI_SERVICE`

C52 monitor 中应看到：

- `terminal config ... device_id=sensair_shuttle_02`
- `WiFi connected`
- `registered local terminal status=200`
- 默认关闭 CSI 时：`CSI service disabled by MAIN_ENABLE_CSI_SERVICE`

S3 monitor 中应分别看到：

- `child registered device_id=sensair_shuttle_01`
- `child registered device_id=sensair_shuttle_02`

当前 S3 没有对外暴露“子设备列表”HTTP 接口；最小验收用上述 registry 串口日志确认子设备列表状态。

## 5. BME 数据上传验收

C5 monitor 中应看到周期性 BME 读数日志和上传动作。S3 monitor 中应看到 `/local/v1/sensor` 对应的转发结果：

- server 可达时：S3 monitor 中 server status 为 2xx。
- server 不可达或 S3 STA 未配置时：本地响应仍为 `202 {"ok":1,"id":...}`，但 S3 monitor/offline policy 会记录 `gateway_offline`、`server_unavailable`、`timeout` 或 `server_rejected`。

字段检查重点：

- C5 -> S3 本地 body 使用短字段：`id=1|2`、`t=bme`、`u=<uptime_ms>`、`v=[...]`。
- `id=1` 由 S3 映射为 `device_id=sensair_shuttle_01`；`id=2` 映射为 `device_id=sensair_shuttle_02`。
- S3 -> Server 完整 JSON 仍使用 `payload_type=sensor.bme690`，payload 内包含 `sensor_id=bme690_01`、温湿压、gas、air_quality、baseline、sample_count。
- BME `v` 数组顺序为 `temperature_c, humidity_percent, pressure_hpa, gas_resistance_ohm, air_quality_score, gas_baseline_ohm, gas_ratio, gas_score, humidity_score, baseline_ready, warmup_done, sample_count`。

## 6. Voice Proxy 验收

触发 C5 Mic/VAD 后，C5 应向 S3 发起：

- `POST /local/v1/voice/turn`
- `Content-Type=audio/L16; rate=16000; channels=1`
- `X-Audio-Format=pcm_s16le_mono_16k`

S3 只做代理，不做本地 ASR/LLM/TTS。server 可达时，S3 将请求转发到 `/api/voice/turn` 并把返回 PCM 流式回 C5；server 不可达时，C5 应收到 JSON 错误或 turn 失败日志。

常见失败：

- `voice_busy`：S3 单会话互斥，等待当前 turn 完成。
- `payload_too_large`：C5 单次上传超过 S3 `384 KiB` 上限。
- `gateway_offline/server_unavailable`：S3 STA 或 server 不可达。

## 7. Command Router 验收

C5 每 5s 拉取：

```text
GET /local/v1/commands/pending?id=<1|2>&limit=1
```

S3 会先把短 `id` 映射成完整 `device_id`，再尝试从 server 的 `/api/commands/pending?device_id=...` 同步命令，最后返回轻量本地队列：

```json
{"ok":1,"id":1,"cmds":[{"cid":"cmd-001","c":2,"seq":7,"ttl_ms":30000,"a":{"text":"hello","ttl_ms":5000}}]}
```

当前 C5 执行范围：

- `device.noop`
- `lcd.show_text`
- `display.show_text`

本地命令码：`c=1` 为 noop，`c=2` 为显示文字；其他 server 命令可被 S3 压成保留码或 unsupported，C5 不接真实 LCD。

命令执行后 C5 回传：

```text
POST /local/v1/commands/{id}/ack
```

ack body 使用轻量格式：

```json
{"id":1,"cid":"cmd-001","ok":1,"e":0}
```

S3 再转发到 server：

```text
POST /api/commands/{id}/ack
```

真实 LCD 未接入时，`lcd.show_text/display.show_text` 只验证 display placeholder 接口稳定，不验证屏幕硬件显示。

## 8. Server 转发验证

完整 S3 -> Server 转发需要 S3 STA 能联网，并且 `GATEWAY_CONFIG_SERVER_BASE_URL` 指向可用的 server。满足条件后检查：

- S3 health 中 `server_available=true` 或 S3 monitor 无持续 offline policy warning。
- BME/status 本地响应为 `{"ok":1,"id":...}`；server 转发状态看 S3 monitor/offline policy。
- voice turn server 返回 PCM 或明确业务错误。
- command pending/ack 在 S3 monitor 中能看到 server polling/ack status。

如果 S3 STA 为空或 server 不可达，本轮仍可完成 C5 -> S3 本地最小验收；Server 转发验收应标记为环境未满足，而不是固件 build 失败。

## 9. CSI 默认断开验收

CSI 当前不做阶段 B 主链路联调。本节只确认默认构建不启动、不触发、不上报。

### 9.1 默认关闭验收

不改默认宏时重新构建/刷机：

```bash
cd /Users/zhiqin/ESP-111/ESPC51 && idf.py build
cd /Users/zhiqin/ESP-111/ESPC52 && idf.py build
cd /Users/zhiqin/ESP-111/ESPS3 && idf.py build
```

验收标准：

- C5 monitor 只出现 `CSI service disabled by MAIN_ENABLE_CSI_SERVICE`，不出现 `CSI summary task started`。
- S3 monitor 出现 `CSI trigger disabled by GATEWAY_CONFIG_ENABLE_CSI_TRIGGER` 和 `CSI trigger enabled=0 result_ingest=0`，不出现 `CSI trigger task started`。
- 手动向 `/local/v1/csi/result` 注入预留 JSON 时，S3 只返回本地 ACK 并记录 `ingest disabled by GATEWAY_CONFIG_ENABLE_CSI_RESULT_INGEST`，不产生 `payload_type=csi.motion` 上云。
- register、heartbeat、BME690、voice、command 仍按第 4-8 节通过。

### 9.2 未来接入顺序

CSI 后续接入必须按以下顺序推进：

- 先完成 S3 + 单 C5 基础联调，确认 register、heartbeat、BME、voice、command 稳定。
- 再完成双 C5 基础联调，确认 C51/C52 身份、短 `id`、registry 和上报互不覆盖。
- 最后才逐步启用 CSI 独立测试，先验证离线样本、窗口统计、result codec 和日志边界。
- CSI 独立测试通过后，再由用户明确确认是否手动接入 C5 启动链路、S3 trigger、S3 result ingest 和 Server/Dashboard 上报。

### 9.3 手动本地 result 注入

如果暂时只想确认预留 handler 不破坏本地 HTTP，可以注入一条 placeholder result。该检查不代表真实 CSI 算法、C5 Wi-Fi CSI callback、窗口统计或主链路上报通过：

```bash
curl -s -X POST http://192.168.4.1/local/v1/csi/result \
  -H 'Content-Type: application/json' \
  -d '{"p":2,"id":1,"t":5,"u":123456,"q":88,"v":[1,0.73,0.0182,-58,96,1781100000000]}'
```

预期 S3 返回本地 ACK；默认构建不会在 Server 侧看到 `csi.motion`，dashboard occupancy 也不会因为该注入变为 available。

## 10. 失败排查

| 现象 | 优先检查 |
|---|---|
| C5 无法连接 WiFi | S3 是否已启动 SoftAP；SSID/password 是否仍为 `SensaiHub_S3_01/sensaihub123`；C5 是否刷错工程 |
| `invalid_device_id` 或本地 `e=3` | C5 短 `id` 是否为 1/2；S3 allowlist 是否包含映射后的完整 `device_id`；C52 是否被旧 NVS 覆盖，必要时 `erase-flash` |
| 两块 C5 被识别为同一设备 | C52 monitor 中的 `device_id`；旧 NVS 优先级高于编译默认值 |
| BME 本地 accepted 但 Server 没数据 | S3 STA、server 地址、server 可用性；看 S3 monitor/offline policy 区分 `gateway_offline/timeout/server_unavailable/server_rejected` |
| voice turn 失败 | S3 是否 busy；PCM body 是否过大；server `/api/voice/turn` 是否可达 |
| command pending 一直为空 | server 队列为空或 S3 server polling 失败；看 S3 monitor 的 server polling 日志 |
| 默认构建 CSI 没有效果 | 预期行为；三个 CSI 宏默认都是 0，不启动 callback、trigger 或 result ingest |
| 手动注入 CSI 后 Server 没数据 | 预期行为；`GATEWAY_CONFIG_ENABLE_CSI_RESULT_INGEST=0` 时只保留本地 ACK |
| 未来开启 CSI 后没有结果 | 先回到 9.2 顺序；基础联调未完成前不要继续扩大 CSI 接入 |
| CSI 结果互相覆盖 | 检查 C51/C52 短 `id` 和 `device_id`，C52 旧 NVS 可能覆盖默认身份，必要时 `erase-flash`；CSI 接入前先完成双 C5 基础联调 |
| 日志出现 raw CSI 大数组 | 不符合阶段 B 验收；应立即关闭 CSI 开关并回滚相关日志/上传代码 |
| LCD 没有显示 | 预期行为；当前只有 display placeholder，不接真实 LCD 驱动 |

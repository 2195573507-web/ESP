# ESP-111 固件架构与清理记录

更新日期：2026-06-11

## 范围

本文件记录 `/Users/zhiqin/ESP-111` 中三套 ESP-IDF 固件的清理后结构：

- `ESPC51`：ESP32-C5 终端固件，默认 `device_id=sensair_shuttle_01`。
- `ESPC52`：ESP32-C5 终端固件，逻辑代码与 `ESPC51` 保持一致；默认 `device_id=sensair_shuttle_02`，用于两块 C5 同时联调。
- `ESPS3`：ESP32-S3 本地网关固件，提供 SoftAP、`/local/v1` 本地 HTTP、子设备注册、传感/语音/命令转发。

本轮未修改 `managed_components/`，未修改 `ESP-server/`，未删除 sensor、voice、command 的本地接口。`/local/v1` 路径保持不变，但 C5 <-> S3 JSON body 已简化为轻量短字段；S3 <-> Server 仍保持完整 v1 JSON 协议与原有 `/api/*` 路径。协议公共组件现在内置在三套固件工程内，三套工程可脱离顶层 `shared_components/` 独立构建。

## 清理原则

- C5 侧只保留终端职责：WiFi 连接 S3、`server_comm`、BME690、Mic/VAD/wake、speaker、`server_voice`、`device_protocol`、command、runtime、display placeholder、CSI 独立/占位文件。
- S3 侧只保留网关职责：`gateway_wifi`、`local_http_server`、`child_registry`、`protocol_adapter`、`server_client`、`sensor_aggregator`、`voice_proxy`、`command_router`、`offline_policy`、`gateway_orchestrator`；`csi_placeholder_gateway` 只作为预留模块保留，默认不由启动链路初始化。
- CSI 当前为预留/独立模块：C5 默认不注册 WiFi CSI callback、不启动采集任务、不上传 CSI result；S3 默认不定时 ping C5、不把 `/local/v1/csi/result` 接入 `sensor_aggregator` 或 Server 上报。
- C5/S3 共享的完整 v1 常量、轻量本地字段、类型码、`/local/v1` 路径、音频格式、默认网关身份放入三套工程各自的 `components/esp111_protocol_common`。`shared_components/esp111_protocol_common` 仅保留为归档/参考，不再作为构建依赖；后续协议修改必须同步三份内置头文件。

## 启动链路

### ESPC51 / ESPC52

`main/app_main` 创建 `app_startup` 任务，然后进入：

```text
app_main
  -> app_startup_task
    -> app_orchestrator_start
      -> wifi_manager_init
      -> wifi_connect_to_ap
      -> system_service_init
      -> bme_sensor_service_start
      -> voice_chain_start
```

`voice_chain_start` 内部继续启动 wake、audio player、server voice client、Mic ADC/VAD。`system_service_init` 启动 register/heartbeat/status/command polling 相关任务。

### ESPS3

`main/app_main` 创建 `gateway_startup` 任务，然后进入：

```text
app_main
  -> gateway_startup_task
    -> gateway_orchestrator_start
      -> offline_policy_init
      -> child_registry_init
      -> command_router_init
      -> sensor_aggregator_init
      -> voice_proxy_init
      -> gateway_wifi_start
      -> local_http_server_start
```

默认构建中 `GATEWAY_CONFIG_ENABLE_CSI_TRIGGER=0`，所以 `gateway_orchestrator_start()` 不调用 `csi_placeholder_gateway_init()`。

## 模块审计

### ESPC51 / ESPC52

| 模块 | 编译 | include | 运行时调用 | 结论 |
|---|---:|---:|---:|---|
| `wifi` | 是 | 是 | `app_orchestrator_start -> wifi_manager_init/wifi_connect_to_ap` | 保留，C5 连接 S3 SoftAP |
| `server_comm` | 是 | 是 | BME、voice、command、wake prompt 请求共用 | 保留，C5 本地 HTTP 客户端 |
| `sensor_domain/bme690` | 是 | 是 | `bme_sensor_service_start` | 保留，sensor 上报主链路 |
| `sensor_domain/csi_placeholder` | 是 | 否，默认启动链路不 include | `MAIN_ENABLE_CSI_SERVICE=0` 时不编译 orchestrator 内的 init/start 调用 | 保留为独立/占位模块，不默认注册 CSI callback 或上传 |
| `mic` | 是 | 是 | `voice_chain_start -> mic_adc_test_start` | 保留，Mic/VAD 链路 |
| `wake` | 是 | 是 | `voice_chain_start -> local_wake_word_init` | 保留，wake 与 prompt cache |
| `speaker` | 是 | 是 | `voice_chain_start -> audio_player_init` | 保留，S3/服务端 PCM 播放 |
| `server_voice` | 是 | 是 | `voice_chain_start -> server_voice_client_init` | 保留，`/local/v1/voice/turn` |
| `device_protocol` | 是 | 是 | BME、command、voice metadata 使用 | 保留，统一设备 metadata |
| `command_domain/system_command` | 是 | 是 | `system_service_init` | 保留，register/heartbeat/status/command |
| `runtime` | 是 | 是 | voice/wake 运行期暂停恢复使用 | 保留，voice 与 sensor 协调 |
| `display_placeholder` | 是 | 是 | `system_service_init -> screen_service_init` | 保留，LCD 上层占位 |
| `app_orchestrator` | 是 | 是 | `app_main` 直接调用 | 保留，C5 启动编排 |
| `terminal_config` | 随 `Middlewares` 编译上下文使用 | 是 | `server_comm_config`、BME、WiFi 使用 | 保留，设备身份和 S3 连接配置 |
| `app_config` | header-only | 是 | `app_main`、stack/log 配置使用 | 保留，构建/运行配置 |
| `app_time_sync` | 是 | 是 | metadata 读取 uptime/time sync 状态 | 保留，但 `app_time_sync_once` 当前未在启动链路调用 |
| `BSP/IIC` | 是 | 是 | BME690 driver 使用 | 保留，硬件支撑 |
| `BSP/IIS` | 是 | 是 | speaker 使用 | 保留，硬件支撑 |
| `server_upload_bridge` | 是，旧组件 | 否，只有自引用 | 无 | 已移出 active components |

### ESPS3

| 模块 | 编译 | include | 运行时调用 | 结论 |
|---|---:|---:|---:|---|
| `gateway_config` | 是 | 是 | `gateway_orchestrator_start` 和各网关模块使用 | 保留，网关身份/SoftAP/云端配置 |
| `gateway_wifi` | 是 | 是 | `gateway_orchestrator_start -> gateway_wifi_start` | 保留，SoftAP + STA |
| `local_http_server` | 是 | 是 | `gateway_orchestrator_start -> local_http_server_start` | 保留，`/local/v1` 契约入口 |
| `child_registry` | 是 | 是 | register/heartbeat/voice/command 使用 | 保留，子设备登记与在线状态 |
| `protocol_adapter` | 是 | 是 | register/status/sensor 解析与转发构造；CSI 仅保留解析预留 | 保留，本地协议适配 |
| `server_client` | 是 | 是 | sensor/voice/command 上云使用 | 保留，S3 到 `ESP-server` 客户端 |
| `sensor_aggregator` | 是 | 是 | `/local/v1/status`、`/local/v1/sensor` 调用 | 保留，传感/状态转发 |
| `voice_proxy` | 是 | 是 | `/local/v1/voice/turn` 调用 | 保留，C5 语音代理到云端 |
| `command_router` | 是 | 是 | `/local/v1/commands/*` 调用 | 保留，命令队列与 ack 转发 |
| `offline_policy` | 是 | 是 | 网关启动、sensor/voice/command 上云结果记录 | 保留，离线/错误状态 |
| `csi_placeholder_gateway` | 是 | 是，`local_http_server` handler 预留 | 默认只 ACK/记录预留 result，不写 `sensor_aggregator`，不上传 Server；trigger 默认不初始化 | 保留为 S3 CSI 占位接收边界 |
| `gateway_orchestrator` | 是 | 是 | `app_main` 直接调用 | 保留，S3 启动编排 |
| C5 旧模块：`app_orchestrator`、`wifi`、`mic`、`wake`、`speaker`、`server_voice`、`voice_domain`、`sensor_domain`、`command_domain`、`device_protocol`、`display_placeholder`、`runtime`、`server_comm` | 否 | 仅旧文件内部自引用 | 无 | 已移出 active `Middlewares` |
| 旧 top-level：`BSP`、`app_time_sync`、`server_upload_bridge` | 否 | 无 active 引用 | 无 | 已移出 active `components` |

## 清理后目录

```text
ESP-111/
  shared_components/
    esp111_protocol_common/
      # archived reference only; firmware builds no longer depend on this path
      CMakeLists.txt
      include/esp111_protocol_common.h
  ESPC51/
    main/
    components/
      esp111_protocol_common/
        CMakeLists.txt
        include/esp111_protocol_common.h
      app_config/
      app_time_sync/
      BSP/{IIC,IIS}/
      Middlewares/
        app_orchestrator/
        terminal_config/
        wifi/
        server_comm/
        sensor_domain/{bme690,csi_placeholder}/
        mic/
        wake/
        speaker/
        server_voice/
        voice_domain/
        device_protocol/
        command_domain/system_command/
        runtime/
        display_placeholder/
  ESPC52/
    same as ESPC51, including components/esp111_protocol_common; device identity/config may differ
  ESPS3/
    main/
    components/
      esp111_protocol_common/
        CMakeLists.txt
        include/esp111_protocol_common.h
      app_config/
      Middlewares/
        gateway_config/
        gateway_wifi/
        local_http_server/
        child_registry/
        protocol_adapter/
        server_client/
        sensor_aggregator/
        voice_proxy/
        command_router/
        offline_policy/
        csi_placeholder_gateway/
        gateway_orchestrator/
  archive/legacy_modules_20260610/
    ESPC51/components/server_upload_bridge/
    ESPC52/components/server_upload_bridge/
    ESPS3/components/{BSP,app_time_sync,server_upload_bridge}/
    ESPS3/components/Middlewares/{old C5-side modules}/
```

## 接口契约固化

本轮契约以三套工程内置的 `components/esp111_protocol_common/include/esp111_protocol_common.h` 为固化源，当前三份内容必须保持一致。`shared_components/esp111_protocol_common/include/esp111_protocol_common.h` 保留为归档/参考，不能再作为 `idf.py build` 的外部公共组件依赖。C5 只连 ESPS3 SoftAP，默认网关地址为 `192.168.4.1`，S3 再按需转发到 `ESP-server`。本轮未修改 `ESP-server` 业务代码。

协议分层：

- C5 <-> S3：轻量 JSON，本地字段为 `id/t/u/v/cid/c/a/ok/e/cmds`。
- S3 <-> Server：完整 v1 JSON，路径保持 `/api/device/v1/ingest`、`/api/voice/turn`、`/api/commands/pending`、`/api/commands/{id}/ack`。
- 适配边界：`ESPS3/components/Middlewares/protocol_adapter` 把本地短字段映射成完整 `device_id`、`payload_type`、`payload`、错误码字符串和命令字符串。

### 三端职责

| 端 | 默认身份 | 当前职责 | 明确不做 |
|---|---|---|---|
| `ESPC51` | `device_id=sensair_shuttle_01`，`gateway_id=sensair_s3_gateway_01` | 连接 S3 SoftAP；注册、心跳、状态、BME690 上传；Mic/VAD 发起语音 turn；轮询并执行少量系统/display 命令 | 不直连公网 server；不运行 CSI 算法；不接真实 LCD 底层 |
| `ESPC52` | `device_id=sensair_shuttle_02`，`gateway_id=sensair_s3_gateway_01` | 与 `ESPC51` 使用同一协议和中间件代码，只在工程配置层区分默认设备身份 | 不分叉业务协议；不引入单独 server 路径 |
| `ESPS3` | `gateway_id=sensair_s3_gateway_01`，SoftAP `SensaiHub_S3_01` | SoftAP + 本地 HTTP；子设备 allowlist/registry；sensor/status 转发；voice proxy；command router；CSI 接收占位 | 不实现真实 CSI；不做本地 ASR/LLM/TTS；不执行 C5 display 命令 |

### 身份与公共字段

| 名称 | 语义 | 当前取值/来源 |
|---|---|---|
| `gateway_id` | S3 网关整机身份 | `sensair_s3_gateway_01`，C5 与 S3 必须一致 |
| `id` | C5 本地短身份 | C51 为 `1`，映射 `sensair_shuttle_01`；C52 为 `2`，映射 `sensair_shuttle_02` |
| `device_id` | 完整 C5 终端整机身份 | 不再放入 C5 <-> S3 轻量 JSON body；由 S3 按 `id` 还原后用于 registry、voice header 和 S3 -> Server JSON |
| `sensor_id` | C5 内部传感模块身份 | BME690 payload 内为 `bme690_01`，不能代替整机 `device_id` |
| `target_device_id` | S3 command router 给 C5 下发命令时的目标 | 来自 server pending command 或本地队列，必须匹配 allowlist |
| `u` | C5 uptime_ms | S3 映射为完整 v1 `uptime_ms` |
| `seq` | S3 命令队列序号 | C5 <-> S3 数据上报不再依赖本地 body `seq`；命令 pending 中仍可带 S3 queue `seq` 供 ack/log 使用 |

C5 HTTP header 仍保留 `X-Schema-Version`、`X-Device-Id`、`X-Gateway-Id`、`X-Device-Type`、`X-Firmware-Version`、`X-Request-Seq`、`X-Esp-Uptime-Ms`、`X-Time-Synced`、`X-Payload-Type`，可选 `X-Esp-Time-Ms` 与 `X-Room-Id`，用于诊断和 voice 代理兼容；本地 JSON body 不再携带完整 envelope。

### 本地轻量 JSON 示例

```json
{"id":1,"t":"reg","u":12345}
```

```json
{"id":1,"t":"hb","u":22345}
```

```json
{"id":1,"t":"st","u":32345,"v":[1]}
```

```json
{"id":1,"t":"bme","u":42345,"v":[24.12,51.30,1008.42,12500.00,82,11000.00,1.1364,72,10,1,1,64]}
```

BME `v` 数组顺序固定为：

`temperature_c`、`humidity_percent`、`pressure_hpa`、`gas_resistance_ohm`、`air_quality_score`、`gas_baseline_ohm`、`gas_ratio`、`gas_score`、`humidity_score`、`baseline_ready`、`warmup_done`、`sample_count`。

```json
{"ok":1,"id":1,"cmds":[{"cid":"cmd-001","c":2,"seq":7,"ttl_ms":30000,"a":{"text":"hello","ttl_ms":5000}}]}
```

```json
{"id":1,"cid":"cmd-001","ok":1,"e":0}
```

错误码数字映射：`0=none`、`1=command_failed`、`2=unsupported_command`、`3=invalid_command_payload`、`4=timeout`、`255=unknown`。

### C5 -> S3 本地接口

| 方法与路径 | C5 message/body | 超时 | S3 成功响应 | 主要错误码 | 边界 |
|---|---|---:|---|---|---|
| `POST /local/v1/register` | `{"id":1,"t":"reg","u":...}` | 5000 ms | `200 {"ok":1,"id":1}` | `e=3/255` | S3 按 `id` 补全 `device.register`、`device_id`、alias、capabilities，写入 registry |
| `POST /local/v1/heartbeat` | `{"id":1,"t":"hb","u":...}` | 5000 ms | `200 {"ok":1,"id":1}` | `e=3/255` | S3 映射 `device.heartbeat` 并刷新在线时间；S3 30s 心跳超时判离线 |
| `POST /local/v1/status` | `{"id":1,"t":"st","u":...,"v":[1]}` | 5000 ms | `202 {"ok":1,"id":1}` | `e=1/3/255` | S3 补完整 status payload 后转成 server ingest JSON |
| `POST /local/v1/sensor` | `{"id":1,"t":"bme","u":...,"v":[...]}` | 5000 ms | `202 {"ok":1,"id":1}` | `e=1/3/255` | BME 数据主链路；S3 展开 `v` 为完整 sensor payload |
| `POST /local/v1/csi/result` | 预留 `{"id":1,"t":"csi","u":...,"v":[...]}` 形状 | 5000 ms 级本地 JSON | `202 {"ok":1,"id":1}` | `e=3/255` | CSI placeholder：默认 C5 不产生该请求；S3 默认只 ACK/打日志，不写 aggregator、不转发 Server |
| `POST /local/v1/voice/turn` | raw PCM16 mono 16 kHz；`Content-Type=audio/L16; rate=16000; channels=1`；header 含 `X-Audio-Format=pcm_s16le_mono_16k` | 30000 ms | S3 流式回传 PCM，C5 speaker 播放 | `invalid_device_id`、`voice_busy`、`payload_too_large`、`invalid_voice_payload`、`gateway_offline`、`timeout`、`server_unavailable`、`server_rejected` | voice proxy：S3 不做 ASR/LLM/TTS，只代理到 server |
| `GET /local/v1/commands/pending?id=1&limit=1` | query 使用短 `id` | 5000 ms | `200 {"ok":1,"id":1,"cmds":[...]}` | `e=1/3/255` | C5 拉取待执行命令；S3 先按完整 `device_id` 从 server 同步，再压缩为本地命令 |
| `POST /local/v1/commands/{id}/ack` | `{"id":1,"cid":"cmd-001","ok":1,"e":0}` | 5000 ms | `200 {"ok":1,"id":1}` | `e=1/3/255` | C5 命令执行结果回传；S3 映射为 server ack 字符串错误码 |

`/local/v1/voice/prompt-cache` 与 `/local/v1/time/now` 当前不参与最小联调：C5 侧保留调用/配置代码或常量，S3 当前没有活动 handler；默认启动链路也不调用 `app_time_sync_once()` 或 `wake_prompt_cache_start_async()`。

### S3 -> C5 本地回包与命令

| 场景 | S3 输出 | C5 行为 |
|---|---|---|
| register/heartbeat/csi ok | `{"ok":1,"id":1}` | register/heartbeat 进入主状态；CSI 默认只作为预留 ACK，不影响后台服务 |
| status/sensor accepted | `{"ok":1,"id":1}` | 本地 HTTP 成功即认为本次交给 S3；server 转发降级看 S3 monitor/offline policy |
| voice turn | PCM16 mono 16 kHz response chunk | C5 `server_voice_client` 解码 S16LE 并写 speaker；非 2xx 视为 voice turn 失败 |
| commands pending | `cmds[]` 中每项含 `cid`、`c`、`seq`、`ttl_ms`、`a` | C5 当前执行 `c=1 device.noop`、`c=2 lcd.show_text/display.show_text`；其他命令回 unsupported |
| unsupported command | C5 ack `{"ok":0,"e":2}` | S3 映射为 `unsupported_command` 并转发 ack 给 server |

### S3 本地映射规则

| 本地短字段 | S3 映射 |
|---|---|
| `id=1` | `device_id=sensair_shuttle_01`，alias `SensaiShuttle` |
| `id=2` | `device_id=sensair_shuttle_02`，alias `SensaiShuttle02` |
| `t=reg` | `message_type=device.register`，payload 补 `protocol_version=local-compact-json-v1`、`firmware_role=terminal`、支持命令列表 |
| `t=hb` | `message_type=device.heartbeat`，payload 补 `wifi_connected=true`、`role=terminal` |
| `t=st` | `message_type=device.status`，payload 补 `role`、`gateway_ip`、`voice_client`、`command_poll` |
| `t=bme` | `message_type=sensor.bme690`，按固定 `v` 顺序展开为完整 BME payload |
| `t=csi` | `message_type=csi.result`，只补 placeholder payload；默认不进入 aggregator/server |
| `c=1/2/3/4/5/255` | `device.noop`、`lcd.show_text`、`speaker.play_audio`、`speaker.set_volume`、`config.set`、unsupported |
| `e=0/1/2/3/4/255` | 空错误、`command_failed`、`unsupported_command`、`invalid_command_payload`、`timeout`、`unknown` |

### S3 -> Server 上云接口

| 方法与路径 | S3 请求 | 超时 | 降级/错误策略 |
|---|---|---:|---|
| `POST /api/device/v1/ingest` | JSON：`schema_version`、`payload_type`、`device_id`、`gateway_id`、`source=s3_gateway`、`seq`、可选 `room_id/firmware_version/timestamp_ms/uptime_ms`、原始 `payload` | 8000 ms | 非 2xx 或 STA 未连通时返回 `gateway_offline`、`timeout`、`server_unavailable`、`server_rejected`，本地 `/local/v1/status|sensor` 仍 `202` |
| `POST /api/voice/turn` | raw PCM；header `Content-Type`、`X-Audio-Format`、`X-Device-Id`、`X-Gateway-Id` | 20000 ms | S3 单会话代理，最大 body `384 KiB`；失败时回 C5 JSON 错误 |
| `GET /api/commands/pending?device_id=...` | query 使用目标 C5 `device_id` | 8000 ms | 成功后写入 S3 本地 command queue；失败时 C5 pending 仍返回轻量空队列，错误细节看 S3 monitor/offline policy |
| `POST /api/commands/{id}/ack` | JSON：`status`、`error_code`、`error_message`、`result.applied`、`result.gateway_id` | 8000 ms | S3 记录 offline policy；本地 ack 已收到时不阻塞 C5 |

### Placeholder 与代理边界

- CSI placeholder：C5 `csi_service`/`csi_server_client` 只保留独立模块能力，默认不由 orchestrator 调用，因此不采集 raw CSI、不生成特征、不上传结果；S3 `csi_placeholder_gateway` 默认只接受轻量 JSON 映射后的 envelope 并打日志，不写 aggregator、不上云真实 CSI。后续真实 CSI 优先使用 `application/octet-stream` 或 base64 compact frame，避免把 CSI 原始帧放成 JSON 大数组。
- Display placeholder：C5 `screen_service` 和 `ai_screen_bridge` 只执行上层命令结构并返回 `ESP_OK`/日志，不接真实 LCD 驱动；`lcd.show_text` 与 `display.show_text` 只是稳定命令入口。
- Voice proxy：C5 负责 Mic/VAD、PCM 上传、speaker 播放；S3 只做 body 缓冲、单会话互斥、server 转发和 PCM 回流，不新增语音业务逻辑。
- Command router：S3 只缓存/转发命令并把 server 命令名映射到 C5 可理解的类型；真正执行发生在 C5，当前只支持 noop 和 display text。

### 超时与容量策略

| 项 | 当前值 |
|---|---:|
| C5 连接 S3 SoftAP | 15000 ms |
| C5 JSON 本地 HTTP 默认超时 | 5000 ms |
| C5 BME/register/heartbeat/status/command HTTP 超时 | 5000 ms |
| C5 voice turn 超时 | 30000 ms |
| C5 wake prompt cache 下载超时 | 5000 ms，当前默认不启动 |
| S3 local JSON body 上限 | 4096 bytes |
| S3 voice upload body 上限 | 384 KiB |
| S3 voice busy 锁等待 | 50 ms |
| S3 child heartbeat 在线窗口 | 30000 ms |
| S3 -> Server JSON 超时 | 8000 ms |
| S3 -> Server voice 超时 | 20000 ms |

## 后续可清理项

- C5 的 `app_time_sync_once()` 当前未在启动链路调用，只由 metadata 读取 uptime/time sync 状态；后续可决定是否接入 S3 `/local/v1/time/now` 或移除 one-shot HTTP 同步代码。
- C5 的 `csi_placeholder` 和 `csi_phase_a` 当前只作为独立/占位模块保留；后续接入顺序必须先完成 S3+单 C5 基础联调，再完成双 C5 基础联调，最后才逐步启用 CSI 独立测试。CSI 独立测试通过后，才考虑手动接入主链路。
- `display_placeholder` 目前只保持 LCD 上层命令接口，不接真实屏幕底层；如果接入 LCD，应继续保持 system command 接口兼容。
- `archive/legacy_modules_20260610/` 中的旧模块可在确认一段时间无回滚需求后删除。

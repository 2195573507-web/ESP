# ESP-111 设备职责与 S3 网关迁移计划

## 0. 审计边界

本轮审计对象为 `/Users/zhiqin/ESP-111` 下的 `ESPC51`、`ESPC52`、`ESPS3`、`ESP-server`。执行方式为只读扫描固件、后端和配置文件，并新增本文件。未修改 `ESPC51`、`ESPC52`、`ESPS3`、`ESP-server` 的源码、`sdkconfig`、`sdkconfig.defaults`、`CMakeLists.txt`、`partitions.csv`、`idf_component.yml`、`public/`、`db/`、`package.json` 或真实数据库。

实际工作区说明：附件中写的是 `/Users/zhiqin/Projects/ESP-111`，本机当前可访问并已审计的目录是 `/Users/zhiqin/ESP-111`。顶层不是 git 仓库；`ESP-server` 自身是独立 git 仓库。

## 1. 当前目录审计摘要

### 1.1 ESPC51

`ESPC51` 是当前 C5 SensaiShuttle 终端固件工程。入口为 `ESPC51/main/main.c` 的 `app_main()`，实际启动逻辑在 `app_startup_task()` 中调用 `app_orchestrator_start()`。主 CMake 为 `ESPC51/CMakeLists.txt`，项目名为 `00_Learn`，额外组件目录包含 `components/Middlewares`、`components/Middlewares/server_comm`、`components/BSP/IIS`、`components/BSP/IIC`。

关键文件和模块：

| 类别 | 实际路径 / 证据 |
|---|---|
| 入口文件 | `ESPC51/main/main.c` |
| main 组件 | `ESPC51/main/CMakeLists.txt`，依赖 `Middlewares app_config freertos log` |
| 组件根 | `ESPC51/components/Middlewares/CMakeLists.txt` |
| 顶层 CMake | `ESPC51/CMakeLists.txt` |
| sdkconfig | `ESPC51/sdkconfig` |
| sdkconfig.defaults | `ESPC51/sdkconfig.defaults` |
| 分区表 | `ESPC51/partitions.csv` |
| idf_component.yml | `ESPC51/main/idf_component.yml`，直接依赖 `espressif/esp-sr` |
| 网络模块 | `components/Middlewares/wifi/wifi_manager.c`、`wifi_credentials.h`，当前为 STA 扫描已知 WiFi |
| 服务器通信 | `components/Middlewares/server_comm/*`，硬编码 ESP-server base URL 和 device_id 默认值 |
| 传感器 | `components/Middlewares/sensor_domain/bme690/*`、`csi_placeholder/*` |
| 麦克风 | `components/Middlewares/mic/mic_adc_test.*`、`mic_adc_pcm.*`、`mic_vad.*` |
| 喇叭 | `components/Middlewares/speaker/*`、`components/BSP/IIS/*` |
| 屏幕 | `components/Middlewares/display_placeholder/*` |
| 状态机 / orchestrator | `components/Middlewares/app_orchestrator/app_orchestrator.c`、`voice_domain/voice_chain.c`、`runtime/app_runtime.*` |
| 服务器语音 | `components/Middlewares/server_voice/*` |
| 命令轮询 | `components/Middlewares/command_domain/system_command/*` |
| 当前 target | `ESPC51/sdkconfig` 为 `CONFIG_IDF_TARGET="esp32c5"`、`CONFIG_IDF_TARGET_ESP32C5=y` |

当前职责事实：`app_orchestrator_start()` 初始化 STA WiFi，等待 WiFi 稳定后调用 `app_time_sync_once(APP_TIME_SYNC_SERVER_URL)`，启动 `wake_prompt_cache_start_async()`，启动 `system_service_init()`，启动 `bme_sensor_service_start()`，再启动 `voice_chain_start()`。注释和代码均显示当前链路是 `Mic/VAD -> server_voice_client POST /api/voice/turn -> speaker 播放服务器裸 PCM -> 恢复 Mic`。

### 1.2 ESPC52

`ESPC52` 与 `ESPC51` 在排除 `build/`、`managed_components/`、`.DS_Store` 后，`diff -qr ESPC51 ESPC52` 无差异输出。它当前不是独立业务实现，而是同一套 C5 终端固件副本。

关键文件和模块与 `ESPC51` 相同：

| 类别 | 实际路径 / 证据 |
|---|---|
| 入口文件 | `ESPC52/main/main.c` |
| main 组件 | `ESPC52/main/CMakeLists.txt` |
| 组件根 | `ESPC52/components/Middlewares/CMakeLists.txt` |
| 顶层 CMake | `ESPC52/CMakeLists.txt` |
| sdkconfig | `ESPC52/sdkconfig` |
| sdkconfig.defaults | `ESPC52/sdkconfig.defaults` |
| 分区表 | `ESPC52/partitions.csv` |
| idf_component.yml | `ESPC52/main/idf_component.yml` |
| 网络、传感器、Mic、speaker、screen、orchestrator、server_comm、server_voice、command | 与 `ESPC51` 同名路径 |
| 当前 target | `ESPC52/sdkconfig` 为 `CONFIG_IDF_TARGET="esp32c5"`、`CONFIG_IDF_TARGET_ESP32C5=y` |

### 1.3 ESPS3

`ESPS3` 当前不是 S3 网关工程，而是 C5 终端工程副本。排除 `build/`、`managed_components/`、`.DS_Store` 后，`diff -qr ESPC51 ESPS3` 无差异输出。源码、配置、分区、依赖和注释仍以 C5 终端为中心。

关键文件和模块：

| 类别 | 实际路径 / 证据 |
|---|---|
| 入口文件 | `ESPS3/main/main.c`，注释仍写 ESP32-C5 启动 |
| main 组件 | `ESPS3/main/CMakeLists.txt` |
| 组件根 | `ESPS3/components/Middlewares/CMakeLists.txt` |
| 顶层 CMake | `ESPS3/CMakeLists.txt`，项目名仍 `00_Learn` |
| sdkconfig | `ESPS3/sdkconfig`，当前 target 仍 `esp32c5` |
| sdkconfig.defaults | `ESPS3/sdkconfig.defaults`，内容与 C5 相同 |
| 分区表 | `ESPS3/partitions.csv`，内容与 C5 相同 |
| idf_component.yml | `ESPS3/main/idf_component.yml`，直接依赖 `espressif/esp-sr` |
| dependencies.lock | `ESPS3/dependencies.lock` 末尾 `target: esp32c5` |
| 网络模块 | `ESPS3/components/Middlewares/wifi/*`，当前只实现 STA |
| 服务器通信 | `ESPS3/components/Middlewares/server_comm/*`，仍直连 ESP-server |
| 传感器/Mic/喇叭/屏幕 | 全量保留 C5 BME690、Mic ADC/VAD、IIS/PDM speaker、display_placeholder |
| 当前构建缓存 | `ESPS3/build/CMakeCache.txt` 中 `IDF_TARGET:STRING=esp32c5`，toolchain 为 `toolchain-esp32c5.cmake` |

### 1.4 ESP-server

`ESP-server` 是现有 Node/Express 后端，本计划只作为接口契约参考，不修改。入口为 `ESP-server/server.js`，`package.json` 的 `main` 为 `server.js`。依赖包括 `express`、`sqlite3`、`dotenv`、`cors`。

关键结构：

| 类别 | 实际路径 / 证据 |
|---|---|
| 入口文件 | `ESP-server/server.js` |
| package | `ESP-server/package.json` |
| 数据库 | `ESP-server/db/database.db`，本轮未读取写入真实 DB |
| 路由 | `ESP-server/src/routes/*` |
| 设备/命令/语音/LLM 服务 | `src/services/*`、`src/commands/*`、`src/voice/*`、`src/llm/*` |
| 时间同步 | `ESP-server/server-time-sync/timeSync.js` |
| API 文档 | `ESP-server/docs/api.md` |
| 前端边界 | `ESP-server/public/*`，由 `server.js` 以静态资源挂载，本计划不修改 |

当前可参考接口包括 `/api/device/v1/ingest`、`/api/voice/turn`、`/api/voice/prompt-cache`、`/api/voice/prompt`、`/api/devices/capabilities`、`/api/commands/pending`、`/api/commands/:command_id/ack`、`/api/time/now`、legacy `/sensor`、`/sensor/latest`、`/sensor/history`。

## 2. 各设备职责边界

### 2.1 ESPC51 / ESPC52 未来职责

两个 C5 应定位为本地 SensaiShuttle terminal。默认正式模式只连接 `ESPS3` SoftAP，不连接家庭 WiFi，不直接访问云端 ESP-server。

C5 应负责：

- 只连接 `ESPS3` SoftAP，STA only。
- 本地 BME690 等传感器采集和必要校准。
- 本地唤醒、Mic ADC/VAD、录音窗口、PCM 打包。
- 本地屏幕显示与喇叭播放。
- 向 S3 上报 `sensor`、`status`、`heartbeat`、`voice`。
- 执行 S3 下发的 `lcd`、`speaker`、`config`、`control` 命令。
- 维护本地状态机、半双工资源 gate、基础离线提示。
- 保留调试兜底模式，但默认不直连 ESP-server。

C5 不应负责：

- 直接访问 ESP-server。
- 家庭 WiFi 配网和多 WiFi 扫描选择。
- ASR、LLM、TTS 供应商逻辑。
- 用户画像、每日/每周总结、长期记忆。
- Dashboard 逻辑。
- 多设备拓扑管理、子设备注册表、跨设备语音互斥。

### 2.2 ESPS3 未来职责

`ESPS3` 应定位为本地幕后大脑 / 本地网关 / 区域协调器，而不是第三个 SensaiShuttle 终端。

S3 应负责：

- ESP32-S3 target 工程配置。
- `WIFI_MODE_APSTA`。
- SoftAP 给两个 C5 接入，建议固定 `192.168.4.1/24`。
- STA 连接家庭 WiFi。
- 本地 HTTP 服务，后续可选 WebSocket 服务。
- C5 设备注册表、allowlist、心跳和在线状态。
- 传感器数据汇聚与转发。
- 语音会话互斥锁，同一时间只允许一个 C5 进行 voice turn。
- 转发 C5 语音请求到 ESP-server，接收返回 PCM 并分发给指定 C5。
- 本地命令队列，向 C5 分发 LCD/喇叭/配置/控制命令。
- 离线降级策略和本地规则引擎预留。
- 日志脱敏、server 连接状态维护、重试和错误映射。

### 2.3 ESP-server 职责

ESP-server 保持现状不改，只作为云端 AI、SQLite 数据库、Dashboard、ASR/LLM/TTS、总结、用户画像和命令队列服务。若现有接口不能天然表达 C5/S3 拓扑，本计划仅记录为 `ESPS3` 侧适配层风险，不规划 ESP-server 修改任务。

## 3. ESPS3 从 C5 复制工程带来的问题清单

### 3.1 target / sdkconfig / 构建缓存

明确 C5 残留：

- `ESPS3/sdkconfig` 仍为 `CONFIG_IDF_TARGET="esp32c5"`、`CONFIG_IDF_TARGET_ESP32C5=y`。
- `ESPS3/sdkconfig` 仍为 RISC-V 架构：`CONFIG_IDF_TARGET_ARCH="riscv"`；ESP32-S3 应切到 Xtensa。
- `ESPS3/sdkconfig` 存在 C5 专属项，如 `CONFIG_ESP32C5_REV_MIN_100`、`CONFIG_ESP32C5_UNIVERSAL_MAC_ADDRESSES_FOUR`。
- `ESPS3/dependencies.lock` 写 `target: esp32c5`。
- `ESPS3/build/CMakeCache.txt` 写 `IDF_TARGET:STRING=esp32c5`，toolchain 为 `toolchain-esp32c5.cmake`。
- `ESPS3/build/project_description.json` 写 `target: esp32c5`，且 build paths 指向旧 `/Users/zhiqin/Projects/ESP/Whole-project`，说明构建缓存不可作为当前 S3 工程真相。
- `ESPS3/build/project_elf_src_esp32c5.c` 存在，构建日志也出现 `Creating esp32c5 image` 和 `--chip esp32c5`。

结论：`ESPS3` 不能直接在当前配置上继续开发 S3 网关。必须先执行 target 纠偏和配置重生计划，旧 `sdkconfig`、`dependencies.lock`、`build/` 只能作为审计证据和参考，不能沿用为 S3 配置。

### 3.2 PSRAM / Flash / 分区表

当前问题：

- `ESPS3/sdkconfig` 中 `# CONFIG_SPIRAM is not set`，未启用 PSRAM。
- `ESPS3/sdkconfig` 中 Flash size 为 `CONFIG_ESPTOOLPY_FLASHSIZE="2MB"`。
- `ESPS3/partitions.csv` 与 C5 一样：`nvs 0x6000`、`factory 0x170000`、`model spiffs 0x60000`、`storage spiffs 0x20000`。
- `ESPS3/sdkconfig.defaults` 启用 `CONFIG_SR_WN_WN9S_NIHAOXIAOZHI=y`，分区里有 `model`，这对应 C5 本地唤醒模型而不是 S3 网关核心职责。
- `main/idf_component.yml` 依赖 `espressif/esp-sr`，`dependencies.lock` 拉入 `esp-sr`、`esp-dsp`、`dl_fft`、`cjson`。

对 S3 网关的影响：

- 2MB flash 对 APSTA、HTTP server、voice proxy、命令队列、日志与 OTA 余量不足。
- `model` SPIFFS 可能不需要保留；S3 作为幕后网关不应默认加载 C5 wake model。
- `storage` SPIFFS 是否保留要看本地规则、临时音频、日志缓存需求。若只存配置和设备表，优先用 NVS。
- PSRAM 对 voice buffer、本地 HTTP buffer、并发 C5 会话和未来 WebSocket 更稳，当前未启用会放大大包转发风险。

### 3.3 业务角色残留

`ESPS3` 仍包含 C5 终端业务逻辑：

- `app_orchestrator_start()` 当前按终端启动：STA WiFi、server time sync、wake prompt cache、system_service、BME service、voice_chain。
- `wifi_manager.c` 只创建 `esp_netif_create_default_wifi_sta()`，设置 `WIFI_MODE_STA`，没有 SoftAP、APSTA、DHCP server 固定网关 IP、C5 接入管理。
- `wifi_credentials.h` 仍包含家庭 WiFi SSID/密码明文条目；本文件不复写具体密钥。
- `server_comm_config.h` 仍硬编码 `SERVER_COMM_BASE_URL` 指向公网 ESP-server，`SERVER_COMM_DEVICE_ID` 为 `esp32-c5-whole-001`。
- `device_protocol_metadata.h` 仍定义 `DEVICE_PROTOCOL_DEVICE_TYPE "esp32c5_env_voice_node"`。
- `BME_SENSOR_DEVICE_ID` 仍为 `bme690_01`。
- `server_voice_client` 仍直接打开 `/api/voice/turn`，C5/S3/C5 的本地语音代理不存在。
- `system_server_client` 仍直接轮询 ESP-server 的 `/api/commands/pending` 并执行 `display.show_text`，未改为本地 S3 命令分发。
- `app_main_config.h` 默认打开 `MAIN_ENABLE_MIC_CHAIN=1`、`MAIN_ENABLE_BME_SERVICE=1`、`MAIN_ENABLE_SPEAKER_CHAIN=1`，这不是 S3 最小系统网关的默认职责。

### 3.4 外设和引脚残留

`ESPS3` 仍带 C5 外设假设：

- BME690 I2C：`components/BSP/IIC/iic.h` 固定 `IIC_MASTER_PORT I2C_NUM_0`、SDA `GPIO_NUM_2`、SCL `GPIO_NUM_3`。
- Mic ADC：`components/Middlewares/mic/mic_adc_test.h` 固定 `MIC_ADC_GPIO_NUM 6`、`ADC_UNIT_1`、`ADC_CHANNEL_5`、`ADC_DIGI_OUTPUT_FORMAT_TYPE2`，注释为 ESP32-C5 DMA 格式。
- Speaker：`components/BSP/IIS/iis.h` 写明 `ESP32-C5 IIS/I2S PDM 扬声器 BSP`，PDM CLK `GPIO_NUM_8`、DATA `GPIO_NUM_7`、PA `GPIO_NUM_1`。
- Screen：`display_placeholder/*` 是 placeholder bridge，不是 S3 网关 UI。

这些模块在 `idf.py set-target esp32s3` 后可能出现编译错误或运行不适配，尤其是 ADC continuous 输出格式、GPIO 可用性、I2S/PDM 配置、esp-sr 模型分区和 C5 注释/宏。

### 3.5 需要删除、保留、重命名或抽象的模块

| 模块 | 当前状态 | S3 网关迁移建议 |
|---|---|---|
| `wifi/*` | STA only，家庭 WiFi 扫描 | 重写/重命名为 gateway wifi，支持 APSTA、SoftAP IP、STA 家庭 WiFi、C5 接入事件 |
| `server_comm/*` | 直连 ESP-server 的公共 HTTP client | 保留为 S3 到 ESP-server client，移除 C5 device_id 默认值，加入 gateway_id 和脱敏日志 |
| `server_voice/*` | 终端直接 voice turn client | 可保留底层 HTTP 语音转发能力，但改为 S3 voice proxy，不直接绑定本地 Mic/speaker |
| `voice_domain/*` | 本地 Mic/VAD 半双工链路 | S3 默认删除或禁用；如未来 S3 也有 Mic，再抽象为独立可选模块 |
| `mic/*` | C5 Mic ADC/VAD | S3 最小系统默认删除或禁用 |
| `speaker/*`、`BSP/IIS/*` | C5 speaker/PDM | S3 最小系统默认删除或禁用 |
| `sensor_domain/bme690/*`、`BSP/IIC/*` | C5 本地 BME690 | S3 默认不作为自身传感器；如要接本地传感器，重做 S3 引脚和角色配置 |
| `display_placeholder/*` | C5 LCD placeholder | S3 侧不执行 LCD；保留协议定义或删除实现 |
| `command_domain/system_command/*` | 直连 server poll/ack | 改为 S3 本地命令队列和 C5 命令路由；S3 到 server 可继续参考命令 API |
| `device_protocol/*` | C5 envelope metadata | 保留思想，拆成 local C5-S3 envelope 与 S3-server envelope |
| `app_orchestrator/*` | 终端编排 | 重写为 gateway orchestrator |
| `app_runtime/*` | 语音独占 gate | 可保留为网关级 voice mutex 的参考，但实现应改成跨 C5 会话锁 |
| `app_time_sync/*` | 直连 server time now | 保留为 S3 到 ESP-server 时间同步；C5 可向 S3 获取本地时间 |
| `server_upload_bridge/*` | legacy `/sensor` 上传 | S3 可作为兼容适配参考；新链路优先 `/api/device/v1/ingest` |

## 4. ESPC51 / ESPC52 差异审计与通用固件化建议

### 4.1 当前差异结论

审计命令 `diff -qr ESPC51 ESPC52 -x build -x managed_components -x .DS_Store` 无输出，说明两份 C5 在源码、配置、分区、CMake、文档副本层面没有实际差异。`ESPC51` 和 `ESPC52` 当前都硬编码同一个 `SERVER_COMM_DEVICE_ID "esp32-c5-whole-001"`，因此它们不是两个已分化终端，而是两份相同固件拷贝。

结论：应合并为同一套通用 SensaiShuttle terminal 固件，通过 NVS、出厂配置、编译 profile 或首次配对写入运行时参数，避免维护两套业务代码。

### 4.2 应转为运行时配置的差异项

| 配置项 | 当前证据 | 建议 |
|---|---|---|
| `device_id` | `SERVER_COMM_DEVICE_ID` 当前公共默认 `esp32-c5-whole-001` | NVS 保存，出厂写入 `sensair_shuttle_01` / `sensair_shuttle_02` |
| `room_id` | 当前未见独立配置 | NVS 保存，用于 S3 本地规则和 server 上下文 |
| `gateway_id` | 当前未见独立配置 | NVS 保存，默认 `sensair_s3_gateway_01` |
| `gateway_ssid` | 当前 `wifi_credentials.h` 是家庭 WiFi 列表 | 改为只保存 S3 SoftAP SSID，例如 `SensaiHub_XXXX` |
| `gateway_password` | 当前 WiFi 密码随固件写入 flash | 改为 NVS 配置，文档和日志禁止明文输出 |
| `gateway_ip` | 当前没有本地 gateway IP 配置 | 默认 `192.168.4.1`，可 NVS 覆盖 |
| 设备别名 | 当前未见 | NVS 保存，例如 “客厅终端”“卧室终端” |
| 上传周期 | `BME_SENSOR_READ_UPLOAD_PERIOD_MS=2000` | 运行时配置，S3 可下发 `config.set` |
| 屏幕默认页面 | 当前 screen placeholder 仅命令显示 | 运行时配置 |
| 传感器校准参数 | BME 和 ADC 参数在头文件宏中 | 把校准参数迁移到 NVS，保留宏默认值 |
| 调试兜底 server URL | `SERVER_COMM_BASE_URL` 当前硬编码公网 server | 正式模式不保存云端 URL；仅调试 profile/NVS 开关允许直连 |

### 4.3 C5 通用固件化建议

统一 C5 固件应保留终端能力：sensor、mic、speaker、lcd、wake。启动时读取 NVS 的 `device_id`、`gateway_id`、`gateway_ssid`、`gateway_password`、`gateway_ip`、`room_id`、`alias`、`upload_period_ms`、`screen_default_page`、校准参数。缺失时进入本地配对/调试模式，而不是扫描家庭 WiFi。

推荐将现有模块调整为：

- `wifi_manager` 从“扫描家庭 WiFi 列表”改为“连接指定 gateway SoftAP”。
- `server_comm` 抽象为 `local_gateway_comm`，默认 base URL 为 `http://192.168.4.1`。
- `server_voice_client` 改为 C5 到 S3 的 local voice client。
- `system_server_client` 改为 local command poll client，轮询 S3 的命令队列。
- `device_protocol_metadata` 保留 seq、uptime、firmware、capabilities，但新增 `gateway_id`、`room_id`、`capabilities`、`version`。
- 保留本地调试兜底 profile，但必须显式启用，且不得成为正式默认。

## 5. ESPS3 网关化建议

### 5.1 工程身份纠偏

S3 网关工程第一阶段不应先改业务功能，而应先完成身份纠偏：

1. 确认 `ESPS3` 是独立 S3 gateway 工程，不再复用 `00_Learn` / C5 终端身份。
2. 执行 `idf.py set-target esp32s3`。这是必须动作，因为当前 `sdkconfig`、`dependencies.lock`、`build/` 均显示 `esp32c5`。
3. 删除或隔离旧 `build/`，重新生成 `sdkconfig`。旧 `sdkconfig` 只能作为参考，不能直接沿用。
4. 重整 `sdkconfig.defaults`，只保留 S3 网关需要的最小默认项。
5. 审计 `main/idf_component.yml`，移除 S3 网关不需要的 `esp-sr` 直接依赖，除非决定 S3 也要本地唤醒/语音模型。
6. 重命名项目和设备类型，例如 `sensair_s3_gateway`、`DEVICE_PROTOCOL_DEVICE_TYPE "sensair_s3_gateway"`。

### 5.2 推荐 S3 功能模块

S3 网关应建立新的模块边界：

| 模块 | 职责 |
|---|---|
| `gateway_config` | NVS 配置：gateway_id、SoftAP、STA、server_base_url、allowlist、日志等级 |
| `gateway_wifi` | APSTA、SoftAP IP、DHCP、STA 重连、状态事件 |
| `local_http_server` | C5 注册、heartbeat、sensor/status、voice upload、command polling |
| `child_registry` | C5 allowlist、在线状态、capabilities、last_seen |
| `sensor_aggregator` | 汇聚 C5 sensor/status，做限流、缓存、转发 |
| `voice_proxy` | 单会话锁、C5 PCM 接收、转发 ESP-server、PCM 回传 |
| `command_router` | 从 server 或本地规则获得命令，排队给指定 C5 |
| `server_client` | 调 ESP-server 现有接口，做字段转换和错误映射 |
| `offline_policy` | server 不可用时的本地降级、缓存和提示 |
| `log_sanitizer` | 统一隐藏 WiFi 密码、server URL token、API key、音频正文 |

### 5.3 不适合直接带入 S3 的 C5 模块

S3 最小系统网关不应默认启动 `MAIN_ENABLE_MIC_CHAIN`、`MAIN_ENABLE_BME_SERVICE`、`MAIN_ENABLE_SPEAKER_CHAIN`、WakeNet 模型、IIS/PDM speaker、Mic ADC/VAD 和 BME690 本机采集。这些属于 C5 terminal 能力。S3 的语音职责是代理和互斥，而不是本机录音播放。

## 6. ESPS3 target / sdkconfig / 分区表 / PSRAM / Flash 配置迁移计划

### 6.1 必须执行的配置迁移动作

- 必须执行 `idf.py set-target esp32s3`。
- 本项目实际 ESPS3 模块为 N32R16，后续 gateway 固件按 32MB Flash + 16MB PSRAM 规划。
- 建议重新生成 `sdkconfig`，不要在旧 C5 `sdkconfig` 上手工搜索替换。
- `sdkconfig.defaults` 需要重新整理，只保留 S3 网关基础项。
- 旧 C5 `sdkconfig` 只能作为“原功能参数参考”，不能直接沿用。
- Flash size 不能继续使用 C5 的 2MB 配置。
- ESPS3 后续 gateway 固件必须启用并验证 PSRAM；硬件带 16MB PSRAM 不代表固件已自动启用。
- S3 gateway 的 `sdkconfig`、`sdkconfig.defaults`、partition table 必须按 N32R16 重新生成和规划。
- 旧 `build/` 应 fullclean 或删除后重建，因为当前缓存指向 C5 toolchain 和旧路径。
- `dependencies.lock` 应由 ESP-IDF component manager 在 S3 target 下重新生成。

### 6.2 PSRAM / Flash 建议

本项目实际硬件为 ESP32-S3 N32R16，因此按 N32R16 规划。N32R16 表示 32MB Flash + 16MB PSRAM。N16R8 仅作为最低可行参考，不作为本项目目标配置。

| 项 | 建议 |
|---|---|
| target | `esp32s3` |
| 模块 | ESP32-S3 N32R16 |
| Flash | 32MB；必须在 `sdkconfig` / flash 参数 / partition table 中按 32MB 验证 |
| PSRAM | 16MB；必须启用并验证初始化成功 |
| 当前 2MB C5 配置 | 不适合 S3 gateway，不能沿用 |
| TLS/HTTP buffer | 可参考当前 16KB mbedTLS 输入/输出，但需结合 PSRAM 和 server 并发重新评估 |
| voice buffer | 第一阶段使用有限大小 HTTP buffer；禁止一次性把大 PCM 全载入内部 RAM |

容量边界：N32R16 给 S3 gateway 提供更充足的 OTA、日志、离线缓存和 buffer 余量，但不改变设备职责。S3 仍是 local gateway / hub，不承担 ASR、LLM、TTS、用户画像、每日/每周总结等云端 AI 与业务服务。

### 6.3 分区表建议

当前 `ESPS3/partitions.csv`：

```text
nvs      0x6000
phy_init 0x1000
factory  0x170000
model    0x60000
storage  0x20000
```

建议迁移方向：

- 不再使用 C5 的 2MB 分区表。当前 `factory 0x170000`、`model 0x60000`、`storage 0x20000` 是 C5 terminal/wake model 语义，不适合作为 S3 gateway 默认方案。
- NVS 建议至少 128KB，用于 `gateway_id`、SoftAP/STA 配置、`children_allowlist`、`child_registry`、命令队列 checkpoint、离线缓存索引。
- 建议预留 OTA 双分区，例如 `ota_0` / `ota_1`。
- 单个 app 分区建议按 S3 gateway 实际构建大小留足余量，可先规划 6MB-8MB 级别，再根据构建产物调整。
- `storage` 分区可预留 2MB-4MB，用于本地规则、离线队列、少量日志或诊断信息。
- `coredump` 分区可选，建议预留用于调试。
- `model` 分区默认不保留，因为 S3 gateway 不负责本地 wake model、Mic、speaker、BME terminal 功能。
- 如果未来 S3 也要承担本地语音模型，再单独规划 `model` 分区，不在当前 gateway 默认方案里保留。
- 不建议用 SPIFFS 做高频 voice PCM 缓存，voice 应优先流式转发或有限 buffer。

N32R16 分区规划草案：

| 分区 | 建议大小 | 用途 |
|---|---:|---|
| `nvs` | >= 128KB | gateway 配置、allowlist、child registry snapshot、checkpoint |
| `otadata` | 8KB | OTA 状态 |
| `phy_init` | 4KB | RF 初始化数据 |
| `ota_0` | 6MB-8MB | S3 gateway app |
| `ota_1` | 6MB-8MB | S3 gateway OTA app |
| `storage` | 2MB-4MB | 本地规则、离线队列、少量日志/诊断 |
| `coredump` | 256KB-512KB | 可选调试分区 |

具体 offset 和剩余空间由 P1 重新生成 `partitions.csv` 时计算。本表只定义 N32R16 方向和容量级别，不是本轮代码/配置修改。

### 6.4 PSRAM 使用策略

16MB PSRAM 主要用于：

- `local_http_server` buffer。
- `voice_proxy` buffer。
- `server_client` response buffer。
- 短期 offline cache。
- WebSocket 预留。
- CSI 结果队列预留。

PSRAM 使用约束：

- 不允许因为有 16MB PSRAM 就长期缓存完整语音大包。
- voice turn 仍应限制单次录音时长和 `voice_upload_max_bytes`。
- CSI raw data 默认不上传、不缓存；只预留 `csi.summary` / `csi.event` / `csi.result` 等轻量结果。
- PSRAM 使用必须有上限，避免 voice、CSI、HTTP 并发时耗尽内存。
- 内部 RAM 优先留给 WiFi、TCP/IP、任务栈和实时状态机。

### 6.5 S3 buffer 建议

- `local_http_rx_buffer_size` 和 `local_http_tx_buffer_size` 需要按 PSRAM 可用情况设置上限。
- `voice_proxy` 应使用分片 / 流式策略，避免一次性 `malloc` 大块连续内存。
- `sensor`、`status`、`heartbeat` 不应进入大缓存，只保留最近状态和必要重试队列。
- `offline_cache_limit` 必须配置化。
- CSI 预留队列只保存轻量结果，不保存 raw CSI 数组。

### 6.6 S3 配置项建议

S3 NVS / Kconfig / defaults 应包含：

- `gateway_id=sensair_s3_gateway_01`
- `hardware_module=esp32s3_n32r16`
- `flash_size_mb=32`
- `psram_size_mb=16`
- `psram_enabled=true`
- `softap_ssid_prefix=SensaiHub`
- `softap_password`
- `softap_ip=192.168.4.1`
- `softap_netmask=255.255.255.0`
- `softap_channel`
- `softap_max_connection=2` 或预留到 4
- `sta_ssid` / `sta_password`
- `server_base_url`
- `children_allowlist=["sensair_shuttle_01","sensair_shuttle_02"]`
- `local_http_port=80` 或 `8080`
- `local_http_rx_buffer_size`
- `local_http_tx_buffer_size`
- `voice_upload_max_bytes`
- `voice_turn_timeout_ms`
- `voice_single_session=true`
- `sensor_forward_period_ms`
- `heartbeat_timeout_ms`
- `offline_cache_limit`
- `log_level`
- `log_redact_secrets=true`

## 7. 推荐芯片配置规划

### 7.1 ESPC51 / ESPC52

| 项 | 推荐 |
|---|---|
| target | `esp32c5` |
| 固件角色 | SensaiShuttle terminal |
| WiFi | STA only，只连接 ESPS3 SoftAP，不连接家庭 WiFi |
| device_id | `sensair_shuttle_01` / `sensair_shuttle_02` |
| gateway_id | `sensair_s3_gateway_01` |
| gateway_ip | `192.168.4.1` |
| capabilities | `sensor`, `mic`, `speaker`, `lcd`, `wake` |
| 配置存储 | NVS |
| 云端 server_base_url | 正式模式不保存；仅调试兜底 profile 可选 |

### 7.2 ESPS3

| 项 | 推荐 |
|---|---|
| target | `esp32s3` |
| 模块 | ESP32-S3 N32R16 |
| Flash | 32MB |
| PSRAM | 16MB |
| 固件角色 | local gateway / hub |
| WiFi | APSTA |
| SoftAP IP | `192.168.4.1/24` |
| SoftAP SSID | `SensaiHub_XXXX` |
| STA | 连接家庭 WiFi |
| gateway_id | `sensair_s3_gateway_01` |
| server_base_url | 指向现有 ESP-server |
| children_allowlist | `sensair_shuttle_01`、`sensair_shuttle_02` |
| capabilities | `gateway`, `apsta`, `local_http`, `command_router`, `voice_proxy`, `offline_policy`, `csi_placeholder` |
| 配置存储 | NVS |
| 配置边界 | 实际硬件为 N32R16，因此按 N32R16 规划；N16R8 仅作为最低可行参考，不作为本项目目标配置 |

## 8. 芯片配置与 NVS 规划

### 8.1 C5 NVS

C5 应保存：

- `device_id`
- `gateway_id`
- `room_id`
- `alias`
- `gateway_ssid`
- `gateway_password`
- `gateway_ip`
- `upload_period_ms`
- `screen_default_page`
- `sensor_calibration`
- `mic_calibration`
- `speaker_volume`
- `firmware_role=terminal`
- `debug_direct_server_enabled=false`

### 8.2 S3 NVS

S3 应保存：

- `gateway_id`
- `softap_ssid`
- `softap_password`
- `softap_ip`
- `sta_ssid`
- `sta_password`
- `server_base_url`
- `children_allowlist`
- `child_registry_snapshot`
- `command_queue_checkpoint`
- `offline_cache_checkpoint`
- `log_level`
- `firmware_role=gateway`

敏感字段如 WiFi 密码、server URL token、API key 不应输出到日志。当前 C5 `wifi_credentials.h` 存在明文 WiFi 条目，迁移时应改为 NVS 写入和日志脱敏。

## 9. 本地通信协议草案

### 9.1 C5 到 S3

第一阶段使用 HTTP JSON 和 HTTP raw PCM：

- `POST /local/v1/register`
- `POST /local/v1/heartbeat`
- `POST /local/v1/status`
- `POST /local/v1/sensor`
- `POST /local/v1/voice/turn`
- `GET /local/v1/commands/pending?device_id=...`
- `POST /local/v1/commands/{command_id}/ack`

通用 JSON envelope：

```json
{
  "schema_version": 1,
  "gateway_id": "sensair_s3_gateway_01",
  "device_id": "sensair_shuttle_01",
  "room_id": "living_room",
  "seq": 123,
  "timestamp_ms": 1780000000000,
  "uptime_ms": 123456,
  "firmware_version": "0.1.0",
  "capabilities": ["sensor", "mic", "speaker", "lcd", "wake"],
  "payload_type": "sensor.bme690",
  "payload": {}
}
```

要求：

- 所有请求必须带 `device_id`、`gateway_id`、`seq`、`timestamp_ms` 或 `uptime_ms`、`payload`、`capabilities/version`。
- `seq` 在单设备内单调递增；S3 用于去重和重试识别。
- `timestamp_ms` 可由 S3 下发校时；未同步时 C5 必须带 `time_synced=false` 或使用 `uptime_ms`。

### 9.2 voice turn

第一阶段 C5 到 S3 的 voice turn 可复用 ESP-server 当前 PCM 契约：

- 请求 `Content-Type: audio/L16; rate=16000; channels=1`
- header 或 query 带 `X-Audio-Format: pcm_s16le_mono_16k`
- header 带 `X-Device-Id`、`X-Gateway-Id`、`X-Request-Seq`、`X-Payload-Type: voice.turn`
- S3 若 voice mutex 空闲，则向 ESP-server `/api/voice/turn` 发起转发。
- S3 收到服务器 PCM 后回传给指定 C5。

后续可升级为 HTTP chunked 或 WebSocket streaming。升级前必须先验证 S3 APSTA + HTTP 大包稳定性。

### 9.3 S3 到 C5

第一阶段采用 polling，降低复杂度：

- C5 定时 `GET /local/v1/commands/pending?device_id=sensair_shuttle_01`。
- S3 返回命令数组，并将命令状态标记为 dispatched。
- C5 执行后 `POST /local/v1/commands/{command_id}/ack`。

命令类型建议：

- `lcd.show_text`
- `speaker.play_audio`
- `config.set`
- `device.reboot`
- `voice.busy`
- `voice.release`
- `device.noop`

后续可升级 WebSocket 下发；在 P7 前不要把 WebSocket 当作第一阶段必需项。

### 9.4 ack / retry / timeout / error_code

ack 格式：

```json
{
  "ok": true,
  "gateway_id": "sensair_s3_gateway_01",
  "device_id": "sensair_shuttle_01",
  "seq": 124,
  "ack_seq": 123,
  "command_id": "cmd-001",
  "status": "applied",
  "error_code": "",
  "message": ""
}
```

重试策略：

- C5 上报：网络失败可按 1s、2s、5s 指数退避，最多保留最近 N 条状态/sensor。
- S3 转发 server：server 不可用时进入 `gateway_offline` / `server_unavailable`，传感器可短期缓存，voice 不做长时间缓存。
- 命令：C5 未 ack 时 S3 可在 TTL 内重新投递，超过 TTL 标记 `timeout`。

错误码建议：

- `voice_busy`: S3 当前已有 C5 占用语音会话。
- `device_offline`: 目标 C5 心跳超时。
- `gateway_offline`: S3 STA 未连接家庭 WiFi。
- `server_unavailable`: ESP-server 不可达或返回 5xx。
- `invalid_device_id`: C5 不在 allowlist。
- `invalid_gateway_id`: gateway_id 不匹配。
- `payload_too_large`: sensor/voice 超出限制。
- `timeout`: 本地或 server 请求超时。
- `unsupported_command`: C5 不支持该命令。

## 10. S3 到 ESP-server 的适配策略

本计划不修改 ESP-server。S3 只调用现有接口，并在 S3 侧做字段转换。

### 10.1 sensor/status 适配

优先调用 `POST /api/device/v1/ingest`。当前 ESP-server 要求 `payload_type` 为 `sensor.bme690`，`schema_version=1`，`device_id` 必填，`payload` 中至少包含 `temperature_c`、`humidity_percent`、`pressure_hpa`、`gas_resistance_ohm`。

S3 适配策略：

- C5 本地 `device_id` 透传为 ESP-server 的 `device_id`，这样 Dashboard 仍能按 C5 看设备数据。
- S3 在 envelope metadata 中可通过 `device_type` 或扩展 metadata 标记 gateway，但不要求后端新增字段。
- 若 S3 需要代表子设备聚合状态，优先以 `device_id=sensair_shuttle_01/02` 分别上传，避免 server 误以为只有一个 gateway 设备。

限制：ESP-server 当前设备协议不是完整多层拓扑模型；S3 gateway 与 child 的关系需要由 S3 本地维护，server 侧只能看到被映射后的设备 ID 和模块状态。

### 10.2 voice 适配

调用 `POST /api/voice/turn`。当前 ESP-server 校验：

- `Content-Type: audio/L16; rate=16000; channels=1`
- `X-Audio-Format: pcm_s16le_mono_16k`
- `X-Device-Id`
- 可选统一设备协议 v1 headers

S3 适配策略：

- S3 对 C5 发起的 voice turn 先做本地互斥；只有锁成功才转发 ESP-server。
- 转发时 `X-Device-Id` 使用发起 C5 的 `device_id`，`X-Gateway-Id` 可作为额外 header 供日志参考，但不能要求 server 必须理解。
- Server 返回的 `audio/L16` PCM 由 S3 原样回传给对应 C5。

限制：ESP-server 自身也有 `VOICE_TURN_MAX_CONCURRENT` 和 active device 限制；S3 本地互斥必须在请求前挡住双 C5 并发，避免后端 409/429 成为正常控制流。

### 10.3 command 适配

现有 ESP-server 命令接口为：

- `GET /api/commands/whitelist`
- `POST /api/devices/capabilities`
- `GET /api/devices/:device_id/capabilities`
- `POST /api/commands`
- `GET /api/commands/pending?device_id=...`
- `POST /api/commands/:command_id/ack`
- `GET /api/commands/history`

当前白名单包括 `device.noop`、`voice.set_volume`、`sensor.set_upload_interval`、`display.show_text`、`alert.play_tone`。C5 当前固件侧只实际处理 `device.noop` 和 `display.show_text`，其他能力需要后续 C5 实现。

S3 适配策略：

- S3 可代表 C5 向 server 注册 capabilities，也可注册 gateway 自身能力。
- S3 从 server 拉取目标 C5 的命令后，转为本地命令类型，例如 server 的 `display.show_text` 映射到本地 `lcd.show_text`。
- C5 ack 后，S3 再向 server ack。

限制：ESP-server 不知道 S3 本地命令队列状态；S3 需要自行处理本地 dispatched、retry、timeout。

### 10.4 time / prompt 适配

- S3 可调用 `GET /api/time/now` 校准 gateway 时间，再向 C5 提供本地时间。
- S3 可按需调用 `GET /api/voice/prompt-cache?prompt_key=wake_ack_zh&device_id=...`，缓存唤醒提示 PCM 后下发给 C5；也可让 C5 保留本地提示音，减少网络依赖。

## 11. 分阶段实施计划

### P0：只读审计和文档

本阶段完成当前目录审计、ESPS3 C5 残留问题、C5 通用固件化建议、S3 网关化计划、通信协议草案、ESP-server 现有接口适配策略、风险和验收标准。只新增/修改本文档。

### P1：ESPS3 工程身份纠偏

- 对 `ESPS3` 执行 `idf.py set-target esp32s3`。
- fullclean 或删除旧 `build/` 后重新生成配置。
- 重整 `sdkconfig.defaults`。
- 改项目名、设备类型、gateway_id 默认值。
- 移除或禁用 C5-only Mic/speaker/BME/wake model/screen 模块。
- 重新设计 S3 分区表、Flash、PSRAM、NVS。

### P2：统一 C5 职责模型

- 合并 `ESPC51` / `ESPC52` 为一套通用 terminal 固件。
- 把 `device_id`、`room_id`、`gateway_id`、SoftAP 凭据、上传周期、校准参数迁移到 NVS。
- 默认禁用直连 ESP-server。
- 保留调试兜底模式，但必须显式开启。

### P3：ESPS3 APSTA 和本地 HTTP

- S3 开 SoftAP，固定 `192.168.4.1/24`。
- S3 STA 连接家庭 WiFi。
- S3 本地 HTTP server 提供 register、heartbeat、status、sensor。
- S3 建立 child registry 和 allowlist。

### P4：C5 只连接 S3 SoftAP

- C5 WiFi 从家庭 WiFi 列表改为 S3 SoftAP 配置。
- C5 sensor/status/heartbeat 发给 S3。
- C5 命令 polling 指向 S3。

### P5：S3 转发 sensor/status 到 ESP-server

- S3 将 C5 sensor/status 转为 `/api/device/v1/ingest` 或现有兼容接口。
- 保持 server/Dashboard 仍按 C5 device_id 看数据。
- 加入短期离线缓存和重试。

### P6：语音链路 C5 -> S3 -> ESP-server -> S3 -> C5

- S3 增加 voice mutex。
- C5 PCM 上传到 S3。
- S3 转发 `/api/voice/turn`，并把返回 PCM 给对应 C5。
- 处理 `voice_busy`、server 409/429、timeout 和大包失败。

### P7：命令队列、LCD/喇叭控制、本地规则

- S3 本地命令队列。
- S3 到 C5 polling 命令分发。
- LCD/喇叭/config/control 命令落地。
- 本地规则引擎预留，例如空气质量阈值触发屏幕提示。

### P8：离线降级、稳定性测试和回滚策略

- 家庭 WiFi 断开时保留 SoftAP 和 C5 本地控制。
- ESP-server 不可用时缓存 sensor，voice 返回本地错误提示。
- APSTA 长时间稳定性、双 C5 心跳、voice 大包、重启恢复测试。
- C5 保留调试直连 profile 作为故障隔离手段。

## 12. 风险与回滚

| 风险 | 说明 | 回滚 / 缓解 |
|---|---|---|
| ESPS3 是 C5 复制工程 | target、sdkconfig、分区表、外设、身份、构建缓存均可能不适配 S3 | P1 单独做身份纠偏，旧配置只作参考 |
| S3 APSTA 稳定性 | 同时 SoftAP + STA 对 WiFi 驱动、信道、功耗、内存有压力 | 先验证双 C5 heartbeat，再加语音大包 |
| 语音大包转发超时 | C5 -> S3 -> server -> S3 -> C5 链路更长 | 限制单轮时长，流式转发，明确 timeout 和 retry |
| S3 单点故障 | C5 不直连 server 后，S3 故障会影响全系统 | C5 保留调试直连 profile；S3 watchdog 和离线提示 |
| C5 双固件分叉维护成本 | 当前 C51/C52 无差异却维护两套目录 | 合并通用固件，差异放 NVS |
| 设备身份冲突 | 当前三份固件都默认 `esp32-c5-whole-001` | 出厂写入唯一 `device_id`，S3 allowlist 校验 |
| ESP-server 不改带来的适配压力 | server 不理解 gateway-child 拓扑 | S3 维护本地拓扑，server 侧只接映射后的 device_id |
| C5 不直连服务器后调试难度增加 | 问题可能出在 C5、S3 或 server 任一段 | 分段健康检查、trace_id、调试直连 profile |
| 日志和密钥泄露风险 | 当前 WiFi 凭据硬编码在源码，server URL 和 device_id 也硬编码 | 迁移到 NVS，日志脱敏，禁止文档复写密钥 |
| N32R16 配置未生效 | 实际模块为 N32R16，但当前 S3 仍是 C5 复制工程：2MB flash、无 PSRAM、C5 分区和 C5 terminal 启动链路 | P1 必须验证 `sdkconfig` Flash size 为 32MB、PSRAM 已启用并初始化成功、分区表按 N32R16 重规划 |
| 容量充足导致职责漂移 | N32R16 容量充足，但 S3 仍只是 local gateway / hub，不是 AI 推理大脑 | 不把 ASR、LLM、TTS、用户画像、每日/每周总结迁移到 S3，继续由 ESP-server 承担 |
| APSTA / 空口竞争 / voice / CSI 风险 | Flash/PSRAM 不是主要瓶颈；APSTA 稳定性、WiFi 空口竞争、voice 大包转发和未来 CSI probe 才是主要风险 | 先验证 APSTA 与双 C5 heartbeat，再验证 voice 分片/流式，CSI 默认 `csi_enabled=false` 并限流 |
| C5 外设配置误用于 S3 | S3 当前保留 C5 ADC/I2S/I2C GPIO 和驱动假设 | S3 最小系统默认移除终端外设模块 |
| 命令能力不一致 | server 白名单多于当前固件实际执行能力 | S3/C5 分别注册真实 capabilities，未知命令拒绝 |

## 13. 验收标准

### 13.1 本轮 P0 验收

- 本文档明确列出当前目录审计摘要。
- 本文档明确列出各设备职责边界。
- 本文档明确列出 ESPS3 的 C5 残留配置和迁移建议。
- 本文档包含 ESPS3 target、sdkconfig、分区表、PSRAM、Flash 配置迁移计划。
- 本文档包含 ESPC51/ESPC52 差异审计。
- 本文档包含 C5 通用固件化建议。
- 本文档包含 ESPS3 网关化建议。
- 本文档包含芯片配置与 NVS 规划。
- 本文档包含本地通信协议草案。
- 本文档包含 S3 到 ESP-server 的适配策略。
- 本文档包含分阶段实施计划、风险与回滚、验收标准。
- 本文档明确写出 ESPS3 实际硬件为 N32R16。
- 本文档明确 S3 gateway 按 32MB Flash + 16MB PSRAM 规划。
- 本文档明确不再沿用 C5 2MB flash / 无 PSRAM 配置。
- 本文档明确 `model` 分区默认不保留。
- 本文档明确 PSRAM 只是增强 gateway buffer 和稳定性，不改变 S3 的职责边界。
- 本文档明确 S3 不承担 ASR/LLM/TTS/用户画像/总结。
- 本轮只新增/修改 `docs/esp-111-device-roles-and-s3-gateway-migration-plan.md`，不修改固件、后端、配置、分区、构建、数据库或前端文件。

### 13.2 后续实施验收

- C5 不连接家庭 WiFi，只连接 S3 SoftAP。
- S3 能开 SoftAP，SoftAP IP 为 `192.168.4.1/24`。
- S3 能作为 STA 连接家庭 WiFi。
- S3 能看到两个 C5 在线，并维护心跳/last_seen。
- S3 能连接 ESP-server。
- ESPC51 和 ESPC52 使用同一套 `/local/v1` API。
- S3 不存在 `/c51` 或 `/c52` 专用接口。
- S3 不按源码目录名识别设备。
- C5 正式模式不保存 `server_base_url`。
- C5 所有业务 HTTP 请求只指向 `gateway_ip`。
- C5 上报 `sensor`、`status`、`heartbeat`、`voice` 到 S3 成功。
- S3 能根据 `device_id` 区分两个 C5。
- S3 能用同一套 `protocol_adapter` 转发两个 C5 的数据到 ESP-server。
- C5 遇到未知命令能返回 `unsupported_command`。
- S3 遇到未知 `message_type` 能返回 `unsupported_payload_type`。
- C5 传感器数据经 S3 上传成功。
- Dashboard/服务器仍能看到设备数据。
- 同一时间只有一个 C5 能发起语音会话。
- S3 能把 ESP-server 返回的 voice PCM 分发给发起的 C5。
- 命令队列能从 server 到 S3 再到指定 C5，并能 ack。
- CSI 只完成接口和模块占位，不实现实际 CSI 采集、分析和服务器存储。
- `csi_enabled=false` 时 CSI 预留逻辑不能影响 sensor、voice、command 主链路。
- ESP-server 没有任何代码变更。
- 文档中能清晰看到 C5 terminal 和 S3 gateway 的目标固件结构。

## 14. 下一步建议

下一步先执行 P1，不要直接写业务功能。P1 的最小成功标准是：`ESPS3` 在 `esp32s3` target 下按 N32R16 重新生成配置，验证 32MB Flash 与 16MB PSRAM，移除 C5 终端默认启动链路，明确 gateway 项目名、gateway device type、APSTA 配置项、S3 分区表和 PSRAM/Flash 策略。P1 完成并能构建后，再进入 P3 的 APSTA 和本地 HTTP server。

## 15. 最终目标拓扑图

最终目标是两个 C5 都只作为本地 SensaiShuttle terminal，唯一上行出口是 ESPS3 local gateway。两个 C5 不连接家庭 WiFi，不直接访问 ESP-server；ESPS3 同时承担 SoftAP 接入点、STA 家庭 WiFi 出口和 ESP-server 代理。

```text
┌──────────────────┐       local HTTP / PCM       ┌──────────────────┐
│   ESPC51 / C5-01 │ ◀──────────────────────────▶ │                  │
│  SensaiShuttle   │                              │                  │
└──────────────────┘                              │                  │
                                                  │      ESPS3       │
┌──────────────────┐       local HTTP / PCM       │  Local Gateway   │
│   ESPC52 / C5-02 │ ◀──────────────────────────▶ │  SoftAP + STA    │
│  SensaiShuttle   │                              │  192.168.4.1     │
└──────────────────┘                              │                  │
                                                  └────────┬─────────┘
                                                           │
                                                           │ existing ESP-server API
                                                           ▼
                                                  ┌──────────────────┐
                                                  │    ESP-server    │
                                                  │ ASR / LLM / TTS  │
                                                  │ DB / Dashboard   │
                                                  └──────────────────┘
```

拓扑约束：

- C5-01 不连接家庭 WiFi。
- C5-02 不连接家庭 WiFi。
- 两个 C5 都不直接访问 ESP-server。
- 两个 C5 都只连接 ESPS3 SoftAP。
- ESPS3 是唯一 WiFi 出口和服务器代理。
- ESP-server 不改，继续作为现有云端 API、ASR/LLM/TTS、数据库、Dashboard、总结和用户画像服务。

## 16. C5 端精简原则

C5 不再是完整云端设备，而是本地 SensaiShuttle terminal。C5 的正式职责应收敛到本地采集、唤醒、音频、显示、状态机和与 S3 的本地通信。

C5 只保留：

- 传感器采集。
- 本地唤醒。
- 麦克风录音。
- 喇叭播放。
- 屏幕显示。
- 本地状态机。
- 连接 ESPS3 SoftAP。
- 向 ESPS3 上报 `sensor`、`status`、`heartbeat`、`voice`。
- 执行 ESPS3 下发的 `lcd`、`speaker`、`config`、`control` 命令。
- 预留 CSI 分析结果上报接口，但本阶段不实现 CSI 实际功能。

C5 不再负责：

- 连接家庭 WiFi。
- 直接访问 ESP-server。
- 维护公网 `server_base_url`。
- ASR。
- LLM。
- TTS 供应商逻辑。
- 用户画像。
- 每日/每周总结。
- Dashboard 逻辑。
- 多设备拓扑管理。
- 跨设备语音互斥。
- 云端命令队列直接轮询。
- 服务器侧 CSI 业务判断。

精简要求：

- C5 端 WiFi 模块必须从“家庭 WiFi 扫描 / 多 SSID / 服务器直连”精简为“固定连接 gateway SoftAP”。
- C5 端 HTTP 通信必须从 `server_comm` 精简或抽象为 `local_gateway_comm` / `gateway_comm`。
- 正式模式下 C5 不保存 ESP-server 公网地址；直连服务器只能作为 `debug_direct_server_enabled` 显式开启的调试兜底模式。
- C5 上报字段只包含终端本地事实，不混入 `server_recv_ms`、Dashboard、用户画像、每日总结、每周总结等服务器侧字段。

## 17. C5 与 S3 统一接口原则

ESPC51 和 ESPC52 必须使用完全相同的本地 API。两块 C5 的区别只能来自运行时配置，而不是源码目录、编译目录或接口路径。

允许运行时区分的配置项：

| 配置项 | 用途 |
|---|---|
| `device_id` | 唯一终端身份，S3 按此区分 C5-01 / C5-02 |
| `room_id` | 房间或区域归属 |
| `alias` | 用户可读设备别名 |
| `gateway_id` | 绑定的 S3 gateway |
| `gateway_ssid` | S3 SoftAP SSID |
| `gateway_password` | S3 SoftAP 密码 |
| `gateway_ip` | 默认 `192.168.4.1` |
| `upload_period_ms` | sensor/status 上报周期 |
| `calibration` | 传感器、麦克风、喇叭等校准参数 |
| `csi_role` | CSI 预留角色 |
| `csi_enabled` | CSI 预留开关，第一阶段默认 `false` |

统一接口约束：

- S3 只按 `device_id` 区分设备，不为 C51/C52 写不同接口。
- S3 不允许按源码目录名 `ESPC51` / `ESPC52` 判断设备身份。
- C5 不允许写死自身是 C51 或 C52 的业务分支。
- 本地接口必须版本化，例如 `/local/v1`。
- CSI 预留接口也必须走同一套 `/local/v1` 协议，不允许另开 C51/C52 专用路径。

禁止出现：

- `/local/v1/c51/...`
- `/local/v1/c52/...`
- 针对某一块 C5 的硬编码接口。
- S3 侧为 C51/C52 分别写两套 adapter。
- C5 侧写死不同业务路径。

## 18. 推荐统一本地 API

第一阶段使用 HTTP polling 和 HTTP JSON/raw PCM，所有接口统一挂在 `/local/v1` 下。S3 不主动推送命令，C5 通过 pending 接口轮询获取命令；后续可以升级 WebSocket，但 HTTP polling 是第一阶段统一标准。

C5 -> S3：

| 方法 | 路径 | message_type | 说明 |
|---|---|---|---|
| `POST` | `/local/v1/register` | `device.register` | C5 注册身份、能力、固件版本和房间信息 |
| `POST` | `/local/v1/heartbeat` | `device.heartbeat` | C5 在线心跳和基础健康状态 |
| `POST` | `/local/v1/status` | `device.status` | 电量、WiFi RSSI、任务状态、错误摘要 |
| `POST` | `/local/v1/sensor` | `sensor.bme690` | BME690 等本地传感器事实 |
| `POST` | `/local/v1/voice/turn` | `voice.turn` | PCM voice turn，第一阶段固定 `pcm_s16le_mono_16k` |
| `POST` | `/local/v1/csi/result` | `csi.result` | CSI 结果占位接口，只传处理后结果，不传 raw CSI |
| `GET` | `/local/v1/commands/pending?device_id=...` | `command.pending` | C5 拉取待执行命令 |
| `POST` | `/local/v1/commands/{command_id}/ack` | `command.ack` | C5 回传命令执行结果 |

S3 -> C5：

- 第一阶段不要求 S3 主动推送。
- C5 通过 `GET /local/v1/commands/pending?device_id=...` 拉取命令。
- S3 返回统一 command envelope 数组。
- C5 执行后通过 ack 接口确认。
- 后续可升级为 WebSocket，但必须保持 `/local/v1` 的同一套语义和命令 envelope。

CSI 相关接口只做协议预留，不实现实际 CSI 采集、分析、算法或数据转发功能。

## 19. 统一 envelope 规范

所有 C5 -> S3 JSON 请求必须使用统一 envelope。local protocol 使用 `message_type`；当 ESP-server 当前接口要求 `payload_type` 时，由 S3 的 `protocol_adapter` 在 S3 -> server 边界转换，不让 C5 感知 server 字段差异。

C5 -> S3 envelope：

```json
{
  "schema_version": 1,
  "message_type": "sensor.bme690",
  "gateway_id": "sensair_s3_gateway_01",
  "device_id": "sensair_shuttle_01",
  "room_id": "living_room",
  "seq": 123,
  "timestamp_ms": 1780000000000,
  "uptime_ms": 123456,
  "firmware_version": "0.1.0",
  "capabilities": ["sensor", "mic", "speaker", "lcd", "wake", "csi_client"],
  "payload": {}
}
```

所有 S3 -> C5 命令必须使用统一 command envelope：

```json
{
  "schema_version": 1,
  "command_id": "cmd-001",
  "target_device_id": "sensair_shuttle_01",
  "command_type": "lcd.show_text",
  "seq": 456,
  "ttl_ms": 30000,
  "params": {}
}
```

ack envelope：

```json
{
  "schema_version": 1,
  "ok": true,
  "gateway_id": "sensair_s3_gateway_01",
  "device_id": "sensair_shuttle_01",
  "seq": 457,
  "ack_seq": 456,
  "command_id": "cmd-001",
  "status": "applied",
  "error_code": "",
  "message": ""
}
```

统一校验规则：

- `schema_version` 第一版固定为 `1`。
- `seq` 在单个 `device_id` 或 gateway command stream 内单调递增。
- C5 上报必须带 `gateway_id`，S3 必须校验是否匹配当前 gateway。
- C5 上报必须带 `device_id`，S3 必须校验 allowlist。
- `payload` 只放该 `message_type` 的业务事实，不混入 server-only 字段。
- `timestamp_ms` 可由 S3 校时后提供；未校时时必须保留 `uptime_ms`。

## 20. 推荐统一 message_type / payload_type

本地 C5 -> S3 协议统一使用 `message_type`。S3 -> ESP-server 适配时，如现有 server API 使用 `payload_type`，由 S3 集中映射，不要求 ESP-server 改字段。

| 本地 `message_type` | 可映射的 server `payload_type` | 说明 |
|---|---|---|
| `device.register` | `device.register` | 设备注册和能力声明 |
| `device.heartbeat` | `device.heartbeat` | 在线心跳 |
| `device.status` | `device.status` | 本地状态 |
| `sensor.bme690` | `sensor.bme690` | BME690 传感器事实 |
| `voice.turn` | `voice.turn` | 语音回合 |
| `csi.result` | `csi.result` | CSI 处理后结果占位 |
| `command.pending` | `command.pending` | 命令拉取 |
| `command.ack` | `command.ack` | 命令 ack |
| `error.report` | `error.report` | 本地错误上报 |

协议演进规则：

- 第一版固定 `schema_version=1`。
- C5 与 S3 都必须校验 `schema_version`。
- S3 遇到未知 `message_type` 返回 `unsupported_payload_type`。
- C5 遇到未知 `command_type` 返回 `unsupported_command`。
- 后续协议升级必须新增字段，不破坏旧字段。
- 字段命名统一使用 `snake_case`。
- 单位必须写进字段名，例如 `temperature_c`、`humidity_percent`、`pressure_hpa`、`gas_resistance_ohm`、`uptime_ms`、`csi_sample_count`、`csi_window_ms`。

## 21. CSI 预留设计

CSI 本阶段只做接口、配置和模块边界预留，不做实际 CSI 功能。

用户目标：

- ESPS3 后续可作为 CSI 触发/发包端，持续或定时向两个 C5 发送本地 WiFi 包。
- 两个 C5 后续可作为 CSI 接收与边缘分析端。
- C5 本地分析 CSI 原始数据，得到处理后的特征或事件结果。
- C5 只把处理后的 CSI 结果上报给 S3，不上传原始大体量 CSI 数据。
- S3 汇总 CSI 结果，可后续再决定是否转发服务器。
- 本阶段不实现 CSI 发包、不实现 CSI 采集、不实现 CSI 算法、不实现 server 存储，只预留接口、配置和模块边界。

ESPS3 侧预留角色：

- 预留 `csi_packet_sender` / `csi_beacon_trigger` 模块名。
- 预留 `csi_scheduler`。
- 预留 `csi_result_collector`。
- 预留 `csi_to_server_adapter`，但默认禁用。
- 后续负责定时或按规则给 C5 发包，作为 CSI 触发源。
- 后续负责接收 C5 的 `csi.result`。

C5 侧预留角色：

- 预留 `csi_receiver`。
- 预留 `csi_feature_extractor`。
- 预留 `csi_result_reporter`。
- 后续负责接收 S3 包产生 CSI。
- 后续负责本地处理 CSI 数据。
- 后续只上报处理后的结果，不上报原始 CSI 流。

CSI 与主链路隔离要求：

- `csi_enabled` 第一阶段默认 `false`。
- CSI 预留模块不能阻塞 sensor、heartbeat、command polling 和 voice turn。
- CSI 后续发包必须受调度器限流，不能与 voice turn 高峰并发抢占本地 WiFi 空口。

## 22. CSI 预留 payload 草案

`csi.result` 第一阶段只作为占位协议，允许 C5 发送 `csi_enabled=false` 的空结果，S3 能识别但不执行实际 CSI 业务。

```json
{
  "schema_version": 1,
  "message_type": "csi.result",
  "gateway_id": "sensair_s3_gateway_01",
  "device_id": "sensair_shuttle_01",
  "room_id": "living_room",
  "seq": 2001,
  "timestamp_ms": 1780000000000,
  "uptime_ms": 987654,
  "firmware_version": "0.1.0",
  "capabilities": ["csi_client"],
  "payload": {
    "csi_enabled": false,
    "result_type": "placeholder",
    "window_ms": 1000,
    "sample_count": 0,
    "features": {},
    "event": {
      "event_type": "none",
      "confidence": 0.0
    }
  }
}
```

约束：

- `csi.result` 第一阶段只作为占位协议。
- `csi_enabled` 默认 `false`。
- `sample_count` 第一阶段可为 `0`。
- `features` 第一阶段为空对象。
- 不传 `raw_csi`。
- 不传大数组。
- 不在本阶段定义具体人体检测、动作识别、存在检测算法。
- 后续如果增加 CSI 算法，只扩展 `payload.features` 和 `payload.event`，不破坏 envelope。

## 23. C5 精简后的模块边界

C5 terminal 推荐模块边界：

- `device_identity`
- `wifi_gateway_client`
- `local_gateway_comm`
- `sensor_service`
- `voice_capture`
- `wake_word`
- `speaker_player`
- `lcd_service`
- `command_executor`
- `csi_receiver_placeholder`
- `csi_result_reporter_placeholder`
- `app_orchestrator`

建议逐步弱化或移除：

- 家庭 WiFi 扫描。
- 多 SSID 自动选择。
- 云端 `server_base_url`。
- 服务器时间同步强绑定。
- 直接 ESP-server command polling。
- 直接 ESP-server voice turn。
- `server_upload_bridge` 直连模式。

迁移原则：

- 与网络出口相关的能力归并到 `wifi_gateway_client` 和 `local_gateway_comm`。
- 与本地行为相关的能力留在 terminal 模块内。
- 与云端字段、Dashboard、总结、用户画像相关的字段不进入 C5 terminal envelope。

## 24. ESPS3 精简后的模块边界

ESPS3 gateway 推荐模块边界：

- `gateway_config`
- `gateway_wifi`
- `local_http_server`
- `child_registry`
- `protocol_adapter`
- `server_client`
- `sensor_aggregator`
- `voice_proxy`
- `command_router`
- `offline_policy`
- `csi_packet_sender_placeholder`
- `csi_scheduler_placeholder`
- `csi_result_collector_placeholder`
- `csi_to_server_adapter_placeholder`
- `log_sanitizer`
- `gateway_orchestrator`

ESPS3 应逐步禁用或移除从 C5 复制来的终端模块：

- `mic`
- `voice_chain`
- `local_wake_word`
- `speaker`
- BME 本机采集。
- display terminal 逻辑。
- `wake_prompt_cache`
- C5 terminal `app_orchestrator` 启动链路。

S3 网关的语音职责是代理和互斥，不是本机录音、唤醒和播放；S3 的传感器职责是汇聚与转发，不是默认本机 BME 采集。

## 25. 各设备固件结构目标

以下只是目标结构和模块边界，不要求本轮改目录。后续改代码时应逐步靠近这个结构。

C5 terminal 目标结构草案：

```text
ESPC5-terminal/
  main/
    main.c
  components/
    app_config/
    device_identity/
    wifi_gateway_client/
    local_gateway_comm/
    sensor_service/
    voice_capture/
    wake_word/
    speaker_player/
    lcd_service/
    command_executor/
    csi_receiver_placeholder/
    csi_result_reporter_placeholder/
    app_orchestrator/
```

S3 gateway 目标结构草案：

```text
ESPS3-gateway/
  main/
    main.c
  components/
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
    csi_packet_sender_placeholder/
    csi_scheduler_placeholder/
    csi_result_collector_placeholder/
    csi_to_server_adapter_placeholder/
    log_sanitizer/
    gateway_orchestrator/
```

结构约束：

- ESPC51/ESPC52 不应长期保持两套不同业务目录。
- ESPS3 不应继续沿用 C5 terminal 目录语义。
- 如果短期仍保留现有目录，应通过模块重命名、启动链路隔离和 CMake 边界逐步收敛。

## 26. S3 侧统一适配层

S3 是 C5 本地协议与现有 ESP-server API 之间的唯一适配层。

适配边界：

- S3 对下只认 `/local/v1` 统一本地协议。
- S3 对上只调用现有 ESP-server API。
- ESP-server 不改。
- S3 负责把 C5 本地 envelope 转换成 ESP-server 当前接口需要的格式。
- 适配逻辑集中放在 S3 的 `server_client` / `protocol_adapter`。
- 不允许把 ESP-server 字段适配逻辑散落在 `local_http_server`、`voice_proxy`、`sensor_aggregator`、`command_router`、`csi_result_collector` 等模块里。
- C5 不需要知道 ESP-server 的 API 路径、字段、鉴权、ASR/LLM/TTS 细节。
- S3 不能为 C51/C52 分别写不同 adapter；只能基于 `device_id`、`message_type`、`capabilities` 做统一转换。
- CSI 结果是否转发 server 由 S3 的 `csi_to_server_adapter_placeholder` 决定，默认不转发。

建议映射规则：

| 本地来源 | S3 适配动作 | Server 目标 |
|---|---|---|
| `sensor.bme690` | 校验本地 envelope，映射为 server 当前 sensor payload | `/api/device/v1/ingest` 或兼容 `/sensor` |
| `device.status` | 聚合在线、本地错误、RSSI、电量等事实 | server 现有设备状态/ingest 能力 |
| `voice.turn` | 获取 gateway voice mutex，转发 PCM 和 headers | `/api/voice/turn` |
| `command.pending` | S3 从 server 拉取后转换为本地 command envelope | `/local/v1/commands/pending` |
| `command.ack` | C5 ack 后由 S3 转发或汇总 server ack | `/api/commands/:command_id/ack` |
| `csi.result` | 默认只本地识别和丢弃/缓存，不转发 | 默认无 server 调用 |

## 27. P1 详细执行边界

P1 是后续实施阶段，不是本轮文档任务。本轮只在本文档中定义边界，P1 实施时只处理 ESPS3 工程身份纠偏，不先做完整业务。

P1 允许修改范围：

- `ESPS3/sdkconfig.defaults`
- `ESPS3/CMakeLists.txt`
- `ESPS3/main/CMakeLists.txt`
- `ESPS3/main/main.c`
- `ESPS3/partitions.csv`
- `ESPS3/components` 中与 gateway 启动相关的模块。
- 必要时新增 `ESPS3/components` 下的 `gateway_config`、`gateway_wifi`、`local_http_server`、`server_client`、`protocol_adapter` 等骨架模块。

P1 禁止修改：

- `ESPC51`
- `ESPC52`
- `ESP-server`
- `ESP-server/docs/api.md`
- `ESP-server/public`
- `ESP-server/db`
- 真实数据库。

P1 必须完成：

- `idf.py set-target esp32s3`
- 把 ESPS3 配置目标明确为 `esp32s3` + N32R16。
- 检查 `sdkconfig` 中 Flash size 是否为 32MB。
- 检查 PSRAM 是否启用并能初始化。
- 清理旧 build 缓存。
- 重新生成 `sdkconfig`。
- 整理 `sdkconfig.defaults`。
- 重新规划 `partitions.csv`，不再沿用 C5 2MB 分区。
- 纠正项目身份为 S3 gateway。
- 禁用或隔离 C5 terminal 默认启动链路。
- 不再默认启动 Mic、speaker、BME、wake model、`voice_chain`。
- 明确 `gateway_id`、SoftAP、STA、`server_base_url`、NVS、PSRAM、Flash、分区表策略。
- 至少能 build。

P1 不做：

- 不接入真实 C5。
- 不实现完整 voice proxy。
- 不实现 `voice_proxy` 大缓存。
- 不实现实际 CSI 功能。
- 不实现 CSI raw 采集、缓存、上传或转发。
- 不实现 AI 本地推理。
- 不改 ESP-server。
- 不改 C5 固件业务。

## 28. S3 SoftAP 网络细节

S3 SoftAP 第一阶段建议：

- SoftAP IP：`192.168.4.1`
- Netmask：`255.255.255.0`
- DHCP 范围建议：`192.168.4.2` - `192.168.4.20`
- C5 不要求固定 IP，S3 按 `device_id` 识别设备。
- `softap_max_connection` 第一阶段设 `2`，预留到 `4`。
- 认证方式建议 WPA2-PSK。
- SSID 建议 `SensaiHub_<gateway_suffix>`。
- C5 不依赖 mDNS，直接访问 `192.168.4.1`。
- S3 SoftAP 和 STA 信道可能受家庭 WiFi 影响，需要在 P3/P8 稳定性测试中覆盖。
- 后续 CSI 触发发包会增加本地 WiFi 空口占用，必须与 voice turn、sensor 上报错峰或限流。

SoftAP 运行原则：

- S3 STA 断开时，SoftAP 仍应保持可见，C5 可继续 heartbeat 和获取本地错误。
- S3 重启后，C5 应能自动重连，不要求 C5 手工重配。
- SoftAP 密码不写日志，不写入文档示例明文。

## 29. Voice 大包与内存策略

语音链路是 S3 gateway 最容易暴露内存、WiFi 和超时问题的路径，必须在协议层先收窄。

第一阶段策略：

- 第一阶段同一时间只允许一个 voice turn。
- C5 单次录音最大时长建议先限制在 8-12 秒。
- PCM 格式固定 `pcm_s16le_mono_16k`。
- S3 不应长期缓存完整 PCM。
- 优先边接收边转发；如果 P1/P6 初期做不到流式，必须限制 buffer 和最大上传字节数。
- 超过 `voice_upload_max_bytes` 返回 `payload_too_large`。
- server 超时返回 `server_unavailable` 或 `timeout`。
- 另一个 C5 请求时返回 `voice_busy`。
- S3 voice mutex 必须在转发 ESP-server 前获取，不能依赖 ESP-server 并发限制作为主控制。
- 如果 S3 STA 断开，C5 voice turn 应立即返回 `gateway_offline` 或 `server_unavailable`，不要长时间卡住。
- 后续 CSI 发包与 voice turn 不能同时高频运行，voice turn 优先级高于 CSI 触发发包。

建议配置项：

- `voice_single_session=true`
- `voice_upload_max_bytes`
- `voice_turn_timeout_ms`
- `voice_server_connect_timeout_ms`
- `voice_server_read_timeout_ms`
- `voice_busy_retry_after_ms`

## 30. C5 身份写入方式

C5 身份写入优先级：

1. NVS 出厂写入：正式推荐。
2. 串口命令写入：开发调试推荐。
3. 编译 profile：短期可用，但不推荐长期维护。
4. 代码宏写死：仅临时验证使用。

必须保证：

- 两块 C5 不能共享同一个 `device_id`。
- S3 allowlist 校验 `device_id`。
- C5 首次启动缺少 `device_id` 时进入配置/等待配对状态。
- `device_id`、`room_id`、`alias`、`gateway_id`、`gateway_ssid`、`gateway_ip`、`upload_period_ms`、`calibration`、`csi_enabled` 都应是运行时配置。
- ESPC51/ESPC52 不应因为设备身份不同而维护两套业务代码。

建议身份示例：

| 设备 | device_id | alias | room_id |
|---|---|---|---|
| C5-01 | `sensair_shuttle_01` | `living_room_terminal` | `living_room` |
| C5-02 | `sensair_shuttle_02` | `bedroom_terminal` | `bedroom` |
| S3 | `sensair_s3_gateway_01` | `home_gateway` | `home` |

## 31. 测试矩阵

| 测试项 | 验证目标 | 通过标准 |
|---|---|---|
| ESPS3 target 测试 | `set-target esp32s3` 后能 build | `ESPS3` 不再使用 C5 target/cache，S3 target 构建通过 |
| S3 APSTA 测试 | SoftAP 可见，STA 可连家庭 WiFi | SoftAP 保持 `192.168.4.1/24`，STA 获取家庭网络连接 |
| C5 入网测试 | 两个 C5 能连接 S3 SoftAP | C5-01/C5-02 均能获得 DHCP 地址并访问 `192.168.4.1` |
| C5 统一 API 测试 | C51/C52 使用同一套 `/local/v1` API | 不存在 `/c51`、`/c52` 或硬编码目录分支 |
| register 测试 | S3 能登记两个 C5 | 记录 `device_id`、`capabilities`、`last_seen` |
| heartbeat 测试 | S3 能记录在线状态和超时离线 | 心跳恢复在线，超时后标记 offline |
| sensor 测试 | C5 -> S3 -> ESP-server 成功 | server/Dashboard 能看到按 C5 `device_id` 映射后的数据 |
| command polling 测试 | C5 能从 S3 拉取命令并 ack | S3 本地命令状态能从 pending 到 acked |
| voice_busy 测试 | 两个 C5 同时发起语音时互斥 | 只有一个通过，另一个收到 `voice_busy` |
| csi.result 占位接口测试 | C5 可发送 `csi_enabled=false` 的 `csi.result` | S3 能识别但不执行实际 CSI 业务 |
| server 断网测试 | S3 保持 SoftAP，C5 能收到本地错误 | C5 voice 返回 `gateway_offline` 或 `server_unavailable`，sensor 可短期缓存 |
| S3 重启测试 | C5 自动重连 | S3 重启后 C5 恢复 register/heartbeat |
| ESP-server 不变更测试 | 后续实施不修改 server | `git diff -- ESP-server` 为空，`ESP-server/public`、`db` 无变化 |

测试顺序建议：先跑 P1 target/build，再跑 P3 APSTA/local HTTP，再接双 C5 register/heartbeat，最后接 sensor、command 和 voice。CSI 占位测试只验证 envelope 识别，不验证 CSI 算法。

## 32. 回滚策略

阶段回滚原则：

- 每个阶段单独 commit。
- P1 前保留 ESPS3 当前 C5-copy 状态。
- ESPS3 `set-target` / `sdkconfig` / 分区表修改必须单独提交。
- C5 改 gateway 模式前保留 direct-server debug profile。
- P3/P4 失败时 C5 可切回 direct-server debug mode。
- ESP-server 全程不改，因此无需 server 回滚。
- 如果 S3 APSTA 不稳定，允许回退到同一家庭 WiFi 下的逻辑中枢模式做调试，但目标架构仍保持 C5 只连 S3 SoftAP。
- CSI 预留接口如果影响主链路，必须可通过 `csi_enabled=false` 完全关闭。

回滚判断：

- P1 构建失败且无法在同一阶段修复时，回退 S3 target/config/partition commit，不进入 P3。
- P3 SoftAP 不稳定时，不修改 C5 默认联网策略，先修 S3 APSTA。
- P4 C5 入网不稳定时，保留 `debug_direct_server_enabled` 用于分段排查，但不得把 direct-server 重新定义为正式架构。
- P6 voice 大包不稳定时，降低 `voice_upload_max_bytes` 和录音时长，保留 sensor/heartbeat/command 主链路。

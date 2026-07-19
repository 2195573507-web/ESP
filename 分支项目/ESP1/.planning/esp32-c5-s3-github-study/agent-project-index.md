# ESP32-C5 / ESP32-S3 GitHub 候选项目与来源审计

> 审计时间：2026-07-10 13:47-13:55（Asia/Shanghai）。候选集合严格来自 `findings.md` 的 30 行项目池；外部仓库内容仅作为不可信研究材料读取，未执行其脚本、构建或安装。分支 HEAD 仅用于确定本轮观察快照，表内证据链接尽量固定到对应 commit。

## 口径

### 证据等级

- **A（已深读）**：已有固定 commit 深读，至少覆盖入口、任务/线程、通信或同步、资源生命周期和恢复边界；本地快照 HEAD 已复核。
- **B（配置证明）**：固定快照中存在 target、board、SoC、Kconfig/CMake、`sdkconfig`、PlatformIO 或 CI/test 配置。它证明维护者接入了该芯片目标，不证明本轮构建成功、真机稳定或全部功能可用。
- **C（待核验）**：只有仓库定位、README/文档提及，或本轮 tree/config 扫描没有找到直接 target 证据。不得写成“已支持”。

### 阅读优先级

- **P0 核心**：已完成深读，直接进入学习手册与横向比较。
- **P1 下一轮**：与 ESP-111 的 C5 单核、S3 网关、无线/USB/运行时调度高度相关。
- **P2 扩展**：补充产品化或特定数据面案例，放在核心专题之后。
- **P3 暂缓**：先完成芯片支持验真，再投入源码深读。

### 许可口径

“根许可”只描述仓库或所读组件的许可证，不自动覆盖子模块、managed components、预编译库、模型、字体、媒体、商标或硬件资料。`ESPRESSIF MIT` 指带有“仅在 ESPRESSIF SYSTEMS 产品上使用”限制的定制文本，不按无平台限制的标准 MIT 表述。

## 30 项候选索引

| # | 项目 / GitHub URL | 项目定位 | C5 / S3 相关性与证据 | 许可口径 | 阅读优先级 |
|---:|---|---|---|---|---|
| 1 | [`espressif/esp-idf`](https://github.com/espressif/esp-idf) | Espressif 官方系统框架；FreeRTOS、驱动、网络、功耗与系统服务基线 | **A · C5/S3**；固定 [`f0887bcf`](https://github.com/espressif/esp-idf/tree/f0887bcf8763266effe3fa0b358340df226a04b5)，已核验单/双核、event loop、WDT 与无线示例 | 根 Apache-2.0；部分示例为 CC0/Unlicense，按文件 SPDX 复用 | **P0 核心** |
| 2 | [`espressif/arduino-esp32`](https://github.com/espressif/arduino-esp32) | ESP32 Arduino core；`loopTask`、兼容层、库与板级 variants | **B · C5/S3**；快照 [`b1208ef0`](https://github.com/espressif/arduino-esp32/tree/b1208ef0293c363dd93fa2aa704fc05791433317) 有 [`XIAO_ESP32C5`](https://github.com/espressif/arduino-esp32/tree/b1208ef0293c363dd93fa2aa704fc05791433317/variants/XIAO_ESP32C5) 和多组 S3 variant/validation 配置；未深读 core 成熟度 | 根 LGPL-2.1；第三方库与 binary 另计 | **P1 下一轮** |
| 3 | [`esp-rs/esp-hal`](https://github.com/esp-rs/esp-hal) | Rust `no_std` HAL、Embassy/RTOS、radio C ABI 适配与 ownership | **A · C5/S3**；固定 [`035f29ce`](https://github.com/esp-rs/esp-hal/tree/035f29ce083794e845a81efe3c43cca6c944c1dd)，已区分 C5 partial bring-up 与 S3 双核/专用外设 | 标准 `MIT OR Apache-2.0` 双许可；依赖和厂商 blob 不自动同许可 | **P0 核心** |
| 4 | [`espressif/esp-zigbee-sdk`](https://github.com/espressif/esp-zigbee-sdk) | 官方 Zigbee SDK；endpoint、callback、ZCL、OTA、commissioning 与网关示例 | **A · C5**；固定 [`7eff0fbe`](https://github.com/espressif/esp-zigbee-sdk/tree/7eff0fbe19bcf2acd112ba0b5f080530efc49626)，已深读单一 Zigbee owner task、跨任务 lock/queue post、BDB、NVS/deep-sleep 与恢复边界。S3 不宣称原生 802.15.4 | 根/公开 component Apache-2.0、示例多 CC0；v2 核心是 proprietary 预编译 Zigbee stack，内部调度不可审计 | **P0 核心** |
| 5 | [`espressif/esp-thread-br`](https://github.com/espressif/esp-thread-br) | Thread Border Router 产品集成、backbone、配网、RCP 更新与 Web 管理 | **A · C5 host + 外置 RCP**；固定 [`25ab2049`](https://github.com/espressif/esp-thread-br/tree/25ab20499f64506ec724a2844a643d9227bceeeb)，深读已确认 C5 单天线限制与官方推荐外置 RCP，而非把 C5 写成同芯片并发 Thread+Wi-Fi BR | 根/主要组件 Apache-2.0；部分示例 CC0 | **P0 核心** |
| 6 | [`espressif/esp-matter`](https://github.com/espressif/esp-matter) | Matter 设备 SDK；CHIP event、commissioning、属性、持久化与 OTA | **A · C5/S3**；固定 [`0b82a305`](https://github.com/espressif/esp-matter/tree/0b82a30549606689e6ea050d7579b382406316dc)，Light 有两目标 HAL/defaults；C5 可 Wi-Fi/Thread，S3 为 Wi-Fi 路径 | 根 Apache-2.0；Light 示例多 CC0；认证、商标与 connectedhomeip 第三方权利另计 | **P0 核心** |
| 7 | [`espressif/esp-iot-bridge`](https://github.com/espressif/esp-iot-bridge) | 多 netif、NAT/bridge、WAN/LAN、DNS 与网络迁移 | **A · C5/S3**；固定 [`667766b0`](https://github.com/espressif/esp-iot-bridge/tree/667766b0feefb199afc5de7c59fc330048641fcb)，CI/Kconfig/target defaults 与调度路径已深读；未读取在线 CI 结果 | **无统一根许可**；所读 `iot_bridge` component 为 Apache-2.0，Linux host driver 子树含 GPL-2.0 | **P0 核心** |
| 8 | [`espressif/esp-rainmaker`](https://github.com/espressif/esp-rainmaker) | 云端 agent、参数模型、claim/provisioning、work queue 与 OTA | **B · C5/S3**；快照 [`c93e0ed2`](https://github.com/espressif/esp-rainmaker/tree/c93e0ed2de2f42bac6dd13424dee45ad2db50c92) 有多个 [`sdkconfig.defaults.esp32c5`](https://github.com/espressif/esp-rainmaker/blob/c93e0ed2de2f42bac6dd13424dee45ad2db50c92/examples/fan/sdkconfig.defaults.esp32c5) 及 S3 camera defaults；未闭环 agent task/work queue | 根 Apache-2.0；云 SDK 依赖和示例组件另计 | **P1 下一轮** |
| 9 | [`espressif/esp-csi`](https://github.com/espressif/esp-csi) | Wi-Fi CSI 采集、雷达/检测应用与端到端工具 | **A · C5/S3**；固定 [`8633d671`](https://github.com/espressif/esp-csi/tree/8633d67152db2808f141cc1595970aa9cf406045)，已深读 C5 HE/带宽分支、S3 legacy 路径与 callback/worker 正反例 | 根 Apache-2.0；示例/工具依赖另计 | **P0 核心** |
| 10 | [`espressif/esp-sr`](https://github.com/espressif/esp-sr) | AFE、WakeNet、MultiNet、VAD 与语音模型/预编译库 | **A · S3**；固定 [`7ff63a7d`](https://github.com/espressif/esp-sr/tree/7ff63a7da40e15e502681be48c4d0e78475544a3)，S3 AFE build/test 配置已深读。C5 的 WakeNet 线索不能外推为完整 AFE 支持 | 根为受限 **ESPRESSIF MIT**；部分头文件 Apache-2.0、测试 CC0，模型/唤醒词与品牌权利另计 | **P0 核心** |
| 11 | [`espressif/esp-adf`](https://github.com/espressif/esp-adf) | 音频 element、ringbuffer、event interface 与 pipeline 生命周期 | **A · S3**；固定 [`b875a6e3`](https://github.com/espressif/esp-adf/tree/b875a6e3385f730a49794a59954d4b7940502962)，已深读 element task、ringbuffer、abort/stop 与 core/PSRAM 参数 | 根为受限 **ESPRESSIF MIT**；codec 与第三方组件逐项核查 | **P0 核心** |
| 12 | [`espressif/esp-who`](https://github.com/espressif/esp-who) | camera、检测/识别、LCD 与视觉 pipeline | **A · S3**；固定 [`2475f145`](https://github.com/espressif/esp-who/tree/2475f1456e49492d71e2e20e499377a8fd747ae4)，S3-EYE/Korvo BSP、S3 camera 实现、task affinity 与 frame ring 已深读 | 根为受限 **ESPRESSIF MIT**；ESP-DL、camera、BSP/model 依赖另计 | **P0 核心** |
| 13 | [`espressif/esp-dl`](https://github.com/espressif/esp-dl) | AIoT 推理库、模型部署、算子、内存与芯片优化 | **B · S3**；快照 [`c568e56f`](https://github.com/espressif/esp-dl/tree/c568e56fc24ef78235fa7b0311781380d6379ce6) 有大量 S3 example `sdkconfig.defaults` 与 S3 model artifact；未深读 allocator/算子调度 | 根为标准 MIT；模型、测试数据与外部依赖另计 | **P2 扩展** |
| 14 | [`espressif/esp-box`](https://github.com/espressif/esp-box) | ESP32-S3-BOX 产品参考；语音、显示、网络与板级交互 | **B · S3**；快照 [`aae1b7a6`](https://github.com/espressif/esp-box/tree/aae1b7a6d193d968cf53e42f291a60c4ca1c622c) 的 factory/chatgpt demo defaults 明确 `CONFIG_IDF_TARGET=esp32s3`；尚未闭环任务和恢复 | 根 Apache-2.0；示例依赖、模型与媒体另计 | **P2 扩展** |
| 15 | [`espressif/esp-brookesia`](https://github.com/espressif/esp-brookesia) | HMI components、TaskScheduler、ServiceManager、GUI/runtime 与 profiler | **A · S3**；固定 [`39871f5f`](https://github.com/espressif/esp-brookesia/tree/39871f5f0f491fec476e54b8736661e73f3c8997)，已深读 Asio worker/strand、service RAII、统计与 stop 边界 | 根及所读 scheduler/service 组件 Apache-2.0；媒体、模拟器和第三方组件另计 | **P0 核心** |
| 16 | [`espressif/esp-iot-solution`](https://github.com/espressif/esp-iot-solution) | 显示、USB、传感器、低功耗与应用组件集合 | **B · S3；C · C5**；快照 [`9971a469`](https://github.com/espressif/esp-iot-solution/tree/9971a4692b5c50fbe055db786a9bd6f541372b6e) 有多组 S3 display/USB test `sdkconfig`；本轮未找到 C5 target 配置，不能沿用原候选的 “S3/C5” 强表述 | 根 Apache-2.0；各 component、预编译 codec 与示例依赖另计 | **P1 下一轮（S3）** |
| 17 | [`espressif/esp-usb`](https://github.com/espressif/esp-usb) + [`hathach/tinyusb`](https://github.com/hathach/tinyusb) | Espressif USB host/device components 与其上游跨平台 USB stack | **B · S3**；快照 [`esp-usb@82d60579`](https://github.com/espressif/esp-usb/tree/82d60579e0fd930ca3b45789ff873a0464b94d4c) 有 S3 UVC defaults/USBCV 结果；[`tinyusb@fa750d6b`](https://github.com/hathach/tinyusb/tree/fa750d6bf045f1df4314d283dfe0508d0d066559) 有 Espressif S3 BSP。两仓库不能合并成一个许可或支持声明 | `esp-usb` **无统一根许可**，所见 device/UVC components 为 Apache-2.0；TinyUSB 根 MIT | **P1 下一轮** |
| 18 | [`espressif/esp32-camera`](https://github.com/espressif/esp32-camera) | DVP camera driver、DMA、frame buffer、PSRAM 与 sensor 支持 | **B · S3**；快照 [`202df95d`](https://github.com/espressif/esp32-camera/tree/202df95d7b1dc72e9303ad78f47b8dc9f339e6a1) 有专用 [`target/esp32s3/ll_cam.c`](https://github.com/espressif/esp32-camera/blob/202df95d7b1dc72e9303ad78f47b8dc9f339e6a1/target/esp32s3/ll_cam.c)；未独立深读 driver ISR/ownership | 根 Apache-2.0；sensor/vendor 内容另计 | **P1 下一轮** |
| 19 | [`espressif/esp-video-components`](https://github.com/espressif/esp-video-components) | camera/video capture、V4L2、UVC、storage 与 server 组件 | **B · C5/S3 配置**；快照 [`67bdb9b5`](https://github.com/espressif/esp-video-components/tree/67bdb9b5a40776aae582d7a23dfd373670495ae8) 同时有 C5/S3 capture、server、custom-format defaults；需逐例确认 C5 是哪类输入/输出，不外推为 C5 拥有 S3 camera 外设 | **无统一根许可**；`esp_video` component 为受限 ESPRESSIF MIT，其他组件逐项核查 | **P1 下一轮** |
| 20 | [`78/xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32) | 多板型语音终端；状态机、Opus、WebSocket/MQTT+UDP、OTA、MCP 与显示/camera | **A · S3**；固定 [`7b190b78`](https://github.com/78/xiaozhi-esp32/tree/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8)，已深读任务/音频队列/重连/OTA；103 个 S3 manifest 与 workflow 不等于该 SHA 全部构建成功 | 根标准 MIT；Component Manager 多方依赖和 vendored 代码另计 | **P0 核心** |
| 21 | [`scottbez1/smartknob`](https://github.com/scottbez1/smartknob) | 力反馈旋钮；FOC、电机 owner task、GUI、串口与状态发布 | **A · S3 兼容实例**；固定 [`4eb98839`](https://github.com/scottbez1/smartknob/tree/4eb988399c3fda6ffd3006772856093dfe9adb86)，S3 来自两个 PlatformIO variant，默认环境仍是旧 ESP32；不能写成 S3-first 项目 | 根 Apache-2.0；硬件设计与第三方库另计 | **P0 核心** |
| 22 | [`BasedHardware/OpenGlass`](https://github.com/BasedHardware/OpenGlass) | AI smart glasses；camera/audio/wireless 与手机/服务端协同 | **C · S3 待核验**；观察快照 [`19f07775`](https://github.com/BasedHardware/OpenGlass/tree/19f077757813265515b83e291807ce34a60751e8)。本轮 recursive tree 和常见 PlatformIO 路径未找到直接 S3 target 配置，不能只因 README/产品描述而升级 | 根许可文本为标准 MIT；硬件、模型、服务端和第三方依赖另计 | **P3 暂缓** |
| 23 | [`i-am-shodan/USBArmyKnife`](https://github.com/i-am-shodan/USBArmyKnife) | USB 复合设备与近距离安全测试工具；脚本/存储/显示编排 | **B · S3**；快照 [`59db1bab`](https://github.com/i-am-shodan/USBArmyKnife/tree/59db1bab0ce06497023118211a4c073ec6f36453) 的 [`platformio.ini`](https://github.com/i-am-shodan/USBArmyKnife/blob/59db1bab0ce06497023118211a4c073ec6f36453/platformio.ini) 含 Generic/Waveshare 等多组 `esp32-s3-devkitc-1` env；未深读 USB task/安全边界 | 根标准 MIT；payload、第三方库及使用合规性另计 | **P2 扩展** |
| 24 | [`geo-tp/ESP32-Bit-Pirate`](https://github.com/geo-tp/ESP32-Bit-Pirate) | Web CLI 多协议硬件工具；I2C/SPI/UART/GPIO 与资源仲裁 | **B · S3**；快照 [`9b5abd77`](https://github.com/geo-tp/ESP32-Bit-Pirate/tree/9b5abd77cde0c70e676f3cc4549617a8ac63099f) 的 [`platformio.ini`](https://github.com/geo-tp/ESP32-Bit-Pirate/blob/9b5abd77cde0c70e676f3cc4549617a8ac63099f/platformio.ini) 有多组 S3 board/env；未闭环调度与并发访问 | 根标准 MIT；Web assets/库依赖另计 | **P2 扩展** |
| 25 | [`esphome/esphome`](https://github.com/esphome/esphome) | 配置生成型 IoT 框架；component scheduler、网络恢复、OTA 与自动化 | **B · C5/S3**；快照 [`88ca0d44`](https://github.com/esphome/esphome/tree/88ca0d44e0b0cc7d6c91b2d638ebc1ee773438e2) 有 C5/S3 IDF test YAML，S3 defaults 与 USB/OTA/deep-sleep tests；未深读 scheduler/runtime 差异 | **混合许可**：C/C++ runtime 为 GPLv3，Python 及其余代码为 MIT，见根 LICENSE；组件依赖另计 | **P1 下一轮** |
| 26 | [`arendst/Tasmota`](https://github.com/arendst/Tasmota) | 本地优先 IoT 固件；cooperative loop、driver dispatch、规则、MQTT/HTTP 与 OTA | **B · C5/S3**；快照 [`a6454b74`](https://github.com/arendst/Tasmota/tree/a6454b7459969f9569182ae00e55af4f8d240fdb) 有 `boards/esp32c5*.json` 与多组 `esp32s3*.json`；board JSON 不证明所有 driver 可用或该 SHA 构建通过 | 根 GPL-3.0；bundled libraries/字体/媒体另计 | **P2 扩展** |
| 27 | [`wled/WLED`](https://github.com/wled/WLED) | 实时 LED 控制、网络服务、效果引擎与用户扩展 | **B · S3**；原候选 owner `Aircoookie` 当前重定向到 canonical `wled/WLED`。快照 [`bc2c80d9`](https://github.com/wled/WLED/tree/bc2c80d985d09cac2f9f912f6b14ee9f71f4a9bd) 的 [`platformio.ini`](https://github.com/wled/WLED/blob/bc2c80d985d09cac2f9f912f6b14ee9f71f4a9bd/platformio.ini) 有通用 S3 定义和多组 S3 env | 根 **EUPL-1.2 or later**，不是 MIT；usermods/依赖另计 | **P2 扩展** |
| 28 | [`meshtastic/firmware`](https://github.com/meshtastic/firmware) | 离网 mesh 固件；radio、队列、sleep、设备状态机与多板支持 | **B · S3**；快照 [`9060ab44`](https://github.com/meshtastic/firmware/tree/9060ab44187f75bfbe558c78e34dd5016921b434) 有 `variants/esp32s3/*/platformio.ini` 和多个 S3 board JSON；未深读 radio/PM task | 根 GPL-3.0；协议库、字体、板级 assets 另计 | **P2 扩展** |
| 29 | [`apache/nuttx`](https://github.com/apache/nuttx) | POSIX 风格实时操作系统；arch/board/driver/SMP 与网络模型 | **B · S3；C · C5**；快照 [`95e45611`](https://github.com/apache/nuttx/tree/95e4561145f626420bd03b81efe8e3beb9fd75e1) 有 `arch/xtensa/src/esp32s3` 和多块 S3 board；本轮 recursive tree 未发现 `esp32c5` port，故原候选的 C5/S3 必须降为 S3 已配置、C5 待核验 | 根 Apache-2.0；外部 apps/子模块另计 | **P1 下一轮（S3）** |
| 30 | [`zephyrproject-rtos/zephyr`](https://github.com/zephyrproject-rtos/zephyr) | Devicetree/Kconfig 驱动型 RTOS；workqueue、网络、PM 与多架构 | **B · C5/S3**；快照 [`cde0a961`](https://github.com/zephyrproject-rtos/zephyr/tree/cde0a961d820b34c2141792e22971573b52de3aa) 有 C5/S3 SoC、board、DTS、defconfig 与测试路径；尚未深读 Espressif port 的 runtime/driver 完整度 | 根 Apache-2.0；模块和 HAL 各自许可另计 | **P1 下一轮** |

## A 级固定快照复核

以下 13 个项目满足“固定 commit 深读”门槛。本轮逐仓执行 `git rev-parse HEAD` 与只读工作树检查，HEAD 均与研究记录一致，外部仓库工作树均干净。

| # | 深读仓库 | 记录 SHA / 本地 HEAD | 复核结果 |
|---:|---|---|---|
| 1 | `espressif/esp-idf` | `f0887bcf8763266effe3fa0b358340df226a04b5` | 一致、干净 |
| 2 | `esp-rs/esp-hal` | `035f29ce083794e845a81efe3c43cca6c944c1dd` | 一致、干净 |
| 3 | `espressif/esp-thread-br` | `25ab20499f64506ec724a2844a643d9227bceeeb` | 一致、干净 |
| 4 | `espressif/esp-matter` | `0b82a30549606689e6ea050d7579b382406316dc` | 一致、干净 |
| 5 | `espressif/esp-iot-bridge` | `667766b0feefb199afc5de7c59fc330048641fcb` | 一致、干净 |
| 6 | `espressif/esp-csi` | `8633d67152db2808f141cc1595970aa9cf406045` | 一致、干净 |
| 7 | `espressif/esp-sr` | `7ff63a7da40e15e502681be48c4d0e78475544a3` | 一致、干净 |
| 8 | `espressif/esp-adf` | `b875a6e3385f730a49794a59954d4b7940502962` | 一致、干净 |
| 9 | `espressif/esp-who` | `2475f1456e49492d71e2e20e499377a8fd747ae4` | 一致、干净 |
| 10 | `espressif/esp-brookesia` | `39871f5f0f491fec476e54b8736661e73f3c8997` | 一致、干净 |
| 11 | `scottbez1/smartknob` | `4eb988399c3fda6ffd3006772856093dfe9adb86` | 一致、干净 |
| 12 | `78/xiaozhi-esp32` | `7b190b78e4f8dfef14126f6cd478c134b3cd3cd8` | 一致、干净 |
| 13 | `espressif/esp-zigbee-sdk` | `7eff0fbe19bcf2acd112ba0b5f080530efc49626` | 一致、干净 |

## 必须降级或限定的表述

1. **OpenGlass**：保持 C。项目描述/README 与 S3 相关不等于 build target；本轮未找到直接 S3 target/board 配置。
2. **NuttX**：只写“S3 port/boards 存在”；当前 tree 未发现 C5 port，不写“C5/S3 均支持”。
3. **esp-iot-solution**：只确认 S3 display/USB 测试配置；C5 相关性仍为 C，不能从泛 ESP32 组件集合推断。
4. **esp-thread-br**：C5 深读结论是“主处理器 + 外置 RCP 的推荐 BR 组合”，不是 C5 单芯片同时承载 Wi-Fi backbone 与 Thread radio 的证明。
5. **esp-zigbee-sdk**：C5 公开集成层深读为 A，但 v2 核心 stack 是 proprietary `.a`；内部 queue、锁、路由和调度公平性仍不可见。
6. **esp-sr**：S3 AFE 是 A；C5 的 WakeNet/目标线索不能外推为完整 AFE、MultiNet 或同等性能。
7. **SmartKnob**：两个 S3 PlatformIO environment 证明兼容实例；默认环境仍是旧 ESP32，因此不称 S3-first 产品框架。
8. **xiaozhi-esp32、Arduino、RainMaker、Tasmota、Zephyr**：board/target/test/workflow 配置证明接入意图；未读取固定 SHA 的绿色 CI run，也未真机构建，不能写“全部板型已验证”。
9. **许可证不能扁平化**：`esp-iot-bridge`、`esp-usb`、`esp-video-components` 无统一根许可；`esp-sr`/`esp-adf`/`esp-who`/`esp_video` 是受限 ESPRESSIF MIT；ESPHome 是 runtime GPLv3 + 其他代码 MIT；WLED 是 EUPL-1.2-or-later。
10. **WLED 仓库身份**：原候选 `Aircoookie/WLED` 当前重定向到 `wled/WLED`，来源清单应使用 canonical URL。

## 统计与后续顺序

- 候选行：**30**；涉及 upstream 仓库：**31**（`esp-usb / tinyusb` 为一行、两个独立仓库）。
- 证据等级：**A 13**、**B 16**、**C 1**；其中 `esp-iot-solution` 与 NuttX 是分芯片 `B/C`，按已确认的一侧计入 B。
- 本地快照：**13**；全部已有固定提交深读报告。
- 下一轮 P1 建议顺序：`esp-video-components` → `esp-usb/TinyUSB` → `esp32-camera` → ESPHome/Zephyr → RainMaker；`esp-iot-solution` 聚焦 S3，NuttX 聚焦 S3，避免在未证实的 C5 路径上消耗深读预算。

## 审计限制

- GitHub repository API 的 license 字段只作发现线索；最终许可口径以根/组件许可证原文和已有固定快照为准。
- `git ls-remote` 记录的是审计时 branch HEAD，不代表 release，也不代表 CI 通过；表内链接已固定到观察到的 commit，后续维护时应重新取样。
- recursive tree/path 命中只能证明文件存在；B 级没有升级成运行结论。反过来，路径未命中也只支持保守降级，不能证明项目永久不支持该芯片。
- 本轮未执行任何外部仓库脚本、构建、安装、测试、烧录或硬件操作，也未读取 GitHub Actions/GitLab 对应 SHA 的在线运行结果。

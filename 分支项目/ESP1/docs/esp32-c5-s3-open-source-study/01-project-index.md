# 项目索引与阅读地图

## 1. 筛选方法

候选项目按五个维度选择：

| 维度 | 判断问题 |
|------|----------|
| 芯片相关性 | 是否存在 C5/S3 target、板卡、CI、Kconfig/CMake 或源码分支？ |
| 代码价值 | 是否能看到真实 task、queue、callback、网络、多媒体、功耗或恢复代码？ |
| 可追溯性 | 是否能固定到 commit、路径、行号和许可证？ |
| 工程成熟度 | 是否有构建矩阵、测试、release、依赖锁或明确生命周期？ |
| 互补性 | 是否补充了官方基础、协议、媒体、HMI、社区产品或 RTOS 的不同视角？ |

状态含义：

- **深读 A**：已固定 commit 并闭环到调度、所有权、恢复和限制源码。
- **候选 B**：已有明确芯片/板级/构建证据，但本轮未完整分析内部实现。
- **候选 C**：方向有价值，仍需 target/CI/源码两类证据验真。

## 2. 三十个候选项目

| # | 项目 | 分类 | C5/S3 相关性 | 状态 | 主要学习价值 |
|---|------|------|---------------|------|--------------|
| 1 | [espressif/esp-idf](https://github.com/espressif/esp-idf) | 官方框架 | C5 + S3 | 深读 A | FreeRTOS SMP、event loop、WDT、PM、无线和内存能力基线 |
| 2 | [espressif/arduino-esp32](https://github.com/espressif/arduino-esp32) | 官方框架 | C5/S3 待逐 target 核验 | 候选 C | Arduino loop task、IDF 兼容层、板级变体 |
| 3 | [esp-rs/esp-hal](https://github.com/esp-rs/esp-hal) | Rust HAL | C5 + S3 | 深读 A | `no_std` HAL、Embassy async、interrupt executor、radio controller |
| 4 | [espressif/esp-zigbee-sdk](https://github.com/espressif/esp-zigbee-sdk) | 官方无线 | C5 | 深读 A | Zigbee signal handler、scheduler alarm、commissioning/rejoin、endpoint 模型 |
| 5 | [espressif/esp-thread-br](https://github.com/espressif/esp-thread-br) | 官方网关 | C5 主处理器 + 外置 RCP；S3 + RCP | 深读 A | OpenThread ownership、跨协议 lock、RCP 更新与多接口网关 |
| 6 | [espressif/esp-matter](https://github.com/espressif/esp-matter) | 官方协议 | C5 + S3 配置支持 | 深读 A | CHIP event loop、commissioning、统一属性入口、延迟持久化 |
| 7 | [espressif/esp-iot-bridge](https://github.com/espressif/esp-iot-bridge) | 官方网关 | C5 + S3 build matrix | 深读 A | WAN/LAN、NAPT、DNS/地址迁移、网络恢复 |
| 8 | [espressif/esp-rainmaker](https://github.com/espressif/esp-rainmaker) | 官方云 | C5/S3 待逐 target 核验 | 候选 C | agent、work queue、provisioning、claim 和 OTA |
| 9 | [espressif/esp-csi](https://github.com/espressif/esp-csi) | 官方应用 | C5 + S3 | 深读 A | CSI callback、worker、queue-full、采样和功耗取舍 |
| 10 | [espressif/esp-sr](https://github.com/espressif/esp-sr) | 官方语音 | S3 完整 AFE；C5 局部 WakeNet/AEC | 深读 A | feed/fetch、borrowed result、模型和 PSRAM ownership |
| 11 | [espressif/esp-adf](https://github.com/espressif/esp-adf) | 官方多媒体 | S3 主路径；C5 能力有限 | 深读 A | audio element task、ringbuffer、event iface、EOF/abort/stop |
| 12 | [espressif/esp-who](https://github.com/espressif/esp-who) | 官方视觉 | S3 | 深读 A | camera -> detect -> display、latest-frame、有损过载恢复 |
| 13 | [espressif/esp-dl](https://github.com/espressif/esp-dl) | 官方 AI | S3 | 候选 B | 推理算子、模型内存、SIMD/硬件加速基础 |
| 14 | [espressif/esp-box](https://github.com/espressif/esp-box) | 官方产品参考 | S3 | 候选 B | BOX 板级语音、显示、网络和产品集成 |
| 15 | [espressif/esp-brookesia](https://github.com/espressif/esp-brookesia) | 官方 HMI/服务框架 | S3 测试配置 | 深读 A | Asio worker、strand、service binding、task profiler |
| 16 | [espressif/esp-iot-solution](https://github.com/espressif/esp-iot-solution) | 官方组件集 | S3；C5 需逐组件核验 | 候选 B | USB、display、sensor、低功耗与组件化驱动 |
| 17 | [espressif/esp-usb](https://github.com/espressif/esp-usb) | 官方 USB | S3 | 候选 B | USB host/device class、ISR/task 边界、热插拔恢复 |
| 18 | [espressif/esp32-camera](https://github.com/espressif/esp32-camera) | 官方驱动 | S3 | 候选 B | DMA、framebuffer、PSRAM、grab mode 和 buffer ownership |
| 19 | [espressif/esp-video-components](https://github.com/espressif/esp-video-components) | 官方视频 | S3 | 候选 B | camera/video component、帧池和编码生命周期 |
| 20 | [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) | 社区产品 | S3 多板型 | 深读 A | 语音终端状态机、有界音频队列、协议、OTA |
| 21 | [scottbez1/smartknob](https://github.com/scottbez1/smartknob) | 社区产品 | S3 板级环境 | 深读 A | 双核 motor/display/interface、depth-1 state、实时任务 |
| 22 | [BasedHardware/OpenGlass](https://github.com/BasedHardware/OpenGlass) | 社区产品 | S3 待验真 | 候选 C | camera/audio/无线可穿戴系统 |
| 23 | [i-am-shodan/USBArmyKnife](https://github.com/i-am-shodan/USBArmyKnife) | 社区 USB | S3 待验真 | 候选 C | USB 复合设备、配置和任务编排 |
| 24 | [geo-tp/ESP32-Bit-Pirate](https://github.com/geo-tp/ESP32-Bit-Pirate) | 社区工具 | S3 待验真 | 候选 C | 多协议工具、Web/CLI 与外设资源仲裁 |
| 25 | [esphome/esphome](https://github.com/esphome/esphome) | 社区框架 | C5/S3 待逐版本验真 | 候选 C | component scheduler、网络恢复、代码生成和生态规模 |
| 26 | [arendst/Tasmota](https://github.com/arendst/Tasmota) | 社区固件 | C5/S3 待逐版本验真 | 候选 C | cooperative loop、driver dispatch、规则和持久化 |
| 27 | [Aircoookie/WLED](https://github.com/Aircoookie/WLED) | 社区产品 | S3 | 候选 B | LED 实时输出与 Web/network 服务的调度取舍 |
| 28 | [meshtastic/firmware](https://github.com/meshtastic/firmware) | 社区产品 | S3 待板型核验 | 候选 C | mesh、sleep、消息队列和设备状态机 |
| 29 | [apache/nuttx](https://github.com/apache/nuttx) | RTOS | C5/S3 port 待版本核验 | 候选 C | POSIX RTOS、SMP、驱动和网络模型 |
| 30 | [zephyrproject-rtos/zephyr](https://github.com/zephyrproject-rtos/zephyr) | RTOS | C5/S3 board/SoC 待版本核验 | 候选 C | devicetree、workqueue、网络和电源管理 |

候选表故意保留“待核验”。GitHub repository search 命中、README 提及或 issue 讨论都不足以证明当前 target 可构建。后续扩展时应优先补完 B/C 项的 board、CI、manifest 和源码条件分支。

## 3. 固定提交深读清单

| # | 项目 / commit | 许可口径 | 代表源码入口 | 本轮重点 |
|---|---------------|----------|--------------|----------|
| 1 | [esp-idf `f0887bc`](https://github.com/espressif/esp-idf/tree/f0887bcf8763266effe3fa0b358340df226a04b5) | Apache-2.0；示例按文件许可 | `components/freertos`、`esp_event`、`esp_system`、C5/S3 `soc_caps` | 单/双核、event loop、WDT、caps task/queue、OpenThread/BLE PM |
| 2 | [esp-csi `8633d67`](https://github.com/espressif/esp-csi/tree/8633d67152db2808f141cc1595970aa9cf406045) | Apache-2.0 | `examples/get-started`、`esp-radar`、`esp-crab` | C5/S3 CSI config、callback/worker、pointer queue 正反例 |
| 3 | [esp-matter `0b82a30`](https://github.com/espressif/esp-matter/tree/0b82a30549606689e6ea050d7579b382406316dc) | Apache-2.0；Matter/第三方权利另计 | `examples/light`、`esp_matter_core.cpp`、data model | CHIP task、属性入口、commissioning、fixed-window persistence |
| 4 | [esp-iot-bridge `667766b`](https://github.com/espressif/esp-iot-bridge/tree/667766b0feefb199afc5de7c59fc330048641fcb) | `iot_bridge` Apache-2.0；仓库含 GPL 子树 | `bridge_common.c`、`bridge_wifi.c` | WAN/LAN、DNS event、地址迁移、重连和 lifecycle |
| 5 | [esp-adf `b875a6e`](https://github.com/espressif/esp-adf/tree/b875a6e3385f730a49794a59954d4b7940502962) | Espressif MIT，仅限 Espressif 产品；第三方另计 | `audio_element.c`、`ringbuf.c`、`audio_event_iface.c` | element task、数据/控制面、EOF/abort、stop |
| 6 | [smartknob `4eb9883`](https://github.com/scottbez1/smartknob/tree/4eb988399c3fda6ffd3006772856093dfe9adb86) | Apache-2.0 | `main.cpp`、`motor_task.cpp`、`display_task.cpp` | S3 双核实例、depth-1 overwrite、命令和 pointer 泄漏反例 |
| 7 | [esp-brookesia `39871f5`](https://github.com/espressif/esp-brookesia/tree/39871f5f0f491fec476e54b8736661e73f3c8997) | Apache-2.0；组件另核对 | `task_scheduler.cpp`、`thread_profiler.cpp`、ServiceManager | Asio worker/strand、fixed-delay、RAII binding、stop 风险 |
| 8 | [esp-sr `7ff63a7`](https://github.com/espressif/esp-sr/tree/7ff63a7da40e15e502681be48c4d0e78475544a3) | Espressif MIT，仅限 Espressif 产品；资产另计 | `esp_afe_sr_iface.h`、`model_path.c`、AFE tests | S3 feed/fetch、borrowed view、模型 mapping、停止顺序 |
| 9 | [esp-thread-br `25ab204`](https://github.com/espressif/esp-thread-br/tree/25ab20499f64506ec724a2844a643d9227bceeeb) | Apache-2.0；示例部分 CC0 | `border_router_launch.c`、Wi-Fi config、RCP update | C5 + H2 RCP、OpenThread lock、配网、有限重刷和 reboot |
| 10 | [esp-who `2475f14`](https://github.com/espressif/esp-who/tree/2475f1456e49492d71e2e20e499377a8fd747ae4) | Espressif MIT，仅限 Espressif 产品；依赖另计 | `who_frame_cap_node.cpp`、`who_detect.cpp`、`who_yield2idle.cpp` | S3 latest-frame pipeline、buffer lease 缺口、有损恢复 |
| 11 | [xiaozhi-esp32 `7b190b7`](https://github.com/78/xiaozhi-esp32/tree/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8) | 标准 MIT；组件另计 | `application.cc`、`audio_service.cc`、protocols、OTA | 状态 owner task、有界队列、重连窗口、OTA/stop |
| 12 | [esp-hal `035f29c`](https://github.com/esp-rs/esp-hal/tree/035f29ce083794e845a81efe3c43cca6c944c1dd) | Apache-2.0 OR MIT | C5/S3 device crates、Embassy executor、radio controller | Rust async/interrupt/critical-section 与芯片抽象 |
| 13 | [esp-zigbee-sdk `7eff0fb`](https://github.com/espressif/esp-zigbee-sdk/tree/7eff0fbe19bcf2acd112ba0b5f080530efc49626) | Apache-2.0；预编译栈/第三方另核对 | C5 device examples、common handlers、scheduler alarm | Zigbee main loop、callback、commissioning、rejoin、功耗 |

## 4. 按问题找项目

| 想解决的问题 | 优先阅读 | 再对照 |
|--------------|----------|--------|
| 高频 callback 不阻塞 | esp-csi、esp-idf | ESP-111 C5 callback/worker |
| 状态只保留最新 | SmartKnob、ESP-WHO | S3 snapshot/CSI fusion |
| 连续流如何停止 | ESP-ADF、esp-sr | Xiaozhi AudioService |
| 协议栈单线程 ownership | esp-matter、esp-thread-br | Brookesia strand/group |
| 多网络接口和重连 | esp-iot-bridge、esp-thread-br | Xiaozhi MQTT/WebSocket |
| S3 双核怎么分 | ESP-WHO、SmartKnob、esp-sr | ESP-IDF `sys_evt` 与实际 profile |
| PSRAM 放什么 | ESP-IDF caps API、ESP-ADF、ESP-WHO | Xiaozhi/S3 UDP/audio task |
| 服务生命周期 | Brookesia、Matter | ADF element stop |
| OTA/外设更新 | Xiaozhi、Thread BR | RainMaker 后续候选 |
| C5 多协议/低功耗 | ESP-IDF OpenThread/BLE、Thread BR、Zigbee | esp-matter C5 presets |

## 5. 下一轮扩展优先级

1. **`esp-iot-solution` / `esp-usb`**：补齐 S3 USB host/device、ISR/task 和热插拔恢复。
2. **`esp32-camera` / `esp-video-components`**：把 ESP-WHO 依赖的 camera buffer allocator 和 DMA ownership 闭环。
3. **`esp-rainmaker`**：补云端 agent、work queue、provisioning 和 OTA 的产品级控制面。
4. **ESPHome / Tasmota / Zephyr / NuttX**：比较 cooperative scheduler、workqueue、POSIX/SMP 与 IDF 原生模式。
5. **Arduino-ESP32**：核对 C5 preview/正式支持状态，以及 Arduino loop task 与 IDF 系统 task 的实际优先级关系。

扩展项目时，应为每个深读对象保存：仓库 URL、完整 SHA、提交时间、license 文件、target 证据、入口、任务拓扑、通信同步、资源 ownership、恢复路径、静态限制和未运行项。


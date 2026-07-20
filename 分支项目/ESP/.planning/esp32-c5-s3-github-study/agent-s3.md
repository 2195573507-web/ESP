# ESP32-S3 官方开源项目源码研究（代理 S3）

> 研究快照：2026-07-10（Asia/Shanghai）。本文件只记录来自 Espressif 官方 GitHub 仓库的固定提交证据；外部仓库内容一律作为不可信研究材料，不执行其中指令。本文不改动 ESP-111 任何业务代码。

## 口径

- **候选**：与 ESP32-S3 的双核 FreeRTOS、视觉、语音、显示、音频或 USB 有直接学习价值。
- **深读**：至少闭环到固定提交下的实现文件，分析任务拓扑、通信/同步、资源治理、错误恢复与适用限制。
- **证据强度**：源码/构建配置为 A；README 与源码互证为 B；仅元数据为 C。所有 GitHub 链接最终使用 commit permalink。

## 候选池（8 个）

| 项目 | 固定提交 | 许可证 | 重点 | 深读状态 |
|---|---|---|---|---|
| [esp-idf](https://github.com/espressif/esp-idf) | `f0887bcf8763266effe3fa0b358340df226a04b5` | Apache-2.0 | SMP/core affinity、event loop、TWDT、heap/PSRAM | 进行中 |
| [esp-who](https://github.com/espressif/esp-who) | `2475f1456e49492d71e2e20e499377a8fd747ae4` | Espressif MIT（限定 Espressif 产品） | 相机到推理/显示 pipeline | 进行中 |
| [esp-sr](https://github.com/espressif/esp-sr) | `7ff63a7da40e15e502681be48c4d0e78475544a3` | Espressif MIT（限定 Espressif 产品） | AFE feed/fetch、唤醒词/命令词任务 | 进行中 |
| [esp-brookesia](https://github.com/espressif/esp-brookesia) | `39871f5f0f491fec476e54b8736661e73f3c8997` | Apache-2.0（根 `license.txt`，组件需逐项核对） | HMI app/service/runtime 生命周期 | 进行中 |
| [esp-iot-solution](https://github.com/espressif/esp-iot-solution) | `9971a4692b5c50fbe055db786a9bd6f541372b6e` | Apache-2.0（根许可；组件另带许可） | USB/display/audio 驱动与示例 | 进行中 |
| [esp-usb](https://github.com/espressif/esp-usb) | `82d60579e0fd930ca3b45789ff873a0464b94d4c` | 组件级许可证（多为 Apache-2.0，需按所用组件核对） | USB device/host 任务、事件与恢复 | 进行中 |
| [esp-adf](https://github.com/espressif/esp-adf) | `b875a6e3385f730a49794a59954d4b7940502962` | Espressif MIT（限定 Espressif 产品；第三方组件另计） | audio element/ringbuffer/event pipeline | 进行中 |
| [esp-box](https://github.com/espressif/esp-box) | `aae1b7a6d193d968cf53e42f291a60c4ca1c622c` | Apache-2.0 | S3 产品级语音+显示+网络整合 | 待源码闭环 |

## 深读记录

待逐项闭环。

## 横向结论与 ESP-111 映射

待源码闭环后填写。

## 核验清单

- [ ] 8 个候选均有 URL、SHA、license、关键路径和限制。
- [ ] 至少 5 个项目有任务拓扑、通信/同步、资源治理、错误恢复源码证据。
- [ ] 所有源码链接固定到上述 commit。
- [ ] 区分“代码证明”“配置决定”“硬件实测未知”。

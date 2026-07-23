# 2026-07-23 - C5/C51/C52 startup chain refactor

## 修改目标

将 C51/C52 启动顺序调整为 LCD 优先、本地传感器和本地运行时先于 Wi-Fi，网络和云端服务失败时保留离线运行能力。语音拆分为本地 MIC/ADC/VAD/唤醒检测与网络 PCM transport 两段。

## 修改文件

- `ESPC51/components/Middlewares/app_orchestrator/app_orchestrator.c`
- `ESPC52/components/Middlewares/app_orchestrator/app_orchestrator.c`
- `ESPC51/components/Middlewares/runtime/c5_backpressure_controller.c`
- `ESPC52/components/Middlewares/runtime/c5_backpressure_controller.c`
- `ESPC51/components/Middlewares/wifi/wifi_manager.c`
- `ESPC52/components/Middlewares/wifi/wifi_manager.c`
- `ESPC51/components/Middlewares/mic/mic_adc_test.c`
- `ESPC52/components/Middlewares/mic/mic_adc_test.c`
- `ESPC51/components/Middlewares/radar_ble/include/radar_ble_binding_config.h`
- `ESPC52/components/Middlewares/radar_ble/include/radar_ble_binding_config.h`

## 修改原因与架构变化

编排器现在按 `BOOT -> LCD_READY -> LOCAL_SENSOR_READY -> LOCAL_RUNTIME_READY -> NETWORK_START -> NETWORK_READY/CLOUD_READY` 推进。LCD bootstrap 使用有界等待和降级；BME690、Radar BLE runtime 和 MIC/VAD 在 Wi-Fi 前启动。EventBus、队列、handler、dispatcher 及其 BME/system workers 在网络阶段前建立。Wi-Fi 连接等待有界，失败进入 `OFFLINE_MODE`，重连任务仍可继续工作。

Radar BLE 默认开启，接收、解析和 track pipeline 不依赖 Wi-Fi；仅 Gateway 上传保留网络门控。MIC ADC/VAD 启动不再检查 `wifi_is_stable()`，而 PCM upload、server response 和 playback 仍要求 Gateway ready。BME 本地采样允许离线，HTTP 上传继续由资源和 Gateway 策略限制。

## 资源与风险

没有扩大既有任务栈；LCD bootstrap 保持 Internal static，非 DMA worker/voice 栈沿用现有 PSRAM/资源管理路径。Wi-Fi 初始化仍包含 IDF `ESP_ERROR_CHECK` 路径，个别底层初始化错误可能在进入 `OFFLINE_MODE` 前 abort；真实 Internal/DMA heap 峰值、BLE/Wi-Fi 共存和 worker queue 压力仍需设备遥测确认。

## Build 结果

仅使用 ESP-IDF 5.5.4 和隔离输出目录执行 build：

| Target | Output | Result |
| --- | --- | --- |
| ESPC51 | `/tmp/codex-final-c51-build` | Passed, 1848/1848; `00_Learn.bin` 0x22b920, 57% partition free |
| ESPC52 | `/tmp/codex-final-c52-real-build` | Passed, 1850/1850; `00_Learn.bin` 0x23f020, 55% partition free |

未执行 flash、串口 monitor、硬件冷启动、Wi-Fi/BLE/BME/MIC/语音或服务端验收；build 结果不代表运行时成功。

# LD2450 BLE 链路发现

## 2026-07-18 基线

- C51/C52 `sdkconfig.defaults` 均已打开 `CONFIG_BT_ENABLED`、`CONFIG_BT_NIMBLE_ENABLED` 与 Central role。
- 现有 `radar_ble_transport` 在编译启用分支使用 NimBLE GAP scan/connect、按 UUID 发现 service/characteristic、注册 notify、回调收帧和 1-30 秒退避重连。
- 两端 binding header 目前 `RADAR_BLE_BINDING_ENABLED=0`、address type 未指定、Service/Notify UUID 为空。因此当前启动结果是 unavailable，不能真实连接。
- C5 已有 512 B 有界 Notify 流、LD2450 30-byte parser、latest-only report client。S3 已有 `radar_gateway_ingest`、coordinate transform、zone map、tracker、spatial state 和本地 debug handler。
- 现有活跃 C5->S3 协议端点是 `/local/v1/radar/result`，不应按旧文档改回 `/local/v1/radar/state`。
- 当前 C5 transport 不包含用户指定的 `RADAR_BLE_*` 大写诊断日志，启用阶段需要按状态转换补齐。

## 2026-07-18 协议与硬件复核

- C51/C52 当前 codec 输出旧的 `device_id/local_id/timestamp` JSON；S3 现役 `radar_gateway_ingest` 严格要求 `p=2,id,t,u,q,v`。两端 C5 host test 在此协议漂移处失败，S3 host tests 通过。
- 现役 S3 ingress 在 v2 target 内使用 `slot,x_mm,y_mm,speed_cm_s,resolution_mm,distance_mm`，未携带用户要求的 edge `confidence`。新增协议必须经 adapter 归一化为该稳定输入，不能改写 coordinate transform、zone map、tracker 或 spatial state。
- 主机可见的两个 USB 串口都识别为同一 ESP32-S3（MAC `90:e5:b1:cc:ee:40`），不是 C51/C52；未发现可用于 C5 BLE 运行验证的设备。

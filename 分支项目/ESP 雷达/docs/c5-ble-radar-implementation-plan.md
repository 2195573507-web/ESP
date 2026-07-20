# C5 BLE LD2450 实施计划

## 架构

1. `radar_ble_transport`：NimBLE Central 扫描、绑定 MAC/地址类型、服务与特征发现、Notify CCCD 订阅、写特征 API、断线后上限 30 秒指数退避。
2. `radar_ble_stream`：固定 512 字节 latest-only 缓冲区，统计 Notify 数、字节数、溢出和重同步。
3. `radar_ld2450`：C51/C52 使用与当前 S3 parser 一致的 30 字节协议和 presence 状态机；空间 tracker/zone 仍未迁移。
4. `radar_ble_runtime`：优先级 2、栈 4096 的 `radar_ble_rx` 任务，将流数据交给 `radar_service`；现有 `radar_state_client` 负责快照上报。

## 状态与失败恢复

状态为 `disabled -> unavailable/scanning -> connecting -> discovering -> subscribed -> backoff`。默认空绑定直接进入 `unavailable`。绑定后只接受精确 MAC/地址类型；连接失败、服务/特征发现失败或 Notify 断开均进入有界重试路径。真实设备 UUID、MAC、Notify 成功和手机占用场景仍需硬件验证。

## 资源预算

- Notify 固定缓冲区：512 bytes；解析临时块：128 bytes。
- BLE 回调不分配堆内存、不执行 HTTP、不运行 parser。
- `radar_ble_rx`：priority 2、stack 4096；latest-only，队列深度为 0。
- NimBLE host/controller 由 ESP-IDF `bt` 组件提供，增量资源未在无硬件运行时测量。

## CSI 边界

C51/C52 `sdkconfig.defaults` 和当前 `sdkconfig` 均关闭 `CONFIG_ESP_WIFI_CSI_ENABLED`；CSI 历史源码保留，不删除 Wi-Fi、语音、BME690、命令或显示链路。

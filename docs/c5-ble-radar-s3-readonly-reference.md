# C5 BLE 雷达与 ESPS3 只读行为基准

## 基准结论

ESPS3 仅作为只读行为基准，本任务未修改 ESPS3。C51/C52 复用其 LD2450 字节协议、字段解释和存在状态参数；输入介质由 UART 改为 BLE Notify。

## 源码证据

- `ESPS3/components/radar_ld2450/include/radar_config.h:23-61`：UART1、GPIO18/GPIO17、256000 baud、8N1、1024 字节 RX ring、128 字节读取和 100 ms 读取超时。
- `ESPS3/components/radar_ld2450/ld2450_uart.c:47-75`：UART 参数为 8 data bits、无校验、1 stop bit，驱动使用事件队列和 RX ring。
- `ESPS3/components/radar_ld2450/ld2450_parser.c:11-18,226-274`：帧头 `AA FF 03 00`、固定 30 字节、帧尾 `55 CC`、逐字节重同步、无 CRC。
- `ESPS3/components/radar_ld2450/ld2450_parser.c:77-119,131-161`：X/speed 使用方向位编码，Y 使用 `raw - 32768`，分辨率为 little-endian mm；全零槽位无效，异常坐标被拒绝，并计算欧氏距离。
- `ESPS3/components/radar_ld2450/radar_presence.c:18-56,144-223`：3 帧窗口、2 帧进入、1000 ms 短间隔、900000 ms hold 超时、3000 ms freshness/online 超时；状态为 `unknown`、`motion`、`hold`、`vacant_inferred`。
- `ESPS3/components/Middlewares/radar_domain/radar_local_adapter.c:36-83`：S3 将空间快照转换为统一 registry；远端 C5 不需要改 S3。
- `ESPS3/components/Middlewares/local_http_server/local_http_server.c:888-965,1188`：现有本地 ingest 路由为 `/local/v1/radar/state`，字段由 `radar_protocol` 严格校验。

## 迁移边界

C5 的 BLE 回调只复制 Notify 数据到固定 512 字节流缓冲区；低优先级 `radar_ble_rx` 任务调用现有 `radar_service_ingest_ble_bytes()`，不在回调中执行 parser、HTTP、malloc 或目标状态计算。`radar_state_client` 继续使用已存在的 `/local/v1/radar/state` JSON 契约。

BLE UUID 资料同时给出旧 App 的 FFF1/FFF2 和透传固件的 FFF3/FFF4，因此默认绑定保持空配置；没有唯一 MAC、地址类型和 UUID 时进入 `unavailable`，不会按名称连接第一个设备。

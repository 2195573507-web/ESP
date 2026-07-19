# LD2450 BLE Control Transaction Implementation

日期：2026-07-18

## 范围

本次只修改 C51/C52 的共享 `components/Middlewares/radar_ble` BLE 控制事务。未修改 LD2450 parser、filter、ESPS3 或 HTTP 协议。

## 已确认的 GATT profile

- Service：`FFF0`
- Control characteristic：`FFF2`，动态发现 value handle，属性必须包含 Write Without Response
- Notify characteristic：`FFF1`，动态发现 value handle，属性必须包含 Notify

发现完成后继续输出：

```text
RADAR_BLE_PROFILE_FOUND service=FFF0 write_uuid=FFF2 ... notify_uuid=FFF1 ...
```

代码不按固定 handle 写入，也不接受同名但属性不匹配的 characteristic。

## 事务状态机

```text
GATT discovery complete
  -> RADAR_BLE_STATE_CONTROL_START
  -> radar_ble_send_control_command() [FFF2 Write Without Response]
  -> subscribe FFF1 CCCD
  -> wait for FFF1 notify response
  -> RADAR_BLE_STATE_READY
```

`RADAR_BLE_STATE_READY` 只有在 `notify_subscribed` 和 `control_response_received` 都为真时才可进入。通知原始字节仍按原路径交给下游，不在 C5 增加 parser 或 filter 逻辑。

## 控制写入 API

新增：

```c
int radar_ble_set_control_command(const uint8_t *data, size_t length);
int radar_ble_send_control_command(void);
```

`radar_ble_set_control_command()` 将经过设备抓包确认的 payload 复制到 64 字节预留 buffer。默认 buffer 长度为 0；此时 `radar_ble_send_control_command()` 只输出 `RADAR_BLE_CONTROL_COMMAND_REQUIRED` 并返回失败，绝不发送空写入或猜测的启动字节。

有 payload 且动态发现到 FFF2 后，发送日志为：

```text
RADAR_BLE_CONTROL_TX uuid=FFF2 len=<n>
RADAR_BLE_CONTROL_WAIT_RESPONSE uuid=FFF1
```

收到选定 FFF1 handle 的首个通知时输出：

```text
RADAR_BLE_CONTROL_RESPONSE uuid=FFF1 len=<n>
```

随后控制等待标志清除，响应状态置位，运行时才会把链路报告为 online/READY。

## 未实现和安全边界

- 启动 payload 仍未定义；`RADAR_BLE_START_COMMAND_PLACEHOLDER` 不再触发任何写入。
- 不猜测 ACK 内容、长度或启动命令；当前响应判定是“选定 FFF1 收到通知”。
- 未发现 FFF2 或没有 command buffer 时 fail-closed，不回退到 notify-only READY。
- C51/C52 的 transport、runtime、transport header 保持字节一致；仅 binding identity/MAC 允许不同。

## 验证

软件验证应运行：

```sh
tools/check_c5_radar_parity.sh
tools/test_c5_radar_ble_stream.sh
```

随后分别在 `build-radar-c51` 与 `build-radar-c52` 构建。构建通过只证明源码和编译兼容，不代表真实设备已接受某个未提供的启动 payload；真实 payload、响应语义和首帧时序仍需对同一 LD2450 固件做 ATT 抓包确认。

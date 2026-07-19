# ESPS3 LD2450 UART Timeout / Recovery Fix

## 1. 修改文件

- `ESPS3/components/radar_ld2450/ld2450_parser.c`
- `ESPS3/components/radar_ld2450/include/ld2450_parser.h`
- `ESPS3/components/radar_ld2450/radar_service.c`
- `ESPS3/components/radar_ld2450/radar_uart_recovery.c`
- `ESPS3/components/radar_ld2450/include/radar_uart_recovery.h`
- `ESPS3/components/radar_ld2450/ld2450_uart.c`
- `ESPS3/components/radar_ld2450/include/ld2450_uart.h`
- `ESPS3/components/radar_ld2450/include/radar_config.h`
- `ESPS3/components/radar_ld2450/tests/test_radar_core.c`
- `ESPS3/components/Middlewares/radar_domain/radar_diagnostics.c`
- `ESPS3/components/Middlewares/radar_domain/tests/test_radar_spatial.c`

另外新增设计记录：`docs/superpowers/specs/2026-07-17-radar-uart-timeout-recovery-design.md`。

## 2. 修改原因

- 普通 `uart_read_bytes()` timeout 不再 reset parser，partial frame 会保留。
- parser 增加 `partial_timeout_keep_count` 和 `partial_force_reset_count`；partial 连续无变化达到 2000 ms 才强制清理。
- recovery 不再由短时 no-valid-frame 触发。read error 持续达到阈值、UART overflow/buffer-full、或完全无 RX 字节达到 3000 ms 才进入 recovery。
- recovery 进入时记录 partial 长度、discard 数量和最近 RX 时间，然后停止读取、flush UART FIFO、deinit；重建 UART 后才执行显式 parser force reset。
- `RADAR_UART_RAW_DEBUG` 默认关闭，开启后输出 read 长度及最多前 32 字节 hex。
- 增加 `RADAR_UART_CONFIG` 和 `RADAR_RX` 运行日志；模块诊断日志同步输出 partial keep/force-reset 与 no-RX timeout 计数。

未修改 GPIO、LD2450 协议、30 字节帧格式、`radar_domain` 空间算法、occupancy 或 tracker 逻辑，也未触及 gateway/WiFi/HTTP 和其他传感链路。

## 3. 风险分析

- 普通 read timeout 不清空 partial，若 UART 长时间只收到残缺数据，partial 会保留到 2000 ms 后才清理；这避免了正常帧间空读导致的丢帧，但会延后坏流清理。
- 真实 UART overflow 会立即进入 recovery，可能造成一次重建；这是驱动已明确报告异常时的预期行为。
- recovery 期间 parser 会显式清理，避免旧 partial 跨越 UART 重建；协议和上层状态接口不变。
- 未进行 flash、monitor 或真机验收；硬件行为仍需使用实际 LD2450 数据确认。

## 4. 验证方法

- `sh ESPS3/components/radar_ld2450/tests/run_host_tests.sh`：PASS
- `sh ESPS3/components/Middlewares/radar_domain/tests/run_host_tests.sh`：PASS
- `IDF_PYTHON_ENV_PATH=/Users/zhiqin/.espressif/tools/python_env/idf5.5_py3.14_env idf.py -C ESPS3 build`：PASS
- `git diff --check -- ESPS3`：PASS

ESPS3 构建产物：`ESPS3/build/sensair_s3_gateway.bin`，大小 `0x116eb0` bytes。未执行烧录或 monitor。
